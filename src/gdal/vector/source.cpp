#include "gdal/vector/source.hpp"

#include <ogr_core.h>
#include <ogr_feature.h>
#include <ogr_geometry.h>
#include <ogr_spatialref.h>
#include <ogr_srs_api.h>

#include <gdal.h>
#include <gdal_priv.h>

#include <memory>
#include <ogrsf_frmts.h>
#include <stdexcept>
#include <string>

#include "gdal/common.hpp"
#include "miniverse/gdal/vector.hpp"
#include "miniverse/geo/types.hpp"

namespace miniverse::gdal::detail {

GDALDatasetUniquePtr opened(const VectorSource& source)
{
  register_drivers();

  GDALDatasetUniquePtr dataset(GDALDataset::Open(
      source.path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY | GDAL_OF_VERBOSE_ERROR));
  if ( ! dataset ) {
    fail("'" + source.path + "' can't be opened as a vector file");
  }

  return dataset;
}

OGRLayer& layer_of(GDALDataset& dataset, const VectorSource& source)
{
  if ( ! source.layer.empty() ) {
    OGRLayer* named = dataset.GetLayerByName(source.layer.c_str());
    if ( named == nullptr ) {
      throw std::runtime_error("'" + source.path + "' has no layer '" + source.layer + "'");
    }

    return *named;
  }

  if ( dataset.GetLayerCount() != 1 ) {
    throw std::runtime_error("'" +
                             source.path +
                             "' has " +
                             std::to_string(dataset.GetLayerCount()) +
                             " layers: name the one to read");
  }

  return *dataset.GetLayer(0);
}

std::unique_ptr<OGRCoordinateTransformation> to_wgs84_from(OGRLayer&           layer,
                                                           const VectorSource& source)
{
  // The layer's own description says which axis its data has first; only the target's order is ours
  // to choose.
  const OGRSpatialReference* from = layer.GetSpatialRef();
  if ( from == nullptr ) {
    throw std::runtime_error(
        "'" + source.path + "' names no coordinate system, so its coordinates could be in any");
  }

  OGRSpatialReference wgs84;
  if ( wgs84.importFromEPSG(geo::WGS84_SRID) != OGRERR_NONE ) {
    fail("GDAL has no definition of WGS 84 (EPSG:4326): is PROJ's data installed?");
  }
  wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);  // longitude first

  std::unique_ptr<OGRCoordinateTransformation> transformation(
      OGRCreateCoordinateTransformation(from, &wgs84));
  if ( ! transformation ) {
    fail("the coordinates of '" + source.path + "' can't be transformed to WGS 84");
  }

  return transformation;
}

std::unique_ptr<OGRGeometry> in_wgs84(std::unique_ptr<OGRGeometry> stolen,
                                      const OGRFeature&            feature,
                                      OGRCoordinateTransformation& to_wgs84,
                                      const VectorSource&          source)
{
  stolen->flattenTo2D();
  if ( stolen->hasCurveGeometry() ) {
    stolen.reset(stolen->getLinearGeometry());
  }

  if ( stolen->transform(&to_wgs84) != OGRERR_NONE ) {
    fail("feature " +
         std::to_string(feature.GetFID()) +
         " of '" +
         source.path +
         "' can't be transformed to WGS 84");
  }

  return stolen;
}

}  // namespace miniverse::gdal::detail
