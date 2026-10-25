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
 * miniverse::gdal::read_features<miniverse::geo::MultiPolygon>({.path = "buildings.gpkg"}, 10000, [&](auto buildings) {
 *   world.push<Buildings>(std::move(buildings)).get();  // a chunk at a time: the file is never held whole
 * });
 * @endcode
 *
 * - **Coordinates** are transformed to WGS 84 longitude and latitude from the layer's own system; a layer that names none is
 *   refused. Z and M values are dropped.
 * - **Geometries** must be of the type asked for. A single geometry is promoted where a multi-geometry is asked for (a
 *   polygon read as a `MultiPolygon` of one), so a dataset that mixes the two reads as the multi type; a multi-geometry of
 *   one part reads as its part where the single type is asked for; curves are made into lines. Anything else is an error
 *   that names the feature: a multipolygon of several parts is never made into one polygon. A feature with no geometry, or
 *   an empty one, is left out and counted.
 *   Rings run as they do in the file: a shapefile's outer rings are clockwise, and `boost::geometry::correct` turns them.
 * - **Tags** are the feature's other fields as one JSON object, each under its field's name: whole numbers and reals as
 *   numbers, booleans as `true` and `false`, everything else as the text OGR writes it as (a date as `2026/10/04`, a list as
 *   `(2:a,b)`, a real that is not a number as `nan`), and a field that is unset or NULL left out. Of two fields of one name
 *   the later is kept.
 * - **Ids** are OGR's own (FID), or a whole-number field's. A feature with neither is an error.
 *
 * A file can also be a layer itself, read in place of a table (`feature_file`, feature_file.hpp): for a test, or a user with
 * files alone.
 *
 * Part of the optional component `miniverse::gdal` (CMake: `MINIVERSE_BUILD_GDAL`; gdal.hpp includes all of it); the rest of
 * miniverse does not need GDAL.
 * Errors are `std::runtime_error` with GDAL's own message. While a read runs, GDAL's errors on the calling thread are kept
 * for those messages, not printed: also the ones GDAL calls made inside `on_chunk` raise.
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
 * @brief Reads `source`'s features and hands them to `on_chunk`, `chunk_features` at a time (the last of what is left), in
 * the file's order, on the calling thread.
 * @throws std::invalid_argument if `chunk_features` is 0.
 * @throws std::runtime_error if the file can't be opened, has no such layer or id field (or several layers and none named),
 * names no coordinate system, or holds a feature that is not a `geometry_t` or has no id. Chunks handed over before an error
 * stay handed over. What `on_chunk` throws is passed on.
 */
template <geo::wkb::Geometry geometry_t>
VectorRead read_features(const VectorSource& source, std::size_t chunk_features, const std::function<void(Features<geometry_t>)>& on_chunk);

/** @return Every feature of `source`, as above: for a file small enough to hold. Features with no geometry are left out. */
template <geo::wkb::Geometry geometry_t>
[[nodiscard]] Features<geometry_t> read_features(const VectorSource& source);

/**
 * @brief Reads the features of `source` that intersect `location`, a polygon in WGS 84, and hands them to `on_chunk`,
 * `chunk_features` at a time (the last of what is left), in the file's order, on the calling thread, until it returns `false`.
 *
 * Which features intersect is the test PostGIS's `ST_Intersects` makes of the same geometries: on the features as they are
 * in WGS 84, by GEOS, which GDAL must have been built with (PostGIS has code of its own for some pairs, which may differ by
 * rounding on a location's very edge). The file's own spatial index, if it has one (a GeoPackage's, a FlatGeobuf's, a
 * shapefile's `.qix`), keeps the read to the features near the location; without one every feature is read and tested. A
 * feature in the location that is not a `geometry_t` is an error, as in `read_features`; one with no geometry is nowhere,
 * and is not counted. "The file's order" is the order its driver reads it in: for a GeoPackage, by feature id.
 * @throws std::invalid_argument and std::runtime_error as `read_features`; std::runtime_error if GDAL has no GEOS.
 */
template <geo::wkb::Geometry geometry_t>
VectorRead read_features_in(
    const VectorSource& source, const geo::Polygon& location, std::size_t chunk_features, const std::function<bool(Features<geometry_t>)>& on_chunk
);

/**
 * @return What keeps `source` from being read as features of `geometry_t`, each in words: a file that can't be opened, a
 * layer or id field it lacks, no coordinate system, a GDAL without GEOS, and geometries of another type where the layer
 * says what its are (a GeoJSON file of several types does not, and a load of it fails at the feature). Nothing, if it can
 * be: the features themselves are not read.
 */
template <geo::wkb::Geometry geometry_t>
[[nodiscard]] std::vector<std::string> problems_of(const VectorSource& source);

// Each is defined for each geometry type: Point, LineString, Polygon, MultiLineString and MultiPolygon.

}  // namespace miniverse::gdal
