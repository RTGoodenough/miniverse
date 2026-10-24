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
#include <vector>

#include "gdal/common.hpp"
#include "gdal/raster/source.hpp"
#include "miniverse/gdal/raster.hpp"
#include "miniverse/geo/concepts/raster.hpp"
#include "miniverse/geo/raster.hpp"

/**
 * @file
 * One window of the grid filled from a file: GDAL's warp, with the options that make a pixel the same whichever window it
 * is read in. Not installed.
 */
namespace miniverse::gdal::detail {

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

/** @brief Fills `window`'s pixels from `dataset`: warped onto the window's own pixels, the grid's nodata where it has no data. */
template <geo::Pixel pixel_t>
void warp_into(
    geo::Raster<pixel_t>& window, GDALDataset& dataset, const RasterSource& source, const std::int64_t pixels_per_degree, const Scale& scale
) {
  CPLErrorReset();  // from here, GDAL's last error is this window's own

  // The window's far edges, from its near ones and its size in pixels: with `-ts`, GDAL's pixels are then the grid's.
  const double east = window.west + (static_cast<double>(window.width) / static_cast<double>(pixels_per_degree));
  const double south = window.north - (static_cast<double>(window.height) / static_cast<double>(pixels_per_degree));

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
      "0",  // exact, not approximated a scanline at a time: else a pixel depends on the window it is read in
      "-ovr",
      "NONE",  // the file's own pixels, not its overviews
      "-wo",
      std::format("XSCALE={}", scale.across),  // the file's, not each window's own: for the same reason
      "-wo",
      std::format("YSCALE={}", scale.down),
  };
  std::vector<char*> arguments;
  arguments.reserve(words.size() + 1);
  for ( std::string& word : words ) {
    arguments.push_back(word.data());
  }
  arguments.push_back(nullptr);

  const std::unique_ptr<GDALWarpAppOptions, void (*)(GDALWarpAppOptions*)> options(
      GDALWarpAppOptionsNew(arguments.data(), nullptr), GDALWarpAppOptionsFree
  );
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
  if ( warped->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, width, height, window.pixels.data(), width, height, gdal_type<pixel_t>(), 0, 0) !=
       CE_None ) {
    fail("the pixels of '" + source.path + "' can't be read");
  }

  // A file of reals may hold "not a number" without naming it as its nodata: no data, all the same.
  if constexpr ( std::floating_point<pixel_t> ) {
    std::ranges::replace_if(window.pixels, [](pixel_t pixel) { return std::isnan(pixel); }, window.nodata);
  }
}

}  // namespace miniverse::gdal::detail
