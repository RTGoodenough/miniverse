// A grid from a resolution in metres: the nearest whole pixels per degree, within what a grid can be.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <stdexcept>

#include "miniverse/geo/grid.hpp"

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
