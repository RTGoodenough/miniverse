#pragma once

#include <cstdint>
#include <vector>

#include "miniverse/geo/concepts/wkb.hpp"
#include "schemacht/json/json.hpp"

/**
 * @file
 * A feature: what a load of a `FeatureLayer` gives, one by one, and what a reader of features hands over.
 */
namespace miniverse {

/** @brief A feature: one thing of the world as a geometry, with an id and tags. */
template <geo::wkb::Geometry geometry_t>
struct Feature {
  std::int64_t          id = 0;      ///< The table's, from a load; a file's own, from a reader of files.
  geometry_t            geometry{};  ///< Where it is, in WGS 84 longitude and latitude.
  schemacht::json::Json tags{"{}"};  ///< What it is, as one JSON object (`{"building": "yes", ...}`): empty unless given.
};

/** @brief The features a load gives: ordered by id from a table, and in a file's own order from a file. */
template <geo::wkb::Geometry geometry_t>
using Features = std::vector<Feature<geometry_t>>;

}  // namespace miniverse
