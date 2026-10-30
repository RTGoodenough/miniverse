#pragma once

#include <ogr_feature.h>
#include <ogr_geometry.h>
#include <ogr_spatialref.h>

#include <gdal_priv.h>

#include <memory>
#include <ogrsf_frmts.h>

#include "miniverse/gdal/vector.hpp"

/**
 * @file
 * A vector file as a source of features: opened, its layer found, and its coordinates brought to
 * WGS 84. Not installed.
 */
namespace miniverse::gdal::detail {

/** @return `source`'s file, opened for reading its features. */
[[nodiscard]] GDALDatasetUniquePtr opened(const VectorSource& source);

/** @return The layer of `dataset` that `source` asks for: the one it names, or the only one there
 * is. */
[[nodiscard]] OGRLayer& layer_of(GDALDataset& dataset, const VectorSource& source);

/** @return The transformation from `layer`'s coordinate system to WGS 84 longitude and latitude. */
[[nodiscard]] std::unique_ptr<OGRCoordinateTransformation> to_wgs84_from(
    OGRLayer& layer, const VectorSource& source);

/**
 * @return The geometry of `feature` in WGS 84, flat and with its arcs as lines, of whatever type it
 * is; `stolen` is the feature's own, taken from it.
 */
[[nodiscard]] std::unique_ptr<OGRGeometry> in_wgs84(std::unique_ptr<OGRGeometry> stolen,
                                                    const OGRFeature&            feature,
                                                    OGRCoordinateTransformation& to_wgs84,
                                                    const VectorSource&          source);

}  // namespace miniverse::gdal::detail
