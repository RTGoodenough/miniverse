#include "miniverse/gdal/raster.hpp"

#include <boost/geometry/algorithms/envelope.hpp>  // IWYU pragma: keep
#include <boost/geometry/geometries/box.hpp>

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
#include <exception>
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
#include "miniverse/layer/raster_layer.hpp"

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

/**
 * @brief How many of a grid's pixels there are to one of a file's, across and down: over the file as a whole. Given to every
 * warp of the file, since GDAL would else work it out for each window by itself, a little differently for each where the
 * file's pixels are not all of one size in degrees, and weigh the same file pixels differently from window to window.
 */
struct Scale {
  double across = 1;
  double down = 1;
};

/** @return The scale of `dataset`, whose extent is `extent`, on a grid of `pixels_per_degree`. */
[[nodiscard]] Scale scale_of(GDALDataset& dataset, const Extent& extent, std::int64_t pixels_per_degree) {
  const auto ppd = static_cast<double>(pixels_per_degree);

  return {
      .across = ((extent.east - extent.west) * ppd) / static_cast<double>(dataset.GetRasterXSize()),
      .down = ((extent.north - extent.south) * ppd) / static_cast<double>(dataset.GetRasterYSize()),
  };
}

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
void warp_into(geo::Raster<pixel_t>& window, GDALDataset& dataset, const RasterSource& source, const std::int64_t pixels_per_degree, const Scale& scale) {
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
      "-wo",        std::format("XSCALE={}", scale.across),  // the file's, not each window's own: for the same reason
      "-wo",        std::format("YSCALE={}", scale.down),
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

/** @return `source`'s file, opened for reading the band it names. */
[[nodiscard]] GDALDatasetUniquePtr opened(const RasterSource& source) {
  register_drivers();

  GDALDatasetUniquePtr dataset(GDALDataset::Open(source.path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY | GDAL_OF_VERBOSE_ERROR));
  if ( ! dataset ) {
    fail("'" + source.path + "' can't be opened as a raster file");
  }

  if ( source.band < 1 || source.band > dataset->GetRasterCount() ) {
    throw std::runtime_error("'" + source.path + "' has " + std::to_string(dataset->GetRasterCount()) + " bands, so no band " + std::to_string(source.band));
  }

  return dataset;
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
  const auto ppd = static_cast<std::int64_t>(grid.pixels_per_degree);
  const auto tile = static_cast<std::int64_t>(grid.tile_pixels);
  const auto tiles_across = static_cast<double>(ceil_div(DEGREES_ACROSS * ppd, tile));
  const auto tiles_down = static_cast<double>(ceil_div(DEGREES_DOWN * ppd, tile));

  // A box that ends on a tile's edge meets the tile beyond it: the tolerance leans that way, where `tiles_of` leans the other.
  const auto column = [&](double tiles) { return static_cast<std::int64_t>(std::clamp(tiles, 0.0, tiles_across)); };
  const auto row = [&](double tiles) { return static_cast<std::int64_t>(std::clamp(tiles, 0.0, tiles_down)); };

  return {
      .first_column = column(std::floor(((box.min_corner().x() - WEST) * per_degree) - EDGE_TOLERANCE)),
      .end_column = column(std::floor(((box.max_corner().x() - WEST) * per_degree) + EDGE_TOLERANCE) + 1),
      .first_row = row(std::floor(((NORTH - box.max_corner().y()) * per_degree) - EDGE_TOLERANCE)),
      .end_row = row(std::floor(((NORTH - box.min_corner().y()) * per_degree) + EDGE_TOLERANCE) + 1),
  };
}

/** @return The tiles that are both in `one` and in `other`: none, if `first` is not before `end` each way. */
[[nodiscard]] Tiles in_both(const Tiles& one, const Tiles& other) {
  return {
      .first_column = std::max(one.first_column, other.first_column),
      .end_column = std::min(one.end_column, other.end_column),
      .first_row = std::max(one.first_row, other.first_row),
      .end_row = std::min(one.end_row, other.end_row),
  };
}

[[nodiscard]] bool none(const Tiles& tiles) { return tiles.first_column >= tiles.end_column || tiles.first_row >= tiles.end_row; }

/** @return The outline of the tile of `grid` numbered `column` across and `row` down. */
template <geo::Pixel pixel_t>
[[nodiscard]] geo::Box outline_of(std::int64_t column, std::int64_t row, const geo::Grid<pixel_t>& grid) {
  const auto tile = static_cast<std::int64_t>(grid.tile_pixels);
  const auto ppd = static_cast<double>(grid.pixels_per_degree);
  const auto degrees = [&](std::int64_t tiles) { return static_cast<double>(tiles * tile) / ppd; };

  return {{WEST + degrees(column), NORTH - degrees(row + 1)}, {WEST + degrees(column + 1), NORTH - degrees(row)}};
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

template <geo::Pixel pixel_t>
[[nodiscard]] bool has_no_data(const geo::Raster<pixel_t>& raster) {
  return std::ranges::all_of(raster.pixels, [&raster](pixel_t pixel) { return pixel == raster.nodata; });
}

/**
 * @return The tile of `grid` numbered `column` across, cut from `window`, a row of whole tiles that starts at the tile
 * `first_column`: a whole tile always, the grid's nodata where the window was cut short at the world's edge.
 */
template <geo::Pixel pixel_t>
[[nodiscard]] geo::Raster<pixel_t> tile_of(const geo::Raster<pixel_t>& window, std::int64_t column, std::int64_t first_column, const geo::Grid<pixel_t>& grid) {
  const auto        tile = static_cast<std::size_t>(grid.tile_pixels);
  const std::size_t from = static_cast<std::size_t>(column - first_column) * tile;  // the tile's first pixel across, in the window

  // Its west edge from its place in the grid, as `window_of` and a table's tiles have theirs: the same number, to the last bit.
  geo::Raster<pixel_t> cut{
      .west = WEST + (static_cast<double>(column * static_cast<std::int64_t>(grid.tile_pixels)) / static_cast<double>(grid.pixels_per_degree)),
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

}  // namespace

template <geo::Pixel pixel_t>
RasterRead read_raster(
    const RasterSource& source, const geo::Grid<pixel_t>& grid, std::size_t window_tiles, const std::function<void(geo::Raster<pixel_t>)>& on_window
) {
  if ( window_tiles == 0 ) {
    throw std::invalid_argument("a window is at least one tile each way");
  }

  geo::check_grid(grid);

  const QuietErrors          quiet;
  const GDALDatasetUniquePtr dataset = opened(source);

  const Extent extent = extent_of(*dataset, source);
  const Tiles  tiles = tiles_of(extent, grid);
  const Scale  scale = scale_of(*dataset, extent, grid.pixels_per_degree);

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
      warp_into(window, *dataset, source, grid.pixels_per_degree, scale);

      if ( has_no_data(window) ) {
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

template <geo::Pixel pixel_t>
void read_tiles_in(
    const RasterSource& source, const geo::Grid<pixel_t>& grid, const geo::Polygon& location, std::size_t chunk_tiles,
    const std::function<bool(std::vector<geo::Raster<pixel_t>>)>& on_chunk
) {
  if ( chunk_tiles == 0 ) {
    throw std::invalid_argument("a chunk of tiles holds at least one");
  }

  geo::check_grid(grid);

  const QuietErrors          quiet;
  const GDALDatasetUniquePtr dataset = opened(source);

  const Extent extent = extent_of(*dataset, source);
  const Tiles  tiles = in_both(tiles_meeting(location, grid), tiles_of(extent, grid));
  const Scale  scale = scale_of(*dataset, extent, grid.pixels_per_degree);

  const raster::Reach reach(location);

  // Warped a run of tiles of one row at a time, then cut into its tiles: as many as a chunk holds, but no wider a run than
  // some thousands of pixels, however large a chunk was asked for. Of a run, only from the first tile the location reaches
  // into to the last, and of those only the ones it does reach into: the others are not read.
  constexpr std::int64_t            RUN_PIXELS = 4096;
  const std::int64_t                widest = std::max<std::int64_t>(1, RUN_PIXELS / grid.tile_pixels);
  const auto                        run = static_cast<std::int64_t>(std::min<std::size_t>(chunk_tiles, static_cast<std::size_t>(widest)));
  std::vector<geo::Raster<pixel_t>> chunk;
  for ( std::int64_t row = tiles.first_row; row < tiles.end_row; ++row ) {
    for ( std::int64_t first = tiles.first_column; first < tiles.end_column; first += run ) {
      const std::vector<std::int64_t> reached = reached_of(reach, {first, std::min(first + run, tiles.end_column), row, row + 1}, grid);
      if ( reached.empty() ) {
        continue;
      }

      geo::Raster<pixel_t> window = window_of<pixel_t>(
          {.first_column = reached.front(), .end_column = reached.back() + 1, .first_row = row, .end_row = row + 1}, grid
      );
      warp_into(window, *dataset, source, grid.pixels_per_degree, scale);

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

          CPLErrorReset();  // GDAL's last error is this read's own from here on, not one `on_chunk` left
        }
      }
    }
  }

  if ( ! chunk.empty() ) {
    std::ignore = on_chunk(std::move(chunk));
  }
}

template <geo::Pixel pixel_t>
std::vector<std::string> problems_of(const RasterSource& source, const geo::Grid<pixel_t>& grid) {
  try {
    geo::check_grid(grid);

    const QuietErrors          quiet;
    const GDALDatasetUniquePtr dataset = opened(source);
    std::ignore = extent_of(*dataset, source);

    return {};
  } catch ( const std::exception& unreadable ) {
    return {unreadable.what()};
  }
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


template void read_tiles_in(
    const RasterSource&, const geo::Grid<std::int8_t>&, const geo::Polygon&, std::size_t, const std::function<bool(std::vector<geo::Raster<std::int8_t>>)>&
);
template void read_tiles_in(
    const RasterSource&, const geo::Grid<std::uint8_t>&, const geo::Polygon&, std::size_t, const std::function<bool(std::vector<geo::Raster<std::uint8_t>>)>&
);
template void read_tiles_in(
    const RasterSource&, const geo::Grid<std::int16_t>&, const geo::Polygon&, std::size_t, const std::function<bool(std::vector<geo::Raster<std::int16_t>>)>&
);
template void read_tiles_in(
    const RasterSource&, const geo::Grid<std::uint16_t>&, const geo::Polygon&, std::size_t, const std::function<bool(std::vector<geo::Raster<std::uint16_t>>)>&
);
template void read_tiles_in(
    const RasterSource&, const geo::Grid<std::int32_t>&, const geo::Polygon&, std::size_t, const std::function<bool(std::vector<geo::Raster<std::int32_t>>)>&
);
template void read_tiles_in(
    const RasterSource&, const geo::Grid<std::uint32_t>&, const geo::Polygon&, std::size_t, const std::function<bool(std::vector<geo::Raster<std::uint32_t>>)>&
);
template void read_tiles_in(
    const RasterSource&, const geo::Grid<float>&, const geo::Polygon&, std::size_t, const std::function<bool(std::vector<geo::Raster<float>>)>&
);
template void read_tiles_in(
    const RasterSource&, const geo::Grid<double>&, const geo::Polygon&, std::size_t, const std::function<bool(std::vector<geo::Raster<double>>)>&
);

template std::vector<std::string> problems_of(const RasterSource&, const geo::Grid<std::int8_t>&);
template std::vector<std::string> problems_of(const RasterSource&, const geo::Grid<std::uint8_t>&);
template std::vector<std::string> problems_of(const RasterSource&, const geo::Grid<std::int16_t>&);
template std::vector<std::string> problems_of(const RasterSource&, const geo::Grid<std::uint16_t>&);
template std::vector<std::string> problems_of(const RasterSource&, const geo::Grid<std::int32_t>&);
template std::vector<std::string> problems_of(const RasterSource&, const geo::Grid<std::uint32_t>&);
template std::vector<std::string> problems_of(const RasterSource&, const geo::Grid<float>&);
template std::vector<std::string> problems_of(const RasterSource&, const geo::Grid<double>&);

}  // namespace miniverse::gdal
