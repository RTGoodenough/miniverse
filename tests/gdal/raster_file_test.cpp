// A raster file as a layer, with no database: loaded by location, streamed in tiles, and verified.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "gdal/tiffs.hpp"
#include "miniverse/gdal/gdal.hpp"
#include "miniverse/miniverse.hpp"
#include "support/files.hpp"
#include "support/geometry.hpp"

namespace geo = miniverse::geo;
namespace gdal = miniverse::gdal;

using Catch::Matchers::ContainsSubstring;
using test::as_it_is;
using test::Elevation;
using test::Files;
using test::GRID;
using test::GRID_NODATA;
using test::numbered;
using test::polygon;
using test::Raster;
using test::write_tiff;

TEST_CASE("gdal raster: a file as a layer loads the box's pixels on the grid, and streams its tiles, with no database", "[gdal]") {
  // Two degrees each way from (1, 3): four tiles of the grid, numbered pixels.
  const Files       files;
  const std::string path = files.path_of("on_grid.tif");
  write_tiff(path, {.epsg = 4326, .west = 1, .north = 3, .pixel = 0.25, .width = 8, .height = 8, .pixels = numbered(8, 8)});
  miniverse::Miniverse     world(gdal::raster_file<Elevation>(as_it_is(path), GRID));
  std::vector<std::size_t> chunks;

  world.verify().get();
  const Raster whole = world.load<"elevation">(polygon("POLYGON((1 1,3 1,3 3,1 3,1 1))")).get();
  const Raster part = world.load<"elevation">(polygon("POLYGON((1.3 2.3,1.6 2.3,1.6 2.6,1.3 2.6,1.3 2.3))")).get();
  const Raster past = world.load<"elevation">(polygon("POLYGON((2.5 2.5,3.5 2.5,3.5 3.5,2.5 3.5,2.5 2.5))")).get();
  world
      .stream<"elevation">(
          polygon("POLYGON((0 0,5 0,5 5,0 5,0 0))"), [&chunks](const std::vector<Raster>& tiles) { chunks.push_back(tiles.size()); },
          {.chunk_rows = 3}
      )
      .get();

  CHECK(world.table_settings<"elevation">().get() == GRID);
  CHECK(whole.pixels == numbered(8, 8));

  // Widened to whole pixels: from 1.25 to 1.75 across, and from 2.75 down to 2.25.
  CHECK(part.west == 1.25);
  CHECK(part.north == 2.75);
  REQUIRE(part.width == 2);
  REQUIRE(part.height == 2);
  CHECK(part.at(0, 0) == 101);  // column 1, row 1 of the file

  // The box reaches past the file to the north and east: no data there, the file's pixels in its corner.
  REQUIRE(past.width == 4);
  REQUIRE(past.height == 4);
  CHECK(past.at(0, 0) == GRID_NODATA);
  CHECK(past.at(0, 2) == 600);  // column 6, row 0 of the file
  CHECK(past.at(2, 2) == GRID_NODATA);

  CHECK(chunks == std::vector<std::size_t>{3, 1});
  CHECK(world.load<"elevation">(polygon("POLYGON((40 40,41 40,41 41,40 41,40 40))")).get() == Raster{});

  // A triangle in the north west of the file: the south-east tile is in its box, and is not read.
  const geo::Polygon  triangle = polygon("POLYGON((1.1 1.2,2.8 2.9,1.1 2.9,1.1 1.2))");
  std::vector<Raster> reached;
  world.stream<"elevation">(triangle, [&reached](std::vector<Raster> tiles) { reached.insert(reached.end(), tiles.begin(), tiles.end()); }).get();
  const Raster askew = world.load<"elevation">(triangle).get();

  CHECK(reached.size() == 3);
  CHECK(askew.value_at({1.6, 2.4}) == 202);           // column 2, row 2 of the file
  CHECK(askew.value_at({2.6, 1.4}) == std::nullopt);  // in the box, in the tile not read
}

TEST_CASE("gdal raster: verify says what keeps a file from being a layer", "[gdal]") {
  const Files          files;
  miniverse::Miniverse world(gdal::raster_file<Elevation>(as_it_is(files.path_of("missing.tif")), GRID));

  try {
    world.verify().get();
    FAIL("it verified");
  } catch ( const miniverse::TablesDiffer& differ ) {
    REQUIRE(differ.differences().size() == 1);
    CHECK_THAT(differ.differences().front(), ContainsSubstring("missing.tif") && ContainsSubstring("can't be opened"));
  }

  CHECK_THROWS_WITH(world.load<"elevation">(polygon("POLYGON((0 0,1 0,1 1,0 0))")).get(), ContainsSubstring("can't be opened"));
  CHECK_THROWS_AS(
      world.stream<"elevation">(polygon("POLYGON((0 0,1 0,1 1,0 0))"), [](const std::vector<Raster>& /*tiles*/) {}, {.chunk_rows = 0}).get(),
      std::invalid_argument
  );
}
