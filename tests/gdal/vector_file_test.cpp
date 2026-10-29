// A vector file as a layer, with no database: loaded by location, streamed, and verified.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gdal/geojsons.hpp"
#include "miniverse/gdal/gdal.hpp"
#include "miniverse/miniverse.hpp"
#include "support/checks.hpp"
#include "support/files.hpp"
#include "support/geometry.hpp"

namespace geo = miniverse::geo;
namespace gdal = miniverse::gdal;

using Catch::Matchers::ContainsSubstring;
using test::AREAS;
using test::Buildings;
using test::Files;
using test::ids;
using test::NO_SYSTEM;
using test::Paths;
using test::polygon;
using test::Shops;
using test::SHOPS;
using test::TRACKS;

TEST_CASE("gdal: a file as a layer is loaded, streamed and verified with no database", "[gdal]") {
  const Files          files;
  miniverse::Miniverse world(
      gdal::feature_file<Buildings>({.path = files.write("areas.geojson", AREAS), .layer = {}, .id_field = {}}),
      gdal::feature_file<Shops>({.path = files.write("shops.geojson", SHOPS), .layer = {}, .id_field = {}})
  );
  const geo::Polygon       near_origin = polygon("POLYGON((-1 -1,5 -1,5 5,-1 5,-1 -1))");
  std::vector<std::size_t> sizes;

  world.verify().get();
  const auto buildings = world.load<"buildings">(near_origin).get();
  const auto shops = world.load<"shops">(near_origin).get();
  world
      .stream<"buildings">(
          near_origin, [&sizes](const miniverse::Features<geo::MultiPolygon>& chunk) { sizes.push_back(chunk.size()); }, {.chunk_rows = 2}
      )
      .get();

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

  CHECK_THROWS_WITH(world.load<"shops">(polygon("POLYGON((0 0,1 0,1 1,0 0))")).get(), ContainsSubstring("can't be opened"));
}
