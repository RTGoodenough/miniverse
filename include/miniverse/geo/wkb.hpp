#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "miniverse/geo/types.hpp"

/**
 * Well-known binary (WKB), the form PostGIS stores and sends geometry in, as PostGIS's extended WKB (EWKB): WKB with the
 * geometry's SRID after its type.
 *
 * - **Written** as EWKB, little-endian, with the SRID: what a `geometry` column, or a `$n::geometry` argument in hex, accepts.
 * - **Read** from WKB or EWKB in either byte order, with or without an SRID. An SRID other than the one expected is an error
 *   rather than coordinates silently read in the wrong system.
 * - **Two-dimensional only.** A geometry with Z or M values is refused, in EWKB's flags and ISO WKB's type numbers alike.
 *
 * Malformed input (a wrong type, a short buffer, bytes left over, an empty point) throws `std::invalid_argument`.
 */
namespace miniverse::geo::wkb {

/** @brief A geometry type that WKB can be read into and written from. */
template <typename geometry_t>
concept Geometry = std::same_as<geometry_t, Point> || std::same_as<geometry_t, LineString> || std::same_as<geometry_t, Polygon> ||
                   std::same_as<geometry_t, MultiPolygon>;

/** @return `point` as little-endian EWKB with `srid`. */
[[nodiscard]] std::vector<std::byte> write(const Point& point, std::int32_t srid = WGS84_SRID);
/** @return `line` as little-endian EWKB with `srid`. */
[[nodiscard]] std::vector<std::byte> write(const LineString& line, std::int32_t srid = WGS84_SRID);
/** @return `polygon` as little-endian EWKB with `srid`; its rings are written as they are, closed or not. */
[[nodiscard]] std::vector<std::byte> write(const Polygon& polygon, std::int32_t srid = WGS84_SRID);
/** @return `polygons` as little-endian EWKB with `srid` (the SRID once, on the collection). */
[[nodiscard]] std::vector<std::byte> write(const MultiPolygon& polygons, std::int32_t srid = WGS84_SRID);

/**
 * @return The `geometry_t` that `bytes` hold, as WKB or EWKB.
 * @param srid The SRID the geometry must have if it names one.
 * @throws std::invalid_argument if `bytes` are not a two-dimensional `geometry_t` in `srid` (or with no SRID), or have bytes left over.
 */
template <Geometry geometry_t>
[[nodiscard]] geometry_t read(std::span<const std::byte> bytes, std::int32_t srid = WGS84_SRID);

extern template Point        read<Point>(std::span<const std::byte> bytes, std::int32_t srid);
extern template LineString   read<LineString>(std::span<const std::byte> bytes, std::int32_t srid);
extern template Polygon      read<Polygon>(std::span<const std::byte> bytes, std::int32_t srid);
extern template MultiPolygon read<MultiPolygon>(std::span<const std::byte> bytes, std::int32_t srid);

}  // namespace miniverse::geo::wkb
