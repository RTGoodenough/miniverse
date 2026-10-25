#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "miniverse/geo/concepts/raster.hpp"
#include "miniverse/geo/grid.hpp"
#include "miniverse/geo/raster.hpp"
#include "miniverse/geo/types.hpp"

/**
 * @file
 * Pixels moved between a raster and the tiles of a grid, in either direction: what a push and a load do to pixels, with no
 * table in it, whether the tiles are a table's or a file's.
 */
namespace miniverse::raster {

/** @brief A tile cut from a raster: which tile of the grid it is, and its pixels. */
template <geo::Pixel pixel_t>
struct Tile {
  /**
   * @brief Which tile of the grid: `tile_row * tiles_across + tile_column`, tiles counted east from longitude -180 and south
   * from latitude 90, `tiles_across` being `geo::Grid::tiles_across`. A raster table's key (`raster::TileId`).
   */
  std::int64_t         id = 0;
  geo::Raster<pixel_t> pixels;
};

/**
 * @return `raster` cut into the tiles of `grid` it covers, each a whole tile with `grid`'s nodata where the raster has none,
 * in ascending order of id. Its pixels equal to its own nodata become the grid's nodata, and a tile with no data at all is
 * left out.
 * @throws std::invalid_argument if `grid` is not a grid (`geo::check_grid`), or `raster` is not on it (another pixel size, or
 * not aligned to its pixels), or reaches past the world, or its pixels are not `width * height`, or (when its nodata is not
 * the grid's) a pixel with data has the grid's nodata value.
 */
template <geo::Pixel pixel_t>
[[nodiscard]] std::vector<Tile<pixel_t>> cut(const geo::Raster<pixel_t>& raster, const geo::Grid<pixel_t>& grid);

/**
  * @return The pixels of `tiles`, rasters of one grid, in the bounding box of `location` widened to whole pixels. Pixels no tile
 * covers are nodata. With no tiles, or a location of no points, the result is empty (0 by 0). It takes the tiles it is given;
 * which tiles a location has is `Reach`'s to say.
 * @throws std::invalid_argument if the tiles don't share a pixel size, an alignment and a nodata value, or a coordinate of
 * `location` is not a finite number.
 */
template <geo::Pixel pixel_t>
[[nodiscard]] geo::Raster<pixel_t> window(std::span<const geo::Raster<pixel_t>> tiles, const geo::Polygon& location);

/** @brief As above, of a vector of tiles: what a load has them in. */
template <geo::Pixel pixel_t>
[[nodiscard]] geo::Raster<pixel_t> window(const std::vector<geo::Raster<pixel_t>>& tiles, const geo::Polygon& location) {
  return window<pixel_t>(std::span<const geo::Raster<pixel_t>>(tiles), location);
}

// Each is defined for each pixel type a raster can hold: std::int8_t, std::uint8_t, std::int16_t, std::uint16_t, std::int32_t,
// std::uint32_t, float and double.

}  // namespace miniverse::raster
