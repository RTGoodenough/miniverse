#pragma once

#include <cstdint>
#include <vector>

#include "miniverse/geo/types.hpp"
#include "schemacht/json/json.hpp"

/**
 * @file
 * A way: what a load of a `RoadLayer` gives, one by one, and what a reader of ways hands over.
 */
namespace miniverse {

/** @brief A road: an OpenStreetMap way, as a line through its nodes. */
struct Way {
  std::int64_t              id = 0;       ///< The way's own id, the source's: OpenStreetMap's way id, in a table as in a file.
  std::vector<std::int64_t> node_ids;     ///< The way's nodes, in order: one per point of `coordinates`.
  geo::LineString           coordinates;  ///< Where the nodes are.
  schemacht::json::Json     tags{"{}"};   ///< The way's tags, as one JSON object (`{"highway": "primary", ...}`): empty unless given.
};

/** @brief The roads a load gives: ordered by id from a table, and in a file's own order from a file. */
using Ways = std::vector<Way>;

}  // namespace miniverse
