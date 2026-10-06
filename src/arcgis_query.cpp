#include "arcgis_functions.hpp"
#include "arcgis_geometry.hpp"
#include "arcgis_http.hpp"

#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <algorithm>

namespace duckdb {

using namespace duckdb_yyjson; // NOLINT

namespace {

//! Number of object ids requested at once when recovering features a server left out of a truncated response.
constexpr idx_t OBJECT_ID_RETRY_BATCH = 100;
constexpr int64_t DEFAULT_MAX_RECORD_COUNT = 1000;

enum class ArcGISPagination : uint8_t { OFFSET, OBJECT_ID, NONE };

struct ArcGISColumn {
	string name;
	string esri_type;
	LogicalType type;
};

struct ArcGISQueryBindData : public TableFunctionData {
	//! "<layer>/query" including the token and pass-through parameters
	ArcGISUrl query_url;
	string where;
	//! Attribute columns; the column index equals the position in this vector
	vector<ArcGISColumn> fields;
	//! Column index of the geometry column, if the layer has geometries
	optional_idx geometry_column;
	string object_id_field;
	ArcGISPagination pagination = ArcGISPagination::NONE;
	//! Whether a truncated single-request result is an error (true unless pagination := 'none' was requested)
	bool error_on_truncation = true;
	idx_t page_size = 0;
	//! outSR parameter; empty to return geometries in the layer's spatial reference
	string out_sr;
};

struct ArcGISPage {
	idx_t offset = 0;
	idx_t count = 0;
	//! Sorted object ids of the page (object id pagination only)
	vector<int64_t> object_ids;
};

struct ArcGISQueryGlobalState : public GlobalTableFunctionState {
	vector<ArcGISPage> pages;
	atomic<idx_t> next_page {0};
	atomic<idx_t> completed_pages {0};
	vector<column_t> column_ids;
	//! The query request without paging parameters
	ArcGISUrl request_url;
	//! Which cached responses page requests may use (consistent with the planning request)
	ArcGISCachePolicy page_cache;
	idx_t max_threads = 1;

	idx_t MaxThreads() const override {
		return max_threads;
	}
};

struct ArcGISQueryLocalState : public LocalTableFunctionState {
	//! Responses backing `features`
	vector<unique_ptr<ArcGISJSON>> responses;
	vector<yyjson_val *> features;
	idx_t position = 0;
	idx_t batch_index = 0;
	bool has_page = false;
	//! Attribute name (exact and lower-cased) -> output column
	unordered_map<string, idx_t> attribute_outputs;
	unordered_map<string, idx_t> lower_attribute_outputs;
	optional_idx geometry_output;
	string key_buffer;
	string wkb_buffer;
};

LogicalType EsriFieldType(const string &esri_type) {
	if (esri_type == "esriFieldTypeOID" || esri_type == "esriFieldTypeBigInteger") {
		return LogicalType::BIGINT;
	}
	if (esri_type == "esriFieldTypeInteger") {
		return LogicalType::INTEGER;
	}
	if (esri_type == "esriFieldTypeSmallInteger") {
		return LogicalType::SMALLINT;
	}
	if (esri_type == "esriFieldTypeDouble") {
		return LogicalType::DOUBLE;
	}
	if (esri_type == "esriFieldTypeSingle") {
		return LogicalType::FLOAT;
	}
	if (esri_type == "esriFieldTypeDate" || esri_type == "esriFieldTypeTimestampOffset") {
		return LogicalType::TIMESTAMP_TZ;
	}
	if (esri_type == "esriFieldTypeDateOnly") {
		return LogicalType::DATE;
	}
	if (esri_type == "esriFieldTypeTimeOnly") {
		return LogicalType::TIME;
	}
	// esriFieldTypeString, esriFieldTypeGUID, esriFieldTypeGlobalID, esriFieldTypeXML, ...
	return LogicalType::VARCHAR;
}

string CRSFromWkid(int64_t wkid) {
	if (wkid <= 0) {
		return string();
	}
	// Esri-defined codes live in the 53000-54999 and 100000+ ranges; everything else is an EPSG code.
	auto esri = (wkid >= 53000 && wkid < 55000) || wkid >= 100000;
	return (esri ? "ESRI:" : "EPSG:") + to_string(wkid);
}

string CRSFromSpatialReference(yyjson_val *spatial_reference) {
	return CRSFromWkid(ArcGISJSONInt(spatial_reference, "latestWkid", ArcGISJSONInt(spatial_reference, "wkid", 0)));
}

//! Looks up an attribute by name, falling back to a case-insensitive match.
yyjson_val *GetAttribute(yyjson_val *attributes, const string &name) {
	auto val = yyjson_obj_getn(attributes, name.c_str(), name.size());
	if (val) {
		return val;
	}
	size_t idx, max;
	yyjson_val *key, *entry;
	yyjson_obj_foreach(attributes, idx, max, key, entry) {
		if (StringUtil::CIEquals(string(yyjson_get_str(key), yyjson_get_len(key)), name)) {
			return entry;
		}
	}
	return nullptr;
}

bool TryGetObjectId(yyjson_val *feature, const string &object_id_field, int64_t &result) {
	auto val = GetAttribute(yyjson_obj_get(feature, "attributes"), object_id_field);
	if (yyjson_is_int(val)) {
		result = yyjson_get_sint(val);
		return true;
	}
	if (yyjson_is_real(val)) {
		result = static_cast<int64_t>(yyjson_get_real(val));
		return true;
	}
	return false;
}

//===--------------------------------------------------------------------===//
// Bind
//===--------------------------------------------------------------------===//
//! Parameters controlled by arcgis_query; when present in the input URL they are consumed rather than passed through.
const char *const CONTROLLED_PARAMETERS[] = {"f",
                                             "where",
                                             "outFields",
                                             "outSR",
                                             "returnGeometry",
                                             "resultOffset",
                                             "resultRecordCount",
                                             "returnCountOnly",
                                             "returnIdsOnly",
                                             "objectIds"};

unique_ptr<FunctionData> ArcGISQueryBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("arcgis_query: the layer URL cannot be NULL");
	}
	auto layer_url = ArcGISUrl::Parse(StringValue::Get(input.inputs[0]));
	layer_url.StripSuffix("query");

	auto result = make_uniq<ArcGISQueryBindData>();
	// Values in the URL act as defaults for the named parameters
	result->where = layer_url.HasParam("where") ? layer_url.GetParam("where") : "1=1";
	string out_sr = layer_url.HasParam("outSR") ? layer_url.GetParam("outSR") : "4326";
	for (auto param : CONTROLLED_PARAMETERS) {
		layer_url.RemoveParam(param);
	}

	string pagination = "auto";
	Value token;
	optional_idx page_size;
	vector<pair<string, string>> query_params;
	for (auto &entry : input.named_parameters) {
		auto name = StringUtil::Lower(entry.first);
		auto &value = entry.second;
		if (name == "token") {
			token = value;
			continue;
		}
		if (name == "out_sr") {
			out_sr = value.IsNull() ? string() : to_string(value.GetValue<int64_t>());
			continue;
		}
		if (value.IsNull()) {
			throw BinderException("arcgis_query: %s cannot be NULL", entry.first);
		}
		if (name == "where_clause") {
			result->where = StringValue::Get(value);
		} else if (name == "page_size") {
			auto size = value.GetValue<int64_t>();
			if (size <= 0) {
				throw BinderException("arcgis_query: page_size must be positive");
			}
			page_size = static_cast<idx_t>(size);
		} else if (name == "pagination") {
			pagination = StringUtil::Lower(StringValue::Get(value));
			if (pagination != "auto" && pagination != "offset" && pagination != "objectid" && pagination != "none") {
				throw BinderException(
				    "arcgis_query: pagination must be one of 'auto', 'offset', 'objectid' or 'none', got '%s'",
				    pagination);
			}
		} else if (name == "query_params") {
			for (auto &map_entry : MapValue::GetChildren(value)) {
				auto &kv = StructValue::GetChildren(map_entry);
				if (kv[0].IsNull() || kv[1].IsNull()) {
					throw BinderException("arcgis_query: query_params keys and values cannot be NULL");
				}
				query_params.emplace_back(StringValue::Get(kv[0]), StringValue::Get(kv[1]));
			}
		}
	}
	ArcGISApplyToken(context, layer_url, token);

	auto metadata_url = layer_url;
	metadata_url.SetParam("f", "json");
	auto metadata = ArcGISFetchJSON(context, metadata_url);
	auto root = metadata->Root();

	auto fields = yyjson_obj_get(root, "fields");
	if (!yyjson_is_arr(fields)) {
		throw InvalidInputException("arcgis_query: %s is not an ArcGIS layer or table (its metadata has no field "
		                            "list); use arcgis_layers() to list the layers of a service",
		                            layer_url.ToRedactedString());
	}
	auto capabilities = StringUtil::Lower(ArcGISJSONString(root, "capabilities", "query"));
	if (capabilities.find("query") == string::npos) {
		throw InvalidInputException("arcgis_query: layer %s does not support queries (capabilities: %s)",
		                            layer_url.ToRedactedString(), ArcGISJSONString(root, "capabilities"));
	}

	result->object_id_field = ArcGISJSONString(root, "objectIdField");
	size_t idx, max;
	yyjson_val *field;
	yyjson_arr_foreach(fields, idx, max, field) {
		ArcGISColumn column;
		column.name = ArcGISJSONString(field, "name");
		column.esri_type = ArcGISJSONString(field, "type");
		if (column.name.empty() || column.esri_type == "esriFieldTypeGeometry") {
			continue;
		}
		if (column.esri_type == "esriFieldTypeOID" && result->object_id_field.empty()) {
			result->object_id_field = column.name;
		}
		column.type = EsriFieldType(column.esri_type);
		names.push_back(column.name);
		return_types.push_back(column.type);
		result->fields.push_back(std::move(column));
	}

	if (!ArcGISJSONString(root, "geometryType").empty()) {
		string crs;
		if (!out_sr.empty()) {
			int64_t wkid;
			if (TryCast::Operation<string_t, int64_t>(string_t(out_sr), wkid)) {
				crs = CRSFromWkid(wkid);
			}
		} else {
			// Without outSR, MapServer layers return geometries in the map's spatial reference (that of the extent)
			// and FeatureServer layers in the layer's
			auto extent = yyjson_obj_get(root, "extent");
			crs = CRSFromSpatialReference(yyjson_obj_get(extent, "spatialReference"));
			if (crs.empty()) {
				crs = CRSFromSpatialReference(yyjson_obj_get(root, "spatialReference"));
			}
			if (crs.empty()) {
				crs = CRSFromSpatialReference(yyjson_obj_get(root, "sourceSpatialReference"));
			}
		}
		string geometry_name = "geometry";
		for (idx_t suffix = 1;
		     std::any_of(names.begin(), names.end(),
		                 [&](const string &name) { return StringUtil::CIEquals(name, geometry_name); });
		     suffix++) {
			geometry_name = "geometry_" + to_string(suffix);
		}
		result->geometry_column = names.size();
		names.push_back(geometry_name);
		return_types.push_back(crs.empty() ? LogicalType::GEOMETRY() : LogicalType::GEOMETRY(crs));
	}
	if (names.empty()) {
		throw InvalidInputException("arcgis_query: layer %s has no fields", layer_url.ToRedactedString());
	}
	result->out_sr = out_sr;

	auto max_record_count = ArcGISJSONInt(root, "maxRecordCount", DEFAULT_MAX_RECORD_COUNT);
	if (max_record_count <= 0) {
		max_record_count = DEFAULT_MAX_RECORD_COUNT;
	}
	result->page_size = MinValue<idx_t>(page_size.IsValid() ? page_size.GetIndex() : NumericLimits<idx_t>::Maximum(),
	                                    static_cast<idx_t>(max_record_count));

	auto supports_pagination =
	    ArcGISJSONBool(yyjson_obj_get(root, "advancedQueryCapabilities"), "supportsPagination", false);
	if (pagination == "offset") {
		result->pagination = ArcGISPagination::OFFSET;
	} else if (pagination == "objectid") {
		if (result->object_id_field.empty()) {
			throw BinderException("arcgis_query: pagination := 'objectid' requires a layer with an object id field");
		}
		result->pagination = ArcGISPagination::OBJECT_ID;
	} else if (pagination == "none") {
		result->pagination = ArcGISPagination::NONE;
		result->error_on_truncation = false;
	} else if (supports_pagination) {
		result->pagination = ArcGISPagination::OFFSET;
	} else if (!result->object_id_field.empty()) {
		result->pagination = ArcGISPagination::OBJECT_ID;
	} else {
		result->pagination = ArcGISPagination::NONE;
	}

	result->query_url = layer_url.Append("query");
	for (auto &param : query_params) {
		result->query_url.SetParam(param.first, param.second);
	}
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Planning
//===--------------------------------------------------------------------===//
idx_t MaxConcurrentRequests(ClientContext &context) {
	Value setting;
	if (context.TryGetCurrentSetting("arcgis_max_concurrent_requests", setting) && !setting.IsNull()) {
		return MaxValue<idx_t>(setting.GetValue<idx_t>(), 1);
	}
	return 1;
}

//! Plans the pages of an offset paginated scan; returns the cache policy for its page requests
ArcGISCachePolicy PlanOffsetPages(ClientContext &context, const ArcGISQueryBindData &bind, vector<ArcGISPage> &pages) {
	auto url = bind.query_url;
	url.SetParam("where", bind.where);
	url.SetParam("returnCountOnly", "true");
	url.SetParam("f", "json");
	auto response = ArcGISFetchJSON(context, url);
	auto count = ArcGISJSONInt(response->Root(), "count", -1);
	if (count < 0) {
		throw IOException("ArcGIS count query returned no \"count\": %s", url.ToRedactedString());
	}
	for (idx_t offset = 0; offset < static_cast<idx_t>(count); offset += bind.page_size) {
		ArcGISPage page;
		page.offset = offset;
		page.count = MinValue<idx_t>(bind.page_size, static_cast<idx_t>(count) - offset);
		pages.push_back(std::move(page));
	}
	return ArcGISCachePolicy::ForScan(*response);
}

//! Plans the pages of an object id paginated scan; returns the cache policy for its page requests
ArcGISCachePolicy PlanObjectIdPages(ClientContext &context, const ArcGISQueryBindData &bind,
                                    vector<ArcGISPage> &pages) {
	auto url = bind.query_url;
	url.SetParam("where", bind.where);
	url.SetParam("returnIdsOnly", "true");
	url.SetParam("f", "json");
	auto response = ArcGISFetchJSON(context, url);
	auto root = response->Root();
	if (ArcGISJSONBool(root, "exceededTransferLimit", false)) {
		throw IOException("ArcGIS server truncated the object id list of %s; try pagination := 'offset'",
		                  url.ToRedactedString());
	}
	auto ids_val = yyjson_obj_get(root, "objectIds");
	if (!yyjson_is_arr(ids_val) && !yyjson_is_null(ids_val)) {
		throw IOException("ArcGIS object id query returned no \"objectIds\": %s", url.ToRedactedString());
	}
	vector<int64_t> ids;
	ids.reserve(yyjson_arr_size(ids_val));
	size_t idx, max;
	yyjson_val *id;
	yyjson_arr_foreach(ids_val, idx, max, id) {
		if (!yyjson_is_int(id)) {
			throw IOException("ArcGIS object id query returned a non-integer object id: %s", url.ToRedactedString());
		}
		ids.push_back(yyjson_get_sint(id));
	}
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	for (idx_t start = 0; start < ids.size(); start += bind.page_size) {
		ArcGISPage page;
		auto end = MinValue<idx_t>(start + bind.page_size, ids.size());
		page.count = end - start;
		page.object_ids.assign(ids.begin() + static_cast<int64_t>(start), ids.begin() + static_cast<int64_t>(end));
		pages.push_back(std::move(page));
	}
	return ArcGISCachePolicy::ForScan(*response);
}

unique_ptr<GlobalTableFunctionState> ArcGISQueryInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ArcGISQueryBindData>();
	auto result = make_uniq<ArcGISQueryGlobalState>();
	result->column_ids = input.column_ids;

	vector<string> out_fields;
	auto add_field = [&](const string &name) {
		if (std::find(out_fields.begin(), out_fields.end(), name) == out_fields.end()) {
			out_fields.push_back(name);
		}
	};
	bool return_geometry = false;
	for (auto column_id : input.column_ids) {
		if (column_id < bind.fields.size()) {
			add_field(bind.fields[column_id].name);
		} else if (bind.geometry_column.IsValid() && column_id == bind.geometry_column.GetIndex()) {
			return_geometry = true;
		}
	}
	// Object id pagination needs the object ids of returned features to recover from truncated responses
	if (!bind.object_id_field.empty() && (bind.pagination == ArcGISPagination::OBJECT_ID || out_fields.empty())) {
		add_field(bind.object_id_field);
	}
	if (out_fields.empty() && !bind.fields.empty()) {
		add_field(bind.fields[0].name);
	}

	auto &url = result->request_url;
	url = bind.query_url;
	url.SetParam("where", bind.where);
	// "*" keeps URLs short for wide layers
	auto all_fields = out_fields.empty() || out_fields.size() == bind.fields.size();
	url.SetParam("outFields", all_fields ? "*" : StringUtil::Join(out_fields, ","));
	url.SetParam("returnGeometry", return_geometry ? "true" : "false");
	if (return_geometry && !bind.out_sr.empty()) {
		url.SetParam("outSR", bind.out_sr);
	}
	if (bind.pagination == ArcGISPagination::OFFSET && !url.HasParam("orderByFields") &&
	    !bind.object_id_field.empty()) {
		// Offset pagination needs a stable order
		url.SetParam("orderByFields", bind.object_id_field);
	}
	url.SetParam("f", "json");

	switch (bind.pagination) {
	case ArcGISPagination::OFFSET:
		result->page_cache = PlanOffsetPages(context, bind, result->pages);
		break;
	case ArcGISPagination::OBJECT_ID:
		result->page_cache = PlanObjectIdPages(context, bind, result->pages);
		break;
	case ArcGISPagination::NONE:
		result->pages.emplace_back();
		break;
	}
	result->max_threads = MaxValue<idx_t>(MinValue<idx_t>(result->pages.size(), MaxConcurrentRequests(context)), 1);
	return std::move(result);
}

unique_ptr<LocalTableFunctionState> ArcGISQueryInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                         GlobalTableFunctionState *global_state) {
	auto &bind = input.bind_data->Cast<ArcGISQueryBindData>();
	auto &gstate = global_state->Cast<ArcGISQueryGlobalState>();
	auto result = make_uniq<ArcGISQueryLocalState>();
	for (idx_t output_idx = 0; output_idx < gstate.column_ids.size(); output_idx++) {
		auto column_id = gstate.column_ids[output_idx];
		if (column_id < bind.fields.size()) {
			auto &name = bind.fields[column_id].name;
			result->attribute_outputs[name] = output_idx;
			result->lower_attribute_outputs[StringUtil::Lower(name)] = output_idx;
		} else if (bind.geometry_column.IsValid() && column_id == bind.geometry_column.GetIndex()) {
			result->geometry_output = output_idx;
		}
	}
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Fetching pages
//===--------------------------------------------------------------------===//
struct ArcGISResponseInfo {
	idx_t feature_count = 0;
	bool exceeded_transfer_limit = false;
};

ArcGISResponseInfo FetchFeatures(ClientContext &context, const ArcGISQueryGlobalState &gstate,
                                 ArcGISQueryLocalState &state, const ArcGISUrl &url) {
	auto response = ArcGISFetchJSON(context, url, gstate.page_cache);
	auto root = response->Root();
	auto features = yyjson_obj_get(root, "features");
	if (!yyjson_is_arr(features)) {
		throw IOException("ArcGIS query response has no \"features\" array: %s", url.ToRedactedString());
	}
	ArcGISResponseInfo info;
	info.feature_count = yyjson_arr_size(features);
	info.exceeded_transfer_limit = ArcGISJSONBool(root, "exceededTransferLimit", false) ||
	                               ArcGISJSONBool(yyjson_obj_get(root, "properties"), "exceededTransferLimit", false);
	size_t idx, max;
	yyjson_val *feature;
	yyjson_arr_foreach(features, idx, max, feature) {
		state.features.push_back(feature);
	}
	state.responses.push_back(std::move(response));
	return info;
}

void FetchOffsetPage(ClientContext &context, const ArcGISQueryGlobalState &gstate, ArcGISQueryLocalState &state,
                     const ArcGISPage &page) {
	idx_t received = 0;
	while (received < page.count) {
		auto url = gstate.request_url;
		url.SetParam("resultOffset", to_string(page.offset + received));
		url.SetParam("resultRecordCount", to_string(page.count - received));
		auto info = FetchFeatures(context, gstate, state, url);
		if (info.feature_count == 0) {
			// Features were deleted since the count was taken
			break;
		}
		// Servers cap responses at their maximum record count; continue where this response ended
		received += info.feature_count;
	}
	if (received > page.count) {
		// Servers that ignore resultRecordCount return more than requested; those features belong to later pages
		state.features.resize(state.features.size() - (received - page.count));
	}
}

void FetchObjectIdPage(ClientContext &context, const ArcGISQueryBindData &bind, const ArcGISQueryGlobalState &gstate,
                       ArcGISQueryLocalState &state, const ArcGISPage &page) {
	auto url = gstate.request_url;
	auto &oid = bind.object_id_field;
	url.SetParam("where", StringUtil::Format("(%s) AND %s >= %lld AND %s <= %lld", bind.where, oid,
	                                         static_cast<long long>(page.object_ids.front()), oid,
	                                         static_cast<long long>(page.object_ids.back())));
	auto info = FetchFeatures(context, gstate, state, url);
	if (!info.exceeded_transfer_limit) {
		return;
	}
	// The server truncated the response (e.g. a transfer size limit): request the missing features by object id
	unordered_set<int64_t> received;
	for (auto feature : state.features) {
		int64_t id;
		if (TryGetObjectId(feature, oid, id)) {
			received.insert(id);
		}
	}
	vector<int64_t> pending;
	for (auto id : page.object_ids) {
		if (received.find(id) == received.end()) {
			pending.push_back(id);
		}
	}
	while (!pending.empty()) {
		auto batch_size = MinValue<idx_t>(pending.size(), OBJECT_ID_RETRY_BATCH);
		vector<string> batch;
		for (idx_t i = 0; i < batch_size; i++) {
			batch.push_back(to_string(pending[i]));
		}
		auto retry_url = gstate.request_url;
		retry_url.SetParam("objectIds", StringUtil::Join(batch, ","));
		auto first_new = state.features.size();
		auto retry_info = FetchFeatures(context, gstate, state, retry_url);
		for (idx_t i = first_new; i < state.features.size(); i++) {
			int64_t id;
			if (TryGetObjectId(state.features[i], oid, id)) {
				received.insert(id);
			}
		}
		vector<int64_t> still_pending;
		for (idx_t i = 0; i < pending.size(); i++) {
			if (received.find(pending[i]) != received.end()) {
				continue;
			}
			if (i < batch_size && !retry_info.exceeded_transfer_limit) {
				// Answered completely: the remaining ids of the batch no longer match the filter
				continue;
			}
			still_pending.push_back(pending[i]);
		}
		if (still_pending.size() == pending.size()) {
			throw IOException("ArcGIS server keeps truncating responses for %s", retry_url.ToRedactedString());
		}
		pending = std::move(still_pending);
	}
}

void FetchPage(ClientContext &context, const ArcGISQueryBindData &bind, const ArcGISQueryGlobalState &gstate,
               ArcGISQueryLocalState &state, const ArcGISPage &page) {
	state.responses.clear();
	state.features.clear();
	state.position = 0;
	switch (bind.pagination) {
	case ArcGISPagination::OFFSET:
		FetchOffsetPage(context, gstate, state, page);
		break;
	case ArcGISPagination::OBJECT_ID:
		FetchObjectIdPage(context, bind, gstate, state, page);
		break;
	case ArcGISPagination::NONE: {
		auto info = FetchFeatures(context, gstate, state, gstate.request_url);
		if (info.exceeded_transfer_limit && bind.error_on_truncation) {
			throw IOException("ArcGIS server returned a partial result (exceededTransferLimit) for %s, and the layer "
			                  "supports neither pagination nor object ids. Use pagination := 'none' to accept the "
			                  "first %llu features, or narrow the query with where_clause := '...'",
			                  gstate.request_url.ToRedactedString(), static_cast<uint64_t>(info.feature_count));
		}
		break;
	}
	}
}

//===--------------------------------------------------------------------===//
// Value conversion
//===--------------------------------------------------------------------===//
[[noreturn]] void ThrowConversionError(yyjson_val *val, const ArcGISColumn &column) {
	size_t len;
	auto text = yyjson_val_write(val, 0, &len);
	string value_text = text ? string(text, len) : string("?");
	free(text);
	throw ConversionException("arcgis_query: could not convert value %s of field \"%s\" (%s) to %s", value_text,
	                          column.name, column.esri_type, column.type.ToString());
}

template <class T>
void WriteNumber(Vector &vector, idx_t row, yyjson_val *val, const ArcGISColumn &column) {
	T result;
	bool success;
	if (yyjson_is_sint(val)) {
		success = TryCast::Operation<int64_t, T>(yyjson_get_sint(val), result);
	} else if (yyjson_is_uint(val)) {
		success = TryCast::Operation<uint64_t, T>(yyjson_get_uint(val), result);
	} else if (yyjson_is_real(val)) {
		success = TryCast::Operation<double, T>(yyjson_get_real(val), result);
	} else if (yyjson_is_str(val)) {
		success = TryCast::Operation<string_t, T>(string_t(yyjson_get_str(val), yyjson_get_len(val)), result);
	} else {
		success = false;
	}
	if (!success) {
		ThrowConversionError(val, column);
	}
	FlatVector::GetData<T>(vector)[row] = result;
	FlatVector::Validity(vector).SetValid(row);
}

void WriteString(Vector &vector, idx_t row, yyjson_val *val) {
	auto data = FlatVector::GetData<string_t>(vector);
	if (yyjson_is_str(val)) {
		data[row] = StringVector::AddString(vector, yyjson_get_str(val), yyjson_get_len(val));
	} else {
		size_t len;
		auto text = yyjson_val_write(val, 0, &len);
		data[row] = StringVector::AddString(vector, text, len);
		free(text);
	}
	FlatVector::Validity(vector).SetValid(row);
}

void WriteValue(Vector &vector, idx_t row, yyjson_val *val, const ArcGISColumn &column) {
	if (!val || yyjson_is_null(val)) {
		return;
	}
	switch (column.type.id()) {
	case LogicalTypeId::SMALLINT:
		return WriteNumber<int16_t>(vector, row, val, column);
	case LogicalTypeId::INTEGER:
		return WriteNumber<int32_t>(vector, row, val, column);
	case LogicalTypeId::BIGINT:
		return WriteNumber<int64_t>(vector, row, val, column);
	case LogicalTypeId::FLOAT:
		return WriteNumber<float>(vector, row, val, column);
	case LogicalTypeId::DOUBLE:
		return WriteNumber<double>(vector, row, val, column);
	case LogicalTypeId::VARCHAR:
		return WriteString(vector, row, val);
	default:
		break;
	}
	if (column.type.id() == LogicalTypeId::TIMESTAMP_TZ && yyjson_is_num(val)) {
		// esriFieldTypeDate: milliseconds since the Unix epoch (UTC)
		int64_t epoch_ms;
		if (!TryCast::Operation<double, int64_t>(yyjson_get_num(val), epoch_ms)) {
			ThrowConversionError(val, column);
		}
		FlatVector::GetData<timestamp_tz_t>(vector)[row] = timestamp_tz_t(Timestamp::FromEpochMs(epoch_ms));
		FlatVector::Validity(vector).SetValid(row);
		return;
	}
	// DATE / TIME / TIMESTAMP WITH TIME ZONE strings (esriFieldTypeDateOnly, TimeOnly, TimestampOffset)
	if (!yyjson_is_str(val)) {
		ThrowConversionError(val, column);
	}
	Value converted;
	string error;
	if (!Value(string(yyjson_get_str(val), yyjson_get_len(val))).DefaultTryCastAs(column.type, converted, &error)) {
		ThrowConversionError(val, column);
	}
	vector.SetValue(row, converted);
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
void ArcGISQueryScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind = input.bind_data->Cast<ArcGISQueryBindData>();
	auto &gstate = input.global_state->Cast<ArcGISQueryGlobalState>();
	auto &state = input.local_state->Cast<ArcGISQueryLocalState>();

	while (state.position >= state.features.size()) {
		if (state.has_page) {
			state.has_page = false;
			gstate.completed_pages++;
		}
		auto page_idx = gstate.next_page++;
		if (page_idx >= gstate.pages.size()) {
			output.SetCardinality(0);
			return;
		}
		state.batch_index = page_idx;
		state.has_page = true;
		FetchPage(context, bind, gstate, state, gstate.pages[page_idx]);
	}

	auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.features.size() - state.position);
	for (auto &vector : output.data) {
		FlatVector::Validity(vector).SetAllInvalid(count);
	}
	for (idx_t row = 0; row < count; row++) {
		auto feature = state.features[state.position + row];
		auto attributes = yyjson_obj_get(feature, "attributes");
		size_t idx, max;
		yyjson_val *key, *val;
		yyjson_obj_foreach(attributes, idx, max, key, val) {
			state.key_buffer.assign(yyjson_get_str(key), yyjson_get_len(key));
			auto entry = state.attribute_outputs.find(state.key_buffer);
			if (entry == state.attribute_outputs.end()) {
				entry = state.lower_attribute_outputs.find(StringUtil::Lower(state.key_buffer));
				if (entry == state.lower_attribute_outputs.end()) {
					continue;
				}
			}
			auto output_idx = entry->second;
			WriteValue(output.data[output_idx], row, val, bind.fields[gstate.column_ids[output_idx]]);
		}
		if (state.geometry_output.IsValid()) {
			auto &vector = output.data[state.geometry_output.GetIndex()];
			if (ArcGISGeometryToWKB(yyjson_obj_get(feature, "geometry"), state.wkb_buffer)) {
				FlatVector::GetData<string_t>(vector)[row] = StringVector::AddStringOrBlob(vector, state.wkb_buffer);
				FlatVector::Validity(vector).SetValid(row);
			}
		}
	}
	state.position += count;
	output.SetCardinality(count);
}

OperatorPartitionData ArcGISQueryGetPartitionData(ClientContext &context, TableFunctionGetPartitionInput &input) {
	if (input.partition_info.RequiresPartitionColumns()) {
		throw InternalException("arcgis_query: partition columns are not supported");
	}
	return OperatorPartitionData(input.local_state->Cast<ArcGISQueryLocalState>().batch_index);
}

double ArcGISQueryProgress(ClientContext &context, const FunctionData *bind_data,
                           const GlobalTableFunctionState *global_state) {
	auto &gstate = global_state->Cast<ArcGISQueryGlobalState>();
	if (gstate.pages.empty()) {
		return 100.0;
	}
	return 100.0 * static_cast<double>(gstate.completed_pages.load()) / static_cast<double>(gstate.pages.size());
}

} // namespace

void ArcGISQueryFunction::Register(ExtensionLoader &loader) {
	TableFunction function("arcgis_query", {LogicalType::VARCHAR}, ArcGISQueryScan, ArcGISQueryBind,
	                       ArcGISQueryInitGlobal, ArcGISQueryInitLocal);
	function.named_parameters["where_clause"] = LogicalType::VARCHAR;
	function.named_parameters["token"] = LogicalType::VARCHAR;
	function.named_parameters["out_sr"] = LogicalType::INTEGER;
	function.named_parameters["page_size"] = LogicalType::BIGINT;
	function.named_parameters["pagination"] = LogicalType::VARCHAR;
	function.named_parameters["query_params"] = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	function.projection_pushdown = true;
	function.get_partition_data = ArcGISQueryGetPartitionData;
	function.table_scan_progress = ArcGISQueryProgress;
	loader.RegisterFunction(function);
}

} // namespace duckdb
