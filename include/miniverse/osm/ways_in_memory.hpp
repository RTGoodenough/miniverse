#pragma once

#include <cstddef>
#include <functional>
#include <memory>

#include "miniverse/geo/types.hpp"
#include "miniverse/layer/way.hpp"

/**
 * @file
 * Ways held in memory and found by location: the index a road file is read into. Not part of the API: `road_file` is.
 */
namespace miniverse::osm::detail {

/**
 * @brief Ways held in memory and found by location: what a road file is read into, since an OpenStreetMap file has no index
 * of where its ways are. Its members are safe to call from several threads at once.
 */
class WaysInMemory {
 public:
  explicit WaysInMemory(Ways ways);

  /**
   * @brief Hands the ways that intersect `location`, a polygon in WGS 84 with closed rings, to `on_chunk`, `chunk_ways` at a
   * time (the last of what is left), in the order the ways were given, until it returns `false`. Which intersect is what
   * PostGIS's `ST_Intersects` says of the same lines, for a polygon that is valid, and but for rounding on its very edge; a
   * way of no points intersects nothing.
   * @throws std::invalid_argument if `chunk_ways` is 0.
   */
  void in(const geo::Polygon& location, std::size_t chunk_ways, const std::function<bool(Ways)>& on_chunk) const;

  /** @return The ways that intersect `location`, as above. */
  [[nodiscard]] Ways in(const geo::Polygon& location) const;

 private:
  struct Index;  // the ways' boxes in a tree, in the library: its headers are heavy

  Ways                         _ways;
  std::unique_ptr<const Index> _index;

 public:
  WaysInMemory(const WaysInMemory&) = delete;
  WaysInMemory(WaysInMemory&&) = delete;
  WaysInMemory& operator=(const WaysInMemory&) = delete;
  WaysInMemory& operator=(WaysInMemory&&) = delete;
  ~WaysInMemory();
};

}  // namespace miniverse::osm::detail
