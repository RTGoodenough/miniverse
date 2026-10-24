#include "gdal/vector/filter.hpp"

#include <cpl_error.h>
#include <ogr_api.h>
#include <ogr_core.h>
#include <ogr_feature.h>
#include <ogr_geometry.h>
#include <ogr_spatialref.h>
#include <ogrsf_frmts.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "gdal/common.hpp"
#include "miniverse/gdal/vector.hpp"
#include "miniverse/geo/types.hpp"

namespace miniverse::gdal::detail {

namespace {

/** @return `polygon` as OGR has it, its rings closed. */
[[nodiscard]] OGRPolygon ogr_polygon_of(const geo::Polygon& polygon) {
  const auto ring_of = [](const auto& points) {
    auto ring = std::make_unique<OGRLinearRing>();
    for ( const geo::Point& point : points ) {
      ring->addPoint(point.x(), point.y());
    }
    ring->closeRings();

    return ring;
  };

  OGRPolygon made;
  made.addRingDirectly(ring_of(polygon.outer()).release());
  for ( const auto& hole : polygon.inners() ) {
    made.addRingDirectly(ring_of(hole).release());
  }

  return made;
}

/**
 * @brief Has `layer` read only the features whose boxes meet the box of `location`, a polygon in WGS 84, as that box lies in
 * the layer's own coordinate system, and a little more: what its spatial index answers. The box's straight edges are curves
 * there, found by points along them, so the box is widened by a hundredth each way to hold what bulges between the points:
 * the test of each feature decides, and this only spares it features far off. If the box can't be placed there (the
 * location is partly outside what the system covers), every feature is read.
 */
void narrow_to(OGRLayer& layer, const OGRPolygon& location, OGRCoordinateTransformation& to_wgs84) {
  constexpr int    POINTS_ALONG_AN_EDGE = 21;
  constexpr double WIDER_BY = 0.01;      // of the box's own size, each way
  constexpr double AND_AT_LEAST = 1e-7;  // of how far from the origin it lies: a location of one point has no size

  OGREnvelope box;
  location.getEnvelope(&box);

  double                                             west = 0;
  double                                             south = 0;
  double                                             east = 0;
  double                                             north = 0;
  const std::unique_ptr<OGRCoordinateTransformation> from_wgs84(to_wgs84.GetInverse());
  if ( from_wgs84 && from_wgs84->TransformBounds(box.MinX, box.MinY, box.MaxX, box.MaxY, &west, &south, &east, &north, POINTS_ALONG_AN_EDGE) != 0 &&
       std::isfinite(west) && std::isfinite(south) && std::isfinite(east) && std::isfinite(north) && west <= east && south <= north ) {
    const double far = std::max({std::abs(west), std::abs(east), std::abs(south), std::abs(north), 1.0}) * AND_AT_LEAST;
    const double across = ((east - west) * WIDER_BY) + far;
    const double down = ((north - south) * WIDER_BY) + far;
    layer.SetSpatialFilterRect(west - across, south - down, east + across, north + down);
  }

  CPLErrorReset();  // a box that could not be placed is not the read's error
}

}  // namespace

void need_geos() {
  if ( ! OGRGeometryFactory::haveGEOS() ) {
    throw std::runtime_error("this GDAL was built without GEOS, so it can't tell which features intersect a location");
  }
}

Prepared narrowed_to(OGRLayer& layer, const geo::Polygon& location, OGRCoordinateTransformation& to_wgs84, const VectorSource& source) {
  need_geos();

  OGRPolygon polygon = ogr_polygon_of(location);
  narrow_to(layer, polygon, to_wgs84);

  Prepared within(OGRCreatePreparedGeometry(OGRGeometry::ToHandle(&polygon)), OGRDestroyPreparedGeometry);
  if ( ! within ) {
    fail("the location can't be tested against the features of '" + source.path + "'");
  }

  return within;
}

bool intersects(std::remove_pointer_t<OGRPreparedGeometryH>& within, OGRGeometry& geometry, const OGRFeature& feature, const VectorSource& source) {
  const bool found = OGRPreparedGeometryIntersects(&within, OGRGeometry::ToHandle(&geometry)) != 0;
  if ( CPLGetLastErrorType() == CE_Failure ) {
    fail("feature " + std::to_string(feature.GetFID()) + " of '" + source.path + "' can't be tested against the location");
  }

  return found;
}

}  // namespace miniverse::gdal::detail
