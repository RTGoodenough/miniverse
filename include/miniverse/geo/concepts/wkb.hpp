#pragma once

#include <concepts>

#include "miniverse/geo/types.hpp"

namespace miniverse::geo::wkb {

/** @brief A geometry type that WKB can be read into and written from, and that can be a column (geo/column_types.hpp). */
template <typename geometry_t>
concept Geometry = std::same_as<geometry_t, Point> || std::same_as<geometry_t, LineString> || std::same_as<geometry_t, Polygon>;

}  // namespace miniverse::geo::wkb
