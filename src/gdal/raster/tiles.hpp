#pragma once

#include <boost/geometry/algorithms/envelope.hpp>  // IWYU pragma: keep
#include <boost/geometry/geometries/box.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "miniverse/geo/concepts/raster.hpp"
#include "miniverse/geo/grid.hpp"
#include "miniverse/geo/raster.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/layer/raster/reach.hpp"

/**
 * @file
 * The tiles of a grid a file read touches: which tiles an extent or a location's box meets, the window of the grid a run
 * of them is, and one tile cut from such a window. Whole numbers of tiles; no GDAL in it. Not installed.
 */
namespace miniverse::gdal::detail {

/** @brief Tiles of the grid, counted east from longitude -180 and south from latitude 90: `first` to before `end`, each way. */
struct Tiles {
  std::int64_t first_column = 0;
  std::int64_t end_column = 0;
  std::int64_t first_row = 0;
  std::int64_t end_row = 0;
};

/**
 * @brief How far past a tile's edge an extent may reach, in tiles, and still count as ending on it: transforming the corner of
 * a file that ends on a tile's edge lands a rounding step to one side of it.
 */
inline constexpr double EDGE_TOLERANCE = 1e-9;

/** @return The tiles of `grid` that `extent`, a file's in WGS 84, touches, within the world. */
template <geo::Pixel pixel_t>
[[nodiscard]] Tiles tiles_of(const geo::Box& extent, const geo::Grid<pixel_t>& grid) {
  const auto per_degree = static_cast<double>(grid.pixels_per_degree) / static_cast<double>(grid.tile_pixels);  // tiles
  const auto tiles_across = static_cast<double>(grid.tiles_across());
  const auto tiles_down = static_cast<double>(grid.tiles_down());

  // Kept within the world's tiles while still a double: only then is it sure to fit a whole number.
  const auto column = [&](double tiles) { return static_cast<std::int64_t>(std::clamp(tiles, 0.0, tiles_across)); };
  const auto row = [&](double tiles) { return static_cast<std::int64_t>(std::clamp(tiles, 0.0, tiles_down)); };

  return {
      .first_column = column(std::floor(((extent.min_corner().x() - geo::GRID_WEST) * per_degree) + EDGE_TOLERANCE)),
      .end_column = column(std::ceil(((extent.max_corner().x() - geo::GRID_WEST) * per_degree) - EDGE_TOLERANCE)),
      .first_row = row(std::floor(((geo::GRID_NORTH - extent.max_corner().y()) * per_degree) + EDGE_TOLERANCE)),
      .end_row = row(std::ceil(((geo::GRID_NORTH - extent.min_corner().y()) * per_degree) - EDGE_TOLERANCE)),
  };
}

/**
 * @return The tiles of `grid` whose outlines the box of `location` meets, those it only touches too, within the world: the
 * tiles a location can reach into, of which `raster::Reach` says which it does. None, for a location of no points.
 */
template <geo::Pixel pixel_t>
[[nodiscard]] Tiles tiles_meeting(const geo::Polygon& location, const geo::Grid<pixel_t>& grid) {
  // Boost declares return_envelope in a detail header; algorithms/envelope.hpp is the public one.
  const auto box = boost::geometry::return_envelope<boost::geometry::model::box<geo::Point>>(location);  // NOLINT(misc-include-cleaner)
  if ( box.min_corner().x() > box.max_corner().x() ) {
    return {};
  }

  if ( ! std::isfinite(box.min_corner().x()) || ! std::isfinite(box.min_corner().y()) || ! std::isfinite(box.max_corner().x()) ||
       ! std::isfinite(box.max_corner().y()) ) {
    throw std::invalid_argument("the coordinates of a location must be finite numbers");
  }

  const auto per_degree = static_cast<double>(grid.pixels_per_degree) / static_cast<double>(grid.tile_pixels);  // tiles
  const auto tiles_across = static_cast<double>(grid.tiles_across());
  const auto tiles_down = static_cast<double>(grid.tiles_down());

  // A box that ends on a tile's edge meets the tile beyond it: the tolerance leans that way, where `tiles_of` leans the other.
  const auto column = [&](double tiles) { return static_cast<std::int64_t>(std::clamp(tiles, 0.0, tiles_across)); };
  const auto row = [&](double tiles) { return static_cast<std::int64_t>(std::clamp(tiles, 0.0, tiles_down)); };

  return {
      .first_column = column(std::floor(((box.min_corner().x() - geo::GRID_WEST) * per_degree) - EDGE_TOLERANCE)),
      .end_column = column(std::floor(((box.max_corner().x() - geo::GRID_WEST) * per_degree) + EDGE_TOLERANCE) + 1),
      .first_row = row(std::floor(((geo::GRID_NORTH - box.max_corner().y()) * per_degree) - EDGE_TOLERANCE)),
      .end_row = row(std::floor(((geo::GRID_NORTH - box.min_corner().y()) * per_degree) + EDGE_TOLERANCE) + 1),
  };
}

/** @return The tiles that are both in `one` and in `other`: none, if `first` is not before `end` each way. */
[[nodiscard]] inline Tiles in_both(const Tiles& one, const Tiles& other) {
  return {
      .first_column = std::max(one.first_column, other.first_column),
      .end_column = std::min(one.end_column, other.end_column),
      .first_row = std::max(one.first_row, other.first_row),
      .end_row = std::min(one.end_row, other.end_row),
  };
}

/** @return The outline of the tile of `grid` numbered `column` across and `row` down. */
template <geo::Pixel pixel_t>
[[nodiscard]] geo::Box outline_of(std::int64_t column, std::int64_t row, const geo::Grid<pixel_t>& grid) {
  const auto tile = static_cast<std::int64_t>(grid.tile_pixels);
  const auto ppd = static_cast<double>(grid.pixels_per_degree);
  const auto degrees = [&](std::int64_t tiles) { return static_cast<double>(tiles * tile) / ppd; };

  return {
      {geo::GRID_WEST + degrees(column), geo::GRID_NORTH - degrees(row + 1)}, {geo::GRID_WEST + degrees(column + 1), geo::GRID_NORTH - degrees(row)}
  };
}

/** @return The columns of the tiles of `run`, some of one row of `grid`, that `reach`'s location reaches into, from the west. */
template <geo::Pixel pixel_t>
[[nodiscard]] std::vector<std::int64_t> reached_of(const raster::Reach& reach, const Tiles& run, const geo::Grid<pixel_t>& grid) {
  std::vector<std::int64_t> reached;
  for ( std::int64_t column = run.first_column; column < run.end_column; ++column ) {
    if ( reach.into(outline_of(column, run.first_row, grid)) ) {
      reached.push_back(column);
    }
  }

  return reached;
}

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
