//===----------------------------------------------------------------------===//
// arcgis_functions.hpp
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"

namespace duckdb {

class ExtensionLoader;

struct ArcGISQueryFunction {
	static void Register(ExtensionLoader &loader);
};

struct ArcGISCatalogFunctions {
	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
