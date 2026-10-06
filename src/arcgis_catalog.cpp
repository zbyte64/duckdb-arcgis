#include "arcgis_functions.hpp"
#include "arcgis_http.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

using namespace duckdb_yyjson; // NOLINT

namespace {

struct ArcGISCatalogBindData : public TableFunctionData {
	//! URL without token, used to build the URLs reported to the user
	ArcGISUrl url;
	//! URL with token, used for requests
	ArcGISUrl request_url;
};

struct ArcGISCatalogGlobalState : public GlobalTableFunctionState {
	vector<vector<Value>> rows;
	idx_t position = 0;
};

unique_ptr<ArcGISCatalogBindData> BindCatalog(ClientContext &context, TableFunctionBindInput &input,
                                              const string &function_name) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("%s: the URL cannot be NULL", function_name);
	}
	auto result = make_uniq<ArcGISCatalogBindData>();
	result->url = ArcGISUrl::Parse(StringValue::Get(input.inputs[0]));
	result->url.RemoveParam("f");
	Value token;
	auto entry = input.named_parameters.find("token");
	if (entry != input.named_parameters.end()) {
		token = entry->second;
	}
	result->request_url = result->url;
	ArcGISApplyToken(context, result->request_url, token);
	result->url.RemoveParam("token");
	return result;
}

Value OptionalString(yyjson_val *obj, const char *key) {
	auto val = yyjson_obj_get(obj, key);
	if (!yyjson_is_str(val)) {
		return Value(LogicalType::VARCHAR);
	}
	return Value(string(yyjson_get_str(val), yyjson_get_len(val)));
}

void CatalogScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<ArcGISCatalogGlobalState>();
	idx_t count = 0;
	while (state.position < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.position++];
		for (idx_t col = 0; col < row.size(); col++) {
			output.SetValue(col, count, row[col]);
		}
		count++;
	}
	output.SetCardinality(count);
}

//===--------------------------------------------------------------------===//
// arcgis_layers(service_url)
//===--------------------------------------------------------------------===//
unique_ptr<FunctionData> LayersBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names) {
	names = {"id", "name", "type", "geometry_type", "parent_layer_id", "url"};
	return_types = {LogicalType::BIGINT,  LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::BIGINT,  LogicalType::VARCHAR};
	return BindCatalog(context, input, "arcgis_layers");
}

unique_ptr<GlobalTableFunctionState> LayersInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ArcGISCatalogBindData>();
	auto result = make_uniq<ArcGISCatalogGlobalState>();
	auto url = bind.request_url;
	url.SetParam("f", "json");
	auto response = ArcGISFetchJSON(context, url);
	auto root = response->Root();
	auto layers = yyjson_obj_get(root, "layers");
	auto tables = yyjson_obj_get(root, "tables");
	if (!yyjson_is_arr(layers) && !yyjson_is_arr(tables)) {
		throw InvalidInputException("arcgis_layers: %s is not a MapServer or FeatureServer service (no layers or "
		                            "tables in its metadata)",
		                            url.ToRedactedString());
	}
	for (auto list : {layers, tables}) {
		auto default_type = list == tables ? Value("Table") : Value(LogicalType::VARCHAR);
		size_t idx, max;
		yyjson_val *layer;
		yyjson_arr_foreach(list, idx, max, layer) {
			auto id = ArcGISJSONInt(layer, "id", -1);
			if (id < 0) {
				continue;
			}
			auto parent = ArcGISJSONInt(layer, "parentLayerId", -1);
			auto type = OptionalString(layer, "type");
			result->rows.push_back({Value::BIGINT(id), OptionalString(layer, "name"),
			                        type.IsNull() ? default_type : type, OptionalString(layer, "geometryType"),
			                        parent < 0 ? Value(LogicalType::BIGINT) : Value::BIGINT(parent),
			                        Value(bind.url.Append(to_string(id)).ToString())});
		}
	}
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// arcgis_services(url)
//===--------------------------------------------------------------------===//
unique_ptr<FunctionData> ServicesBind(ClientContext &context, TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types, vector<string> &names) {
	names = {"folder", "name", "type", "url"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return BindCatalog(context, input, "arcgis_services");
}

//! The ".../rest/services" prefix of a catalog or folder URL; service names are relative to it.
string ServicesRoot(const string &base) {
	static const string SERVICES_PATH = "/rest/services";
	auto pos = StringUtil::Lower(base).find(SERVICES_PATH);
	if (pos == string::npos) {
		return base;
	}
	return base.substr(0, pos + SERVICES_PATH.size());
}

void AddServices(yyjson_val *root, const string &services_root, vector<vector<Value>> &rows) {
	size_t idx, max;
	yyjson_val *service;
	yyjson_arr_foreach(yyjson_obj_get(root, "services"), idx, max, service) {
		auto name = ArcGISJSONString(service, "name");
		auto type = ArcGISJSONString(service, "type");
		if (name.empty()) {
			continue;
		}
		auto slash = name.rfind('/');
		auto folder = slash == string::npos ? Value(LogicalType::VARCHAR) : Value(name.substr(0, slash));
		auto url = ArcGISJSONString(service, "url");
		if (url.empty()) {
			url = services_root + "/" + name + (type.empty() ? "" : "/" + type);
		}
		rows.push_back({folder, Value(name), type.empty() ? Value(LogicalType::VARCHAR) : Value(type), Value(url)});
	}
}

unique_ptr<GlobalTableFunctionState> ServicesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ArcGISCatalogBindData>();
	auto result = make_uniq<ArcGISCatalogGlobalState>();
	auto services_root = ServicesRoot(bind.url.Base());

	auto url = bind.request_url;
	url.SetParam("f", "json");
	auto response = ArcGISFetchJSON(context, url);
	auto root = response->Root();
	if (!yyjson_is_arr(yyjson_obj_get(root, "services")) && !yyjson_is_arr(yyjson_obj_get(root, "folders"))) {
		throw InvalidInputException("arcgis_services: %s is not an ArcGIS services directory (expected a URL ending "
		                            "in /rest/services or a folder below it)",
		                            url.ToRedactedString());
	}
	AddServices(root, services_root, result->rows);

	// ArcGIS Server folders are one level deep: services of a folder are listed by the folder resource
	size_t idx, max;
	yyjson_val *folder;
	yyjson_arr_foreach(yyjson_obj_get(root, "folders"), idx, max, folder) {
		if (!yyjson_is_str(folder)) {
			continue;
		}
		auto folder_url = bind.request_url.Append(string(yyjson_get_str(folder), yyjson_get_len(folder)));
		folder_url.SetParam("f", "json");
		auto folder_response = ArcGISFetchJSON(context, folder_url);
		AddServices(folder_response->Root(), services_root, result->rows);
	}
	return std::move(result);
}

} // namespace

void ArcGISCatalogFunctions::Register(ExtensionLoader &loader) {
	TableFunction layers("arcgis_layers", {LogicalType::VARCHAR}, CatalogScan, LayersBind, LayersInit);
	layers.named_parameters["token"] = LogicalType::VARCHAR;
	loader.RegisterFunction(layers);

	TableFunction services("arcgis_services", {LogicalType::VARCHAR}, CatalogScan, ServicesBind, ServicesInit);
	services.named_parameters["token"] = LogicalType::VARCHAR;
	loader.RegisterFunction(services);
}

} // namespace duckdb
