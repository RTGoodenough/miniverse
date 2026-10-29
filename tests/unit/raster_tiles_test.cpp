// A raster cut into the grid's tiles, and tiles stitched into the window a load asks for.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "miniverse/geo/raster.hpp"
#include "miniverse/model/raster/tiles.hpp"
#include "support/geometry.hpp"
#include "support/rasters.hpp"

namespace geo = miniverse::geo;

using miniverse::raster::cut;
using miniverse::raster::Tile;
using miniverse::raster::window;
using Raster = geo::Raster<std::int16_t>;
using test::GRID;
using test::NODATA;
using test::numbered;
using test::polygon;

namespace {

/** @return The pixels of `tiles`, as a load has them. */
[[nodiscard]] std::vector<Raster> pixels_of(std::vector<Tile<std::int16_t>> tiles) {
  std::vector<Raster> rasters;

  rasters.reserve(tiles.size());
  for ( Tile<std::int16_t>& tile : tiles ) {
    rasters.push_back(std::move(tile.pixels));
  }

  return rasters;
}

}  // namespace

TEST_CASE("tiles: a raster is cut into the grid's tiles it covers", "[raster_tiles]") {
  // 6 by 6 pixels from (0.5, 1): columns 722 to 727 of the grid and rows 356 to 361, so tiles 180 and 181 across, 89 and 90 down.
  const std::vector<Tile<std::int16_t>> tiles = cut(numbered(0.5, 1, 6, 6), GRID);

  // 360 tiles span the world, so tile 180 across and 89 down is 89 * 360 + 180.
  REQUIRE(tiles.size() == 4);
  CHECK(tiles.front().id == (89 * 360) + 180);

  const Raster& tile = tiles.front().pixels;
  CHECK(tile.west == 0);
  CHECK(tile.north == 1);
  CHECK(tile.width == 4);
  CHECK(tile.height == 4);
  CHECK(tile.pixel_width == 0.25);
  CHECK(tile.at(0, 0) == NODATA);  // west of the raster
  CHECK(tile.at(2, 0) == 0);       // the raster's first pixel
  CHECK(tile.at(3, 3) == 103);

  CHECK(tiles.back().id == (90 * 360) + 181);
  CHECK(tiles.back().pixels.at(0, 0) == 204);  // the raster's column 2, row 4
}

TEST_CASE("tiles: a raster's own nodata becomes the grid's, and a tile with no data is left out", "[raster_tiles]") {
  Raster raster = numbered(0.5, 1, 2, 1);
  raster.nodata = 0;  // the first pixel has no data, and now the second too: the one tile they are in is left out
  raster.pixels.at(1) = 0;

  CHECK(cut(raster, GRID).empty());

  raster.pixels.at(1) = 7;
  const std::vector<Tile<std::int16_t>> tiles = cut(raster, GRID);

  REQUIRE(tiles.size() == 1);
  CHECK(tiles.front().pixels.at(2, 0) == NODATA);
  CHECK(tiles.front().pixels.at(3, 0) == 7);
}

TEST_CASE("tiles: a pixel with data equal to the grid's nodata is refused, not lost", "[raster_tiles]") {
  Raster raster = numbered(0.5, 1, 2, 1);
  raster.nodata = 0;
  raster.pixels.at(1) = NODATA;  // has data in this raster, but would have none on the grid

  CHECK_THROWS_AS(cut(raster, GRID), std::invalid_argument);
}

TEST_CASE("tiles: a raster not on the grid is not cut", "[raster_tiles]") {
  Raster other_size = numbered(0, 1, 2, 2);
  other_size.pixel_width = other_size.pixel_height = 0.5;
  Raster wrong_count = numbered(0, 1, 2, 2);
  wrong_count.pixels.pop_back();

  CHECK_THROWS_AS(cut(other_size, GRID), std::invalid_argument);
  CHECK_THROWS_AS(cut(numbered(0.1, 1, 2, 2), GRID), std::invalid_argument);      // between pixels
  CHECK_THROWS_AS(cut(numbered(-180.25, 1, 2, 2), GRID), std::invalid_argument);  // west of the world
  CHECK_THROWS_AS(cut(numbered(179.75, 1, 2, 2), GRID), std::invalid_argument);   // east of it
  CHECK_THROWS_AS(cut(wrong_count, GRID), std::invalid_argument);
}

TEST_CASE("tiles: a grid that is not one is refused before a raster is cut with it", "[raster_tiles]") {
  CHECK_THROWS_AS(cut(numbered(0, 1, 2, 2), {.pixels_per_degree = 4, .tile_pixels = 0, .nodata = NODATA}), std::invalid_argument);
  CHECK_THROWS_AS(cut(numbered(0, 1, 2, 2), {.pixels_per_degree = 0, .tile_pixels = 4, .nodata = NODATA}), std::invalid_argument);
}

TEST_CASE("tiles: tiles stitched over a raster's own box give the raster back", "[raster_tiles]") {
  const Raster raster = numbered(0.5, 1, 6, 6);

  CHECK(window(pixels_of(cut(raster, GRID)), polygon("POLYGON((0.5 -0.5,2 -0.5,2 1,0.5 1,0.5 -0.5))")) == raster);
}

TEST_CASE("tiles: a box is widened to whole pixels, and pixels no tile covers are nodata", "[raster_tiles]") {
  const std::vector<Raster> tiles = pixels_of(cut(numbered(0, 1, 4, 4), GRID));  // the one tile from (0, 1) to (1, 0)

  // Inside the tile, off its pixels' edges: pixels 1 to 2 across (0.25 to 0.75) and 1 to 2 down (latitude 0.75 to 0.25).
  const Raster inside = window(tiles, polygon("POLYGON((0.3 0.3,0.6 0.3,0.6 0.6,0.3 0.6,0.3 0.3))"));
  CHECK(inside.west == 0.25);
  CHECK(inside.north == 0.75);
  CHECK(inside.width == 2);
  CHECK(inside.height == 2);
  CHECK(inside.pixels == std::vector<std::int16_t>{101, 201, 102, 202});

  // Half a degree past the tile to the west: two pixels of nodata, then the tile's first column.
  const Raster past = window(tiles, polygon("POLYGON((-0.5 0.75,0.25 0.75,0.25 1,-0.5 1,-0.5 0.75))"));
  CHECK(past.west == -0.5);
  CHECK(past.width == 3);
  CHECK(past.height == 1);
  CHECK(past.pixels == std::vector<std::int16_t>{NODATA, NODATA, 0});

  // A point is one pixel.
  CHECK(window(tiles, polygon("POLYGON((0.3 0.6,0.3 0.6,0.3 0.6,0.3 0.6))")).pixels == std::vector<std::int16_t>{101});
}

TEST_CASE("tiles: with no tiles there is no grid, so the window is empty", "[raster_tiles]") {
  const Raster empty = window(std::vector<Raster>{}, polygon("POLYGON((0 0,1 0,1 1,0 1,0 0))"));

  CHECK(empty.width == 0);
  CHECK(empty.height == 0);
  CHECK(empty.pixels.empty());
}

TEST_CASE("tiles: tiles that don't share a grid are not stitched", "[raster_tiles]") {
  std::vector<Raster> tiles = pixels_of(cut(numbered(0, 1, 8, 4), GRID));
  REQUIRE(tiles.size() == 2);
  tiles.back().west += 0.1;

  CHECK_THROWS_AS(window(tiles, polygon("POLYGON((0 0,2 0,2 1,0 1,0 0))")), std::invalid_argument);
}
