#define DUCKDB_EXTENSION_MAIN

#include "arcgis_extension.hpp"
#include "arcgis_functions.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"

namespace duckdb {

static unique_ptr<BaseSecret> CreateArcGISSecret(ClientContext &context, CreateSecretInput &input) {
	auto scope = input.scope;
	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	for (auto &option : input.options) {
		auto key = StringUtil::Lower(option.first);
		if (key != "token") {
			throw InvalidInputException("Unknown parameter '%s' for secret type 'arcgis'", option.first);
		}
		secret->secret_map["token"] = option.second.ToString();
	}
	if (secret->secret_map.find("token") == secret->secret_map.end()) {
		throw InvalidInputException("Secret type 'arcgis' requires a TOKEN");
	}
	secret->redact_keys = {"token"};
	return std::move(secret);
}

static void RegisterSecret(ExtensionLoader &loader) {
	SecretType secret_type;
	secret_type.name = "arcgis";
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	secret_type.extension = "arcgis";
	loader.RegisterSecretType(secret_type);

	CreateSecretFunction config_function = {"arcgis", "config", CreateArcGISSecret, {}};
	config_function.named_parameters["token"] = LogicalType::VARCHAR;
	loader.RegisterFunction(config_function);
}

static void LoadInternal(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption("arcgis_max_concurrent_requests",
	                          "Maximum number of concurrent requests a single arcgis_query scan sends to a server",
	                          LogicalType::UBIGINT, Value::UBIGINT(4));

	RegisterSecret(loader);
	ArcGISQueryFunction::Register(loader);
	ArcGISCatalogFunctions::Register(loader);
}

void ArcgisExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string ArcgisExtension::Name() {
	return "arcgis";
}

std::string ArcgisExtension::Version() const {
#ifdef EXT_VERSION_ARCGIS
	return EXT_VERSION_ARCGIS;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(arcgis, loader) {
	duckdb::LoadInternal(loader);
}
}
