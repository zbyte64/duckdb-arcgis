//===----------------------------------------------------------------------===//
// arcgis_cache.hpp
//
// Persistent, multi-process safe cache of ArcGIS responses (arcgis_cache_directory).
//===----------------------------------------------------------------------===//

#pragma once

#include "arcgis_http.hpp"

namespace duckdb {

class FileSystem;

//! Stores each valid response in <arcgis_cache_directory>/<hh>/<sha256>.json, keyed by the SHA-256 of the request URL
//! without its token. An entry is a JSON object {"key": <url>, "written_at": <us>, "snapshot": <us>, "response": ...}
//! written to a temporary file and renamed into place; unreadable, truncated or mismatching entries are misses.
class ArcGISResponseCache {
public:
	//! Returns nullptr if the cache is disabled (arcgis_cache_directory is empty).
	static unique_ptr<ArcGISResponseCache> Get(ClientContext &context);

	//! Returns the cached response of `url` if it is valid under `policy`, nullptr otherwise.
	unique_ptr<ArcGISJSON> Lookup(const ArcGISUrl &url, const ArcGISCachePolicy &policy);
	//! Stores a validated response body.
	void Store(const ArcGISUrl &url, const string &body, timestamp_t written_at, int64_t snapshot);
	//! Deletes the entries whose URL (without token) starts with `url_prefix` (all entries if empty). Returns the
	//! number of deleted entries.
	idx_t Clear(const string &url_prefix);

private:
	ArcGISResponseCache(FileSystem &fs, string directory, int64_t ttl_micros);

	string EntryDirectory(const string &hash) const;

	FileSystem &fs;
	string directory;
	//! Maximum age of standalone entries; negative if entries never expire
	int64_t ttl_micros;
};

} // namespace duckdb
