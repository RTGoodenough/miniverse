#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

/**
 * @file
 * Coordinates measured in pixels or tiles, against the edges between them: how far from an edge a
 * coordinate worked out in floating point may land and still count as on it, and such coordinates
 * snapped to whole numbers.
 */
namespace miniverse::geo {

/**
 * @brief How far from a pixel's or a tile's edge a coordinate may lie, in pixels or tiles, and
 * still count as on it: a millionth. A position on the grid worked out in degrees lands a rounding
 * step from where it was written (an ulp of 180 degrees is 3e-14 degrees, a billionth of the finest
 * pixel), and nothing a pixel shows is a millionth of a pixel wide. Every test of a position
 * against an edge uses this: a raster's pixels, a grid's tiles, a location's reach, two pixel
 * sizes.
 */
inline constexpr double EDGE_TOLERANCE = 1e-6;

namespace detail {

/** @brief The furthest coordinate kept as it is: a double this far is still a whole number of
 * pixels, and fits a 64-bit integer. */
inline constexpr double FURTHEST = 4'503'599'627'370'496.0;  // 2^52

/** @return `along` within `FURTHEST` either way; 0 if it is no number, which is the caller's to
 * refuse. */
[[nodiscard]] inline double kept(double along)
{
  return std::isfinite(along) ? std::clamp(along, -FURTHEST, FURTHEST) : 0.0;
}

}  // namespace detail

/**
 * @return `along`, a coordinate in pixels or tiles, as the whole number it is to within
 * `EDGE_TOLERANCE`; nothing if it is not one, or is no number, or lies further than any grid
 * reaches.
 */
[[nodiscard]] inline std::optional<std::int64_t> whole(double along)
{
  const double nearest = std::round(along);
  if ( ! std::isfinite(along) ||
       std::abs(along) > detail::FURTHEST ||
       std::abs(along - nearest) > EDGE_TOLERANCE )
  {
    return std::nullopt;
  }

  return static_cast<std::int64_t>(nearest);
}

/** @return `along` rounded down to a whole number, or to the nearest if it is one but for rounding.
 */
[[nodiscard]] inline std::int64_t floor_snapped(double along)
{
  const double along_kept = detail::kept(along);

  return whole(along_kept).value_or(static_cast<std::int64_t>(std::floor(along_kept)));
}

/** @return `along` rounded up to a whole number, or to the nearest if it is one but for rounding.
 */
[[nodiscard]] inline std::int64_t ceil_snapped(double along)
{
  const double along_kept = detail::kept(along);

  return whole(along_kept).value_or(static_cast<std::int64_t>(std::ceil(along_kept)));
}

/** @return Whether `size` and `other`, two pixel sizes, are the same size to within
 * `EDGE_TOLERANCE` of `other`. */
[[nodiscard]] inline bool same_size(double size, double other)
{
  return std::abs(size - other) <= EDGE_TOLERANCE * std::abs(other);
}

}  // namespace miniverse::geo
