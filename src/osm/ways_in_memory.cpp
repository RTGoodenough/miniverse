#include <boost/geometry/algorithms/correct.hpp>
#include <boost/geometry/algorithms/envelope.hpp>  // IWYU pragma: keep
#include <boost/geometry/algorithms/intersects.hpp>  // IWYU pragma: keep
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/index/parameters.hpp>
#include <boost/geometry/index/predicates.hpp>
#include <boost/geometry/index/rtree.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include "miniverse/geo/types.hpp"
#include "miniverse/layer/road_layer.hpp"
#include "miniverse/osm/ways.hpp"

namespace miniverse::osm {

namespace {

using Box = boost::geometry::model::box<geo::Point>;
using Boxed = std::pair<Box, std::size_t>;  // a way's box, and which way it is
constexpr std::size_t BOXES_TO_A_NODE = 16;
using Boxes = boost::geometry::index::rtree<Boxed, boost::geometry::index::quadratic<BOXES_TO_A_NODE>>;

/** @return The box of each way that has points, with its place among `ways`: a way of none is nowhere, and never found. */
[[nodiscard]] std::vector<Boxed> boxed(const Ways& ways) {
  std::vector<Boxed> boxes;

  boxes.reserve(ways.size());
  for ( std::size_t i = 0; i < ways.size(); ++i ) {
    if ( ! ways.at(i).coordinates.empty() ) {
      boxes.emplace_back(boost::geometry::return_envelope<Box>(ways.at(i).coordinates), i);  // NOLINT(misc-include-cleaner)
    }
  }

  return boxes;
}

}  // namespace

/** @brief The ways' boxes in a tree, for finding the few near a location among many. */
struct WaysInMemory::Index {
  Boxes boxes;
};

WaysInMemory::WaysInMemory(Ways ways) : _ways(std::move(ways)), _index(std::make_unique<const Index>(Index{.boxes = Boxes(boxed(_ways))})) {}

WaysInMemory::~WaysInMemory() = default;

void WaysInMemory::in(const geo::Polygon& location, std::size_t chunk_ways, const std::function<bool(Ways)>& on_chunk) const {
  if ( chunk_ways == 0 ) {
    throw std::invalid_argument("a chunk of ways holds at least one");
  }

  // Boost's test, unlike PostGIS's, minds which way a ring runs round.
  geo::Polygon area = location;
  boost::geometry::correct(area);

  const Box box = boost::geometry::return_envelope<Box>(area);  // NOLINT(misc-include-cleaner)
  if ( box.min_corner().x() > box.max_corner().x() ) {
    return;  // a location of no points holds nothing
  }

  // The ways whose boxes meet the location's, in the order they were given.
  std::vector<std::size_t> nearby;
  for ( auto found = _index->boxes.qbegin(boost::geometry::index::intersects(box)); found != _index->boxes.qend(); ++found ) {
    nearby.push_back(found->second);
  }
  std::ranges::sort(nearby);

  Ways chunk;
  for ( const std::size_t near : nearby ) {
    if ( ! boost::geometry::intersects(_ways.at(near).coordinates, area) ) {  // NOLINT(misc-include-cleaner)
      continue;
    }

    chunk.push_back(_ways.at(near));
    if ( chunk.size() == chunk_ways && ! on_chunk(std::exchange(chunk, {})) ) {
      return;
    }
  }

  if ( ! chunk.empty() ) {
    std::ignore = on_chunk(std::move(chunk));
  }
}

Ways WaysInMemory::in(const geo::Polygon& location) const {
  Ways all;

  in(location, std::numeric_limits<std::size_t>::max(), [&all](Ways chunk) {
    all = std::move(chunk);

    return true;
  });

  return all;
}

}  // namespace miniverse::osm
