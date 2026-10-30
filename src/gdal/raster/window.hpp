#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "miniverse/geo/concepts/raster.hpp"
#include "miniverse/geo/grid.hpp"
#include "miniverse/geo/raster.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/model/raster/reach.hpp"

/**
 * @file
 * The pixels of a run of tiles read from a file: which tiles of a row's run a location reaches
 * into, the window of the grid a run is, with no pixels yet, and one tile cut from such a window.
 * Which tiles a file or a location has is `geo::Grid`'s to say. Not installed.
 */
namespace miniverse::gdal::detail {

/** @return The columns of the tiles of `run`, some of one row of `grid`, that `reach`'s location
 * reaches into, from the west. */
template <geo::Pixel pixel_t>
[[nodiscard]] std::vector<std::int64_t> reached_of(
    const raster::Reach& reach, const geo::TileRange& run, const geo::Grid<pixel_t>& grid)
{
  std::vector<std::int64_t> reached;
  for ( std::int64_t column = run.first_column; column < run.end_column; ++column ) {
    if ( reach.into(grid.tile_outline(column, run.first_row)) ) {
      reached.push_back(column);
    }
  }

  return reached;
}

/** @return The window of `grid` that the tiles `tiles` are, cut at the world's edge, with no pixels
 * yet. */
template <geo::Pixel pixel_t>
[[nodiscard]] geo::Raster<pixel_t> window_of(const geo::TileRange&     tiles,
                                             const geo::Grid<pixel_t>& grid)
{
  const auto         tile         = static_cast<std::int64_t>(grid.tile_pixels);
  const std::int64_t first_column = tiles.first_column * tile;
  const std::int64_t first_row    = tiles.first_row * tile;
  const std::int64_t end_column   = std::min(tiles.end_column * tile, grid.pixels_across());  // the
                                                                                            // last
                                                                                            // tile
                                                                                            // may
                                                                                            // reach
                                                                                            // past
                                                                                            // the
                                                                                            // world
  const std::int64_t end_row      = std::min(tiles.end_row * tile, grid.pixels_down());
  const geo::Point   corner       = grid.corner_of(first_column, first_row);

  return {
      .west         = corner.x(),
      .north        = corner.y(),
      .pixel_width  = grid.pixel_degrees(),
      .pixel_height = grid.pixel_degrees(),
      .width        = static_cast<std::size_t>(end_column - first_column),
      .height       = static_cast<std::size_t>(end_row - first_row),
      .nodata       = grid.nodata,
      .pixels       = {},
  };
}

/**
 * @return The tile of `grid` numbered `column` across, cut from `window`, a row of whole tiles that
 * starts at the tile `first_column`: a whole tile always, the grid's nodata where the window was
 * cut short at the world's edge.
 */
template <geo::Pixel pixel_t>
[[nodiscard]] geo::Raster<pixel_t> tile_of(const geo::Raster<pixel_t>& window,
                                           std::int64_t                column,
                                           std::int64_t                first_column,
                                           const geo::Grid<pixel_t>&   grid)
{
  const auto        tile = static_cast<std::size_t>(grid.tile_pixels);
  const std::size_t from = static_cast<std::size_t>(column - first_column) * tile;  // the tile's
                                                                                    // first pixel
                                                                                    // across, in
                                                                                    // the window

  geo::Raster<pixel_t> cut{
      .west         = grid.corner_of(column * grid.tile_pixels, 0).x(),
      .north        = window.north,
      .pixel_width  = window.pixel_width,
      .pixel_height = window.pixel_height,
      .width        = tile,
      .height       = tile,
      .nodata       = grid.nodata,
      .pixels       = std::vector<pixel_t>(tile * tile, grid.nodata),
  };

  const std::size_t across = std::min(tile, window.width - from);
  for ( std::size_t row = 0; row < window.height; ++row ) {
    const auto first =
        window.pixels.begin() + static_cast<std::ptrdiff_t>((row * window.width) + from);
    std::copy(first, first + static_cast<std::ptrdiff_t>(across),
              cut.pixels.begin() + static_cast<std::ptrdiff_t>(row * tile));
  }

  return cut;
}

}  // namespace miniverse::gdal::detail
