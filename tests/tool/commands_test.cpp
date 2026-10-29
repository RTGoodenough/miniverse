// The tool's commands, called as the command line calls them, against tables loaded with the library: a file imported loads
// as the file read, a dataset is replaced and removed, and what does not fit a table is refused before anything is written.
// The files are the examples' (examples/data). Each test needs MINIVERSE_TEST_DB, and is skipped without it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <boost/geometry/io/wkt/write.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "commands.hpp"
#include "layer_of.hpp"
#include "miniverse/gdal/raster.hpp"
#include "miniverse/gdal/vector.hpp"
#include "miniverse/miniverse.hpp"
#include "miniverse/osm/ways.hpp"
#include "support/database.hpp"
#include "support/geometry.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;

using Catch::Matchers::ContainsSubstring;
using test::polygon;
using test::test_db;

namespace {

using Buildings = tool::Layer<miniverse::FeatureModel<geo::MultiPolygon>>;
using Roads = tool::Layer<miniverse::RoadModel>;
using Heights = tool::Layer<miniverse::RasterModel<std::int16_t>>;
using Bytes = tool::Layer<miniverse::RasterModel<std::int8_t>>;

const std::string BUILDINGS_FILE = MINIVERSE_EXAMPLE_DATA "/buildings.geojson";
const std::string STREETS_FILE = MINIVERSE_EXAMPLE_DATA "/streets.osm";
const std::string HEIGHTS_FILE = MINIVERSE_EXAMPLE_DATA "/heights.asc";

// All the world: the example files lie well within it.
const geo::Polygon EVERYWHERE = polygon("POLYGON((-179 -89,179 -89,179 89,-179 89,-179 -89))");

/** @return The geometries of `features` as WKT, sorted: the table mints its own ids, so a load is compared as a set. */
[[nodiscard]] std::vector<std::string> wkts(const miniverse::Features<geo::MultiPolygon>& features) {
  std::vector<std::string> result;
  for ( const miniverse::Feature<geo::MultiPolygon>& feature : features ) {
    result.push_back(bg::to_wkt(feature.geometry));
  }
  std::ranges::sort(result);

  return result;
}

/** @return A world of the one layer `layer_t` in `table`, its table dropped before and after the test. */
template <miniverse::IsLayer layer_t>
[[nodiscard]] test::Fresh<miniverse::Miniverse<layer_t>> fresh(const std::string& table) {
  return test::Fresh<miniverse::Miniverse<layer_t>>([](miniverse::Miniverse<layer_t>& /*world*/) {}, test_db(), layer_t(table));
}

}  // namespace

TEST_CASE("tool: a vector file imported into a table that create made loads as the file reads", "[tool][integration]") {
  const tool::Table table{.conninfo = test_db(), .model = "features:multipolygon", .name = "miniverse_test_tool_buildings"};
  auto              buildings = fresh<Buildings>(table.name);

  tool::create(table, {});
  tool::import(table, {.files = {BUILDINGS_FILE}, .chunk = 1});  // a part a feature
  tool::verify(table);

  const auto loaded = buildings.world().load<"table">(EVERYWHERE).get();
  const auto read = miniverse::gdal::read_features<geo::MultiPolygon>({.path = BUILDINGS_FILE});
  REQUIRE(read.size() > 1);
  CHECK(wkts(loaded) == wkts(read));
}

TEST_CASE("tool: an import replaces its dataset, named for the file unless given, and remove takes it away", "[tool][integration]") {
  const tool::Table table{.conninfo = test_db(), .model = "features:multipolygon", .name = "miniverse_test_tool_buildings"};
  auto              buildings = fresh<Buildings>(table.name);
  const std::size_t in_the_file = miniverse::gdal::read_features<geo::MultiPolygon>({.path = BUILDINGS_FILE}).size();

  tool::create(table, {});
  tool::import(table, {.files = {BUILDINGS_FILE}});
  tool::import(table, {.files = {BUILDINGS_FILE}});                      // the same dataset, replaced
  tool::import(table, {.files = {BUILDINGS_FILE}, .dataset = "again"});  // another beside it
  CHECK(buildings.world().load<"table">(EVERYWHERE).get().size() == 2 * in_the_file);
  tool::datasets(table);

  tool::remove(table, "buildings.geojson");
  CHECK(buildings.world().load<"table">(EVERYWHERE).get().size() == in_the_file);
  tool::remove(table, "again");
  CHECK(buildings.world().load<"table">(EVERYWHERE).get().empty());
  CHECK_THROWS_WITH(tool::remove(table, "again"), ContainsSubstring("has no dataset 'again'"));
}

TEST_CASE("tool: an OpenStreetMap file imported into a table of roads loads as the file reads", "[tool][integration]") {
  const tool::Table table{.conninfo = test_db(), .model = "roads", .name = "miniverse_test_tool_roads"};
  auto              roads = fresh<Roads>(table.name);

  tool::create(table, {});
  tool::import(table, {.files = {STREETS_FILE}, .chunk = 1});

  std::vector<std::vector<std::int64_t>> loaded;
  for ( const miniverse::Way& way : roads.world().load<"table">(EVERYWHERE).get() ) {
    loaded.push_back(way.node_ids);
  }
  std::vector<std::vector<std::int64_t>> read;
  for ( const miniverse::Way& way : miniverse::osm::read_ways({.path = STREETS_FILE, .keys = {"highway"}}) ) {
    read.push_back(way.node_ids);
  }
  std::ranges::sort(loaded);
  std::ranges::sort(read);
  REQUIRE(read.size() > 1);
  CHECK(loaded == read);
}

TEST_CASE("tool: a raster table is made with the grid given, and a raster file imported loads as the file reads onto it", "[tool][integration]") {
  const tool::Table             table{.conninfo = test_db(), .model = "raster:int16", .name = "miniverse_test_tool_heights"};
  auto                          heights = fresh<Heights>(table.name);
  const geo::Grid<std::int16_t> grid{.pixels_per_degree = 16, .tile_pixels = 8, .nodata = -32768};

  tool::create(table, {.pixels_per_degree = 16, .tile_pixels = 8, .nodata = "-32768"});
  CHECK(heights.world().table_settings<"table">().get() == grid);
  tool::settings(table);
  tool::import(table, {.files = {HEIGHTS_FILE}, .resampling = "nearest", .window_tiles = 1});

  const geo::Raster<std::int16_t> read =
      miniverse::gdal::read_raster<std::int16_t>({.path = HEIGHTS_FILE, .band = 1, .resampling = miniverse::gdal::Resampling::Nearest}, grid);
  const geo::Raster<std::int16_t> loaded = heights.world().load<"table">(polygon("POLYGON((7 46,9 46,9 47,7 47,7 46))")).get();
  CHECK(loaded.west == read.west);
  CHECK(loaded.north == read.north);
  CHECK(loaded.pixels == read.pixels);
}

TEST_CASE("tool: a table of another model, an option for another model, or a value a pixel can't hold, is refused", "[tool][integration]") {
  const tool::Table buildings{.conninfo = test_db(), .model = "features:multipolygon", .name = "miniverse_test_tool_buildings"};
  const tool::Table as_points{.conninfo = buildings.conninfo, .model = "features:point", .name = buildings.name};
  const tool::Table bytes{.conninfo = buildings.conninfo, .model = "raster:int8", .name = "miniverse_test_tool_bytes"};
  auto              world = fresh<Buildings>(buildings.name);
  auto              raster = fresh<Bytes>(bytes.name);

  tool::create(buildings, {});

  CHECK_THROWS_AS(tool::verify(as_points), miniverse::TablesDiffer);
  CHECK_THROWS_AS(tool::import(as_points, {.files = {BUILDINGS_FILE}}), miniverse::TablesDiffer);
  CHECK_THROWS_WITH(tool::import(buildings, {.files = {BUILDINGS_FILE}, .band = 2}), ContainsSubstring("--band is not for"));
  CHECK_THROWS_WITH(tool::import(buildings, {.files = {BUILDINGS_FILE}, .window_tiles = 2}), ContainsSubstring("--window-tiles is not for"));
  CHECK_THROWS_WITH(tool::import(buildings, {.files = {BUILDINGS_FILE}, .dataset = ""}), ContainsSubstring("has no name"));
  CHECK_THROWS_WITH(tool::import(buildings, {.files = {BUILDINGS_FILE}, .chunk = 0}), ContainsSubstring("at least 1"));
  CHECK_THROWS_WITH(tool::create(buildings, {.tile_pixels = 4}), ContainsSubstring("only a raster table has a grid"));
  CHECK(world.world().load<"table">(EVERYWHERE).get().empty());

  CHECK_THROWS_WITH(tool::create(bytes, {.pixels_per_degree = 4, .tile_pixels = 4, .nodata = "300"}), ContainsSubstring("'300' is not a value"));
  CHECK_THROWS_WITH(
      tool::create(bytes, {.pixels_per_degree = 4, .metres = 30, .tile_pixels = 4, .nodata = "0"}), ContainsSubstring("one of the two")
  );
  CHECK_THROWS_WITH(tool::remove(bytes, "anything"), ContainsSubstring("has no datasets"));
  CHECK_THROWS_WITH(tool::datasets(bytes), ContainsSubstring("has no datasets"));
  CHECK_THROWS_WITH(tool::import(bytes, {.files = {HEIGHTS_FILE}, .chunk = 5}), ContainsSubstring("--chunk is not for"));
  CHECK_THROWS_WITH(tool::verify({.conninfo = buildings.conninfo, .model = "nothing", .name = "t"}), ContainsSubstring("there is no model"));
}
