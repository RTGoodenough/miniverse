// Vector files read with GDAL's OGR: features in WGS 84 with ids and tags, single geometries
// promoted to multi ones, curves made lines, and a read by location. The files are GeoJSON and CSV
// written as text, and a GeoPackage GDAL makes from one (gdal/geojsons.hpp).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <boost/geometry/io/wkt/write.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "gdal/geojsons.hpp"
#include "miniverse/gdal/gdal.hpp"
#include "miniverse/miniverse.hpp"
#include "support/checks.hpp"
#include "support/files.hpp"
#include "support/geometry.hpp"

namespace bg   = boost::geometry;
namespace geo  = miniverse::geo;
namespace gdal = miniverse::gdal;

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using test::AREAS;
using test::AREAS_AND_A_POINT;
using test::BUILDINGS;
using test::BY_THE_PARALLEL;
using test::CURVED_AREAS;
using test::CURVED_LINES;
using test::Files;
using test::ids;
using test::MERCATOR_PATH;
using test::NO_SYSTEM;
using test::ONE_PART;
using test::polygon;
using test::SHOPS;
using test::to_geopackage;
using test::WGS_84_PRJ;

TEST_CASE(
    "gdal: a polygon is promoted where multipolygons are asked for, and a feature without geometry "
    "is left out",
    "[gdal]")
{
  const Files       files;
  const std::string path = files.write("buildings.geojson", BUILDINGS);

  miniverse::Features<geo::MultiPolygon> buildings;
  const gdal::VectorRead                 read =
      gdal::read_features<geo::MultiPolygon>({.path = path, .layer = {}, .id_field = {}}, 100,
                                             [&](auto chunk) { buildings = std::move(chunk); });

  CHECK(read.features == 2);
  CHECK(read.without_geometry == 1);
  REQUIRE(buildings.size() == 2);
  CHECK(buildings.at(0).id == 1);
  CHECK(bg::to_wkt(buildings.at(0).geometry) ==
        "MULTIPOLYGON(((0 0,4 0,4 4,0 4,0 0),(1 1,1 2,2 2,2 1,1 1)))");
  CHECK(buildings.at(1).id == 2);
  CHECK(bg::to_wkt(buildings.at(1).geometry) ==
        "MULTIPOLYGON(((10 10,11 10,11 11,10 10)),((20 20,21 20,21 21,20 20)))");
}

TEST_CASE(
    "gdal: where polygons are asked for, a multipolygon of one part is one, and one of several is "
    "refused by name",
    "[gdal]")
{
  const Files files;

  const auto one_part = gdal::read_features<geo::Polygon>(
      {.path = files.write("one_part.geojson", ONE_PART), .layer = {}, .id_field = {}});

  REQUIRE(one_part.size() == 1);
  CHECK(bg::to_wkt(one_part.front().geometry) == "POLYGON((0 0,1 0,1 1,0 0))");
  CHECK_THROWS_WITH(
      gdal::read_features<geo::Polygon>(
          {.path = files.write("buildings.geojson", BUILDINGS), .layer = {}, .id_field = {}}),
      ContainsSubstring("feature 2") &&
          ContainsSubstring("MULTIPOLYGON") &&
          ContainsSubstring("is not a Polygon"));
}

TEST_CASE(
    "gdal: coordinates are longitude then latitude, without heights, and transformed to WGS 84",
    "[gdal]")
{
  const Files files;

  const auto shops = gdal::read_features<geo::Point>(
      {.path = files.write("shops.geojson", SHOPS), .layer = {}, .id_field = {}});
  const auto paths = gdal::read_features<geo::LineString>(
      {.path = files.write("path.geojson", MERCATOR_PATH), .layer = {}, .id_field = {}});

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

TEST_CASE("gdal: a feature's other fields are its tags: numbers as numbers, and a null left out",
          "[gdal]")
{
  const Files       files;
  const std::string path = files.write("buildings.geojson", BUILDINGS);

  const auto buildings =
      gdal::read_features<geo::MultiPolygon>({.path = path, .layer = {}, .id_field = "osm_id"});

  REQUIRE(buildings.size() == 2);
  CHECK(buildings.at(0).id == 101);  // the field, not the file's own id
  CHECK(buildings.at(0).tags.text() ==
        R"({"name":"Town hall","levels":3,"height":12.5})");                 // without
                                                                             // the
                                                                             // id's
                                                                             // field
  CHECK(buildings.at(1).tags.text() == R"({"name":"Depot","height":4.0})");  // without its null
                                                                             // levels
}

TEST_CASE("gdal: a boolean field is true or false, and a field's name is kept whole, slash and all",
          "[gdal]")
{
  const Files files;

  const auto shops = gdal::read_features<geo::Point>(
      {.path = files.write("shops.geojson", SHOPS), .layer = {}, .id_field = {}});

  REQUIRE(shops.size() == 3);
  CHECK(shops.at(0).tags.text() ==
        R"({"ref":"a","open":true,"addr\/street":"High Street"})");  // JSON
                                                                     // may
                                                                     // write
                                                                     // a
                                                                     // slash
                                                                     // so
  CHECK(shops.at(1).tags.text() ==
        R"({"ref":"b","open":false,"addr\/street":"Mill Lane","branch":12})");
}

TEST_CASE("gdal: a line is promoted where multilinestrings are asked for", "[gdal]")
{
  const Files files;

  const auto paths = gdal::read_features<geo::MultiLineString>(
      {.path = files.write("path.geojson", MERCATOR_PATH), .layer = {}, .id_field = {}});

  REQUIRE(paths.size() == 1);
  REQUIRE(paths.front().geometry.size() == 1);
  CHECK(paths.front().geometry.front().size() == 2);
}

TEST_CASE("gdal: curves become lines, every point of them on the curve", "[gdal]")
{
  const Files       files;
  const std::string lines  = files.write("lines.csv", CURVED_LINES);
  const std::string areas  = files.write("areas.csv", CURVED_AREAS);
  std::ignore              = files.write("lines.prj", WGS_84_PRJ);
  std::ignore              = files.write("areas.prj", WGS_84_PRJ);
  const auto on_the_circle = [](const geo::Point& point) {
    return std::abs(std::hypot(point.x() - 1, point.y()) - 1) < 1e-9;
  };

  const auto arcs =
      gdal::read_features<geo::LineString>({.path = lines, .layer = {}, .id_field = {}});
  REQUIRE(arcs.size() == 2);
  const geo::LineString& arc = arcs.at(0).geometry;
  REQUIRE(arc.size() > 3);
  CHECK(bg::to_wkt(arc.front()) == "POINT(0 0)");
  CHECK(bg::to_wkt(arc.back()) == "POINT(2 0)");
  CHECK(std::ranges::all_of(arc, on_the_circle));
  CHECK(std::ranges::all_of(arc,
                            [](const geo::Point& point) { return point.y() >= -1e-9; }));  // the
                                                                                           // arc
                                                                                           // over
                                                                                           // the
                                                                                           // top,
                                                                                           // not
                                                                                           // under

  const geo::LineString& road = arcs.at(1).geometry;
  REQUIRE(road.size() == arc.size() + 1);
  CHECK(bg::to_wkt(road.front()) == "POINT(-1 0)");
  CHECK(std::ranges::equal(road | std::views::drop(1), arc,
                           [](const geo::Point& one, const geo::Point& other) {
                             return one.x() == other.x() && one.y() == other.y();
                           }));

  const auto discs =
      gdal::read_features<geo::Polygon>({.path = areas, .layer = {}, .id_field = {}});
  REQUIRE(discs.size() == 1);
  const geo::Polygon& disc = discs.front().geometry;
  REQUIRE(disc.outer().size() > 5);
  CHECK(bg::to_wkt(disc.outer().front()) == bg::to_wkt(disc.outer().back()));
  CHECK(std::ranges::all_of(disc.outer(), on_the_circle));
  CHECK(disc.inners().empty());
  CHECK(gdal::read_features<geo::MultiPolygon>({.path = areas, .layer = {}, .id_field = {}})
            .front()
            .geometry.size() == 1);
}

TEST_CASE("gdal: an id field must be there and be a whole number", "[gdal]")
{
  const Files       files;
  const std::string path = files.write("buildings.geojson", BUILDINGS);

  CHECK_THROWS_WITH(
      gdal::read_features<geo::MultiPolygon>({.path = path, .layer = {}, .id_field = "name"}),
      ContainsSubstring("not a whole number"));
  CHECK_THROWS_WITH(
      gdal::read_features<geo::MultiPolygon>({.path = path, .layer = {}, .id_field = "ref"}),
      ContainsSubstring("no field 'ref'"));

  // The first shop's branch is null: a feature without its id is refused, by name.
  CHECK_THROWS_WITH(
      gdal::read_features<geo::Point>(
          {.path = files.write("shops.geojson", SHOPS), .layer = {}, .id_field = "branch"}),
      ContainsSubstring("feature 1") && ContainsSubstring("has no 'branch'"));
}

TEST_CASE("gdal: features are handed over a chunk at a time, in the file's order", "[gdal]")
{
  const Files       files;
  const std::string path = files.write("shops.geojson", SHOPS);

  std::vector<std::vector<std::int64_t>> chunks;
  const gdal::VectorRead                 read = gdal::read_features<geo::Point>(
      {.path = path, .layer = {}, .id_field = {}}, 2, [&](auto chunk) {
        std::vector<std::int64_t> ids;
        ids.reserve(chunk.size());
        for ( const auto& shop : chunk ) {
          ids.push_back(shop.id);
        }
        chunks.push_back(std::move(ids));
      });

  CHECK(read.features == 3);
  CHECK(chunks == std::vector<std::vector<std::int64_t>>{{1, 2}, {3}});  // the last chunk is what
                                                                         // is left
  CHECK_THROWS_AS(
      gdal::read_features<geo::Point>({.path = path, .layer = {}, .id_field = {}}, 0, [](auto) {}),
      std::invalid_argument);
}

TEST_CASE("gdal: what the callback throws is passed on, after the chunks it took", "[gdal]")
{
  const Files       files;
  const std::string path = files.write("shops.geojson", SHOPS);

  std::size_t chunks     = 0;
  const auto  taking_one = [&](const miniverse::Features<geo::Point>& /*chunk*/) {
    if ( ++chunks == 2 ) {
      throw std::runtime_error("one chunk is enough");
    }
  };

  CHECK_THROWS_WITH(
      gdal::read_features<geo::Point>({.path = path, .layer = {}, .id_field = {}}, 1, taking_one),
      ContainsSubstring("is enough"));
  CHECK(chunks == 2);
}

TEST_CASE("gdal: a file that is not there, or a layer it does not have, is refused with the reason",
          "[gdal]")
{
  const Files       files;
  const std::string path = files.write("shops.geojson", SHOPS);

  CHECK_THROWS_WITH(gdal::read_features<geo::Point>(
                        {.path = files.path_of("nothing.geojson"), .layer = {}, .id_field = {}}),
                    ContainsSubstring("can't be opened"));
  CHECK_THROWS_WITH(
      gdal::read_features<geo::Point>({.path = path, .layer = "streets", .id_field = {}}),
      ContainsSubstring("no layer 'streets'"));
  CHECK_THROWS_WITH(
      gdal::read_features<geo::LineString>({.path = path, .layer = {}, .id_field = {}}),
      ContainsSubstring("is not a LineString"));
}

TEST_CASE(
    "gdal: a file that names no coordinate system, or several layers with none named, is refused",
    "[gdal]")
{
  const Files       files;
  const std::string table = files.write("no_system.csv", NO_SYSTEM);

  // A second table beside it: OGR opens their directory as one dataset of two layers.
  const Files       two;
  const std::string first     = two.write("first.csv", NO_SYSTEM);
  std::ignore                 = two.write("second.csv", NO_SYSTEM);
  const std::string directory = std::filesystem::path(first).parent_path().string();

  CHECK_THROWS_WITH(gdal::read_features<geo::Point>({.path = table, .layer = {}, .id_field = {}}),
                    ContainsSubstring("names no coordinate system"));
  CHECK_THROWS_WITH(
      gdal::read_features<geo::Point>({.path = directory, .layer = {}, .id_field = {}}),
      ContainsSubstring("has 2 layers"));
}

TEST_CASE("gdal: a GeoPackage reads as the GeoJSON it was made from", "[gdal]")
{
  const Files       files;
  const std::string geojson    = files.write("buildings.geojson", BUILDINGS);
  const std::string geopackage = files.path_of("buildings.gpkg");
  to_geopackage(geojson, geopackage);

  const auto from_geojson =
      gdal::read_features<geo::MultiPolygon>({.path = geojson, .layer = {}, .id_field = {}});
  const auto from_geopackage = gdal::read_features<geo::MultiPolygon>(
      {.path = geopackage, .layer = "buildings", .id_field = {}});

  REQUIRE(from_geopackage.size() == from_geojson.size());
  for ( std::size_t i = 0; i < from_geojson.size(); ++i ) {
    CHECK(from_geopackage.at(i).id == from_geojson.at(i).id);
    CHECK(bg::to_wkt(from_geopackage.at(i).geometry) == bg::to_wkt(from_geojson.at(i).geometry));
    CHECK(from_geopackage.at(i).tags == from_geojson.at(i).tags);
  }
}

TEST_CASE("gdal: a file read by location gives the features that intersect it, in the file's order",
          "[gdal]")
{
  const Files              files;
  const gdal::VectorSource source{
      .path = files.write("areas.geojson", AREAS), .layer = {}, .id_field = {}};
  const auto in = [&source](const std::string& wkt) {
    miniverse::Features<geo::MultiPolygon> found;
    std::ignore =
        gdal::read_features_in<geo::MultiPolygon>(source, polygon(wkt), 100, [&found](auto chunk) {
          found = std::move(chunk);

          return true;
        });

    return ids(found);
  };

  CHECK(in("POLYGON((-5 -5,30 -5,30 30,-5 30,-5 -5))") ==
        std::vector<std::int64_t>{5, 1, 2, 3, 4});  // as the file has them, not by id
  CHECK(
      in("POLYGON((-1 -1,5 -1,5 5,-1 5,-1 -1))") == std::vector<std::int64_t>{5, 1, 4});  // 5 by
                                                                                          // its
                                                                                          // corner
                                                                                          // alone
  CHECK(in("POLYGON((1.4 1.4,1.6 1.4,1.6 1.6,1.4 1.6,1.4 1.4))").empty());      // in the hole of 1
  CHECK(in("POLYGON((10 10.5,10.3 10.5,10.3 10.9,10 10.9,10 10.5))").empty());  // in the box of a
                                                                                // triangle, beside
                                                                                // it
  CHECK(
      in("POLYGON((10.6 10,10.9 10,10.9 10.2,10.6 10.2,10.6 10))") == std::vector<std::int64_t>{2});
  CHECK(in("POLYGON((6.5 0.5,7 0.5,7 1,6.5 1,6.5 0.5))") ==
        std::vector<std::int64_t>{3});  // whichever
                                        // way
                                        // its
                                        // ring
                                        // runs
  CHECK(in("POLYGON((50 50,51 50,51 51,50 51,50 50))").empty());
  // A location with a hole of its own, which the small square lies in.
  CHECK(in("POLYGON((3 3,9 3,9 9,3 9,3 3),(4.2 4.2,4.2 5.5,5.5 5.5,5.5 4.2,4.2 4.2))") ==
        std::vector<std::int64_t>{1});
}

TEST_CASE("gdal: a read by location hands over chunks until its callback wants no more", "[gdal]")
{
  const Files              files;
  const gdal::VectorSource source{
      .path = files.write("areas.geojson", AREAS), .layer = {}, .id_field = {}};
  std::vector<std::size_t> sizes;

  const gdal::VectorRead read = gdal::read_features_in<geo::MultiPolygon>(
      source, polygon("POLYGON((-5 -5,30 -5,30 30,-5 30,-5 -5))"), 2, [&sizes](auto chunk) {
        sizes.push_back(chunk.size());

        return sizes.size() < 2;
      });

  CHECK(sizes == std::vector<std::size_t>{2, 2});
  CHECK(read.features == 4);
}

TEST_CASE("gdal: a file in another coordinate system is read by a location in WGS 84", "[gdal]")
{
  const Files              files;
  const gdal::VectorSource source{
      .path = files.write("path.geojson", MERCATOR_PATH), .layer = {}, .id_field = {}};
  const auto count_in = [&source](const std::string& wkt) {
    std::size_t found = 0;
    std::ignore =
        gdal::read_features_in<geo::LineString>(source, polygon(wkt), 100, [&found](auto chunk) {
          found = chunk.size();

          return true;
        });

    return found;
  };

  CHECK(count_in("POLYGON((0.4 0.4,0.6 0.4,0.6 0.6,0.4 0.6,0.4 0.4))") == 1);  // the line runs from
                                                                               // (0, 0) to (1, 1)
  CHECK(count_in("POLYGON((0.6 0.1,0.9 0.1,0.9 0.4,0.6 0.4,0.6 0.1))") == 0);  // in its box, beside
                                                                               // it
  CHECK(count_in("POLYGON((2 2,3 2,3 3,2 3,2 2))") == 0);
}

TEST_CASE(
    "gdal: a location's edge is a curve in the file's own system, and a feature just inside it is "
    "still found",
    "[gdal]")
{
  const Files              files;
  const gdal::VectorSource source{
      .path = files.write("by_the_parallel.geojson", BY_THE_PARALLEL), .layer = {}, .id_field = {}};
  miniverse::Features<geo::Point> found;

  // From longitude 8 to 10.1, its south edge the parallel of 50 degrees: straight here, and bowed
  // in the file's system.
  std::ignore = gdal::read_features_in<geo::Point>(
      source, polygon("POLYGON((8 50,10.1 50,10.1 51,8 51,8 50))"), 100, [&found](auto chunk) {
        found = std::move(chunk);

        return true;
      });

  CHECK(ids(found) == std::vector<std::int64_t>{1});
}

TEST_CASE("gdal: only a feature in the location must be of the type asked for", "[gdal]")
{
  const Files              files;
  const gdal::VectorSource source{
      .path = files.write("mixed.geojson", AREAS_AND_A_POINT), .layer = {}, .id_field = {}};
  const auto areas_in = [&source](const std::string& wkt) {
    miniverse::Features<geo::MultiPolygon> found;
    std::ignore =
        gdal::read_features_in<geo::MultiPolygon>(source, polygon(wkt), 100, [&found](auto chunk) {
          found = std::move(chunk);

          return true;
        });

    return ids(found);
  };

  // The point is in the box of this triangle, and beside it.
  CHECK(areas_in("POLYGON((9 9,12 9,9 12,9 9))") == std::vector<std::int64_t>{2});
  CHECK_THROWS_WITH(areas_in("POLYGON((9 9,12 9,12 12,9 12,9 9))"),
                    ContainsSubstring("feature 9") && ContainsSubstring("not a MultiPolygon"));
}

TEST_CASE("gdal: a read by location gives the same features whatever the size of its chunks",
          "[gdal]")
{
  const Files              files;
  const gdal::VectorSource source{
      .path = files.write("areas.geojson", AREAS), .layer = {}, .id_field = {}};
  const auto in_chunks_of = [&source](std::size_t chunk_features) {
    std::vector<std::int64_t> found;
    std::ignore = gdal::read_features_in<geo::MultiPolygon>(
        source, polygon("POLYGON((-5 -5,30 -5,30 30,-5 30,-5 -5))"), chunk_features,
        [&found](auto chunk) {
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
