#include "gdal/raster/source.hpp"

#include <cpl_error.h>
#include <gdal.h>
#include <gdal_priv.h>
#include <ogr_core.h>
#include <ogr_spatialref.h>
#include <ogr_srs_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>

#include "gdal/common.hpp"
#include "miniverse/gdal/raster.hpp"
#include "miniverse/geo/grid.hpp"
#include "miniverse/geo/types.hpp"

namespace miniverse::gdal::detail {

namespace {

// The grid's corner, and the world's size in degrees.
constexpr double WEST = geo::GRID_WEST;
constexpr double NORTH = geo::GRID_NORTH;
using geo::DEGREES_ACROSS;
using geo::DEGREES_DOWN;

// How far past the world's edge a file may reach, in degrees, and be read up to the edge: a global file whose pixels are
// centred on the edge overhangs it by half a pixel. One that reaches further (longitudes of 0 to 360, say) is refused.
constexpr double PAST_WORLD_ALLOWED = 1;

constexpr std::size_t GEOTRANSFORM_VALUES = 6;

// How many points along each edge of the file's outline are transformed to find its extent in WGS 84: edges curve.
constexpr int EDGE_POINTS = 21;

}  // namespace

GDALDatasetUniquePtr opened(const RasterSource& source) {
  register_drivers();

  GDALDatasetUniquePtr dataset(GDALDataset::Open(source.path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY | GDAL_OF_VERBOSE_ERROR));
  if ( ! dataset ) {
    fail("'" + source.path + "' can't be opened as a raster file");
  }

  if ( source.band < 1 || source.band > dataset->GetRasterCount() ) {
    throw std::runtime_error(
        "'" + source.path + "' has " + std::to_string(dataset->GetRasterCount()) + " bands, so no band " + std::to_string(source.band)
    );
  }

  return dataset;
}

geo::Box extent_of(GDALDataset& dataset, const RasterSource& source) {
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
  const std::array<double, 4> across{
      origin_x, origin_x + (width * column_x), origin_x + (height * row_x), origin_x + (width * column_x) + (height * row_x)
  };
  const std::array<double, 4> down{
      origin_y, origin_y + (width * column_y), origin_y + (height * row_y), origin_y + (width * column_y) + (height * row_y)
  };
  const auto [x_min, x_max] = std::ranges::minmax(across);
  const auto [y_min, y_max] = std::ranges::minmax(down);

  OGRSpatialReference wgs84;
  if ( wgs84.importFromEPSG(geo::WGS84_SRID) != OGRERR_NONE ) {
    fail("GDAL has no definition of WGS 84 (EPSG:4326): is PROJ's data installed?");
  }
  wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);  // longitude first

  const std::unique_ptr<OGRCoordinateTransformation> to_wgs84(OGRCreateCoordinateTransformation(from, &wgs84));
  double                                             west = 0;
  double                                             south = 0;
  double                                             east = 0;
  double                                             north = 0;
  if ( ! to_wgs84 || to_wgs84->TransformBounds(x_min, y_min, x_max, y_max, &west, &south, &east, &north, EDGE_POINTS) == 0 ) {
    fail("the extent of '" + source.path + "' can't be transformed to WGS 84");
  }

  if ( ! std::isfinite(west) || ! std::isfinite(south) || ! std::isfinite(east) || ! std::isfinite(north) ) {
    throw std::runtime_error("the extent of '" + source.path + "' in WGS 84 is not a number: its coordinate system does not reach there");
  }

  if ( east < west ) {
    throw std::runtime_error("'" + source.path + "' crosses the antimeridian: read it as two files, one each side");
  }

  if ( west < WEST - PAST_WORLD_ALLOWED || east > WEST + static_cast<double>(DEGREES_ACROSS) + PAST_WORLD_ALLOWED ||
       north > NORTH + PAST_WORLD_ALLOWED || south < NORTH - static_cast<double>(DEGREES_DOWN) - PAST_WORLD_ALLOWED ) {
    throw std::runtime_error(
        "'" + source.path + "' reaches past the world (longitude " + std::format("{}", west) + " to " + std::format("{}", east) + ", latitude " +
        std::format("{}", south) + " to " + std::format("{}", north) + "): longitudes must be -180 to 180, so shift a file of 0 to 360 first"
    );
  }

  return {{west, south}, {east, north}};
}

Scale scale_of(GDALDataset& dataset, const geo::Box& extent, std::int64_t pixels_per_degree) {
  const auto ppd = static_cast<double>(pixels_per_degree);

  return {
      .across = ((extent.max_corner().x() - extent.min_corner().x()) * ppd) / static_cast<double>(dataset.GetRasterXSize()),
      .down = ((extent.max_corner().y() - extent.min_corner().y()) * ppd) / static_cast<double>(dataset.GetRasterYSize()),
  };
}

}  // namespace miniverse::gdal::detail
