#include "arcgis_http.hpp"
#include "arcgis_cache.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/http_util.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

using namespace duckdb_yyjson; // NOLINT

//===--------------------------------------------------------------------===//
// URL handling
//===--------------------------------------------------------------------===//
static int HexValue(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

static string PercentDecode(const string &input) {
	string result;
	result.reserve(input.size());
	for (idx_t i = 0; i < input.size(); i++) {
		auto c = input[i];
		if (c == '+') {
			result += ' ';
		} else if (c == '%' && i + 2 < input.size() && HexValue(input[i + 1]) >= 0 && HexValue(input[i + 2]) >= 0) {
			result += static_cast<char>(HexValue(input[i + 1]) * 16 + HexValue(input[i + 2]));
			i += 2;
		} else {
			result += c;
		}
	}
	return result;
}

static string PercentEncode(const string &input) {
	static constexpr const char *HEX = "0123456789ABCDEF";
	string result;
	result.reserve(input.size() * 3);
	for (auto ch : input) {
		auto c = static_cast<unsigned char>(ch);
		if (StringUtil::CharacterIsAlphaNumeric(static_cast<char>(c)) || c == '-' || c == '.' || c == '_' || c == '~') {
			result += static_cast<char>(c);
		} else {
			result += '%';
			result += HEX[c >> 4];
			result += HEX[c & 0xF];
		}
	}
	return result;
}

ArcGISUrl ArcGISUrl::Parse(const string &url_p) {
	auto url = url_p;
	StringUtil::Trim(url);
	auto lower = StringUtil::Lower(url);
	if (!StringUtil::StartsWith(lower, "http://") && !StringUtil::StartsWith(lower, "https://")) {
		throw InvalidInputException("ArcGIS URL must start with http:// or https://, got \"%s\"", url);
	}
	auto fragment_pos = url.find('#');
	if (fragment_pos != string::npos) {
		url = url.substr(0, fragment_pos);
	}
	ArcGISUrl result;
	auto query_pos = url.find('?');
	result.base = url.substr(0, query_pos);
	while (!result.base.empty() && result.base.back() == '/') {
		result.base.pop_back();
	}
	if (query_pos != string::npos) {
		for (auto &entry : StringUtil::Split(url.substr(query_pos + 1), '&')) {
			if (entry.empty()) {
				continue;
			}
			auto eq_pos = entry.find('=');
			auto key = PercentDecode(entry.substr(0, eq_pos));
			auto value = eq_pos == string::npos ? string() : PercentDecode(entry.substr(eq_pos + 1));
			if (!key.empty()) {
				result.SetParam(key, value);
			}
		}
	}
	return result;
}

ArcGISUrl ArcGISUrl::Append(const string &segment) const {
	ArcGISUrl result(*this);
	result.base += "/" + segment;
	return result;
}

bool ArcGISUrl::StripSuffix(const string &segment) {
	auto suffix = "/" + StringUtil::Lower(segment);
	if (StringUtil::EndsWith(StringUtil::Lower(base), suffix)) {
		base = base.substr(0, base.size() - suffix.size());
		return true;
	}
	return false;
}

bool ArcGISUrl::HasParam(const string &key) const {
	for (auto &param : params) {
		if (StringUtil::CIEquals(param.first, key)) {
			return true;
		}
	}
	return false;
}

string ArcGISUrl::GetParam(const string &key) const {
	for (auto &param : params) {
		if (StringUtil::CIEquals(param.first, key)) {
			return param.second;
		}
	}
	return string();
}

void ArcGISUrl::SetParam(const string &key, const string &value) {
	for (auto &param : params) {
		if (StringUtil::CIEquals(param.first, key)) {
			param.second = value;
			return;
		}
	}
	params.emplace_back(key, value);
}

void ArcGISUrl::RemoveParam(const string &key) {
	for (idx_t i = 0; i < params.size(); i++) {
		if (StringUtil::CIEquals(params[i].first, key)) {
			params.erase(params.begin() + static_cast<int64_t>(i));
			return;
		}
	}
}

void ArcGISUrl::ClearParams() {
	params.clear();
}

static string BuildUrl(const string &base, const vector<pair<string, string>> &params, bool redact) {
	string result = base;
	for (idx_t i = 0; i < params.size(); i++) {
		result += i == 0 ? '?' : '&';
		result += PercentEncode(params[i].first);
		result += '=';
		if (redact && StringUtil::CIEquals(params[i].first, "token")) {
			result += "***";
		} else {
			result += PercentEncode(params[i].second);
		}
	}
	return result;
}

string ArcGISUrl::ToString() const {
	return BuildUrl(base, params, false);
}

string ArcGISUrl::ToRedactedString() const {
	return BuildUrl(base, params, true);
}

//===--------------------------------------------------------------------===//
// JSON helpers
//===--------------------------------------------------------------------===//
ArcGISJSON::ArcGISJSON(yyjson_doc *doc_p, yyjson_val *root_p, timestamp_t written_at_p, bool from_cache_p)
    : doc(doc_p), root(root_p), written_at(written_at_p), from_cache(from_cache_p) {
}

ArcGISJSON::~ArcGISJSON() {
	yyjson_doc_free(doc);
}

yyjson_doc *ArcGISParseJSON(const string &body, yyjson_read_err &err) {
	// Without YYJSON_READ_INSITU the input is not modified
	return yyjson_read_opts(const_cast<char *>(body.data()), body.size(), YYJSON_READ_ALLOW_INF_AND_NAN, nullptr, &err);
}

ArcGISCachePolicy ArcGISCachePolicy::ForScan(const ArcGISJSON &planning_response) {
	ArcGISCachePolicy result;
	result.read = planning_response.FromCache();
	result.snapshot = planning_response.WrittenAt().value;
	return result;
}

string ArcGISJSONString(yyjson_val *obj, const char *key, const string &default_value) {
	auto val = yyjson_obj_get(obj, key);
	if (!yyjson_is_str(val)) {
		return default_value;
	}
	return string(yyjson_get_str(val), yyjson_get_len(val));
}

int64_t ArcGISJSONInt(yyjson_val *obj, const char *key, int64_t default_value) {
	auto val = yyjson_obj_get(obj, key);
	if (yyjson_is_int(val)) {
		return yyjson_get_sint(val);
	}
	if (yyjson_is_real(val)) {
		return static_cast<int64_t>(yyjson_get_real(val));
	}
	return default_value;
}

bool ArcGISJSONBool(yyjson_val *obj, const char *key, bool default_value) {
	auto val = yyjson_obj_get(obj, key);
	if (yyjson_is_bool(val)) {
		return yyjson_get_bool(val);
	}
	return default_value;
}

//===--------------------------------------------------------------------===//
// Requests
//===--------------------------------------------------------------------===//
//! GET through DuckDB's virtual file system: served by httpfs, or by cache_httpfs when it is loaded.
static string FetchThroughFileSystem(ClientContext &context, const string &url) {
	auto &fs = FileSystem::GetFileSystem(context);
	OpenFileInfo file(url);
	// Query responses are generated per request: HEAD requests and ranged reads are not reliable on ArcGIS servers
	// (e.g. ArcGIS Online answers HEAD with a Content-Length unrelated to the GET body), so ask httpfs to download
	// the whole response with a single GET.
	file.extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
	file.extended_info->options["force_full_download"] = Value::BOOLEAN(true);
	auto handle = fs.OpenFile(file, FileFlags::FILE_FLAGS_READ);
	// Positional read: handles reused from cache_httpfs' file handle cache may not be at offset 0
	string result(handle->GetFileSize(), '\0');
	if (!result.empty()) {
		handle->Read(&result[0], result.size(), 0);
	}
	return result;
}

//! GET through httpfs' HTTP client directly, bypassing the virtual file system and therefore cache_httpfs. httpfs
//! settings (timeouts, retries, proxies, certificates, connection caching) and `http` secrets still apply.
static string FetchDirect(ClientContext &context, const string &url) {
	auto &db = DatabaseInstance::GetDatabase(context);
	if (!db.ExtensionIsLoaded("httpfs")) {
		// Autoload like the file system does for http(s) URLs
		Value autoload;
		auto may_autoload = context.TryGetCurrentSetting("autoload_known_extensions", autoload) && !autoload.IsNull() &&
		                    BooleanValue::Get(autoload);
		if (!may_autoload || !ExtensionHelper::TryAutoLoadExtension(context, "httpfs")) {
			throw MissingExtensionException(
			    "ArcGIS requests require the httpfs extension: INSTALL httpfs; LOAD httpfs;");
		}
	}
	auto &http_util = HTTPUtil::Get(db);
	auto params = http_util.InitializeParameters(context, url);
	HTTPHeaders headers;
	// `bearer` secrets, which httpfs applies to the files it opens
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto bearer = secret_manager.LookupSecret(transaction, url, "bearer");
	if (bearer.HasMatch()) {
		auto &secret = dynamic_cast<const KeyValueSecret &>(*bearer.secret_entry->secret);
		headers.Insert("Authorization", "Bearer " + secret.TryGetValue("token", true).ToString());
	}

	string body;
	GetRequestInfo request(
	    url, headers, *params,
	    [&](const HTTPResponse &) {
		    // A new response (e.g. after a retry) replaces anything received before
		    body.clear();
		    return true;
	    },
	    [&](const_data_ptr_t data, idx_t data_length) {
		    body.append(const_char_ptr_cast(data), data_length);
		    return true;
	    });
	auto response = http_util.Request(request);
	if (!response->Success()) {
		throw HTTPException(*response, "HTTP GET error on '%s' (HTTP %d %s)", url, static_cast<int>(response->status),
		                    HTTPUtil::GetStatusMessage(response->status));
	}
	return body;
}

//! Removes the token (raw or percent-encoded) from messages produced by lower layers, which embed the full URL.
static string RedactToken(string message, const ArcGISUrl &url) {
	auto token = url.GetParam("token");
	if (token.empty()) {
		return message;
	}
	message = StringUtil::Replace(message, PercentEncode(token), "***");
	return StringUtil::Replace(message, token, "***");
}

//! A short, single-line excerpt of a response body for error messages.
static string BodyExcerpt(const string &body) {
	static constexpr idx_t MAX_EXCERPT = 200;
	string result;
	bool last_space = false;
	for (auto c : body) {
		auto space = StringUtil::CharacterIsSpace(c);
		if (space && (last_space || result.empty())) {
			continue;
		}
		result += space ? ' ' : c;
		last_space = space;
		if (result.size() >= MAX_EXCERPT) {
			result += "...";
			break;
		}
	}
	return result;
}

void ArcGISCheckResponse(yyjson_val *root, const ArcGISUrl &url) {
	if (!yyjson_is_obj(root)) {
		throw IOException("ArcGIS server returned unexpected JSON (expected an object) for %s", url.ToRedactedString());
	}
	string code;
	string message;
	vector<string> details;

	auto error = yyjson_obj_get(root, "error");
	if (error && !yyjson_is_null(error)) {
		if (yyjson_is_obj(error)) {
			auto code_val = yyjson_obj_get(error, "code");
			if (yyjson_is_int(code_val)) {
				code = to_string(yyjson_get_sint(code_val));
			} else if (yyjson_is_str(code_val)) {
				code = yyjson_get_str(code_val);
			}
			message = ArcGISJSONString(error, "message");
			auto details_val = yyjson_obj_get(error, "details");
			size_t idx, max;
			yyjson_val *detail;
			yyjson_arr_foreach(details_val, idx, max, detail) {
				if (yyjson_is_str(detail)) {
					details.emplace_back(yyjson_get_str(detail));
				}
			}
		} else if (yyjson_is_str(error)) {
			message = yyjson_get_str(error);
		} else {
			message = "unrecognized error object";
		}
	} else if (StringUtil::CIEquals(ArcGISJSONString(root, "status"), "error")) {
		// Some ArcGIS endpoints (e.g. geoprocessing and admin resources) use {"status": "error", "messages": [..]}
		auto messages_val = yyjson_obj_get(root, "messages");
		size_t idx, max;
		yyjson_val *entry;
		yyjson_arr_foreach(messages_val, idx, max, entry) {
			if (yyjson_is_str(entry)) {
				details.emplace_back(yyjson_get_str(entry));
			}
		}
	} else {
		return;
	}

	string result = "ArcGIS server returned an error";
	if (!code.empty()) {
		result += " (code " + code + ")";
	}
	result += ": " + (message.empty() ? string("no message") : message);
	for (auto &detail : details) {
		if (!detail.empty() && detail != message) {
			result += " - " + detail;
		}
	}
	if (code == "498") {
		result += "\nThe token is invalid or expired.";
	} else if (code == "499" || code == "401" || code == "403") {
		result += "\nThis resource may require a token: pass token := '...' or CREATE SECRET (TYPE arcgis, TOKEN "
		          "'...', SCOPE '<url prefix>').";
	}
	result += "\nRequest: " + url.ToRedactedString();
	throw IOException(result);
}

unique_ptr<ArcGISJSON> ArcGISFetchJSON(ClientContext &context, const ArcGISUrl &url, const ArcGISCachePolicy &policy) {
	auto cache = ArcGISResponseCache::Get(context);
	if (cache && policy.read) {
		auto cached = cache->Lookup(url, policy);
		if (cached) {
			return cached;
		}
	}

	string body;
	try {
		// With the response cache enabled, responses are cached only there: going through the file system would let
		// cache_httpfs (when loaded) store a second copy of every response
		body = cache ? FetchDirect(context, url.ToString()) : FetchThroughFileSystem(context, url.ToString());
	} catch (std::exception &ex) {
		ErrorData error(ex);
		if (error.Type() == ExceptionType::INTERRUPT) {
			throw;
		}
		throw IOException("ArcGIS request failed: %s", RedactToken(error.RawMessage(), url));
	}

	auto written_at = Timestamp::GetCurrentTimestamp();
	yyjson_read_err err;
	auto doc = ArcGISParseJSON(body, err);
	if (!doc) {
		throw IOException("ArcGIS server returned a response that is not valid JSON (%s at byte %llu) for %s%s",
		                  err.msg, static_cast<uint64_t>(err.pos), url.ToRedactedString(),
		                  body.empty() ? string(": empty response") : ": " + BodyExcerpt(body));
	}
	auto result = make_uniq<ArcGISJSON>(doc, yyjson_doc_get_root(doc), written_at, /*from_cache=*/false);
	ArcGISCheckResponse(result->Root(), url);
	if (cache) {
		// Only responses that passed the checks above are stored: errors reported with HTTP 200 are never cached
		cache->Store(url, body, written_at, policy.snapshot);
	}
	return result;
}

void ArcGISApplyToken(ClientContext &context, ArcGISUrl &url, const Value &explicit_token) {
	if (!explicit_token.IsNull()) {
		auto token = StringValue::Get(explicit_token);
		if (token.empty()) {
			url.RemoveParam("token");
		} else {
			url.SetParam("token", token);
		}
		return;
	}
	if (url.HasParam("token")) {
		return;
	}
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto match = secret_manager.LookupSecret(transaction, url.Base(), "arcgis");
	if (!match.HasMatch()) {
		return;
	}
	auto &secret = dynamic_cast<const KeyValueSecret &>(*match.secret_entry->secret);
	auto token = secret.TryGetValue("token");
	if (!token.IsNull() && !token.ToString().empty()) {
		url.SetParam("token", token.ToString());
	}
}

} // namespace duckdb
