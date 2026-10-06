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

//! Owns a parsed yyjson document.
class ArcGISJSON {
public:
	explicit ArcGISJSON(duckdb_yyjson::yyjson_doc *doc);
	~ArcGISJSON();
	ArcGISJSON(const ArcGISJSON &) = delete;
	ArcGISJSON &operator=(const ArcGISJSON &) = delete;

	duckdb_yyjson::yyjson_val *Root() const;

private:
	duckdb_yyjson::yyjson_doc *doc;
};

//! Issues a GET request through DuckDB's virtual file system (served by httpfs, or by cache_httpfs when loaded),
//! parses the body as JSON and throws an IOException if the body is not JSON or reports an ArcGIS error. ArcGIS
//! servers commonly report errors with HTTP status 200 and an {"error": {...}} body.
unique_ptr<ArcGISJSON> ArcGISFetchJSON(ClientContext &context, const ArcGISUrl &url);

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
