#include "arcgis_geometry.hpp"

#include <cmath>
#include <limits>

namespace duckdb {

using namespace duckdb_yyjson; // NOLINT

namespace {

enum class WKBType : uint32_t {
	POINT = 1,
	LINESTRING = 2,
	POLYGON = 3,
	MULTIPOINT = 4,
	MULTILINESTRING = 5,
	MULTIPOLYGON = 6,
};

//! A ring or path as a flat array of x/y pairs.
using Coordinates = vector<double>;

class WKBWriter {
public:
	explicit WKBWriter(string &buffer_p) : buffer(buffer_p) {
		buffer.clear();
	}

	void Header(WKBType type) {
		buffer += static_cast<char>(1); // little-endian
		UInt32(static_cast<uint32_t>(type));
	}
	void UInt32(uint32_t value) {
		char bytes[sizeof(uint32_t)];
		Store<uint32_t>(value, data_ptr_cast(bytes));
		buffer.append(bytes, sizeof(uint32_t));
	}
	void Double(double value) {
		char bytes[sizeof(double)];
		Store<double>(value, data_ptr_cast(bytes));
		buffer.append(bytes, sizeof(double));
	}
	void Count(idx_t count) {
		if (count > NumericLimits<uint32_t>::Maximum()) {
			throw InvalidInputException("ArcGIS geometry has too many parts or vertices for WKB");
		}
		UInt32(static_cast<uint32_t>(count));
	}
	void Points(const Coordinates &coords) {
		Count(coords.size() / 2);
		for (auto value : coords) {
			Double(value);
		}
	}
	void Point(double x, double y) {
		Header(WKBType::POINT);
		Double(x);
		Double(y);
	}
	void LineString(const Coordinates &coords) {
		Header(WKBType::LINESTRING);
		Points(coords);
	}
	void Polygon(const vector<Coordinates> &rings, const vector<idx_t> &ring_indexes) {
		Header(WKBType::POLYGON);
		Count(ring_indexes.size());
		for (auto ring_idx : ring_indexes) {
			Points(rings[ring_idx]);
		}
	}

private:
	string &buffer;
};

double ReadOrdinate(yyjson_val *val) {
	if (yyjson_is_num(val)) {
		return yyjson_get_num(val);
	}
	if (!val || yyjson_is_null(val)) {
		return std::numeric_limits<double>::quiet_NaN();
	}
	if (yyjson_is_str(val) && StringUtil::CIEquals(yyjson_get_str(val), "NaN")) {
		return std::numeric_limits<double>::quiet_NaN();
	}
	throw InvalidInputException("ArcGIS geometry contains a non-numeric coordinate");
}

//! Reads an Esri coordinate array ([[x, y, (z), (m)], ...]); only x and y are kept.
Coordinates ReadCoordinates(yyjson_val *points) {
	if (!yyjson_is_arr(points)) {
		throw InvalidInputException("ArcGIS geometry contains a malformed coordinate list");
	}
	Coordinates result;
	result.reserve(yyjson_arr_size(points) * 2);
	size_t idx, max;
	yyjson_val *point;
	yyjson_arr_foreach(points, idx, max, point) {
		if (!yyjson_is_arr(point) || yyjson_arr_size(point) < 2) {
			throw InvalidInputException("ArcGIS geometry contains a malformed coordinate");
		}
		result.push_back(ReadOrdinate(yyjson_arr_get(point, 0)));
		result.push_back(ReadOrdinate(yyjson_arr_get(point, 1)));
	}
	return result;
}

vector<Coordinates> ReadParts(yyjson_val *parts) {
	if (!yyjson_is_arr(parts)) {
		throw InvalidInputException("ArcGIS geometry contains a malformed list of paths or rings");
	}
	vector<Coordinates> result;
	size_t idx, max;
	yyjson_val *part;
	yyjson_arr_foreach(parts, idx, max, part) {
		auto coords = ReadCoordinates(part);
		if (!coords.empty()) {
			result.push_back(std::move(coords));
		}
	}
	return result;
}

//! Shoelace formula; positive for counter-clockwise rings (y axis pointing up).
double SignedArea(const Coordinates &ring) {
	double area = 0;
	auto n = ring.size() / 2;
	for (idx_t i = 0; i < n; i++) {
		auto j = (i + 1) % n;
		area += ring[2 * i] * ring[2 * j + 1] - ring[2 * j] * ring[2 * i + 1];
	}
	return area / 2;
}

//! Even-odd ray casting test.
bool RingContainsPoint(const Coordinates &ring, double x, double y) {
	bool inside = false;
	auto n = ring.size() / 2;
	for (idx_t i = 0, j = n - 1; i < n; j = i++) {
		auto xi = ring[2 * i], yi = ring[2 * i + 1];
		auto xj = ring[2 * j], yj = ring[2 * j + 1];
		if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) {
			inside = !inside;
		}
	}
	return inside;
}

void CloseRing(Coordinates &ring) {
	auto n = ring.size();
	if (n >= 2 && (ring[0] != ring[n - 2] || ring[1] != ring[n - 1])) {
		ring.push_back(ring[0]);
		ring.push_back(ring[1]);
	}
}

void WritePoint(yyjson_val *geometry, WKBWriter &writer) {
	writer.Point(ReadOrdinate(yyjson_obj_get(geometry, "x")), ReadOrdinate(yyjson_obj_get(geometry, "y")));
}

void WriteMultiPoint(yyjson_val *geometry, WKBWriter &writer) {
	auto coords = ReadCoordinates(yyjson_obj_get(geometry, "points"));
	writer.Header(WKBType::MULTIPOINT);
	writer.Count(coords.size() / 2);
	for (idx_t i = 0; i < coords.size(); i += 2) {
		writer.Point(coords[i], coords[i + 1]);
	}
}

void WritePolyline(yyjson_val *geometry, WKBWriter &writer) {
	auto paths = ReadParts(yyjson_obj_get(geometry, "paths"));
	if (paths.size() == 1) {
		writer.LineString(paths[0]);
		return;
	}
	writer.Header(WKBType::MULTILINESTRING);
	writer.Count(paths.size());
	for (auto &path : paths) {
		writer.LineString(path);
	}
}

void WritePolygonRings(vector<Coordinates> &rings, WKBWriter &writer) {
	vector<double> areas;
	for (auto &ring : rings) {
		CloseRing(ring);
		areas.push_back(SignedArea(ring));
	}
	// Exterior rings are clockwise (negative area); each gets its own polygon.
	vector<vector<idx_t>> polygons;
	vector<idx_t> polygon_exteriors;
	for (idx_t i = 0; i < rings.size(); i++) {
		if (areas[i] <= 0) {
			polygons.push_back({i});
			polygon_exteriors.push_back(i);
		}
	}
	// Holes belong to the smallest exterior ring containing them; holes without one become exterior rings.
	for (idx_t i = 0; i < rings.size(); i++) {
		if (areas[i] <= 0) {
			continue;
		}
		optional_idx best;
		for (idx_t p = 0; p < polygon_exteriors.size(); p++) {
			auto exterior = polygon_exteriors[p];
			if (!RingContainsPoint(rings[exterior], rings[i][0], rings[i][1])) {
				continue;
			}
			if (!best.IsValid() || std::fabs(areas[exterior]) < std::fabs(areas[polygon_exteriors[best.GetIndex()]])) {
				best = p;
			}
		}
		if (best.IsValid()) {
			polygons[best.GetIndex()].push_back(i);
		} else {
			polygons.push_back({i});
			polygon_exteriors.push_back(i);
		}
	}
	if (polygons.size() == 1) {
		writer.Polygon(rings, polygons[0]);
		return;
	}
	if (polygons.empty()) {
		writer.Header(WKBType::POLYGON);
		writer.Count(0);
		return;
	}
	writer.Header(WKBType::MULTIPOLYGON);
	writer.Count(polygons.size());
	for (auto &polygon : polygons) {
		writer.Polygon(rings, polygon);
	}
}

void WriteEnvelope(yyjson_val *geometry, WKBWriter &writer) {
	auto xmin = ReadOrdinate(yyjson_obj_get(geometry, "xmin"));
	auto ymin = ReadOrdinate(yyjson_obj_get(geometry, "ymin"));
	auto xmax = ReadOrdinate(yyjson_obj_get(geometry, "xmax"));
	auto ymax = ReadOrdinate(yyjson_obj_get(geometry, "ymax"));
	vector<Coordinates> rings;
	if (!std::isnan(xmin) && !std::isnan(ymin) && !std::isnan(xmax) && !std::isnan(ymax)) {
		rings.push_back({xmin, ymin, xmin, ymax, xmax, ymax, xmax, ymin, xmin, ymin});
	}
	WritePolygonRings(rings, writer);
}

} // namespace

bool ArcGISGeometryToWKB(yyjson_val *geometry, string &wkb) {
	if (!geometry || yyjson_is_null(geometry)) {
		return false;
	}
	if (!yyjson_is_obj(geometry)) {
		throw InvalidInputException("ArcGIS geometry is not a JSON object");
	}
	if (yyjson_obj_size(geometry) == 0) {
		return false;
	}
	WKBWriter writer(wkb);
	if (yyjson_obj_get(geometry, "rings")) {
		auto rings = ReadParts(yyjson_obj_get(geometry, "rings"));
		WritePolygonRings(rings, writer);
	} else if (yyjson_obj_get(geometry, "paths")) {
		WritePolyline(geometry, writer);
	} else if (yyjson_obj_get(geometry, "points")) {
		WriteMultiPoint(geometry, writer);
	} else if (yyjson_obj_get(geometry, "x")) {
		WritePoint(geometry, writer);
	} else if (yyjson_obj_get(geometry, "xmin")) {
		WriteEnvelope(geometry, writer);
	} else if (yyjson_obj_get(geometry, "curveRings") || yyjson_obj_get(geometry, "curvePaths")) {
		throw InvalidInputException("ArcGIS geometries with true curves are not supported; do not request them "
		                            "with returnTrueCurves");
	} else {
		throw InvalidInputException("Unrecognized ArcGIS geometry");
	}
	return true;
}

} // namespace duckdb
