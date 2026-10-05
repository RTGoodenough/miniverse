#pragma once

#include <cstdint>

#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/linestring.hpp>
#include <boost/geometry/geometries/multi_linestring.hpp>
#include <boost/geometry/geometries/multi_polygon.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>

/**
 * The geometry miniverse reads and writes: Boost.Geometry's models, in longitude and latitude degrees (WGS 84, SRID 4326).
 *
 * They are plain Boost.Geometry types, so its algorithms (`boost::geometry::envelope`, `intersects`, `length`, ...) work on
 * them as they are. Coordinates are `x` = longitude and `y` = latitude, the order PostGIS uses.
 */
namespace miniverse::geo {

/** @brief The SRID of every geometry miniverse stores: WGS 84 longitude and latitude. */
inline constexpr std::int32_t WGS84_SRID = 4326;

/** @brief A position: `x()` is the longitude, `y()` the latitude, in degrees. */
using Point = boost::geometry::model::d2::point_xy<double>;

/** @brief A box between two longitudes and two latitudes: `min_corner()` is its south-west corner, `max_corner()` its north-east. */
using Box = boost::geometry::model::box<Point>;

/** @brief A line through points, such as a road. */
using LineString = boost::geometry::model::linestring<Point>;

/**
 * @brief An area: an outer ring and any holes. Rings are closed (the last point repeats the first) and the outer ring runs
 * counter-clockwise, as in OGC simple features and GeoJSON. PostGIS does not care about the direction; Boost.Geometry's
 * area and validity algorithms do (`boost::geometry::correct` fixes a ring that runs the other way).
 */
using Polygon = boost::geometry::model::polygon<Point, false, true>;

/** @brief Several lines as one geometry, such as a river with its branches: a `std::vector` of `LineString`. */
using MultiLineString = boost::geometry::model::multi_linestring<LineString>;

/** @brief Several areas as one geometry, such as a country with its islands: a `std::vector` of `Polygon`. */
using MultiPolygon = boost::geometry::model::multi_polygon<Polygon>;

}  // namespace miniverse::geo
