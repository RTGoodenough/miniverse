#pragma once

#include <ogr_api.h>
#include <ogr_feature.h>
#include <ogr_geometry.h>
#include <ogr_spatialref.h>

#include <memory>
#include <ogrsf_frmts.h>
#include <type_traits>

#include "miniverse/gdal/vector.hpp"
#include "miniverse/geo/types.hpp"

/**
 * @file
 * The features of a layer that intersect a location: the layer narrowed to the location's box by
 * its own index, and the location made ready in GEOS for the test of each feature. Not installed.
 */
namespace miniverse::gdal::detail {

/** @brief Throws unless GDAL can test whether two geometries intersect: without GEOS it would
 * compare their boxes, and say nothing. */
void need_geos();

/** @brief A location as GEOS has it, made ready once for the test of every feature. */
using Prepared =
    std::unique_ptr<std::remove_pointer_t<OGRPreparedGeometryH>, void (*)(OGRPreparedGeometryH)>;

/** @return `location`, a polygon in WGS 84, ready to test features against; `layer` reads only the
 * features near it from here on. */
[[nodiscard]] Prepared narrowed_to(OGRLayer&                    layer,
                                   const geo::Polygon&          location,
                                   OGRCoordinateTransformation& to_wgs84,
                                   const VectorSource&          source);

/** @return Whether `geometry`, the one of `feature` in WGS 84, intersects the location `within`. */
[[nodiscard]] bool intersects(std::remove_pointer_t<OGRPreparedGeometryH>& within,
                              OGRGeometry&                                 geometry,
                              const OGRFeature&                            feature,
                              const VectorSource&                          source);

}  // namespace miniverse::gdal::detail
