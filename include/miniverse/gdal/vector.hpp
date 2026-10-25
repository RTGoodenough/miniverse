#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "miniverse/geo/concepts/wkb.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/layer/feature.hpp"

/**
 * @file
 * Vector files as features, read with GDAL's OGR: a GeoPackage, a shapefile, GeoJSON, or anything else OGR opens, as the
 * `Features` a `FeatureLayer` is pushed, or read in its place (feature_file.hpp).
 *
 * @code
 * struct Buildings : miniverse::FeatureLayer<miniverse::geo::MultiPolygon> {};
 *
 * auto parts = world.begin_push<Buildings>(miniverse::Dataset{"buildings.gpkg"}).get();
 * miniverse::gdal::read_features<miniverse::geo::MultiPolygon>({.path = "buildings.gpkg"}, 10000, [&](auto buildings) {
 *   parts.add(std::move(buildings)).get();  // a chunk at a time, so the file is not held whole
 * });
 * parts.commit().get();
 * @endcode
 *
 * - **Coordinates** are transformed to WGS 84 longitude and latitude from the layer's own system; a layer that names none is
 *   refused. Z and M values are dropped.
 * - **Geometries** must be of the type asked for. A single geometry is promoted to the multi type asked for (a polygon reads
 *   as a `MultiPolygon` of one), a multi-geometry of one part reads as the single type asked for, and curves become lines.
 *   Anything else is an error naming the feature. A feature with no geometry, or an empty one, is left out and counted.
 *   Rings run as they do in the file (a shapefile's outer rings are clockwise).
  * - **Tags** are the feature's fields but the id field, as one JSON object under their field names: whole numbers and reals as numbers,
 *   booleans as `true` and `false`, everything else as the text OGR writes (a date as `2026/10/04`, a list as `(2:a,b)`, a
 *   real that is not a number as `nan`). A field that is unset or NULL is left out. Of two fields of one name the later is kept.
 * - **Ids** are OGR's own (FID), or a whole-number field's. A feature with neither is an error.
 *
 * A file can also be read in place of a table (`feature_file`, feature_file.hpp).
 *
 * Part of the optional component `miniverse::gdal` (CMake: `MINIVERSE_BUILD_GDAL`; gdal.hpp includes all of it). Errors are
 * `std::runtime_error` with GDAL's own message; GDAL's errors on the calling thread are kept quiet while a read runs,
 * including from GDAL calls made inside `on_chunk`.
 */
namespace miniverse::gdal {

/** @brief Which features to read. */
struct VectorSource {
  std::string path;      ///< The file, or anything else OGR opens.
  std::string layer;     ///< The layer to read, by name; empty for a file with one layer.
  std::string id_field;  ///< The whole-number field that is each feature's id; empty for OGR's own feature id (FID).
};

/** @brief How a read went. */
struct VectorRead {
  std::size_t features = 0;          ///< Handed over.
  std::size_t without_geometry = 0;  ///< Left out: a feature with no geometry, or an empty one.
};

/**
 * @brief Reads `source`'s features and hands them to `on_chunk`, `chunk_features` at a time (the last chunk, what is left), in
 * the file's order, on the calling thread.
 * @throws std::invalid_argument if `chunk_features` is 0.
 * @throws std::runtime_error if the file can't be opened, has no such layer or id field (or several layers and none named),
 * names no coordinate system, or holds a feature that is not a `geometry_t` or has no id. Chunks handed over before the error
 * stay handed over. What `on_chunk` throws is passed on.
 */
template <geo::wkb::Geometry geometry_t>
VectorRead read_features(const VectorSource& source, std::size_t chunk_features, const std::function<void(Features<geometry_t>)>& on_chunk);

/** @return Every feature of `source`, as above: for a file small enough to hold. Features with no geometry are left out. */
template <geo::wkb::Geometry geometry_t>
[[nodiscard]] Features<geometry_t> read_features(const VectorSource& source);

/**
 * @brief Reads the features of `source` that intersect `location`, a polygon in WGS 84, and hands them to `on_chunk`,
 * `chunk_features` at a time, in the file's order (for a GeoPackage, by feature id), on the calling thread, until it returns
 * `false`. Which features intersect is GEOS's test of the geometries in WGS 84 (GDAL must be built with GEOS); it agrees with
 * PostGIS's `ST_Intersects` but for rounding on a location's very edge. The file's spatial index, if it has one, keeps the read to the features near
 * the
 * location. Only a feature in the location must be a `geometry_t`; one with no geometry is nowhere, and not counted.
 * @throws std::invalid_argument and std::runtime_error as `read_features`; std::runtime_error if GDAL has no GEOS.
 */
template <geo::wkb::Geometry geometry_t>
VectorRead read_features_in(
    const VectorSource& source, const geo::Polygon& location, std::size_t chunk_features, const std::function<bool(Features<geometry_t>)>& on_chunk
);

/**
 * @return What keeps `source` from being read as features of `geometry_t`, each in words: a file that can't be opened, a layer
 * or id field it lacks, no coordinate system, a GDAL without GEOS, or geometries of another type where the layer declares
 * one (a GeoJSON file of several types does not, and a load of it fails at the feature). The features are not read.
 */
template <geo::wkb::Geometry geometry_t>
[[nodiscard]] std::vector<std::string> problems_of(const VectorSource& source);

// Each is defined for each geometry type: Point, LineString, Polygon, MultiLineString and MultiPolygon.

}  // namespace miniverse::gdal
