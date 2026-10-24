#pragma once

#include "miniverse/geo/types.hpp"

/**
 * @file
 * Which tiles of a grid a location has: those it reaches into, not those it only touches.
 */
namespace miniverse::raster {

/**
 * @brief Which tiles of a grid a location has: those it reaches into. A load of a raster layer reads these and no others,
 * from a table or from a file, so a long thin area that runs askew costs its own tiles, not all the tiles of its box.
 *
 * A tile the location only touches, at an edge or a corner, is not one of them: no pixel of the location is in it. Touching
 * is told to a billionth of a tile's size, since a tile's edge, worked out in degrees, comes a rounding step to one side of
 * where a location that ends on it was written: a location that reaches no further into a tile than that is not in it. So a
 * location of no area, a point or a line, that lies along the line between tiles has no tile at all.
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
