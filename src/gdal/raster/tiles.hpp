#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "miniverse/geo/concepts/raster.hpp"
#include "miniverse/geo/grid.hpp"
#include "miniverse/geo/raster.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/layer/raster/reach.hpp"

/**
 * @file
 * The tiles of a grid a file read touches: which tiles an extent or a location's box meets, the window of the grid a run
 * of them is, and one tile cut from such a window. Whole numbers of tiles; no GDAL in it, and no pixel type but where
 * pixels are made (`window_of`, `tile_of`): the rest is compiled once, for a `Tiling`. Not installed.
 */
namespace miniverse::gdal::detail {

/** @brief Tiles of the grid, counted east from longitude -180 and south from latitude 90: `first` to before `end`, each way. */
struct Tiles {
  std::int64_t first_column = 0;
  std::int64_t end_column = 0;
  std::int64_t first_row = 0;
  std::int64_t end_row = 0;
};

/** @brief How a grid is tiled, which is the same whatever its pixel type: what the tile arithmetic needs of a `geo::Grid`. */
struct Tiling {
  std::int64_t pixels_per_degree = 0;
  std::int64_t tile_pixels = 0;
  std::int64_t tiles_across = 0;  ///< `geo::Grid::tiles_across()`.
  std::int64_t tiles_down = 0;    ///< `geo::Grid::tiles_down()`.
};

/** @return How `grid` is tiled. */
template <geo::Pixel pixel_t>
[[nodiscard]] Tiling tiling_of(const geo::Grid<pixel_t>& grid) {
  return {
      .pixels_per_degree = grid.pixels_per_degree,
      .tile_pixels = grid.tile_pixels,
      .tiles_across = grid.tiles_across(),
      .tiles_down = grid.tiles_down()
  };
}

/** @return The tiles of `tiling` that `extent`, a file's in WGS 84, touches, within the world. */
[[nodiscard]] Tiles tiles_of(const geo::Box& extent, const Tiling& tiling);

/**
 * @return The tiles of `tiling` whose outlines the box of `location` meets, those it only touches too, within the world: the
 * tiles a location can reach into, of which `raster::Reach` says which it does. None, for a location of no points.
 * @throws std::invalid_argument if a coordinate of the location is not a finite number.
 */
[[nodiscard]] Tiles tiles_meeting(const geo::Polygon& location, const Tiling& tiling);

/** @return The tiles that are both in `one` and in `other`: none, if `first` is not before `end` each way. */
[[nodiscard]] Tiles in_both(const Tiles& one, const Tiles& other);

/** @return The columns of the tiles of `run`, some of one row of `tiling`, that `reach`'s location reaches into, from the west. */
[[nodiscard]] std::vector<std::int64_t> reached_of(const raster::Reach& reach, const Tiles& run, const Tiling& tiling);

/** @return The window of `grid` that the tiles `tiles` are, cut at the world's edge, with no pixels yet. */
template <geo::Pixel pixel_t>
[[nodiscard]] geo::Raster<pixel_t> window_of(const Tiles& tiles, const geo::Grid<pixel_t>& grid) {
  const auto         ppd = static_cast<std::int64_t>(grid.pixels_per_degree);
  const auto         tile = static_cast<std::int64_t>(grid.tile_pixels);
  const std::int64_t first_column = tiles.first_column * tile;
  const std::int64_t first_row = tiles.first_row * tile;
  const std::int64_t end_column = std::min(tiles.end_column * tile, geo::DEGREES_ACROSS * ppd);  // the last tile may reach past the world
  const std::int64_t end_row = std::min(tiles.end_row * tile, geo::DEGREES_DOWN * ppd);

  return {
      .west = geo::GRID_WEST + (static_cast<double>(first_column) / static_cast<double>(ppd)),
      .north = geo::GRID_NORTH - (static_cast<double>(first_row) / static_cast<double>(ppd)),
      .pixel_width = 1.0 / static_cast<double>(ppd),
      .pixel_height = 1.0 / static_cast<double>(ppd),
      .width = static_cast<std::size_t>(end_column - first_column),
      .height = static_cast<std::size_t>(end_row - first_row),
      .nodata = grid.nodata,
      .pixels = {},
  };
}

/**
 * @return The tile of `grid` numbered `column` across, cut from `window`, a row of whole tiles that starts at the tile
 * `first_column`: a whole tile always, the grid's nodata where the window was cut short at the world's edge.
 */
template <geo::Pixel pixel_t>
[[nodiscard]] geo::Raster<pixel_t> tile_of(
    const geo::Raster<pixel_t>& window, std::int64_t column, std::int64_t first_column, const geo::Grid<pixel_t>& grid
) {
  const auto        tile = static_cast<std::size_t>(grid.tile_pixels);
  const std::size_t from = static_cast<std::size_t>(column - first_column) * tile;  // the tile's first pixel across, in the window

  // Its west edge from its place in the grid, as `window_of` and a table's tiles have theirs: the same number, to the last bit.
  geo::Raster<pixel_t> cut{
      .west =
          geo::GRID_WEST + (static_cast<double>(column * static_cast<std::int64_t>(grid.tile_pixels)) / static_cast<double>(grid.pixels_per_degree)),
      .north = window.north,
      .pixel_width = window.pixel_width,
      .pixel_height = window.pixel_height,
      .width = tile,
      .height = tile,
      .nodata = grid.nodata,
      .pixels = std::vector<pixel_t>(tile * tile, grid.nodata),
  };

  const std::size_t across = std::min(tile, window.width - from);
  for ( std::size_t row = 0; row < window.height; ++row ) {
    const auto first = window.pixels.begin() + static_cast<std::ptrdiff_t>((row * window.width) + from);
    std::copy(first, first + static_cast<std::ptrdiff_t>(across), cut.pixels.begin() + static_cast<std::ptrdiff_t>(row * tile));
  }

  return cut;
}

}  // namespace miniverse::gdal::detail
