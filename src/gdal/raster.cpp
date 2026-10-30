#include "miniverse/gdal/raster.hpp"

#include <cpl_error.h>
#include <gdal_priv.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "gdal/common.hpp"
#include "gdal/raster/source.hpp"
#include "gdal/raster/warp.hpp"
#include "gdal/raster/window.hpp"
#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/geo/concepts/raster.hpp"
#include "miniverse/geo/grid.hpp"
#include "miniverse/geo/raster.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/model/raster/reach.hpp"

namespace miniverse::gdal {

namespace {

using detail::extent_of;
using detail::opened;
using detail::QuietErrors;
using detail::reached_of;
using detail::Scale;
using detail::scale_of;
using detail::tile_of;
using detail::warp_into;
using detail::window_of;

template <geo::Pixel pixel_t>
[[nodiscard]] bool has_no_data(const geo::Raster<pixel_t>& raster)
{
  return std::ranges::all_of(raster.pixels,
                             [&raster](pixel_t pixel) { return pixel == raster.nodata; });
}

}  // namespace

template <geo::Pixel pixel_t>
RasterRead read_raster(const RasterSource&                              source,
                       const geo::Grid<pixel_t>&                        grid,
                       std::size_t                                      window_tiles,
                       const std::function<void(geo::Raster<pixel_t>)>& on_window)
{
  if ( window_tiles == 0 ) {
    throw std::invalid_argument("a window is at least one tile each way");
  }

  geo::check_grid(grid);

  const QuietErrors          quiet;
  const GDALDatasetUniquePtr dataset = opened(source);

  const geo::Box       extent = extent_of(*dataset, source);
  const geo::TileRange tiles  = grid.tiles_covering(extent);
  const Scale          scale  = scale_of(*dataset, extent, grid.pixels_per_degree);

  // Capped so that a window's pixels fit GDAL's int: a whole file as one window asks for the most
  // there is.
  const auto step = static_cast<std::int64_t>(std::min<std::size_t>(
      window_tiles, static_cast<std::size_t>(std::numeric_limits<int>::max() / grid.tile_pixels)));

  RasterRead read;
  for ( std::int64_t row = tiles.first_row; row < tiles.end_row; row += step ) {
    for ( std::int64_t column = tiles.first_column; column < tiles.end_column; column += step ) {
      geo::Raster<pixel_t> window =
          window_of<pixel_t>({.first_column = column,
                              .end_column   = std::min(column + step, tiles.end_column),
                              .first_row    = row,
                              .end_row      = std::min(row + step, tiles.end_row)},
                             grid);
      warp_into(window, *dataset, source, extent, grid.pixels_per_degree, scale);

      if ( has_no_data(window) ) {
        ++read.without_data;
        continue;
      }

      on_window(std::move(window));
      CPLErrorReset();  // drop any error `on_window`'s own GDAL calls left
      ++read.windows;
    }
  }

  return read;
}

template <geo::Pixel pixel_t>
geo::Raster<pixel_t> read_raster(const RasterSource& source, const geo::Grid<pixel_t>& grid)
{
  geo::Raster<pixel_t> whole;

  std::ignore =
      read_raster<pixel_t>(source, grid, ONE_CHUNK,
                           [&whole](geo::Raster<pixel_t> window) { whole = std::move(window); });

  return whole;
}

template <geo::Pixel pixel_t>
void read_tiles_in(const RasterSource&                                           source,
                   const geo::Grid<pixel_t>&                                     grid,
                   const geo::Polygon&                                           location,
                   std::size_t                                                   chunk_tiles,
                   const std::function<bool(std::vector<geo::Raster<pixel_t>>)>& on_chunk)
{
  if ( chunk_tiles == 0 ) {
    throw std::invalid_argument("a chunk of tiles holds at least one");
  }

  geo::check_grid(grid);

  const QuietErrors          quiet;
  const GDALDatasetUniquePtr dataset = opened(source);

  const geo::Box                extent = extent_of(*dataset, source);
  const std::optional<geo::Box> box    = geo::box_of(location);
  const geo::TileRange          tiles =
      box ? grid.tiles_meeting(*box).meet(grid.tiles_covering(extent)) : geo::TileRange{};
  const Scale scale = scale_of(*dataset, extent, grid.pixels_per_degree);

  const raster::Reach reach(location);

  // A run of one row's tiles is warped at once, from the first tile the location reaches into to
  // the last, then cut into tiles; the tiles between that it does not reach are dropped. A run
  // holds as many tiles as a chunk, up to `RUN_PIXELS` wide.
  constexpr std::int64_t RUN_PIXELS = 4096;
  const std::int64_t     widest     = std::max<std::int64_t>(1, RUN_PIXELS / grid.tile_pixels);
  const auto             run        = static_cast<std::int64_t>(
      std::min<std::size_t>(chunk_tiles, static_cast<std::size_t>(widest)));
  std::vector<geo::Raster<pixel_t>> chunk;
  for ( std::int64_t row = tiles.first_row; row < tiles.end_row; ++row ) {
    for ( std::int64_t first = tiles.first_column; first < tiles.end_column; first += run ) {
      const std::vector<std::int64_t> reached =
          reached_of(reach, {first, std::min(first + run, tiles.end_column), row, row + 1}, grid);
      if ( reached.empty() ) {
        continue;
      }

      geo::Raster<pixel_t> window = window_of<pixel_t>({.first_column = reached.front(),
                                                        .end_column   = reached.back() + 1,
                                                        .first_row    = row,
                                                        .end_row      = row + 1},
                                                       grid);
      warp_into(window, *dataset, source, extent, grid.pixels_per_degree, scale);

      for ( const std::int64_t column : reached ) {
        geo::Raster<pixel_t> tile = tile_of(window, column, reached.front(), grid);
        if ( has_no_data(tile) ) {
          continue;
        }

        chunk.push_back(std::move(tile));
        if ( chunk.size() == chunk_tiles ) {
          if ( ! on_chunk(std::exchange(chunk, {})) ) {
            return;
          }

          CPLErrorReset();  // drop any error `on_chunk`'s own GDAL calls left
        }
      }
    }
  }

  if ( ! chunk.empty() ) {
    std::ignore = on_chunk(std::move(chunk));
  }
}

template <geo::Pixel pixel_t>
std::vector<std::string> problems_of(const RasterSource& source, const geo::Grid<pixel_t>& grid)
{
  try {
    geo::check_grid(grid);

    const QuietErrors          quiet;
    const GDALDatasetUniquePtr dataset = opened(source);
    std::ignore                        = extent_of(*dataset, source);

    return {};
  } catch ( const std::exception& unreadable ) {
    return {unreadable.what()};
  }
}

template RasterRead read_raster(const RasterSource&,
                                const geo::Grid<std::int8_t>&,
                                std::size_t,
                                const std::function<void(geo::Raster<std::int8_t>)>&);
template RasterRead read_raster(const RasterSource&,
                                const geo::Grid<std::uint8_t>&,
                                std::size_t,
                                const std::function<void(geo::Raster<std::uint8_t>)>&);
template RasterRead read_raster(const RasterSource&,
                                const geo::Grid<std::int16_t>&,
                                std::size_t,
                                const std::function<void(geo::Raster<std::int16_t>)>&);
template RasterRead read_raster(const RasterSource&,
                                const geo::Grid<std::uint16_t>&,
                                std::size_t,
                                const std::function<void(geo::Raster<std::uint16_t>)>&);
template RasterRead read_raster(const RasterSource&,
                                const geo::Grid<std::int32_t>&,
                                std::size_t,
                                const std::function<void(geo::Raster<std::int32_t>)>&);
template RasterRead read_raster(const RasterSource&,
                                const geo::Grid<std::uint32_t>&,
                                std::size_t,
                                const std::function<void(geo::Raster<std::uint32_t>)>&);
template RasterRead read_raster(const RasterSource&,
                                const geo::Grid<float>&,
                                std::size_t,
                                const std::function<void(geo::Raster<float>)>&);
template RasterRead read_raster(const RasterSource&,
                                const geo::Grid<double>&,
                                std::size_t,
                                const std::function<void(geo::Raster<double>)>&);

template geo::Raster<std::int8_t>  read_raster(const RasterSource&, const geo::Grid<std::int8_t>&);
template geo::Raster<std::uint8_t> read_raster(const RasterSource&, const geo::Grid<std::uint8_t>&);
template geo::Raster<std::int16_t> read_raster(const RasterSource&, const geo::Grid<std::int16_t>&);
template geo::Raster<std::uint16_t> read_raster(const RasterSource&,
                                                const geo::Grid<std::uint16_t>&);
template geo::Raster<std::int32_t> read_raster(const RasterSource&, const geo::Grid<std::int32_t>&);
template geo::Raster<std::uint32_t> read_raster(const RasterSource&,
                                                const geo::Grid<std::uint32_t>&);
template geo::Raster<float>         read_raster(const RasterSource&, const geo::Grid<float>&);
template geo::Raster<double>        read_raster(const RasterSource&, const geo::Grid<double>&);

template void read_tiles_in(const RasterSource&,
                            const geo::Grid<std::int8_t>&,
                            const geo::Polygon&,
                            std::size_t,
                            const std::function<bool(std::vector<geo::Raster<std::int8_t>>)>&);
template void read_tiles_in(const RasterSource&,
                            const geo::Grid<std::uint8_t>&,
                            const geo::Polygon&,
                            std::size_t,
                            const std::function<bool(std::vector<geo::Raster<std::uint8_t>>)>&);
template void read_tiles_in(const RasterSource&,
                            const geo::Grid<std::int16_t>&,
                            const geo::Polygon&,
                            std::size_t,
                            const std::function<bool(std::vector<geo::Raster<std::int16_t>>)>&);
template void read_tiles_in(const RasterSource&,
                            const geo::Grid<std::uint16_t>&,
                            const geo::Polygon&,
                            std::size_t,
                            const std::function<bool(std::vector<geo::Raster<std::uint16_t>>)>&);
template void read_tiles_in(const RasterSource&,
                            const geo::Grid<std::int32_t>&,
                            const geo::Polygon&,
                            std::size_t,
                            const std::function<bool(std::vector<geo::Raster<std::int32_t>>)>&);
template void read_tiles_in(const RasterSource&,
                            const geo::Grid<std::uint32_t>&,
                            const geo::Polygon&,
                            std::size_t,
                            const std::function<bool(std::vector<geo::Raster<std::uint32_t>>)>&);
template void read_tiles_in(const RasterSource&,
                            const geo::Grid<float>&,
                            const geo::Polygon&,
                            std::size_t,
                            const std::function<bool(std::vector<geo::Raster<float>>)>&);
template void read_tiles_in(const RasterSource&,
                            const geo::Grid<double>&,
                            const geo::Polygon&,
                            std::size_t,
                            const std::function<bool(std::vector<geo::Raster<double>>)>&);

template std::vector<std::string> problems_of(const RasterSource&, const geo::Grid<std::int8_t>&);
template std::vector<std::string> problems_of(const RasterSource&, const geo::Grid<std::uint8_t>&);
template std::vector<std::string> problems_of(const RasterSource&, const geo::Grid<std::int16_t>&);
template std::vector<std::string> problems_of(const RasterSource&, const geo::Grid<std::uint16_t>&);
template std::vector<std::string> problems_of(const RasterSource&, const geo::Grid<std::int32_t>&);
template std::vector<std::string> problems_of(const RasterSource&, const geo::Grid<std::uint32_t>&);
template std::vector<std::string> problems_of(const RasterSource&, const geo::Grid<float>&);
template std::vector<std::string> problems_of(const RasterSource&, const geo::Grid<double>&);

}  // namespace miniverse::gdal
