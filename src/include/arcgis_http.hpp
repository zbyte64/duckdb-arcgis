//===----------------------------------------------------------------------===//
// arcgis_http.hpp
//
// URL handling and HTTP/JSON access to ArcGIS REST endpoints.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "yyjson.hpp"

namespace duckdb {

//! A URL split into its base (scheme, host and path) and its decoded query parameters.
class ArcGISUrl {
public:
	//! Parses a URL; query parameters are percent-decoded. Throws for anything other than http(s) URLs.
	static ArcGISUrl Parse(const string &url);

	const string &Base() const {
		return base;
	}
	//! Returns a copy of this URL whose path has `segment` appended (separated by a single '/').
	ArcGISUrl Append(const string &segment) const;
	//! Removes a trailing path segment (case-insensitive) if present, e.g. "/query". Returns true if removed.
	bool StripSuffix(const string &segment);

	bool HasParam(const string &key) const;
	string GetParam(const string &key) const;
	//! Sets (or replaces, matching keys case-insensitively) a query parameter.
	void SetParam(const string &key, const string &value);
	void RemoveParam(const string &key);
	void ClearParams();

	//! Full URL with percent-encoded query parameters.
	string ToString() const;
	//! Full URL with the value of the `token` parameter masked, for error messages.
	string ToRedactedString() const;

private:
	string base;
	vector<pair<string, string>> params;
};

//! Owns a parsed yyjson document holding an ArcGIS response.
class ArcGISJSON {
public:
	//! `root` is the response within `doc`; `written_at` is when the response was received from the server
	ArcGISJSON(duckdb_yyjson::yyjson_doc *doc, duckdb_yyjson::yyjson_val *root, timestamp_t written_at,
	           bool from_cache);
	~ArcGISJSON();
	ArcGISJSON(const ArcGISJSON &) = delete;
	ArcGISJSON &operator=(const ArcGISJSON &) = delete;

	duckdb_yyjson::yyjson_val *Root() const {
		return root;
	}
	timestamp_t WrittenAt() const {
		return written_at;
	}
	//! Whether the response was served by the response cache (arcgis_cache_directory)
	bool FromCache() const {
		return from_cache;
	}

private:
	duckdb_yyjson::yyjson_doc *doc;
	duckdb_yyjson::yyjson_val *root;
	timestamp_t written_at;
	bool from_cache;
};

//! Which cached responses ArcGISFetchJSON may serve when the response cache (arcgis_cache_directory) is enabled.
struct ArcGISCachePolicy {
	//! Whether a cached response may be served; when false the request is sent and its response (re)stored
	bool read = true;
	//! 0 for standalone requests, whose cached responses expire after arcgis_cache_ttl_seconds. Otherwise the
	//! WrittenAt() of the planning response of the scan the request belongs to: only responses stored for that same
	//! planning response are served, so all pages of a scan come from one snapshot of the layer.
	int64_t snapshot = 0;

	//! Policy for the page requests of a scan planned from `planning_response`: if the planning response was fetched
	//! fresh, pages are fetched fresh too; if it came from cache, pages stored for it are served from cache.
	static ArcGISCachePolicy ForScan(const ArcGISJSON &planning_response);
};

//! Issues a GET request through DuckDB's virtual file system (served by httpfs, or by cache_httpfs when loaded),
//! parses the body as JSON and throws an IOException if the body is not JSON or reports an ArcGIS error. ArcGIS
//! servers commonly report errors with HTTP status 200 and an {"error": {...}} body. When arcgis_cache_directory is
//! set, valid responses are served from and stored in the persistent response cache according to `policy`.
unique_ptr<ArcGISJSON> ArcGISFetchJSON(ClientContext &context, const ArcGISUrl &url,
                                       const ArcGISCachePolicy &policy = ArcGISCachePolicy());

//! Parses a response body; returns nullptr (with `err` set) if it is not valid JSON.
duckdb_yyjson::yyjson_doc *ArcGISParseJSON(const string &body, duckdb_yyjson::yyjson_read_err &err);

//! Validates a parsed ArcGIS response body, throwing an IOException describing the server-reported error.
void ArcGISCheckResponse(duckdb_yyjson::yyjson_val *root, const ArcGISUrl &url);

//! Sets the `token` parameter of `url`: an explicit token wins, then a token already present in the URL, then the
//! token of the best matching `arcgis` secret.
void ArcGISApplyToken(ClientContext &context, ArcGISUrl &url, const Value &explicit_token);

//! JSON accessors returning defaults for missing or mistyped members.
string ArcGISJSONString(duckdb_yyjson::yyjson_val *obj, const char *key, const string &default_value = string());
int64_t ArcGISJSONInt(duckdb_yyjson::yyjson_val *obj, const char *key, int64_t default_value);
bool ArcGISJSONBool(duckdb_yyjson::yyjson_val *obj, const char *key, bool default_value);

} // namespace duckdb
