#pragma once

#include <cpl_error.h>
#include <gdal.h>
#include <gdal_priv.h>
#include <gdal_utils.h>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "gdal/common.hpp"
#include "gdal/raster/source.hpp"
#include "miniverse/gdal/raster.hpp"
#include "miniverse/geo/concepts/raster.hpp"
#include "miniverse/geo/grid.hpp"
#include "miniverse/geo/raster.hpp"
#include "miniverse/geo/types.hpp"

/**
 * @file
 * One window of the grid filled from a file: GDAL's warp, with the options that make a pixel the
 * same whichever window it is read in. Not installed.
 */
namespace miniverse::gdal::detail {

/** @return The name gdalwarp knows `resampling` by. */
[[nodiscard]] constexpr std::string_view name_of(Resampling resampling)
{
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

/**
 * @return The transformer option that names `extent`, within the world, as the area the file's
 * coordinates are transformed in. GDAL works it out from the file's pixel size alone, which is
 * nothing for a file turned a quarter (its geotransform all rotation), and PROJ finds no
 * transformation for an area of nothing.
 */
[[nodiscard]] inline std::string area_of_interest(const geo::Box& extent)
{
  const double world_east  = geo::GRID_WEST + static_cast<double>(geo::DEGREES_ACROSS);
  const double world_south = geo::GRID_NORTH - static_cast<double>(geo::DEGREES_DOWN);

  return std::format("AREA_OF_INTEREST={},{},{},{}",
                     std::clamp(extent.min_corner().x(), geo::GRID_WEST, world_east),
                     std::clamp(extent.min_corner().y(), world_south, geo::GRID_NORTH),
                     std::clamp(extent.max_corner().x(), geo::GRID_WEST, world_east),
                     std::clamp(extent.max_corner().y(), world_south, geo::GRID_NORTH));
}

/**
 * @brief Fills `window`'s pixels from `dataset`, whose extent in WGS 84 is `extent`: warped onto
 * the window's own pixels, the grid's nodata where it has no data.
 */
template <geo::Pixel pixel_t>
void warp_into(geo::Raster<pixel_t>& window,
               GDALDataset&          dataset,
               const RasterSource&   source,
               const geo::Box&       extent,
               const std::int64_t    pixels_per_degree,
               const Scale&          scale)
{
  CPLErrorReset();  // from here, GDAL's last error is this window's own

  // The window's far edges, from its near ones and its size in pixels: with `-ts`, GDAL's pixels
  // are then the grid's.
  const double east =
      window.west + (static_cast<double>(window.width) / static_cast<double>(pixels_per_degree));
  const double south =
      window.north - (static_cast<double>(window.height) / static_cast<double>(pixels_per_degree));

  std::vector<std::string> words{
      "-of",
      "MEM",
      "-t_srs",
      "EPSG:4326",
      "-te",
      std::format("{}", window.west),
      std::format("{}", south),
      std::format("{}", east),
      std::format("{}", window.north),
      "-ts",
      std::to_string(window.width),
      std::to_string(window.height),
      "-r",
      std::string(name_of(source.resampling)),
      "-ot",
      GDALGetDataTypeName(gdal_type<pixel_t>()),
      "-dstnodata",
      std::format("{}", static_cast<double>(window.nodata)),
      "-b",
      std::to_string(source.band),
      "-et",
      "0",  // the exact transformer; the approximate one is fitted per window, and a pixel then
            // depends on the window
      "-ovr",
      "NONE",  // the file's own pixels; GDAL's default picks an overview to suit the window
      "-wo",
      std::format("XSCALE={}", scale.across),  // one scale for the whole file (`Scale`), for the
                                               // same reason
      "-wo",
      std::format("YSCALE={}", scale.down),
      "-to",
      area_of_interest(extent),
  };

  // A band of reals that names no nodata may still hold "not a number". Named as its nodata, it is
  // left out of the pixels made from it; otherwise every pixel it touches is "not a number" too,
  // which a grid of whole numbers has no way to hold.
  GDALRasterBand* band         = dataset.GetRasterBand(source.band);
  int             names_nodata = 0;
  std::ignore                  = band->GetNoDataValue(&names_nodata);
  if ( GDALDataTypeIsFloating(band->GetRasterDataType()) != 0 && names_nodata == 0 ) {
    words.insert(words.end(), {"-srcnodata", "nan"});
  }

  std::vector<char*> arguments;
  arguments.reserve(words.size() + 1);
  for ( std::string& word : words ) {
    arguments.push_back(word.data());
  }
  arguments.push_back(nullptr);

  const std::unique_ptr<GDALWarpAppOptions, void (*)(GDALWarpAppOptions*)> options(
      GDALWarpAppOptionsNew(arguments.data(), nullptr), GDALWarpAppOptionsFree);
  if ( ! options ) {
    fail("'" + source.path + "' can't be warped onto the grid");
  }

  GDALDatasetH               from = GDALDataset::ToHandle(&dataset);
  const GDALDatasetUniquePtr warped(
      GDALDataset::FromHandle(GDALWarp("", nullptr, 1, &from, options.get(), nullptr)));
  if ( ! warped ) {
    fail("'" + source.path + "' can't be warped onto the grid");
  }

  window.pixels.resize(window.width * window.height);
  const auto width  = static_cast<int>(window.width);
  const auto height = static_cast<int>(window.height);
  if ( warped->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, width, height, window.pixels.data(), width,
                                          height, gdal_type<pixel_t>(), 0, 0) != CE_None )
  {
    fail("the pixels of '" + source.path + "' can't be read");
  }

  // A file of reals that names a number as its nodata may hold "not a number" too.
  if constexpr ( std::floating_point<pixel_t> ) {
    std::ranges::replace_if(
        window.pixels, [](pixel_t pixel) { return std::isnan(pixel); }, window.nodata);
  }
}

}  // namespace miniverse::gdal::detail
