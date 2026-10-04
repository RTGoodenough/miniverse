// Vector files read with GDAL's OGR: features in WGS 84 with ids and tags, single geometries promoted to multi ones, and a file
// pushed into a feature layer and loaded back.
//
// The files are GeoJSON written here as text, and a GeoPackage GDAL makes from one. The last test needs MINIVERSE_TEST_DB (a
// libpq connection string to a scratch database with PostGIS), and is skipped without it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <boost/geometry/io/wkt/read.hpp>
#include <boost/geometry/io/wkt/write.hpp>

#include <gdal.h>
#include <gdal_utils.h>

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
