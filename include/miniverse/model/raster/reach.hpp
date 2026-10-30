#pragma once

#include "miniverse/geo/types.hpp"

/**
 * @file
 * Which tiles of a grid a location has: those it reaches into, not those it only touches.
 */
namespace miniverse::raster {

/**
 * @brief Which tiles of a grid a location has: those it reaches into. A load, from a table or a file, reads these and no
 * others, so a long thin area that runs askew costs its own tiles, not all the tiles of its box. A tile the location only
 * touches, at an edge or a corner, is not one of them. Touching is told to `geo::EDGE_TOLERANCE` of a tile, since a tile's
 * edge worked out in degrees lands a rounding step from where a location that ends on it was written. A location of no area
 * that lies along the line between tiles has no tile.
 */
class Reach {
 public:
  /** @param location A polygon in WGS 84. Its rings may run either way round. */
  explicit Reach(geo::Polygon location);

  /** @return Whether the location reaches into `tile`, a tile's outline. */
  [[nodiscard]] bool into(const geo::Box& tile) const;

  Reach(const Reach&) = default;
  Reach(Reach&&) = default;
  Reach& operator=(const Reach&) = default;
  Reach& operator=(Reach&&) = default;
  ~Reach() = default;

 private:
  geo::Polygon _location;
};

}  // namespace miniverse::raster
