#pragma once

#include <gdal_priv.h>

#include "miniverse/geo/concepts/raster.hpp"
#include "miniverse/geo/raster.hpp"

/**
 * Rasters as GDAL's own datasets: what a load of a raster layer gives, from a table or from a file, handed to GDAL's tools
 * (a warp to a local projection, a slope, contours, a file of any format it writes).
 *
 * @code
 * struct Elevation : miniverse::RasterLayer<std::int16_t> {};
 *
 * const miniverse::geo::Raster<std::int16_t> heights = world.load<Elevation>(area).get();
 * const GDALDatasetUniquePtr                 dataset = miniverse::gdal::dataset_of(heights);
 *
 * // As a GeoTIFF, for a viewer:
 * GDALClose(GetGDALDriverManager()->GetDriverByName("GTiff")->CreateCopy("heights.tif", dataset.get(), FALSE, nullptr, nullptr, nullptr));
 * @endcode
 *
 * Part of the optional component `miniverse::gdal`, and its one header that includes GDAL's: a program that only reads files
 * into layers (raster.hpp, vector.hpp) needs none of them.
 */
namespace miniverse::gdal {

/**
 * @return `raster` as a GDAL dataset held in memory (GDAL's `MEM` driver): one band of `pixel_t` with the raster's pixels and
 * its nodata value, placed where the raster lies (north up, its pixel size in degrees), in WGS 84 with the longitude first.
 *
 * The pixels are copied: the dataset is its own, and outlives the raster. A dataset is GDAL's, with GDAL's rules: one
 * thread at a time.
 * @throws std::invalid_argument if the raster has no pixels, as a load where there are no tiles gives (GDAL has no dataset
 * of none), or its pixels are not `width * height`.
 * @throws std::runtime_error with GDAL's message if GDAL can't make it.
 */
template <geo::Pixel pixel_t>
[[nodiscard]] GDALDatasetUniquePtr dataset_of(const geo::Raster<pixel_t>& raster);

// Defined for each pixel type a raster can hold: std::int8_t, std::uint8_t, std::int16_t, std::uint16_t, std::int32_t,
// std::uint32_t, float and double.

}  // namespace miniverse::gdal
