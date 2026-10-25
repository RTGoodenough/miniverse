#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "miniverse/geo/concepts/raster.hpp"
#include "miniverse/geo/grid.hpp"
#include "miniverse/geo/raster.hpp"
#include "miniverse/geo/types.hpp"

/**
 * @file
 * Raster files as rasters on a table's grid, read with GDAL: a GeoTIFF or anything else GDAL opens, warped onto the grid of
 * a `RasterLayer`'s table, for pushing into it or reading in its place (raster_file.hpp).
 *
 * @code
 * struct Elevation : miniverse::RasterLayer<std::int16_t> {};
 *
 * const auto grid = world.table_settings<Elevation>().get();  // the grid the table was made with
 * miniverse::gdal::read_raster<std::int16_t>({.path = "srtm.tif"}, grid, 4, [&](auto window) {
 *   world.push<Elevation>(std::move(window)).get();  // 4 by 4 tiles at a time, so the file is not held whole
 * });
 * @endcode
 *
 * - **The grid** is the table's: its pixel size, its tiles, its nodata. The file is warped onto it from its own coordinate
 *   system, pixel size and turn (a geotransform with rotation), each grid pixel made from the file's by `resampling`, with
 *   one scale for the whole file, so a pixel comes out the same whichever window it is read in. A file that names no
 *   coordinate system, crosses the antimeridian, or reaches more than a degree past the world (longitudes of 0 to 360) is
 *   refused; within that degree it is read to the edge.
 * - **Windows** are whole tiles of the grid, `window_tiles` by `window_tiles` (the last of a row or column, what is left),
 *   over the file's extent, from the north west, row by row: each tile is written by one window. At the world's far edge,
 *   where the grid's tiles need not divide it, a window is cut short.
 * - **No data**: a grid pixel whose centre the file does not cover, or where it has its own nodata value, is the grid's
 *   nodata. In a file of reals that names no nodata value, "not a number" is its nodata. One that names a number and holds
 *   "not a number" as well makes the grid's nodata of every pixel made from a "not a number" on a grid of reals, but a value
 *   of no meaning on a grid of whole numbers (0 for most types). A window with no data at all is left out and counted. A
 *   pixel whose data is the grid's nodata value is lost: choose a nodata the data does not use.
 * - **Memory**: a window is held twice while it is made. A file near a pole spans every longitude in WGS 84, however small.
 *
 * A file can also be read in place of a table (`raster_file`, raster_file.hpp).
 *
 * Part of the optional component `miniverse::gdal` (gdal.hpp includes all of it). Errors are `std::runtime_error` with GDAL's
 * own message; GDAL's errors on the calling thread are kept quiet while a read runs, including from GDAL calls made inside
 * `on_window`.
 */
namespace miniverse::gdal {

/** @brief How a pixel of the grid is made from the file's pixels around it. */
enum class Resampling : std::uint8_t {
  Nearest,   ///< The nearest pixel's value, unchanged: for classes, and for a file that is on the grid already.
  Bilinear,  ///< A weighted mean of the four nearest: for heights and other measures, on a grid as fine as the file or finer.
  Cubic,     ///< A smoother curve through the sixteen nearest.
  Average,   ///< The mean of the file's pixels that a grid pixel covers: for a grid coarser than the file.
};

/** @brief Which raster to read, and how. */
struct RasterSource {
  std::string path;                               ///< The file, or anything else GDAL opens.
  int         band = 1;                           ///< Which of its bands, counted from 1.
  Resampling  resampling = Resampling::Bilinear;  ///< How grid pixels are made from the file's.
};

/** @brief How a read went. */
struct RasterRead {
  std::size_t windows = 0;       ///< Handed over.
  std::size_t without_data = 0;  ///< Left out: windows of the file's extent where it has no data.
};

/**
 * @brief Reads `source` onto `grid` and hands it to `on_window`, `window_tiles` by `window_tiles` tiles at a time, on the
 * calling thread.
 * @throws std::invalid_argument if `window_tiles` is 0, or `grid` is not a grid.
 * @throws std::runtime_error if the file can't be opened or warped, has no such band, or names no coordinate system or
 * position. Windows handed over before the error stay handed over. What `on_window` throws is passed on.
 */
template <geo::Pixel pixel_t>
RasterRead read_raster(
    const RasterSource& source, const geo::Grid<pixel_t>& grid, std::size_t window_tiles, const std::function<void(geo::Raster<pixel_t>)>& on_window
);

/** @return All of `source` on `grid` as one window, for a file small enough to hold twice; 0 by 0 pixels if it has no data. */
template <geo::Pixel pixel_t>
[[nodiscard]] geo::Raster<pixel_t> read_raster(const RasterSource& source, const geo::Grid<pixel_t>& grid);

/**
 * @brief Hands the tiles of `grid` that `location`, a polygon in WGS 84, reaches into (`raster::Reach`) to `on_chunk`,
 * `chunk_tiles` at a time, from the north west row by row, on the calling thread, until it returns `false`. Each tile is whole,
 * its pixels as `read_raster` makes them. Tiles the location does not reach are not handed over, and a tile with no data is
 * left out, as a table has no row for it: the result is what a table the file was pushed into holds for the location.
 * @throws std::invalid_argument if `chunk_tiles` is 0, a coordinate of `location` is not a finite number, or as `read_raster`;
 * std::runtime_error as `read_raster`.
 */
template <geo::Pixel pixel_t>
void read_tiles_in(
    const RasterSource& source, const geo::Grid<pixel_t>& grid, const geo::Polygon& location, std::size_t chunk_tiles,
    const std::function<bool(std::vector<geo::Raster<pixel_t>>)>& on_chunk
);

/**
 * @return What keeps `source` from being read onto `grid`, each in words: a file that can't be opened, a band it lacks, no coordinate system or
 * position, a grid that is none.
 */
template <geo::Pixel pixel_t>
[[nodiscard]] std::vector<std::string> problems_of(const RasterSource& source, const geo::Grid<pixel_t>& grid);

// Each of the above is defined for each pixel type a raster can hold: std::int8_t, std::uint8_t, std::int16_t,
// std::uint16_t, std::int32_t, std::uint32_t, float and double.

}  // namespace miniverse::gdal
