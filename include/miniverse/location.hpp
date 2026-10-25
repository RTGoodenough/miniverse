#pragma once

#include <cstddef>
#include <stdexcept>

#include "miniverse/geo/types.hpp"

/**
 * @file
 * What a location must be: the check a reader's load makes of its polygon, refusing before it reads what PostGIS would fail a
 * table's load on.
 */
namespace miniverse {

/**
 * @brief Throws unless every ring of `location` is closed (its last point its first again) and has four points or more: what
 * PostGIS requires of a polygon tested against lines, areas and raster tiles (points it tests regardless). A reader's load
 * checks this so that a worker tried on files behaves as on tables. A polygon of no points passes: nothing is in it.
 * @throws std::invalid_argument for a ring that is not so.
 */
inline void check_location(const geo::Polygon& location) {
  const auto check = [](const auto& ring) {
    constexpr std::size_t FEWEST_POINTS = 4;
    if ( ring.empty() ) {
      return;
    }

    if ( ring.size() < FEWEST_POINTS || ring.front().x() != ring.back().x() || ring.front().y() != ring.back().y() ) {
      throw std::invalid_argument("a ring of a location must be closed, its last point its first again, and have four points or more");
    }
  };

  check(location.outer());
  for ( const auto& hole : location.inners() ) {
    check(hole);
  }
}

}  // namespace miniverse
