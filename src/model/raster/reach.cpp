#include "miniverse/model/raster/reach.hpp"

#include <boost/geometry/algorithms/correct.hpp>
#include <boost/geometry/algorithms/intersects.hpp>  // IWYU pragma: keep

#include <utility>

#include "miniverse/geo/snap.hpp"
#include "miniverse/geo/types.hpp"

namespace miniverse::raster {

Reach::Reach(geo::Polygon location) : _location(std::move(location)) {
  boost::geometry::correct(_location);  // Boost's test, unlike PostGIS's, minds which way a ring runs round
}

bool Reach::into(const geo::Box& tile) const {
  // The tile without its edges: how far in a location must reach, see the class.
  const double across = (tile.max_corner().x() - tile.min_corner().x()) * geo::EDGE_TOLERANCE;
  const double down = (tile.max_corner().y() - tile.min_corner().y()) * geo::EDGE_TOLERANCE;

  const geo::Box inside{
      {tile.min_corner().x() + across, tile.min_corner().y() + down},
      {tile.max_corner().x() - across, tile.max_corner().y() - down},
  };

  return boost::geometry::intersects(inside, _location);  // NOLINT(misc-include-cleaner)
}

}  // namespace miniverse::raster
