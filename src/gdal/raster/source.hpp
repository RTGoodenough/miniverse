#pragma once

#include <gdal_priv.h>

#include <cstdint>

#include "miniverse/gdal/raster.hpp"
#include "miniverse/geo/types.hpp"

/**
 * @file
 * A raster file as a source: opened for its band, where it lies in WGS 84, how its pixels scale to
 * the grid's. Not installed.
 */
namespace miniverse::gdal::detail {

/**
 * @brief How many of a grid's pixels one step of the file's is, along its rows and down its columns
 * (which run east and south unless the file is turned), over the whole file: GDAL's `XSCALE` and
 * `YSCALE`, given to every warp of the file. Left to GDAL, the scale is worked out per window, and
 * where the file's pixels are not all of one size in degrees it differs a little from window to
 * window, so a pixel depends on the window it is read in.
 */
struct Scale {
  double across = 1;
  double down   = 1;
};

/** @return `source`'s file, opened for reading the band it names. */
[[nodiscard]] GDALDatasetUniquePtr opened(const RasterSource& source);

/**
 * @return The extent of `dataset` in WGS 84, from its south-west corner to its north-east: its
 * outline, transformed from its own coordinate system.
 */
[[nodiscard]] geo::Box extent_of(GDALDataset& dataset, const RasterSource& source);

/** @return The scale of `dataset`, whose extent is `extent`, on a grid of `pixels_per_degree`. */
[[nodiscard]] Scale scale_of(
    GDALDataset& dataset, const geo::Box& extent, std::int64_t pixels_per_degree);

}  // namespace miniverse::gdal::detail
