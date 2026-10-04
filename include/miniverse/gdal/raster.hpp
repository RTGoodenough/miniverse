#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "miniverse/geo/concepts/raster.hpp"
#include "miniverse/geo/raster.hpp"

/**
 * Raster files as rasters on a table's grid, read with GDAL: a GeoTIFF or anything else GDAL opens, warped onto the grid of
 * a `RasterLayer`'s table, for pushing into it.
 *
 * @code
 * struct Elevation : miniverse::RasterLayer<std::int16_t> {};
 *
 * const auto grid = world.table_settings<Elevation>().get();  // the grid the table was made with
 * miniverse::gdal::read_raster<std::int16_t>({.path = "srtm.tif"}, grid, 4, [&](auto window) {
 *   world.push<Elevation>(std::move(window)).get();  // 4 by 4 tiles at a time: the file is never held whole
 * });
 * @endcode
 *
 * - **The grid** is the table's: its pixel size, its tiles, its nodata. The file is warped onto it from its own coordinate
 *   system and pixel size (it may be rotated), each grid pixel made from the file's own pixels by `resampling`, exactly: a
 *   pixel is the same whichever window it is read in. A file that names no coordinate system, crosses the antimeridian, or
 *   reaches more than a degree past the world (longitudes of 0 to 360) is refused; within that degree it is read up to the
 *   world's edge.
 * - **Windows** are whole tiles of the grid, `window_tiles` by `window_tiles` of them (the last of a row or column what is
 *   left), over the file's extent in WGS 84, from the north west, row by row. A tile is so never written by two windows, which
 *   a push would have to merge. Only at the world's far edge, where a grid's tiles need not divide it, is a window cut short.
 * - **No data**: a grid pixel whose centre the file does not cover, or where it has its own nodata value (or, in a file of
 *   reals, "not a number"), is the grid's nodata. A window with no data at all is left out and counted. A file's pixel that
 *   has the grid's nodata value as its data is lost: choose a nodata for the table that its data does not use.
 * - **Memory**: a window is held twice while it is made, so `window_tiles` squared tiles, twice. A file near a pole spans
 *   every longitude once in WGS 84, however small it is.
 *
 * Part of the optional component `miniverse::gdal`, as vector.hpp is. Errors are `std::runtime_error` with GDAL's own message.
 * While a read runs, GDAL's errors on the calling thread are kept for those messages, not printed: also the ones GDAL calls
 * made inside `on_window` raise.
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
  std::string path;                                ///< The file, or anything else GDAL opens.
  int         band = 1;                            ///< Which of its bands, counted from 1.
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
 * @throws std::invalid_argument if `window_tiles` is 0, or `grid` has no pixels per degree or no tile size.
 * @throws std::runtime_error if the file can't be opened or warped, has no such band, or names no coordinate system or
 * position. Windows handed over before an error stay handed over. What `on_window` throws is passed on.
 */
template <geo::Pixel pixel_t>
RasterRead read_raster(
    const RasterSource& source, const geo::Grid<pixel_t>& grid, std::size_t window_tiles, const std::function<void(geo::Raster<pixel_t>)>& on_window
);

/**
 * @return All of `source` on `grid`, as one window: for a file whose extent in WGS 84, rounded out to whole tiles, is small
 * enough to hold twice. 0 by 0 pixels if it has no data.
 */
template <geo::Pixel pixel_t>
[[nodiscard]] geo::Raster<pixel_t> read_raster(const RasterSource& source, const geo::Grid<pixel_t>& grid);

// Defined for each pixel type a raster can hold: std::int8_t, std::uint8_t, std::int16_t, std::uint16_t, std::int32_t,
// std::uint32_t, float and double.

}  // namespace miniverse::gdal
