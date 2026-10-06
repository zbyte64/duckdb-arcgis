#include "arcgis_cache.hpp"
#include "arcgis_functions.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "mbedtls_wrapper.hpp"

namespace duckdb {

using namespace duckdb_yyjson; // NOLINT

namespace {

constexpr const char *ENTRY_EXTENSION = ".json";

//! The URL a response is cached under: the request URL without its (short-lived) token
string CacheKey(const ArcGISUrl &url) {
	auto key_url = url;
	key_url.RemoveParam("token");
	return key_url.ToString();
}

string Sha256Hex(const string &input) {
	char hash[duckdb_mbedtls::MbedTlsWrapper::SHA256_HASH_LENGTH_BYTES];
	duckdb_mbedtls::MbedTlsWrapper::ComputeSha256Hash(input.data(), input.size(), hash);
	char hex[duckdb_mbedtls::MbedTlsWrapper::SHA256_HASH_LENGTH_TEXT];
	duckdb_mbedtls::MbedTlsWrapper::ToBase16(hash, hex, sizeof(hash));
	return string(hex, sizeof(hex));
}

string JSONQuote(const string &input) {
	static constexpr const char *HEX = "0123456789abcdef";
	string result = "\"";
	for (auto ch : input) {
		auto c = static_cast<unsigned char>(ch);
		if (c == '"' || c == '\\') {
			result += '\\';
			result += static_cast<char>(c);
		} else if (c < 0x20) {
			result += "\\u00";
			result += HEX[c >> 4];
			result += HEX[c & 0xF];
		} else {
			result += static_cast<char>(c);
		}
	}
	return result + "\"";
}

//! Reads a local file; returns false if it does not exist or cannot be read.
bool TryReadFile(FileSystem &fs, const string &path, string &result) {
	try {
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS);
		if (!handle) {
			return false;
		}
		result = string(handle->GetFileSize(), '\0');
		if (!result.empty()) {
			handle->Read(&result[0], result.size(), 0);
		}
		return true;
	} catch (std::exception &ex) {
		ErrorData error(ex);
		if (error.Type() == ExceptionType::INTERRUPT) {
			throw;
		}
		// E.g. the entry was removed between opening and reading it
		return false;
	}
}

//! A parsed cache entry; `response` is nullptr if the entry is malformed (e.g. truncated).
struct CacheEntry {
	unique_ptr<ArcGISJSON> response;
	string key;
	int64_t snapshot = 0;
};

CacheEntry ParseEntry(const string &content) {
	CacheEntry entry;
	yyjson_read_err err;
	auto doc = ArcGISParseJSON(content, err);
	if (!doc) {
		return entry;
	}
	auto root = yyjson_doc_get_root(doc);
	auto key = yyjson_obj_get(root, "key");
	auto written_at = yyjson_obj_get(root, "written_at");
	auto snapshot = yyjson_obj_get(root, "snapshot");
	auto response = yyjson_obj_get(root, "response");
	if (!yyjson_is_str(key) || !yyjson_is_int(written_at) || !yyjson_is_int(snapshot) || !yyjson_is_obj(response)) {
		yyjson_doc_free(doc);
		return entry;
	}
	entry.key = string(yyjson_get_str(key), yyjson_get_len(key));
	entry.snapshot = yyjson_get_sint(snapshot);
	entry.response =
	    make_uniq<ArcGISJSON>(doc, response, timestamp_t(yyjson_get_sint(written_at)), /*from_cache=*/true);
	return entry;
}

} // namespace

ArcGISResponseCache::ArcGISResponseCache(FileSystem &fs_p, string directory_p, int64_t ttl_micros_p)
    : fs(fs_p), directory(std::move(directory_p)), ttl_micros(ttl_micros_p) {
}

unique_ptr<ArcGISResponseCache> ArcGISResponseCache::Get(ClientContext &context) {
	Value setting;
	if (!context.TryGetCurrentSetting("arcgis_cache_directory", setting) || setting.IsNull()) {
		return nullptr;
	}
	auto directory = StringValue::Get(setting);
	if (directory.empty()) {
		return nullptr;
	}
	int64_t ttl_micros = -1;
	if (context.TryGetCurrentSetting("arcgis_cache_ttl_seconds", setting) && !setting.IsNull()) {
		auto ttl_seconds = setting.GetValue<int64_t>();
		if (ttl_seconds < 0) {
			throw InvalidInputException("arcgis_cache_ttl_seconds must be NULL or at least 0, got %d", ttl_seconds);
		}
		ttl_micros = ttl_seconds > NumericLimits<int64_t>::Maximum() / Interval::MICROS_PER_SEC
		                 ? NumericLimits<int64_t>::Maximum()
		                 : ttl_seconds * Interval::MICROS_PER_SEC;
	}
	auto &fs = FileSystem::GetFileSystem(context);
	return unique_ptr<ArcGISResponseCache>(new ArcGISResponseCache(fs, fs.ExpandPath(directory), ttl_micros));
}

string ArcGISResponseCache::EntryDirectory(const string &hash) const {
	return fs.JoinPath(directory, hash.substr(0, 2));
}

unique_ptr<ArcGISJSON> ArcGISResponseCache::Lookup(const ArcGISUrl &url, const ArcGISCachePolicy &policy) {
	auto key = CacheKey(url);
	auto hash = Sha256Hex(key);
	string content;
	if (!TryReadFile(fs, fs.JoinPath(EntryDirectory(hash), hash + ENTRY_EXTENSION), content)) {
		return nullptr;
	}
	auto entry = ParseEntry(content);
	if (!entry.response || entry.key != key) {
		return nullptr;
	}
	if (policy.snapshot != 0) {
		// Pages of a scan: only serve responses stored for the scan's planning response
		return entry.snapshot == policy.snapshot ? std::move(entry.response) : nullptr;
	}
	if (ttl_micros >= 0) {
		auto age = Timestamp::GetCurrentTimestamp().value - entry.response->WrittenAt().value;
		if (age > ttl_micros) {
			return nullptr;
		}
	}
	return std::move(entry.response);
}

void ArcGISResponseCache::Store(const ArcGISUrl &url, const string &body, timestamp_t written_at, int64_t snapshot) {
	auto key = CacheKey(url);
	auto hash = Sha256Hex(key);
	auto entry_directory = EntryDirectory(hash);
	auto path = fs.JoinPath(entry_directory, hash + ENTRY_EXTENSION);
	// Unique per writer: concurrent writers (threads of a scan, other processes) never share a temporary file, and
	// the rename makes complete entries visible atomically
	auto temp_path = path + ".tmp." + UUID::ToString(UUID::GenerateRandomUUID());
	try {
		fs.CreateDirectoriesRecursive(entry_directory);
		auto header = StringUtil::Format("{\"key\":%s,\"written_at\":%d,\"snapshot\":%d,\"response\":", JSONQuote(key),
		                                 written_at.value, snapshot);
		string footer = "}\n";
		{
			auto handle = fs.OpenFile(temp_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
			handle->Write(const_cast<char *>(header.data()), header.size());
			handle->Write(const_cast<char *>(body.data()), body.size());
			handle->Write(const_cast<char *>(footer.data()), footer.size());
			handle->Close();
		}
		fs.MoveFile(temp_path, path);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		try {
			fs.TryRemoveFile(temp_path);
		} catch (std::exception &) { // NOLINT: the original error is more useful
		}
		if (error.Type() == ExceptionType::INTERRUPT) {
			throw;
		}
		throw IOException("Could not store an ArcGIS response in arcgis_cache_directory (%s): %s", directory,
		                  error.RawMessage());
	}
}

idx_t ArcGISResponseCache::Clear(const string &url_prefix) {
	if (!fs.DirectoryExists(directory)) {
		return 0;
	}
	vector<string> subdirectories;
	fs.ListFiles(directory, [&](const string &name, bool is_dir) {
		if (is_dir && name.size() == 2 && StringUtil::CharacterIsHex(name[0]) && StringUtil::CharacterIsHex(name[1])) {
			subdirectories.push_back(fs.JoinPath(directory, name));
		}
	});
	idx_t deleted = 0;
	for (auto &subdirectory : subdirectories) {
		vector<string> entries;
		// Temporary files belong to writers in progress and are left alone
		fs.ListFiles(subdirectory, [&](const string &name, bool is_dir) {
			if (!is_dir && StringUtil::EndsWith(name, ENTRY_EXTENSION)) {
				entries.push_back(fs.JoinPath(subdirectory, name));
			}
		});
		for (auto &path : entries) {
			if (!url_prefix.empty()) {
				string content;
				if (!TryReadFile(fs, path, content)) {
					continue;
				}
				auto entry = ParseEntry(content);
				// Malformed entries are never served; remove them along with the matching ones
				if (entry.response && !StringUtil::StartsWith(entry.key, url_prefix)) {
					continue;
				}
			}
			if (fs.TryRemoveFile(path)) {
				deleted++;
			}
		}
	}
	return deleted;
}

//===--------------------------------------------------------------------===//
// arcgis_clear_cache([url_prefix])
//===--------------------------------------------------------------------===//
namespace {

struct ClearCacheBindData : public TableFunctionData {
	string url_prefix;
};

struct ClearCacheState : public GlobalTableFunctionState {
	bool done = false;
};

unique_ptr<FunctionData> ClearCacheBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<ClearCacheBindData>();
	if (!input.inputs.empty()) {
		if (input.inputs[0].IsNull()) {
			throw BinderException("arcgis_clear_cache: the URL prefix cannot be NULL");
		}
		result->url_prefix = StringValue::Get(input.inputs[0]);
		if (result->url_prefix.empty()) {
			throw BinderException("arcgis_clear_cache: the URL prefix cannot be empty; call arcgis_clear_cache() to "
			                      "clear the whole cache");
		}
	}
	names = {"deleted"};
	return_types = {LogicalType::BIGINT};
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> ClearCacheInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<ClearCacheState>();
}

void ClearCacheScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<ClearCacheState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto &bind = input.bind_data->Cast<ClearCacheBindData>();
	auto cache = ArcGISResponseCache::Get(context);
	if (!cache) {
		throw InvalidInputException("arcgis_clear_cache: the response cache is disabled (arcgis_cache_directory is "
		                            "empty)");
	}
	output.SetValue(0, 0, Value::BIGINT(static_cast<int64_t>(cache->Clear(bind.url_prefix))));
	output.SetCardinality(1);
}

} // namespace

void ArcGISCacheFunctions::Register(ExtensionLoader &loader) {
	TableFunctionSet clear_cache("arcgis_clear_cache");
	clear_cache.AddFunction(TableFunction({}, ClearCacheScan, ClearCacheBind, ClearCacheInit));
	clear_cache.AddFunction(TableFunction({LogicalType::VARCHAR}, ClearCacheScan, ClearCacheBind, ClearCacheInit));
	loader.RegisterFunction(clear_cache);
}

} // namespace duckdb
