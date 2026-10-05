// Vector files read with GDAL's OGR: features in WGS 84 with ids and tags, single geometries promoted to multi ones, a file
// pushed into a feature layer and loaded back, and a file as a layer itself, read by polygon as a table is.
//
// The files are GeoJSON written here as text, and a GeoPackage GDAL makes from one. The tests named "integration" need
// MINIVERSE_TEST_DB (a libpq connection string to a scratch database with PostGIS), and are skipped without it.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <boost/geometry/io/wkt/read.hpp>
#include <boost/geometry/io/wkt/write.hpp>

#include <gdal.h>
#include <gdal_utils.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "support/files.hpp"
#include "miniverse/gdal/vector.hpp"
#include "miniverse/geo/concepts/wkb.hpp"
#include "miniverse/miniverse.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;
namespace gdal = miniverse::gdal;

using test::Files;
using test::test_db;

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace {

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

// A line in Web Mercator (EPSG:3857), in metres: from (0, 0) to what is longitude 1, latitude 1.
constexpr std::string_view MERCATOR_PATH = R"({"type": "FeatureCollection",
  "crs": {"type": "name", "properties": {"name": "urn:ogc:def:crs:EPSG::3857"}}, "features": [
  {"type": "Feature", "id": 1, "properties": {}, "geometry": {"type": "LineString", "coordinates": [[0, 0], [111319.49079327357, 111325.14286638486]]}}
]})";

/** @brief Copies the vector file `from` to a GeoPackage at `to`, keeping its feature ids: a file made by GDAL itself. */
void to_geopackage(const std::string& from, const std::string& to) {
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

struct Buildings : miniverse::FeatureLayer<geo::MultiPolygon> {};
struct Shops : miniverse::FeatureLayer<geo::Point> {};
struct Paths : miniverse::FeatureLayer<geo::LineString> {};

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

[[nodiscard]] geo::Polygon polygon(const std::string& wkt) { return bg::from_wkt<geo::Polygon>(wkt); }

template <geo::wkb::Geometry geometry_t>
[[nodiscard]] std::vector<std::int64_t> ids(const miniverse::Features<geometry_t>& features) {
  std::vector<std::int64_t> result;
  for ( const miniverse::Feature<geometry_t>& feature : features ) {
    result.push_back(feature.id);
  }

  return result;
}

/**
 * @brief Checks that the file at each of `sources` gives, for each of `locations`, the features a table gives that was
 * pushed the first of them: the same ones, with the same geometries; the table's by id, the file's in its own order.
 */
template <gdal::FeatureKind kind_t>
void check_loads_as_a_table(const std::string& table, const std::vector<gdal::VectorSource>& sources, const std::vector<geo::Polygon>& locations) {
  using Geometry = gdal::GeometryOf<kind_t>;

  miniverse::Miniverse<kind_t> tables(test_db(), miniverse::Layer<kind_t>(table));
  tables.drop_tables();
  tables.create_tables();
  tables.template push<kind_t>(gdal::read_features<Geometry>(sources.front())).get();

  for ( const gdal::VectorSource& source : sources ) {
    miniverse::Miniverse<kind_t> files(gdal::feature_file<kind_t>(source));

    for ( std::size_t i = 0; i < locations.size(); ++i ) {
      INFO(source.path << ", location " << i << ": " << bg::to_wkt(locations.at(i)));
      const miniverse::Features<Geometry> from_table = tables.template load<kind_t>(locations.at(i)).get();
      miniverse::Features<Geometry>       from_file = files.template load<kind_t>(locations.at(i)).get();
      std::ranges::sort(from_file, {}, &miniverse::Feature<Geometry>::id);

      REQUIRE(ids(from_file) == ids(from_table));
      for ( std::size_t feature = 0; feature < from_table.size(); ++feature ) {
        CHECK(bg::to_wkt(from_file.at(feature).geometry) == bg::to_wkt(from_table.at(feature).geometry));
      }
    }
  }

  tables.drop_tables();
}

}  // namespace

TEST_CASE("gdal: a polygon is promoted where multipolygons are asked for, and a feature without geometry is left out", "[gdal]") {
  const Files       files;
  const std::string path = files.write("buildings.geojson", BUILDINGS);

  miniverse::Features<geo::MultiPolygon> buildings;
  const gdal::VectorRead read = gdal::read_features<geo::MultiPolygon>({.path = path, .layer = {}, .id_field = {}}, 100, [&](auto chunk) {
    buildings = std::move(chunk);
  });

  CHECK(read.features == 2);
  CHECK(read.without_geometry == 1);
  REQUIRE(buildings.size() == 2);
  CHECK(buildings.at(0).id == 1);
  CHECK(bg::to_wkt(buildings.at(0).geometry) == "MULTIPOLYGON(((0 0,4 0,4 4,0 4,0 0),(1 1,1 2,2 2,2 1,1 1)))");
  CHECK(buildings.at(1).id == 2);
  CHECK(bg::to_wkt(buildings.at(1).geometry) == "MULTIPOLYGON(((10 10,11 10,11 11,10 10)),((20 20,21 20,21 21,20 20)))");
}

TEST_CASE("gdal: where polygons are asked for, a multipolygon of one part is one, and one of several is refused by name", "[gdal]") {
  const Files files;

  const auto one_part = gdal::read_features<geo::Polygon>({.path = files.write("one_part.geojson", ONE_PART), .layer = {}, .id_field = {}});

  REQUIRE(one_part.size() == 1);
  CHECK(bg::to_wkt(one_part.front().geometry) == "POLYGON((0 0,1 0,1 1,0 0))");
  CHECK_THROWS_WITH(
      gdal::read_features<geo::Polygon>({.path = files.write("buildings.geojson", BUILDINGS), .layer = {}, .id_field = {}}),
      ContainsSubstring("feature 2") && ContainsSubstring("MULTIPOLYGON") && ContainsSubstring("is not a Polygon")
  );
}

TEST_CASE("gdal: coordinates are longitude then latitude, without heights, and transformed to WGS 84", "[gdal]") {
  const Files files;

  const auto shops = gdal::read_features<geo::Point>({.path = files.write("shops.geojson", SHOPS), .layer = {}, .id_field = {}});
  const auto paths = gdal::read_features<geo::LineString>({.path = files.write("path.geojson", MERCATOR_PATH), .layer = {}, .id_field = {}});

  REQUIRE(shops.size() == 3);
  CHECK(bg::to_wkt(shops.at(0).geometry) == "POINT(1.5 2.5)");
  CHECK(bg::to_wkt(shops.at(1).geometry) == "POINT(-3.25 40)");
  CHECK(bg::to_wkt(shops.at(2).geometry) == "POINT(7 8)");  // a multipoint of one is that point
  REQUIRE(paths.size() == 1);
  REQUIRE(paths.front().geometry.size() == 2);
  CHECK_THAT(paths.front().geometry.at(0).x(), WithinAbs(0, 1e-9));
  CHECK_THAT(paths.front().geometry.at(1).x(), WithinAbs(1, 1e-9));  // longitude
  CHECK_THAT(paths.front().geometry.at(1).y(), WithinAbs(1, 1e-9));  // latitude
}

TEST_CASE("gdal: a feature's other fields are its tags: numbers as numbers, and a null left out", "[gdal]") {
  const Files       files;
  const std::string path = files.write("buildings.geojson", BUILDINGS);

  const auto buildings = gdal::read_features<geo::MultiPolygon>({.path = path, .layer = {}, .id_field = "osm_id"});

  REQUIRE(buildings.size() == 2);
  CHECK(buildings.at(0).id == 101);  // the field, not the file's own id
  CHECK(buildings.at(0).tags.text() == R"({"name":"Town hall","levels":3,"height":12.5})");  // without the id's field
  CHECK(buildings.at(1).tags.text() == R"({"name":"Depot","height":4.0})");                    // without its null levels
}

TEST_CASE("gdal: a boolean field is true or false, and a field's name is kept whole, slash and all", "[gdal]") {
  const Files files;

  const auto shops = gdal::read_features<geo::Point>({.path = files.write("shops.geojson", SHOPS), .layer = {}, .id_field = {}});

  REQUIRE(shops.size() == 3);
  CHECK(shops.at(0).tags.text() == R"({"ref":"a","open":true,"addr\/street":"High Street"})");  // JSON may write a slash so
  CHECK(shops.at(1).tags.text() == R"({"ref":"b","open":false,"addr\/street":"Mill Lane","branch":12})");
}

TEST_CASE("gdal: a line is promoted where multilinestrings are asked for", "[gdal]") {
  const Files files;

  const auto paths = gdal::read_features<geo::MultiLineString>({.path = files.write("path.geojson", MERCATOR_PATH), .layer = {}, .id_field = {}});

  REQUIRE(paths.size() == 1);
  REQUIRE(paths.front().geometry.size() == 1);
  CHECK(paths.front().geometry.front().size() == 2);
}

TEST_CASE("gdal: an id field must be there and be a whole number", "[gdal]") {
  const Files       files;
  const std::string path = files.write("buildings.geojson", BUILDINGS);

  CHECK_THROWS_WITH(
      gdal::read_features<geo::MultiPolygon>({.path = path, .layer = {}, .id_field = "name"}), ContainsSubstring("not a whole number")
  );
  CHECK_THROWS_WITH(gdal::read_features<geo::MultiPolygon>({.path = path, .layer = {}, .id_field = "ref"}), ContainsSubstring("no field 'ref'"));

  // The first shop's branch is null: a feature without its id is refused, by name.
  CHECK_THROWS_WITH(
      gdal::read_features<geo::Point>({.path = files.write("shops.geojson", SHOPS), .layer = {}, .id_field = "branch"}),
      ContainsSubstring("feature 1") && ContainsSubstring("has no 'branch'")
  );
}

TEST_CASE("gdal: features are handed over a chunk at a time, in the file's order", "[gdal]") {
  const Files       files;
  const std::string path = files.write("shops.geojson", SHOPS);

  std::vector<std::vector<std::int64_t>> chunks;
  const gdal::VectorRead read = gdal::read_features<geo::Point>({.path = path, .layer = {}, .id_field = {}}, 2, [&](auto chunk) {
    std::vector<std::int64_t> ids;
    ids.reserve(chunk.size());
    for ( const auto& shop : chunk ) {
      ids.push_back(shop.id);
    }
    chunks.push_back(std::move(ids));
  });

  CHECK(read.features == 3);
  CHECK(chunks == std::vector<std::vector<std::int64_t>>{{1, 2}, {3}});  // the last chunk is what is left
  CHECK_THROWS_AS(gdal::read_features<geo::Point>({.path = path, .layer = {}, .id_field = {}}, 0, [](auto) {}), std::invalid_argument);
}

TEST_CASE("gdal: what the callback throws is passed on, after the chunks it took", "[gdal]") {
  const Files       files;
  const std::string path = files.write("shops.geojson", SHOPS);

  std::size_t chunks = 0;
  const auto  taking_one = [&](const miniverse::Features<geo::Point>& /*chunk*/) {
    if ( ++chunks == 2 ) {
      throw std::runtime_error("one chunk is enough");
    }
  };

  CHECK_THROWS_WITH(gdal::read_features<geo::Point>({.path = path, .layer = {}, .id_field = {}}, 1, taking_one), ContainsSubstring("is enough"));
  CHECK(chunks == 2);
}

TEST_CASE("gdal: a file that is not there, or a layer it does not have, is refused with the reason", "[gdal]") {
  const Files       files;
  const std::string path = files.write("shops.geojson", SHOPS);

  CHECK_THROWS_WITH(
      gdal::read_features<geo::Point>({.path = files.path_of("nothing.geojson"), .layer = {}, .id_field = {}}), ContainsSubstring("can't be opened")
  );
  CHECK_THROWS_WITH(gdal::read_features<geo::Point>({.path = path, .layer = "streets", .id_field = {}}), ContainsSubstring("no layer 'streets'"));
  CHECK_THROWS_WITH(gdal::read_features<geo::LineString>({.path = path, .layer = {}, .id_field = {}}), ContainsSubstring("is not a LineString"));
}

TEST_CASE("gdal: a file that names no coordinate system, or several layers with none named, is refused", "[gdal]") {
  const Files       files;
  const std::string table = files.write("no_system.csv", NO_SYSTEM);

  // A second table beside it: OGR opens their directory as one dataset of two layers.
  const Files       two;
  const std::string first = two.write("first.csv", NO_SYSTEM);
  std::ignore = two.write("second.csv", NO_SYSTEM);
  const std::string directory = std::filesystem::path(first).parent_path().string();

  CHECK_THROWS_WITH(gdal::read_features<geo::Point>({.path = table, .layer = {}, .id_field = {}}), ContainsSubstring("names no coordinate system"));
  CHECK_THROWS_WITH(gdal::read_features<geo::Point>({.path = directory, .layer = {}, .id_field = {}}), ContainsSubstring("has 2 layers"));
}

TEST_CASE("gdal: a GeoPackage reads as the GeoJSON it was made from", "[gdal]") {
  const Files       files;
  const std::string geojson = files.write("buildings.geojson", BUILDINGS);
  const std::string geopackage = files.path_of("buildings.gpkg");
  to_geopackage(geojson, geopackage);

  const auto from_geojson = gdal::read_features<geo::MultiPolygon>({.path = geojson, .layer = {}, .id_field = {}});
  const auto from_geopackage = gdal::read_features<geo::MultiPolygon>({.path = geopackage, .layer = "buildings", .id_field = {}});

  REQUIRE(from_geopackage.size() == from_geojson.size());
  for ( std::size_t i = 0; i < from_geojson.size(); ++i ) {
    CHECK(from_geopackage.at(i).id == from_geojson.at(i).id);
    CHECK(bg::to_wkt(from_geopackage.at(i).geometry) == bg::to_wkt(from_geojson.at(i).geometry));
    CHECK(from_geopackage.at(i).tags == from_geojson.at(i).tags);
  }
}

TEST_CASE("integration: a GeoPackage pushed a chunk at a time loads back as it was read", "[gdal][integration]") {
  const Files       files;
  const std::string geopackage = files.path_of("buildings.gpkg");
  to_geopackage(files.write("buildings.geojson", BUILDINGS), geopackage);
  const gdal::VectorSource source{.path = geopackage, .layer = {}, .id_field = "osm_id"};

  miniverse::Miniverse world(test_db(), miniverse::Layer<Buildings>("miniverse_test_gdal_buildings"));
  world.drop_tables();
  world.create_tables();

  const gdal::VectorRead read = gdal::read_features<geo::MultiPolygon>(source, 1, [&](auto chunk) { world.push<Buildings>(std::move(chunk)).get(); });
  const auto             loaded = world.load<Buildings>(bg::from_wkt<geo::Polygon>("POLYGON((-1 -1,30 -1,30 30,-1 30,-1 -1))")).get();
  const auto             in_file = gdal::read_features<geo::MultiPolygon>(source);

  CHECK(read.features == 2);
  REQUIRE(loaded.size() == in_file.size());
  for ( std::size_t i = 0; i < in_file.size(); ++i ) {
    CHECK(loaded.at(i).id == in_file.at(i).id);
    CHECK(bg::to_wkt(loaded.at(i).geometry) == bg::to_wkt(in_file.at(i).geometry));
  }
  CHECK(loaded.at(0).tags.text() == R"({"name": "Town hall", "height": 12.5, "levels": 3})");  // jsonb's own order: shorter keys first
  CHECK(loaded.at(1).tags.text() == R"({"name": "Depot", "height": 4.0})");

  world.drop_tables();
}

TEST_CASE("integration: a file pushed in parts is written whole, or not at all", "[gdal][integration]") {
  const Files       files;
  const std::string path = files.write("buildings.geojson", BUILDINGS);
  const gdal::VectorSource source{.path = path, .layer = {}, .id_field = {}};
  const auto               everywhere = bg::from_wkt<geo::Polygon>("POLYGON((-1 -1,30 -1,30 30,-1 30,-1 -1))");

  miniverse::Miniverse world(test_db(), miniverse::Layer<Buildings>("miniverse_test_gdal_buildings"));
  world.drop_tables();
  world.create_tables();

  // A chunk to a part. The first time the reader stops after its first chunk, as a file damaged half way would: nothing is written.
  {
    auto        parts = world.begin_push<Buildings>().get();
    std::size_t chunks = 0;
    CHECK_THROWS_WITH(
        gdal::read_features<geo::MultiPolygon>(
            source, 1,
            [&](auto chunk) {
              parts.add(std::move(chunk)).get();
              if ( ++chunks == 1 ) {
                throw std::runtime_error("the file ends here");
              }
            }
        ),
        ContainsSubstring("ends here")
    );
  }
  const auto after_half = world.load<Buildings>(everywhere).get();

  auto parts = world.begin_push<Buildings>().get();
  std::ignore = gdal::read_features<geo::MultiPolygon>(source, 1, [&](auto chunk) { parts.add(std::move(chunk)).get(); });
  parts.commit().get();
  const auto after_all = world.load<Buildings>(everywhere).get();

  CHECK(after_half.empty());
  CHECK(after_all.size() == 2);

  world.drop_tables();
}

TEST_CASE("gdal: a file read by location gives the features that intersect it, in the file's order", "[gdal]") {
  const Files              files;
  const gdal::VectorSource source{.path = files.write("areas.geojson", AREAS), .layer = {}, .id_field = {}};
  const auto               in = [&source](const std::string& wkt) {
    miniverse::Features<geo::MultiPolygon> found;
    std::ignore = gdal::read_features_in<geo::MultiPolygon>(source, polygon(wkt), 100, [&found](auto chunk) {
      found = std::move(chunk);

      return true;
    });

    return ids(found);
  };

  CHECK(in("POLYGON((-5 -5,30 -5,30 30,-5 30,-5 -5))") == std::vector<std::int64_t>{5, 1, 2, 3, 4});  // as the file has them, not by id
  CHECK(in("POLYGON((-1 -1,5 -1,5 5,-1 5,-1 -1))") == std::vector<std::int64_t>{5, 1, 4});             // 5 by its corner alone
  CHECK(in("POLYGON((1.4 1.4,1.6 1.4,1.6 1.6,1.4 1.6,1.4 1.4))").empty());                              // in the hole of 1
  CHECK(in("POLYGON((10 10.5,10.3 10.5,10.3 10.9,10 10.9,10 10.5))").empty());                          // in the box of a triangle, beside it
  CHECK(in("POLYGON((10.6 10,10.9 10,10.9 10.2,10.6 10.2,10.6 10))") == std::vector<std::int64_t>{2});
  CHECK(in("POLYGON((6.5 0.5,7 0.5,7 1,6.5 1,6.5 0.5))") == std::vector<std::int64_t>{3});              // whichever way its ring runs
  CHECK(in("POLYGON((50 50,51 50,51 51,50 51,50 50))").empty());
  // A location with a hole of its own, which the small square lies in.
  CHECK(in("POLYGON((3 3,9 3,9 9,3 9,3 3),(4.2 4.2,4.2 5.5,5.5 5.5,5.5 4.2,4.2 4.2))") == std::vector<std::int64_t>{1});
}

TEST_CASE("gdal: a read by location hands over chunks until its callback wants no more", "[gdal]") {
  const Files              files;
  const gdal::VectorSource source{.path = files.write("areas.geojson", AREAS), .layer = {}, .id_field = {}};
  std::vector<std::size_t> sizes;

  const gdal::VectorRead read = gdal::read_features_in<geo::MultiPolygon>(source, polygon("POLYGON((-5 -5,30 -5,30 30,-5 30,-5 -5))"), 2, [&sizes](auto chunk) {
    sizes.push_back(chunk.size());

    return sizes.size() < 2;
  });

  CHECK(sizes == std::vector<std::size_t>{2, 2});
  CHECK(read.features == 4);
}

TEST_CASE("gdal: a file in another coordinate system is read by a location in WGS 84", "[gdal]") {
  const Files              files;
  const gdal::VectorSource source{.path = files.write("path.geojson", MERCATOR_PATH), .layer = {}, .id_field = {}};
  const auto               count_in = [&source](const std::string& wkt) {
    std::size_t found = 0;
    std::ignore = gdal::read_features_in<geo::LineString>(source, polygon(wkt), 100, [&found](auto chunk) {
      found = chunk.size();

      return true;
    });

    return found;
  };

  CHECK(count_in("POLYGON((0.4 0.4,0.6 0.4,0.6 0.6,0.4 0.6,0.4 0.4))") == 1);  // the line runs from (0, 0) to (1, 1)
  CHECK(count_in("POLYGON((0.6 0.1,0.9 0.1,0.9 0.4,0.6 0.4,0.6 0.1))") == 0);  // in its box, beside it
  CHECK(count_in("POLYGON((2 2,3 2,3 3,2 3,2 2))") == 0);
}

TEST_CASE("gdal: a file as a layer is loaded, streamed and verified with no database", "[gdal]") {
  const Files          files;
  miniverse::Miniverse world(
      gdal::feature_file<Buildings>({.path = files.write("areas.geojson", AREAS), .layer = {}, .id_field = {}}),
      gdal::feature_file<Shops>({.path = files.write("shops.geojson", SHOPS), .layer = {}, .id_field = {}})
  );
  const geo::Polygon       near_origin = polygon("POLYGON((-1 -1,5 -1,5 5,-1 5,-1 -1))");
  std::vector<std::size_t> sizes;

  world.verify().get();
  const auto buildings = world.load<Buildings>(near_origin).get();
  const auto shops = world.load<Shops>(near_origin).get();
  world.stream<Buildings>(near_origin, [&sizes](const miniverse::Features<geo::MultiPolygon>& chunk) { sizes.push_back(chunk.size()); }, {.chunk_rows = 2}).get();

  CHECK(ids(buildings) == std::vector<std::int64_t>{5, 1, 4});
  CHECK(buildings.at(1).tags.text() == R"({"name":"holed"})");
  CHECK(ids(shops) == std::vector<std::int64_t>{1});
  CHECK(sizes == std::vector<std::size_t>{2, 1});
}

TEST_CASE("gdal: verify says what keeps a file from being a layer", "[gdal]") {
  const Files          files;
  miniverse::Miniverse world(
      gdal::feature_file<Buildings>({.path = files.write("tracks.geojson", TRACKS), .layer = {}, .id_field = {}}),
      gdal::feature_file<Shops>({.path = files.path_of("missing.gpkg"), .layer = {}, .id_field = {}}),
      gdal::feature_file<Paths>({.path = files.write("no_system.csv", NO_SYSTEM), .layer = {}, .id_field = {}})
  );

  try {
    world.verify().get();
    FAIL("it verified");
  } catch ( const miniverse::TablesDiffer& differ ) {
    REQUIRE(differ.differences().size() == 3);
    CHECK_THAT(differ.differences().at(0), ContainsSubstring("tracks.geojson") && ContainsSubstring("are Line String, not MultiPolygon"));
    CHECK_THAT(differ.differences().at(1), ContainsSubstring("missing.gpkg") && ContainsSubstring("can't be opened"));
    CHECK_THAT(differ.differences().at(2), ContainsSubstring("no_system.csv") && ContainsSubstring("names no coordinate system"));
  }

  CHECK_THROWS_WITH(world.load<Shops>(polygon("POLYGON((0 0,1 0,1 1,0 0))")).get(), ContainsSubstring("can't be opened"));
}

TEST_CASE("integration: a file of polygons loads as the table it was pushed into, as GeoJSON and as a GeoPackage", "[gdal][integration]") {
  const Files       files;
  const std::string geojson = files.write("areas.geojson", AREAS);
  const std::string geopackage = files.path_of("areas.gpkg");
  to_geopackage(geojson, geopackage);

  check_loads_as_a_table<Buildings>(
      "miniverse_test_gdal_areas",
      {{.path = geojson, .layer = {}, .id_field = {}}, {.path = geopackage, .layer = "buildings", .id_field = {}}},
      {
          polygon("POLYGON((-5 -5,30 -5,30 30,-5 30,-5 -5))"),
          polygon("POLYGON((-1 -1,5 -1,5 5,-1 5,-1 -1))"),
          polygon("POLYGON((1.4 1.4,1.6 1.4,1.6 1.6,1.4 1.6,1.4 1.4))"),
          polygon("POLYGON((10 10.5,10.3 10.5,10.3 10.9,10 10.9,10 10.5))"),
          polygon("POLYGON((10.6 10,10.9 10,10.9 10.2,10.6 10.2,10.6 10))"),
          polygon("POLYGON((6.5 0.5,7 0.5,7 1,6.5 1,6.5 0.5))"),
          polygon("POLYGON((9 9,12 9,9 12,9 9))"),
          polygon("POLYGON((3 3,9 3,9 9,3 9,3 3),(4.2 4.2,4.2 5.5,5.5 5.5,5.5 4.2,4.2 4.2))"),
          polygon("POLYGON((4 0,6 0,6 2,4 2,4 0))"),  // an edge of 1 on one side, an edge of 3 on the other
          polygon("POLYGON((50 50,51 50,51 51,50 51,50 50))"),
      }
  );
}

TEST_CASE("integration: files of points and of lines load as the tables they were pushed into", "[gdal][integration]") {
  const Files files;

  check_loads_as_a_table<Shops>(
      "miniverse_test_gdal_shops", {{.path = files.write("shops.geojson", SHOPS), .layer = {}, .id_field = {}}},
      {
          polygon("POLYGON((-10 -10,50 -10,50 50,-10 50,-10 -10))"),
          polygon("POLYGON((1 2,2 2,2 3,1 3,1 2))"),
          polygon("POLYGON((1.5 2.5,2 2.5,2 3,1.5 3,1.5 2.5))"),  // the first shop is its corner
          polygon("POLYGON((6 7,8 7,8 9,6 9,6 7),(6.5 7.5,6.5 8.5,7.5 8.5,7.5 7.5,6.5 7.5))"),  // the third is in its hole
      }
  );
  check_loads_as_a_table<Paths>(
      "miniverse_test_gdal_tracks", {{.path = files.write("tracks.geojson", TRACKS), .layer = {}, .id_field = {}}},
      {
          polygon("POLYGON((-10 -10,50 -10,50 50,-10 50,-10 -10))"),
          polygon("POLYGON((0 0,1 0,1 1,0 1,0 0))"),  // track 1 crosses it; track 2's box overlaps it, and it passes by
          polygon("POLYGON((3 2,4 2,4 3,3 3,3 2))"),  // track 1 ends on its corner
          polygon("POLYGON((39 39,42 39,42 42,39 42,39 39))"),
      }
  );
}

TEST_CASE("gdal: a location's edge is a curve in the file's own system, and a feature just inside it is still found", "[gdal]") {
  const Files              files;
  const gdal::VectorSource source{.path = files.write("by_the_parallel.geojson", BY_THE_PARALLEL), .layer = {}, .id_field = {}};
  miniverse::Features<geo::Point> found;

  // From longitude 8 to 10.1, its south edge the parallel of 50 degrees: straight here, and bowed in the file's system.
  std::ignore = gdal::read_features_in<geo::Point>(source, polygon("POLYGON((8 50,10.1 50,10.1 51,8 51,8 50))"), 100, [&found](auto chunk) {
    found = std::move(chunk);

    return true;
  });

  CHECK(ids(found) == std::vector<std::int64_t>{1});
}

TEST_CASE("gdal: only a feature in the location must be of the type asked for", "[gdal]") {
  const Files              files;
  const gdal::VectorSource source{.path = files.write("mixed.geojson", AREAS_AND_A_POINT), .layer = {}, .id_field = {}};
  const auto               areas_in = [&source](const std::string& wkt) {
    miniverse::Features<geo::MultiPolygon> found;
    std::ignore = gdal::read_features_in<geo::MultiPolygon>(source, polygon(wkt), 100, [&found](auto chunk) {
      found = std::move(chunk);

      return true;
    });

    return ids(found);
  };

  // The point is in the box of this triangle, and beside it.
  CHECK(areas_in("POLYGON((9 9,12 9,9 12,9 9))") == std::vector<std::int64_t>{2});
  CHECK_THROWS_WITH(areas_in("POLYGON((9 9,12 9,12 12,9 12,9 9))"), ContainsSubstring("feature 9") && ContainsSubstring("not a MultiPolygon"));
}

TEST_CASE("gdal: a read by location gives the same features whatever the size of its chunks", "[gdal]") {
  const Files              files;
  const gdal::VectorSource source{.path = files.write("areas.geojson", AREAS), .layer = {}, .id_field = {}};
  const auto               in_chunks_of = [&source](std::size_t chunk_features) {
    std::vector<std::int64_t> found;
    std::ignore = gdal::read_features_in<geo::MultiPolygon>(source, polygon("POLYGON((-5 -5,30 -5,30 30,-5 30,-5 -5))"), chunk_features, [&found](auto chunk) {
      for ( const auto& feature : chunk ) {
        found.push_back(feature.id);
      }

      return true;
    });

    return found;
  };

  CHECK(in_chunks_of(1) == std::vector<std::int64_t>{5, 1, 2, 3, 4});
  CHECK(in_chunks_of(2) == in_chunks_of(1));
  CHECK(in_chunks_of(5) == in_chunks_of(1));  // the last chunk full: no empty one after it
  CHECK(in_chunks_of(100) == in_chunks_of(1));
}

TEST_CASE("integration: a file in another coordinate system loads as the table it was pushed into", "[gdal][integration]") {
  const Files files;

  check_loads_as_a_table<Shops>(
      "miniverse_test_gdal_shops", {{.path = files.write("by_the_parallel.geojson", BY_THE_PARALLEL), .layer = {}, .id_field = {}}},
      {
          polygon("POLYGON((8 50,10.1 50,10.1 51,8 51,8 50))"),   // its south edge between the two points
          polygon("POLYGON((8 49,10.1 49,10.1 50,8 50,8 49))"),   // its north edge there
          polygon("POLYGON((8 49,10.1 49,10.1 51,8 51,8 49))"),
          polygon("POLYGON((20 49,21 49,21 51,20 51,20 49))"),    // in the next zones: far from the file
      }
  );
}
