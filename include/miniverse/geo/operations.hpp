#pragma once

#include <string_view>

#include "miniverse/geo/column_types.hpp"
#include "miniverse/geo/types.hpp"

/**
 * @file
 * PostGIS tests for schemacht's query builder: `Ways::col<"geom">().apply<geo::Intersects>(query::arg<0>())`. Each names
 * its SQL function or operator and the type of its argument, which may differ from the column's (a polygon tested against a line).
 */
namespace miniverse::geo {

/**
 * @brief `ST_Intersects("column", $n::geometry(Polygon,4326))`: the geometry shares any point with a polygon. PostGIS 3 gives
 * the function a planner support function, so a GiST index on the column answers it.
 */
struct Intersects {
  static constexpr std::string_view FUNCTION = "ST_Intersects";
  using argument_type = Polygon;
};

/**
 * @brief `ST_Intersects(ST_ConvexHull("column"), $n::geometry(Polygon,4326))`: a raster's outline shares a point with a
 * polygon. A GiST index on `ST_ConvexHull(rast)` answers it. PostGIS's own `ST_Intersects(raster, geometry)` tests the same
 * outline through PL/pgSQL for every candidate tile, and is slower.
 */
struct OutlineIntersects {
  static constexpr std::string_view FUNCTION = "ST_Intersects";
  static constexpr std::string_view COLUMN_PREFIX = "ST_ConvexHull(";
  static constexpr std::string_view COLUMN_SUFFIX = ")";
  using argument_type = Polygon;
};

}  // namespace miniverse::geo
