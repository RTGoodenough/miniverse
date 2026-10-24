#pragma once

#include <cstddef>
#include <stdexcept>

#include "miniverse/geo/types.hpp"

/**
 * @file
 * What a location must be: what a reader's load holds a polygon to, which is what a table's load would refuse, so that a
 * worker tried on files does not fail on tables.
 */
namespace miniverse {

/**
 * @brief Throws unless `location` is a polygon every table's load takes: each of its rings closed (its last point its first
 * again), and of four points or more. PostGIS can't test an unclosed ring against a line, an area or a raster's tile, and
 * fails the load when it comes to one (against points it has a test of its own, which does not mind); a miniverse refuses
 * such a location for every layer with a reader, before the reader is asked, so that a worker tried on files does not fail
 * on tables. A polygon of no points at all is taken: nothing is in it.
 * @throws std::invalid_argument for such a ring.
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
