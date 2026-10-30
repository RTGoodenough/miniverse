// A grid: from a resolution in metres, the nearest whole pixels per degree within what a grid can be; where its pixels and
// tiles lie, and which tiles a box covers or meets.

#include <catch2/catch_test_macros.hpp>

#include <boost/geometry/io/wkt/read.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

#include "miniverse/geo/grid.hpp"
#include "miniverse/geo/types.hpp"

TEST_CASE("grid: a resolution in metres is the nearest whole pixels per degree", "[grid]") {
  using miniverse::geo::pixels_per_degree_of_metres;

  const miniverse::geo::Grid<std::int16_t> thirty{.pixels_per_degree = pixels_per_degree_of_metres(30), .tile_pixels = 256, .nodata = -32768};

  CHECK(thirty.pixels_per_degree == 3711);
  CHECK(thirty.metres_per_pixel() > 29.99);  // what 30 came to: 29.997
  CHECK(thirty.metres_per_pixel() < 30.0);
  CHECK(pixels_per_degree_of_metres(10) == 11132);
  CHECK(pixels_per_degree_of_metres(90) == 1237);
  CHECK(pixels_per_degree_of_metres(miniverse::geo::METRES_PER_DEGREE) == 1);
}

TEST_CASE("grid: a resolution finer than the finest grid, coarser than a degree, or no number above zero, is refused", "[grid]") {
  using miniverse::geo::pixels_per_degree_of_metres;

  CHECK(pixels_per_degree_of_metres(3.1) == 35910);
  CHECK_THROWS_AS(pixels_per_degree_of_metres(3.0), std::invalid_argument);  // 37106
  CHECK_THROWS_AS(pixels_per_degree_of_metres(1), std::invalid_argument);
  CHECK_THROWS_AS(pixels_per_degree_of_metres(250'000), std::invalid_argument);  // under half a pixel to a degree
  CHECK_THROWS_AS(pixels_per_degree_of_metres(0), std::invalid_argument);
  CHECK_THROWS_AS(pixels_per_degree_of_metres(-30), std::invalid_argument);
  CHECK_THROWS_AS(pixels_per_degree_of_metres(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
  CHECK_THROWS_AS(pixels_per_degree_of_metres(std::numeric_limits<double>::infinity()), std::invalid_argument);
}

TEST_CASE("grid: the tiles that span the world, the last past its edge when tiles don't divide it", "[grid]") {
  constexpr miniverse::geo::Grid<std::int16_t> EVEN{.pixels_per_degree = 4, .tile_pixels = 4, .nodata = -32768};
  constexpr miniverse::geo::Grid<std::int16_t> UNEVEN{.pixels_per_degree = 4, .tile_pixels = 35, .nodata = -32768};

  CHECK(EVEN.tiles_across() == 360);
  CHECK(EVEN.tiles_down() == 180);
  CHECK(UNEVEN.tiles_across() == 42);  // 1440 pixels is 41.14 tiles of 35
  CHECK(UNEVEN.tiles_down() == 21);
}

TEST_CASE("grid: where a pixel's corner and a tile lie, worked out in one place", "[grid]") {
  constexpr miniverse::geo::Grid<std::int16_t> GRID{.pixels_per_degree = 4, .tile_pixels = 4, .nodata = -32768};  // tiles of a degree
  constexpr miniverse::geo::Grid<std::int16_t> UNEVEN{.pixels_per_degree = 4, .tile_pixels = 35, .nodata = -32768};

  CHECK(GRID.pixel_degrees() == 0.25);
  CHECK(GRID.pixels_across() == 1440);
  CHECK(GRID.pixels_down() == 720);

  const miniverse::geo::Point corner = GRID.corner_of(721, 356);
  CHECK(corner.x() == 0.25);
  CHECK(corner.y() == 1);

  const miniverse::geo::Box tile = GRID.tile_outline(180, 89);  // the tile from (0, 1)
  CHECK(tile.min_corner().x() == 0);
  CHECK(tile.min_corner().y() == 0);
  CHECK(tile.max_corner().x() == 1);
  CHECK(tile.max_corner().y() == 1);

  const miniverse::geo::Box world = GRID.outline();
  CHECK(world.min_corner().x() == -180);
  CHECK(world.min_corner().y() == -90);
  CHECK(world.max_corner().x() == 180);
  CHECK(world.max_corner().y() == 90);

  const miniverse::geo::Box over = UNEVEN.outline();  // 42 tiles of 35 pixels: 367.5 degrees across, 183.75 down
  CHECK(over.max_corner().x() == 187.5);
  CHECK(over.min_corner().y() == -93.75);
}

TEST_CASE("grid: the tiles a box covers end at its edges, those it meets reach past them", "[grid]") {
  using miniverse::geo::Box;
  using miniverse::geo::TileRange;

  constexpr miniverse::geo::Grid<std::int16_t> GRID{.pixels_per_degree = 4, .tile_pixels = 4, .nodata = -32768};

  // A box that is exactly the tile from (0, 1): tile 180 across, 89 down.
  CHECK(GRID.tiles_covering(Box{{0, 0}, {1, 1}}) == TileRange{.first_column = 180, .end_column = 181, .first_row = 89, .end_row = 90});
  CHECK(GRID.tiles_meeting(Box{{0, 0}, {1, 1}}) == TileRange{.first_column = 179, .end_column = 182, .first_row = 88, .end_row = 91});

  // One inside it: both say the tile alone.
  CHECK(GRID.tiles_covering(Box{{0.1, 0.1}, {0.9, 0.9}}) == TileRange{.first_column = 180, .end_column = 181, .first_row = 89, .end_row = 90});
  CHECK(GRID.tiles_meeting(Box{{0.1, 0.1}, {0.9, 0.9}}) == TileRange{.first_column = 180, .end_column = 181, .first_row = 89, .end_row = 90});

  // A rounding step past the edge is on it; a hair past it is past it.
  CHECK(GRID.tiles_covering(Box{{0, 0}, {1.000000000001, 1}}).end_column == 181);
  CHECK(GRID.tiles_covering(Box{{0, 0}, {1.00001, 1}}).end_column == 182);
  CHECK(GRID.tiles_meeting(Box{{0.000000000001, 0}, {1, 1}}).first_column == 179);
  CHECK(GRID.tiles_meeting(Box{{0.00001, 0}, {1, 1}}).first_column == 180);

  // Within the world, however far the box reaches; a box of no width on an edge covers nothing, but meets the tiles either side.
  CHECK(GRID.tiles_covering(Box{{-200, -100}, {200, 100}}) == TileRange{.first_column = 0, .end_column = 360, .first_row = 0, .end_row = 180});
  CHECK(GRID.tiles_meeting(Box{{-200, -100}, {200, 100}}) == TileRange{.first_column = 0, .end_column = 360, .first_row = 0, .end_row = 180});
  CHECK(GRID.tiles_covering(Box{{0, 0}, {0, 1}}).empty());
  CHECK(GRID.tiles_meeting(Box{{0, 0}, {0, 1}}) == TileRange{.first_column = 179, .end_column = 181, .first_row = 88, .end_row = 91});

  CHECK(
      TileRange{.first_column = 1, .end_column = 5, .first_row = 1, .end_row = 5}.meet(
          {.first_column = 3, .end_column = 9, .first_row = 0, .end_row = 2}
      ) == TileRange{.first_column = 3, .end_column = 5, .first_row = 1, .end_row = 2}
  );
}

TEST_CASE("grid: a corner on a pixel's corner is that pixel, to within the tolerance, off the world too", "[grid]") {
  using miniverse::geo::GridPixel;

  constexpr miniverse::geo::Grid<std::int16_t> GRID{.pixels_per_degree = 4, .tile_pixels = 4, .nodata = -32768};

  CHECK(GRID.corner_pixel({0, 1}) == GridPixel{.column = 720, .row = 356});
  CHECK(GRID.corner_pixel({0.25 + 1e-12, 1 - 1e-12}) == GridPixel{.column = 721, .row = 356});
  CHECK(GRID.corner_pixel({-180.25, 90.25}) == GridPixel{.column = -1, .row = -1});
  CHECK(! GRID.corner_pixel({0.1, 1}));
  CHECK(! GRID.corner_pixel({0.2501, 1}));
  CHECK(! GRID.corner_pixel({std::numeric_limits<double>::quiet_NaN(), 1}));
  CHECK(! GRID.corner_pixel({1e300, 1}));
}

TEST_CASE("grid: the box of a location, none for one of no points, refused for one of no numbers", "[grid]") {
  using miniverse::geo::box_of;
  using miniverse::geo::Polygon;

  Polygon square;
  boost::geometry::read_wkt("POLYGON((0 0,0 1,1 1,1 0,0 0))", square);
  const std::optional<miniverse::geo::Box> box = box_of(square);
  REQUIRE(box);
  CHECK(box->min_corner().x() == 0);
  CHECK(box->max_corner().y() == 1);

  CHECK(! box_of(Polygon{}));

  Polygon unnumbered;
  boost::geometry::read_wkt("POLYGON((0 0,0 1,1 1,1 0,0 0))", unnumbered);
  unnumbered.outer().at(1).y(std::numeric_limits<double>::infinity());
  CHECK_THROWS_AS(box_of(unnumbered), std::invalid_argument);
}
