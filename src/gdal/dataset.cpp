#include "miniverse/gdal/dataset.hpp"

#include <cpl_error.h>
#include <gdal.h>
#include <gdal_priv.h>
#include <ogr_core.h>
#include <ogr_spatialref.h>
#include <ogr_srs_api.h>

#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>

#include "common.hpp"
#include "miniverse/geo/concepts/raster.hpp"
#include "miniverse/geo/raster.hpp"
#include "miniverse/geo/types.hpp"

namespace miniverse::gdal {

namespace {

using detail::fail;
using detail::gdal_type;
using detail::QuietErrors;
using detail::register_drivers;

constexpr std::size_t GEOTRANSFORM_VALUES = 6;

}  // namespace

template <geo::Pixel pixel_t>
GDALDatasetUniquePtr dataset_of(const geo::Raster<pixel_t>& raster) {
  raster.check_pixel_count();

  constexpr auto MOST = static_cast<std::size_t>(std::numeric_limits<int>::max());
  if ( raster.width == 0 || raster.height == 0 || raster.width > MOST || raster.height > MOST ) {
    throw std::invalid_argument("a raster of no pixels is no dataset, nor is one of more than GDAL counts");
  }

  const auto width = static_cast<int>(raster.width);
  const auto height = static_cast<int>(raster.height);

  register_drivers();
  const QuietErrors quiet;

  GDALDriver* memory = GetGDALDriverManager()->GetDriverByName("MEM");
  if ( memory == nullptr ) {
    fail("GDAL has no driver for datasets in memory (MEM)");
  }

  GDALDatasetUniquePtr dataset(memory->Create("", width, height, 1, gdal_type<pixel_t>(), nullptr));
  if ( ! dataset ) {
    fail("GDAL can't make a dataset in memory");
  }

  // Where it lies: the north-west corner, and a pixel's size across and down, with no rotation.
  std::array<double, GEOTRANSFORM_VALUES> position{raster.west, raster.pixel_width, 0, raster.north, 0, -raster.pixel_height};

  OGRSpatialReference wgs84;
  if ( wgs84.importFromEPSG(geo::WGS84_SRID) != OGRERR_NONE ) {
    fail("GDAL has no definition of WGS 84 (EPSG:4326): is PROJ's data installed?");
  }
  wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);  // longitude first

  GDALRasterBand* band = dataset->GetRasterBand(1);
  if ( dataset->SetGeoTransform(position.data()) != CE_None || dataset->SetSpatialRef(&wgs84) != CE_None ||
       band->SetNoDataValue(static_cast<double>(raster.nodata)) != CE_None ) {
    fail("GDAL can't place the dataset");
  }

  // GDAL reads from the buffer when it writes a band, but takes it as one it might write to.
  auto* pixels = const_cast<pixel_t*>(raster.pixels.data());  // NOLINT(cppcoreguidelines-pro-type-const-cast)
  if ( band->RasterIO(GF_Write, 0, 0, width, height, pixels, width, height, gdal_type<pixel_t>(), 0, 0) != CE_None ) {
    fail("GDAL can't copy the pixels");
  }

  return dataset;
}

template GDALDatasetUniquePtr dataset_of(const geo::Raster<std::int8_t>&);
template GDALDatasetUniquePtr dataset_of(const geo::Raster<std::uint8_t>&);
template GDALDatasetUniquePtr dataset_of(const geo::Raster<std::int16_t>&);
template GDALDatasetUniquePtr dataset_of(const geo::Raster<std::uint16_t>&);
template GDALDatasetUniquePtr dataset_of(const geo::Raster<std::int32_t>&);
template GDALDatasetUniquePtr dataset_of(const geo::Raster<std::uint32_t>&);
template GDALDatasetUniquePtr dataset_of(const geo::Raster<float>&);
template GDALDatasetUniquePtr dataset_of(const geo::Raster<double>&);

}  // namespace miniverse::gdal
