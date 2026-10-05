#pragma once

#include <string_view>

#include "miniverse/geo/column_types.hpp"
#include "miniverse/geo/types.hpp"

/**
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
 * @brief `"column" && $n::geometry(Polygon,4326)`: the column's bounding box and the polygon's intersect. On a `raster` column
 * the box is the tile's outline (`ST_ConvexHull`), so a GiST index on `ST_ConvexHull(rast)` answers it, as `raster2pgsql -I`
 * makes one. Every tile that touches the polygon's box matches, even one that only shares an edge.
 */
struct BoxesIntersect {
  static constexpr std::string_view OPERATOR = "&&";
  using argument_type = Polygon;
};

}  // namespace miniverse::geo
