#include "miniverse/gdal/raster.hpp"

#include <cpl_error.h>
#include <gdal.h>
#include <gdal_priv.h>
#include <gdal_utils.h>
#include <ogr_core.h>
#include <ogr_spatialref.h>
#include <ogr_srs_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "common.hpp"
#include "miniverse/geo/concepts/raster.hpp"
#include "miniverse/geo/raster.hpp"
#include "miniverse/geo/types.hpp"

namespace miniverse::gdal {

namespace {

using detail::fail;
using detail::QuietErrors;
using detail::register_drivers;

// The grid's corner, and the world's size in degrees.
constexpr double WEST = geo::GRID_WEST;
constexpr double NORTH = geo::GRID_NORTH;
using geo::DEGREES_ACROSS;
using geo::DEGREES_DOWN;

// How far past the world's edge a file may reach, in degrees, and be read up to the edge: a global file whose pixels are
// centred on the edge overhangs it by half a pixel. One that reaches further (longitudes of 0 to 360, say) is refused.
constexpr double PAST_WORLD_ALLOWED = 1;

constexpr std::size_t GEOTRANSFORM_VALUES = 6;

// How far past a tile's edge an extent may reach, in tiles, and still count as ending on it: transforming the corner of a
// file that ends on a tile's edge lands a rounding step to one side of it.
constexpr double EDGE_TOLERANCE = 1e-9;

// How many points along each edge of the file's outline are transformed to find its extent in WGS 84: edges curve.
constexpr int EDGE_POINTS = 21;

/** @brief An extent in WGS 84 degrees. */
struct Extent {
  double west = 0;
  double south = 0;
  double east = 0;
  double north = 0;
};

/** @brief Tiles of the grid, counted east from longitude -180 and south from latitude 90: `first` to before `end`, each way. */
struct Tiles {
  std::int64_t first_column = 0;
  std::int64_t end_column = 0;
  std::int64_t first_row = 0;
  std::int64_t end_row = 0;
};

/** @return The GDAL type that is `pixel_t`. */
template <geo::Pixel pixel_t>
[[nodiscard]] constexpr GDALDataType gdal_type() {
  if constexpr ( std::same_as<pixel_t, std::int8_t> ) {
    return GDT_Int8;

  } else if constexpr ( std::same_as<pixel_t, std::uint8_t> ) {
    return GDT_Byte;

  } else if constexpr ( std::same_as<pixel_t, std::int16_t> ) {
    return GDT_Int16;

  } else if constexpr ( std::same_as<pixel_t, std::uint16_t> ) {
    return GDT_UInt16;

  } else if constexpr ( std::same_as<pixel_t, std::int32_t> ) {
    return GDT_Int32;

  } else if constexpr ( std::same_as<pixel_t, std::uint32_t> ) {
    return GDT_UInt32;

  } else if constexpr ( std::same_as<pixel_t, float> ) {
    return GDT_Float32;

  } else {
    static_assert(std::same_as<pixel_t, double>, "a new pixel type needs its GDAL type here");

    return GDT_Float64;
  }
}

/** @return The name gdalwarp knows `resampling` by. */
[[nodiscard]] constexpr std::string_view name_of(Resampling resampling) {
  switch ( resampling ) {
    case Resampling::Nearest:
      return "near";
    case Resampling::Bilinear:
      return "bilinear";
    case Resampling::Cubic:
      return "cubic";
    case Resampling::Average:
      return "average";
  }

  return "near";
}

[[nodiscard]] std::int64_t ceil_div(std::int64_t dividend, std::int64_t divisor) { return (dividend + divisor - 1) / divisor; }

/** @return The extent of `dataset` in WGS 84: its outline, transformed from its own coordinate system. */
[[nodiscard]] Extent extent_of(GDALDataset& dataset, const RasterSource& source) {
  std::array<double, GEOTRANSFORM_VALUES> position{};  // x = [0] + column * [1] + row * [2], y = [3] + column * [4] + row * [5]
  if ( dataset.GetGeoTransform(position.data()) != CE_None ) {
    throw std::runtime_error("'" + source.path + "' does not say where it lies (it has no geotransform)");
  }

  const double origin_x = position.at(0);
  const double column_x = position.at(1);
  const double row_x = position.at(2);
  const double origin_y = position.at(3);
  const double column_y = position.at(4);
  const double row_y = position.back();

  const OGRSpatialReference* from = dataset.GetSpatialRef();
  if ( from == nullptr ) {
    throw std::runtime_error("'" + source.path + "' names no coordinate system, so its position could be in any");
  }

  // The box of its four corners, in its own system: it may be rotated.
  const auto                  width = static_cast<double>(dataset.GetRasterXSize());
  const auto                  height = static_cast<double>(dataset.GetRasterYSize());
  const std::array<double, 4> across{origin_x, origin_x + (width * column_x), origin_x + (height * row_x), origin_x + (width * column_x) + (height * row_x)};
  const std::array<double, 4> down{origin_y, origin_y + (width * column_y), origin_y + (height * row_y), origin_y + (width * column_y) + (height * row_y)};
  const auto [x_min, x_max] = std::ranges::minmax(across);
  const auto [y_min, y_max] = std::ranges::minmax(down);

  OGRSpatialReference wgs84;
  if ( wgs84.importFromEPSG(geo::WGS84_SRID) != OGRERR_NONE ) {
    fail("GDAL has no definition of WGS 84 (EPSG:4326): is PROJ's data installed?");
  }
  wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);  // longitude first

  const std::unique_ptr<OGRCoordinateTransformation> to_wgs84(OGRCreateCoordinateTransformation(from, &wgs84));
  Extent                                             extent;
  if ( ! to_wgs84 || to_wgs84->TransformBounds(x_min, y_min, x_max, y_max, &extent.west, &extent.south, &extent.east, &extent.north, EDGE_POINTS) == 0 ) {
    fail("the extent of '" + source.path + "' can't be transformed to WGS 84");
  }

  if ( ! std::isfinite(extent.west) || ! std::isfinite(extent.south) || ! std::isfinite(extent.east) || ! std::isfinite(extent.north) ) {
    throw std::runtime_error("the extent of '" + source.path + "' in WGS 84 is not a number: its coordinate system does not reach there");
  }

  if ( extent.east < extent.west ) {
    throw std::runtime_error("'" + source.path + "' crosses the antimeridian: read it as two files, one each side");
  }

  if ( extent.west < WEST - PAST_WORLD_ALLOWED || extent.east > WEST + static_cast<double>(DEGREES_ACROSS) + PAST_WORLD_ALLOWED ||
       extent.north > NORTH + PAST_WORLD_ALLOWED || extent.south < NORTH - static_cast<double>(DEGREES_DOWN) - PAST_WORLD_ALLOWED ) {
    throw std::runtime_error(
        "'" + source.path + "' reaches past the world (longitude " + std::format("{}", extent.west) + " to " + std::format("{}", extent.east) +
        ", latitude " + std::format("{}", extent.south) + " to " + std::format("{}", extent.north) +
        "): longitudes must be -180 to 180, so shift a file of 0 to 360 first"
    );
  }

  return extent;
}

/** @return The tiles of `grid` that `extent` touches, within the world. */
template <geo::Pixel pixel_t>
[[nodiscard]] Tiles tiles_of(const Extent& extent, const geo::Grid<pixel_t>& grid) {
  const auto per_degree = static_cast<double>(grid.pixels_per_degree) / static_cast<double>(grid.tile_pixels);  // tiles
  const auto ppd = static_cast<std::int64_t>(grid.pixels_per_degree);
  const auto tile = static_cast<std::int64_t>(grid.tile_pixels);
  const auto tiles_across = static_cast<double>(ceil_div(DEGREES_ACROSS * ppd, tile));
  const auto tiles_down = static_cast<double>(ceil_div(DEGREES_DOWN * ppd, tile));

  // Kept within the world's tiles while still a double: only then is it sure to fit a whole number.
  const auto column = [&](double tiles) { return static_cast<std::int64_t>(std::clamp(tiles, 0.0, tiles_across)); };
  const auto row = [&](double tiles) { return static_cast<std::int64_t>(std::clamp(tiles, 0.0, tiles_down)); };

  return {
      .first_column = column(std::floor(((extent.west - WEST) * per_degree) + EDGE_TOLERANCE)),
      .end_column = column(std::ceil(((extent.east - WEST) * per_degree) - EDGE_TOLERANCE)),
      .first_row = row(std::floor(((NORTH - extent.north) * per_degree) + EDGE_TOLERANCE)),
      .end_row = row(std::ceil(((NORTH - extent.south) * per_degree) - EDGE_TOLERANCE)),
  };
}

/** @return The window of `grid` that the tiles `tiles` are, cut at the world's edge, with no pixels yet. */
template <geo::Pixel pixel_t>
[[nodiscard]] geo::Raster<pixel_t> window_of(const Tiles& tiles, const geo::Grid<pixel_t>& grid) {
  const auto         ppd = static_cast<std::int64_t>(grid.pixels_per_degree);
  const auto         tile = static_cast<std::int64_t>(grid.tile_pixels);
  const std::int64_t first_column = tiles.first_column * tile;
  const std::int64_t first_row = tiles.first_row * tile;
  const std::int64_t end_column = std::min(tiles.end_column * tile, DEGREES_ACROSS * ppd);  // the last tile may reach past the world
  const std::int64_t end_row = std::min(tiles.end_row * tile, DEGREES_DOWN * ppd);

  return {
      .west = WEST + (static_cast<double>(first_column) / static_cast<double>(ppd)),
      .north = NORTH - (static_cast<double>(first_row) / static_cast<double>(ppd)),
      .pixel_width = 1.0 / static_cast<double>(ppd),
      .pixel_height = 1.0 / static_cast<double>(ppd),
      .width = static_cast<std::size_t>(end_column - first_column),
      .height = static_cast<std::size_t>(end_row - first_row),
      .nodata = grid.nodata,
      .pixels = {},
  };
}

/** @brief Fills `window`'s pixels from `dataset`: warped onto the window's own pixels, the grid's nodata where it has no data. */
template <geo::Pixel pixel_t>
void warp_into(geo::Raster<pixel_t>& window, GDALDataset& dataset, const RasterSource& source, const std::int64_t pixels_per_degree) {
  CPLErrorReset();  // from here, GDAL's last error is this window's own

  // The window's far edges, from its near ones and its size in pixels: with `-ts`, GDAL's pixels are then the grid's.
  const double east = window.west + (static_cast<double>(window.width) / static_cast<double>(pixels_per_degree));
  const double south = window.north - (static_cast<double>(window.height) / static_cast<double>(pixels_per_degree));

  std::vector<std::string> words{
      "-of",        "MEM",
      "-t_srs",     "EPSG:4326",
      "-te",        std::format("{}", window.west), std::format("{}", south), std::format("{}", east), std::format("{}", window.north),
      "-ts",        std::to_string(window.width), std::to_string(window.height),
      "-r",         std::string(name_of(source.resampling)),
      "-ot",        GDALGetDataTypeName(gdal_type<pixel_t>()),
      "-dstnodata", std::format("{}", static_cast<double>(window.nodata)),
      "-b",         std::to_string(source.band),
      "-et",        "0",     // exact, not approximated a scanline at a time: else a pixel depends on the window it is read in
      "-ovr",       "NONE",  // the file's own pixels, not its overviews
  };
  std::vector<char*> arguments;
  arguments.reserve(words.size() + 1);
  for ( std::string& word : words ) {
    arguments.push_back(word.data());
  }
  arguments.push_back(nullptr);

  const std::unique_ptr<GDALWarpAppOptions, void (*)(GDALWarpAppOptions*)> options(GDALWarpAppOptionsNew(arguments.data(), nullptr), GDALWarpAppOptionsFree);
  if ( ! options ) {
    fail("'" + source.path + "' can't be warped onto the grid");
  }

  GDALDatasetH               from = GDALDataset::ToHandle(&dataset);
  const GDALDatasetUniquePtr warped(GDALDataset::FromHandle(GDALWarp("", nullptr, 1, &from, options.get(), nullptr)));
  if ( ! warped ) {
    fail("'" + source.path + "' can't be warped onto the grid");
  }

  window.pixels.resize(window.width * window.height);
  const auto width = static_cast<int>(window.width);
  const auto height = static_cast<int>(window.height);
  if ( warped->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, width, height, window.pixels.data(), width, height, gdal_type<pixel_t>(), 0, 0) != CE_None ) {
    fail("the pixels of '" + source.path + "' can't be read");
  }

  // A file of reals may hold "not a number" without naming it as its nodata: no data, all the same.
  if constexpr ( std::floating_point<pixel_t> ) {
    std::ranges::replace_if(window.pixels, [](pixel_t pixel) { return std::isnan(pixel); }, window.nodata);
  }
}

}  // namespace

template <geo::Pixel pixel_t>
RasterRead read_raster(
    const RasterSource& source, const geo::Grid<pixel_t>& grid, std::size_t window_tiles, const std::function<void(geo::Raster<pixel_t>)>& on_window
) {
  if ( window_tiles == 0 ) {
    throw std::invalid_argument("a window is at least one tile each way");
  }

  if ( grid.pixels_per_degree < 1 || grid.tile_pixels < 1 ) {
    throw std::invalid_argument("a grid has at least one pixel per degree, and tiles of at least one pixel");
  }

  register_drivers();
  const QuietErrors quiet;

  const GDALDatasetUniquePtr dataset(GDALDataset::Open(source.path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY | GDAL_OF_VERBOSE_ERROR));
  if ( ! dataset ) {
    fail("'" + source.path + "' can't be opened as a raster file");
  }

  if ( source.band < 1 || source.band > dataset->GetRasterCount() ) {
    throw std::runtime_error("'" + source.path + "' has " + std::to_string(dataset->GetRasterCount()) + " bands, so no band " + std::to_string(source.band));
  }

  const Tiles tiles = tiles_of(extent_of(*dataset, source), grid);

  // At most what fits the loop's whole numbers, whatever was asked for: a whole file as one window asks for the most there
  // is. A window is never wider than the world, so its pixels fit GDAL's int.
  const auto step = static_cast<std::int64_t>(
      std::min<std::size_t>(window_tiles, static_cast<std::size_t>(std::numeric_limits<int>::max() / grid.tile_pixels))
  );

  RasterRead read;
  for ( std::int64_t row = tiles.first_row; row < tiles.end_row; row += step ) {
    for ( std::int64_t column = tiles.first_column; column < tiles.end_column; column += step ) {
      geo::Raster<pixel_t> window = window_of<pixel_t>(
          {.first_column = column,
           .end_column = std::min(column + step, tiles.end_column),
           .first_row = row,
           .end_row = std::min(row + step, tiles.end_row)},
          grid
      );
      warp_into(window, *dataset, source, grid.pixels_per_degree);

      if ( std::ranges::all_of(window.pixels, [&](pixel_t pixel) { return pixel == grid.nodata; }) ) {
        ++read.without_data;
        continue;
      }

      on_window(std::move(window));
      CPLErrorReset();  // GDAL's last error is this read's own from here on, not one `on_window` left
      ++read.windows;
    }
  }

  return read;
}

template <geo::Pixel pixel_t>
geo::Raster<pixel_t> read_raster(const RasterSource& source, const geo::Grid<pixel_t>& grid) {
  geo::Raster<pixel_t> whole;

  std::ignore = read_raster<pixel_t>(source, grid, std::numeric_limits<std::size_t>::max(), [&whole](geo::Raster<pixel_t> window) {
    whole = std::move(window);
  });

  return whole;
}

template RasterRead read_raster(const RasterSource&, const geo::Grid<std::int8_t>&, std::size_t, const std::function<void(geo::Raster<std::int8_t>)>&);
template RasterRead read_raster(const RasterSource&, const geo::Grid<std::uint8_t>&, std::size_t, const std::function<void(geo::Raster<std::uint8_t>)>&);
template RasterRead read_raster(const RasterSource&, const geo::Grid<std::int16_t>&, std::size_t, const std::function<void(geo::Raster<std::int16_t>)>&);
template RasterRead read_raster(const RasterSource&, const geo::Grid<std::uint16_t>&, std::size_t, const std::function<void(geo::Raster<std::uint16_t>)>&);
template RasterRead read_raster(const RasterSource&, const geo::Grid<std::int32_t>&, std::size_t, const std::function<void(geo::Raster<std::int32_t>)>&);
template RasterRead read_raster(const RasterSource&, const geo::Grid<std::uint32_t>&, std::size_t, const std::function<void(geo::Raster<std::uint32_t>)>&);
template RasterRead read_raster(const RasterSource&, const geo::Grid<float>&, std::size_t, const std::function<void(geo::Raster<float>)>&);
template RasterRead read_raster(const RasterSource&, const geo::Grid<double>&, std::size_t, const std::function<void(geo::Raster<double>)>&);

template geo::Raster<std::int8_t>   read_raster(const RasterSource&, const geo::Grid<std::int8_t>&);
template geo::Raster<std::uint8_t>  read_raster(const RasterSource&, const geo::Grid<std::uint8_t>&);
template geo::Raster<std::int16_t>  read_raster(const RasterSource&, const geo::Grid<std::int16_t>&);
template geo::Raster<std::uint16_t> read_raster(const RasterSource&, const geo::Grid<std::uint16_t>&);
template geo::Raster<std::int32_t>  read_raster(const RasterSource&, const geo::Grid<std::int32_t>&);
template geo::Raster<std::uint32_t> read_raster(const RasterSource&, const geo::Grid<std::uint32_t>&);
template geo::Raster<float>         read_raster(const RasterSource&, const geo::Grid<float>&);
template geo::Raster<double>        read_raster(const RasterSource&, const geo::Grid<double>&);

}  // namespace miniverse::gdal
