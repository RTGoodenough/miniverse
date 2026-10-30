#pragma once

// Small rasters on a coarse grid, for the raster tests that need no database: four pixels to a
// degree, in tiles of one degree.

#include <cstddef>
#include <cstdint>

#include "miniverse/geo/grid.hpp"
#include "miniverse/geo/raster.hpp"

namespace test {

constexpr std::int16_t NODATA = -32768;

constexpr miniverse::geo::Grid<std::int16_t> GRID{
    .pixels_per_degree = 4, .tile_pixels = 4, .nodata = NODATA};

/** @return `width` by `height` pixels of the grid from (`west`, `north`), each `column * 100 +
 * row`. */
[[nodiscard]] inline miniverse::geo::Raster<std::int16_t> numbered(
    double west, double north, std::size_t width, std::size_t height)
{
  miniverse::geo::Raster<std::int16_t> raster{.west         = west,
                                              .north        = north,
                                              .pixel_width  = 0.25,
                                              .pixel_height = 0.25,
                                              .width        = width,
                                              .height       = height,
                                              .nodata       = NODATA,
                                              .pixels       = {}};

  for ( std::size_t row = 0; row < height; ++row ) {
    for ( std::size_t column = 0; column < width; ++column ) {
      raster.pixels.push_back(static_cast<std::int16_t>((column * 100) + row));
    }
  }

  return raster;
}

}  // namespace test
