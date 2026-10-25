#pragma once

// GeoJSON and CSV written as text for the vector tests, a GeoPackage GDAL makes from one, and the feature kinds they are read as.

#include <catch2/catch_test_macros.hpp>

#include <gdal.h>
#include <gdal_utils.h>

#include <string>
#include <string_view>
#include <vector>

#include "miniverse/geo/types.hpp"
#include "miniverse/layer/feature_layer.hpp"

namespace test {

// Three features: a polygon with a hole, a multipolygon of two parts, and one with no geometry.
constexpr std::string_view BUILDINGS = R"({"type": "FeatureCollection", "features": [
  {"type": "Feature", "id": 1, "properties": {"name": "Town hall", "levels": 3, "height": 12.5, "osm_id": 101},
   "geometry": {"type": "Polygon", "coordinates": [[[0, 0], [4, 0], [4, 4], [0, 4], [0, 0]], [[1, 1], [1, 2], [2, 2], [2, 1], [1, 1]]]}},
  {"type": "Feature", "id": 2, "properties": {"name": "Depot", "levels": null, "height": 4.0, "osm_id": 102},
   "geometry": {"type": "MultiPolygon", "coordinates": [[[[10, 10], [11, 10], [11, 11], [10, 10]]], [[[20, 20], [21, 20], [21, 21], [20, 20]]]]}},
  {"type": "Feature", "id": 3, "properties": {"name": "Nowhere", "levels": 1, "height": 1.0, "osm_id": 103}, "geometry": null}
]})";

// A multipolygon of one part, which is a polygon to whoever asks for polygons.
constexpr std::string_view ONE_PART = R"({"type": "FeatureCollection", "features": [
  {"type": "Feature", "id": 7, "properties": {}, "geometry": {"type": "MultiPolygon", "coordinates": [[[[0, 0], [1, 0], [1, 1], [0, 0]]]]}}
]})";

// Points: longitude first, as GeoJSON has them; the second with a height, which is dropped; the third a multipoint of one.
// The first has no branch number, the id some tests ask for.
constexpr std::string_view SHOPS = R"({"type": "FeatureCollection", "features": [
  {"type": "Feature", "id": 1, "properties": {"ref": "a", "open": true, "addr/street": "High Street", "branch": null},
   "geometry": {"type": "Point", "coordinates": [1.5, 2.5]}},
  {"type": "Feature", "id": 2, "properties": {"ref": "b", "open": false, "addr/street": "Mill Lane", "branch": 12},
   "geometry": {"type": "Point", "coordinates": [-3.25, 40, 650]}},
  {"type": "Feature", "id": 3, "properties": {"ref": "c", "open": true, "addr/street": "Quay", "branch": 13},
   "geometry": {"type": "MultiPoint", "coordinates": [[7, 8]]}}
]})";

// A table with a geometry, as OGR reads a CSV with a WKT column, and no word on its coordinate system.
constexpr std::string_view NO_SYSTEM = "WKT,name\n\"POINT (1 2)\",somewhere\n";

// Curves, which GeoJSON has no way to write, in tables of WKT that a .prj of WGS_84_PRJ beside them puts in WGS 84: an arc
// of the circle of radius 1 around (1, 0), from (0, 0) over (1, 1) to (2, 0), alone and after a straight line from (-1, 0);
// and the whole circle as an area.
constexpr std::string_view CURVED_LINES =
    "WKT,name\n"
    "\"CIRCULARSTRING (0 0,1 1,2 0)\",arc\n"
    "\"COMPOUNDCURVE ((-1 0,0 0),CIRCULARSTRING (0 0,1 1,2 0))\",road\n";
constexpr std::string_view CURVED_AREAS = "WKT,name\n\"CURVEPOLYGON (CIRCULARSTRING (0 0,1 1,2 0,1 -1,0 0))\",disc\n";
constexpr std::string_view WGS_84_PRJ =
    R"(GEOGCS["GCS_WGS_1984",DATUM["D_WGS_1984",SPHEROID["WGS_1984",6378137.0,298.257223563]],PRIMEM["Greenwich",0.0],UNIT["Degree",0.0174532925199433]])";

// A line in Web Mercator (EPSG:3857), in metres: from (0, 0) to what is longitude 1, latitude 1.
constexpr std::string_view MERCATOR_PATH = R"({"type": "FeatureCollection",
  "crs": {"type": "name", "properties": {"name": "urn:ogc:def:crs:EPSG::3857"}}, "features": [
  {"type": "Feature", "id": 1, "properties": {}, "geometry": {"type": "LineString", "coordinates": [[0, 0], [111319.49079327357, 111325.14286638486]]}}
]})";

/** @brief Copies the vector file `from` to a GeoPackage at `to`, keeping its feature ids: a file made by GDAL itself. */
inline void to_geopackage(const std::string& from, const std::string& to) {
  GDALAllRegister();

  std::vector<std::string> words{"-f", "GPKG", "-preserve_fid", "-nln", "buildings"};
  std::vector<char*>       arguments;
  arguments.reserve(words.size() + 1);
  for ( std::string& word : words ) {
    arguments.push_back(word.data());
  }
  arguments.push_back(nullptr);

  GDALDatasetH source = GDALOpenEx(from.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
  REQUIRE(source != nullptr);

  GDALVectorTranslateOptions* options = GDALVectorTranslateOptionsNew(arguments.data(), nullptr);
  GDALDatasetH                made = GDALVectorTranslate(to.c_str(), nullptr, 1, &source, options, nullptr);
  GDALVectorTranslateOptionsFree(options);
  REQUIRE(made != nullptr);
  GDALClose(made);
  GDALClose(source);
}

struct Buildings : miniverse::FeatureLayer<miniverse::geo::MultiPolygon> {};
struct Shops : miniverse::FeatureLayer<miniverse::geo::Point> {};
struct Paths : miniverse::FeatureLayer<miniverse::geo::LineString> {};

// Areas to tell locations apart by: a square with a hole; two triangles far from each other; a square whose ring runs
// clockwise, as a shapefile's do; a small square; and one that only touches the corner of a location below.
constexpr std::string_view AREAS = R"({"type": "FeatureCollection", "features": [
  {"type": "Feature", "id": 5, "properties": {"name": "corner"},
   "geometry": {"type": "Polygon", "coordinates": [[[-2, -2], [-1, -2], [-1, -1], [-2, -1], [-2, -2]]]}},
  {"type": "Feature", "id": 1, "properties": {"name": "holed"},
   "geometry": {"type": "Polygon", "coordinates": [[[0, 0], [4, 0], [4, 4], [0, 4], [0, 0]], [[1, 1], [1, 2], [2, 2], [2, 1], [1, 1]]]}},
  {"type": "Feature", "id": 2, "properties": {"name": "triangles"},
   "geometry": {"type": "MultiPolygon", "coordinates": [[[[10, 10], [11, 10], [11, 11], [10, 10]]], [[[20, 20], [21, 20], [21, 21], [20, 20]]]]}},
  {"type": "Feature", "id": 3, "properties": {"name": "clockwise"},
   "geometry": {"type": "Polygon", "coordinates": [[[6, 0], [6, 2], [8, 2], [8, 0], [6, 0]]]}},
  {"type": "Feature", "id": 4, "properties": {"name": "small"},
   "geometry": {"type": "Polygon", "coordinates": [[[4.5, 4.5], [5, 4.5], [5, 5], [4.5, 5], [4.5, 4.5]]]}}
]})";

// Lines: one across the origin's square, one whose box overlaps that square but which passes it by, and one far away.
constexpr std::string_view TRACKS = R"({"type": "FeatureCollection", "features": [
  {"type": "Feature", "id": 1, "properties": {}, "geometry": {"type": "LineString", "coordinates": [[-1, 0.5], [0.5, 0.5], [3, 2]]}},
  {"type": "Feature", "id": 2, "properties": {}, "geometry": {"type": "LineString", "coordinates": [[0.5, 2], [2, 0.5]]}},
  {"type": "Feature", "id": 3, "properties": {}, "geometry": {"type": "LineString", "coordinates": [[40, 40], [41, 41]]}}
]})";

// Two points in UTM zone 32 north (EPSG:32632), on its central meridian, longitude 9: half a metre north of latitude 50,
// and half a metre south of it. In that system the parallel of 50 degrees is a curve that is furthest south just there.
constexpr std::string_view BY_THE_PARALLEL = R"({"type": "FeatureCollection",
  "crs": {"type": "name", "properties": {"name": "urn:ogc:def:crs:EPSG::32632"}}, "features": [
  {"type": "Feature", "id": 1, "properties": {}, "geometry": {"type": "Point", "coordinates": [500000, 5538631.25879034]}},
  {"type": "Feature", "id": 2, "properties": {}, "geometry": {"type": "Point", "coordinates": [500000, 5538630.14694461]}}
]})";

// The areas of AREAS' first two features, and a point among them: not an area at all.
constexpr std::string_view AREAS_AND_A_POINT = R"({"type": "FeatureCollection", "features": [
  {"type": "Feature", "id": 1, "properties": {},
   "geometry": {"type": "Polygon", "coordinates": [[[0, 0], [4, 0], [4, 4], [0, 4], [0, 0]]]}},
  {"type": "Feature", "id": 2, "properties": {},
   "geometry": {"type": "Polygon", "coordinates": [[[10, 10], [11, 10], [11, 11], [10, 10]]]}},
  {"type": "Feature", "id": 9, "properties": {}, "geometry": {"type": "Point", "coordinates": [11.5, 11.5]}}
]})";

}  // namespace test
