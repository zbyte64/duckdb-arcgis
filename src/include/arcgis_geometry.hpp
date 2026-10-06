//===----------------------------------------------------------------------===//
// arcgis_geometry.hpp
//
// Conversion of Esri JSON geometries to WKB.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "yyjson.hpp"

namespace duckdb {

//! Converts an Esri JSON geometry (point, multipoint, polyline, polygon or envelope) into little-endian 2D WKB,
//! written to `wkb` (which is cleared first). Returns false when the geometry is missing or null. Polygon rings are
//! grouped into polygons using the Esri convention: clockwise rings are exterior rings, counter-clockwise rings are
//! holes of the exterior ring that contains them. Throws InvalidInputException for malformed geometries.
bool ArcGISGeometryToWKB(duckdb_yyjson::yyjson_val *geometry, string &wkb);

} // namespace duckdb
