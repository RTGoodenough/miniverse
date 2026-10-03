#pragma once

#include <concepts>
#include <cstddef>
#include <span>
#include <vector>

#include "miniverse/geo/concepts/wkb.hpp"  // IWYU pragma: export
#include "miniverse/geo/types.hpp"
#include "schemacht/util/compile_time.hpp"

/**
 * Well-known binary (WKB), the form PostGIS stores and sends geometry in, as PostGIS's extended WKB (EWKB): WKB with the
 * geometry's SRID after its type.
 *
 * - **Written** as EWKB, little-endian, with SRID 4326: what a `geometry` column, or a `$n::geometry` argument in hex, accepts.
 * - **Read** from EWKB in either byte order, and only with SRID 4326. A geometry in another system, or with no SRID at all,
 *   is an error rather than coordinates silently read in the wrong system.
 * - **Two-dimensional only.** A geometry with Z or M values is refused, in EWKB's flags and ISO WKB's type numbers alike.
 *
 * Malformed input (a wrong type, a short buffer, bytes left over, an empty point) throws `std::invalid_argument`.
 */
namespace miniverse::geo::wkb {

/** @return The name WKB and PostGIS give `geometry_t`: `"Point"`, `"LineString"` or `"Polygon"`. */
template <Geometry geometry_t>
[[nodiscard]] consteval auto type_name() {
  if constexpr ( std::same_as<geometry_t, Point> ) {
    return schemacht::util::CTString("Point");

  } else if constexpr ( std::same_as<geometry_t, LineString> ) {
    return schemacht::util::CTString("LineString");

  } else {
    return schemacht::util::CTString("Polygon");
  }
}

/** @return `geometry` as little-endian EWKB with SRID 4326. A polygon's rings are written as they are, closed or not. */
template <Geometry geometry_t>
[[nodiscard]] std::vector<std::byte> write(const geometry_t& geometry);

/**
 * @return The `geometry_t` that `bytes` hold, as EWKB.
 * @throws std::invalid_argument if `bytes` are not a two-dimensional `geometry_t` with SRID 4326, or have bytes left over.
 */
template <Geometry geometry_t>
[[nodiscard]] geometry_t read(std::span<const std::byte> bytes);

extern template std::vector<std::byte> write<Point>(const Point& geometry);
extern template std::vector<std::byte> write<LineString>(const LineString& geometry);
extern template std::vector<std::byte> write<Polygon>(const Polygon& geometry);

extern template Point      read<Point>(std::span<const std::byte> bytes);
extern template LineString read<LineString>(std::span<const std::byte> bytes);
extern template Polygon    read<Polygon>(std::span<const std::byte> bytes);

}  // namespace miniverse::geo::wkb
