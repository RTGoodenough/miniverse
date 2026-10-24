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
   * Which tile of the grid: `tile_row * tiles_across + tile_column`, tiles counted east from longitude -180 (`tile_column`)
   * and south from latitude 90 (`tile_row`), and `tiles_across` how many tiles span the world's 360 degrees
   * (`geo::Grid::tiles_across`). A raster table's key (`raster::TileId`).
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
 * @return The pixels of `tiles`, rasters of one grid, in the bounding box of `location`, widened to whole pixels of that
 * grid. Pixels no tile covers are the tiles' nodata. With no tiles at all, there is no grid to place pixels on: the result
 * is empty (0 by 0). What a load is, whether its tiles come from a table or from a reader of a file. It takes the tiles
 * it is given: which tiles a location has is `Reach`'s to say.
 * @throws std::invalid_argument if the tiles don't share a grid (pixel size, alignment) and a nodata value.
 */
template <geo::Pixel pixel_t>
[[nodiscard]] geo::Raster<pixel_t> window(std::span<const geo::Raster<pixel_t>> tiles, const geo::Polygon& location);

/** @brief As above, of a vector of tiles: what a load has them in. */
template <geo::Pixel pixel_t>
[[nodiscard]] geo::Raster<pixel_t> window(const std::vector<geo::Raster<pixel_t>>& tiles, const geo::Polygon& location) {
  return window<pixel_t>(std::span<const geo::Raster<pixel_t>>(tiles), location);
}

extern template std::vector<Tile<std::int8_t>>   cut(const geo::Raster<std::int8_t>&, const geo::Grid<std::int8_t>&);
extern template std::vector<Tile<std::uint8_t>>  cut(const geo::Raster<std::uint8_t>&, const geo::Grid<std::uint8_t>&);
extern template std::vector<Tile<std::int16_t>>  cut(const geo::Raster<std::int16_t>&, const geo::Grid<std::int16_t>&);
extern template std::vector<Tile<std::uint16_t>> cut(const geo::Raster<std::uint16_t>&, const geo::Grid<std::uint16_t>&);
extern template std::vector<Tile<std::int32_t>>  cut(const geo::Raster<std::int32_t>&, const geo::Grid<std::int32_t>&);
extern template std::vector<Tile<std::uint32_t>> cut(const geo::Raster<std::uint32_t>&, const geo::Grid<std::uint32_t>&);
extern template std::vector<Tile<float>>         cut(const geo::Raster<float>&, const geo::Grid<float>&);
extern template std::vector<Tile<double>>        cut(const geo::Raster<double>&, const geo::Grid<double>&);

extern template geo::Raster<std::int8_t>   window(std::span<const geo::Raster<std::int8_t>>, const geo::Polygon&);
extern template geo::Raster<std::uint8_t>  window(std::span<const geo::Raster<std::uint8_t>>, const geo::Polygon&);
extern template geo::Raster<std::int16_t>  window(std::span<const geo::Raster<std::int16_t>>, const geo::Polygon&);
extern template geo::Raster<std::uint16_t> window(std::span<const geo::Raster<std::uint16_t>>, const geo::Polygon&);
extern template geo::Raster<std::int32_t>  window(std::span<const geo::Raster<std::int32_t>>, const geo::Polygon&);
extern template geo::Raster<std::uint32_t> window(std::span<const geo::Raster<std::uint32_t>>, const geo::Polygon&);
extern template geo::Raster<float>         window(std::span<const geo::Raster<float>>, const geo::Polygon&);
extern template geo::Raster<double>        window(std::span<const geo::Raster<double>>, const geo::Polygon&);

}  // namespace miniverse::raster
