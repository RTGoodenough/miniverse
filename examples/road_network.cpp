// A road network of your own, built from an area's roads as they arrive.
//
// miniverse hands a worker the roads of an area; what they are loaded into is the worker's own. Here that is a graph to find
// routes in: the nodes of the roads, each with the nodes a road leads to from it and how far they are. The roads are
// streamed, so the worker holds its graph and a few chunks of roads at a time, never all the roads.
//
// Run with a libpq connection string to a database with PostGIS, on PostgreSQL 17 or newer (which `verify` needs):
//   miniverse_example_road_network "host=localhost dbname=gis user=gis"
// It creates the table miniverse_example_roads, and drops it when it is done.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <numbers>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "schemacht/json/json.hpp"
#include "support.hpp"

namespace geo = miniverse::geo;
using schemacht::json::Json;

struct Roads : miniverse::RoadLayer {};

// ==============================================================================
// The worker's own structure
// ==============================================================================

/** @brief A stretch of road from one node to the next. */
struct Stretch {
  std::int64_t to = 0;
  double       metres = 0;
};

/** @brief The network: for each node, by its id, the stretches of road that leave it. Roads that share a node meet there. */
using RoadNetwork = std::unordered_map<std::int64_t, std::vector<Stretch>>;

/** @return The distance over the ground between two positions (the haversine formula, on a sphere). */
[[nodiscard]] double metres_between(const geo::Point& from, const geo::Point& to) {
  constexpr double EARTH_RADIUS = 6'371'000;
  constexpr double RADIANS = std::numbers::pi / 180;

  const double latitudes = (to.y() - from.y()) * RADIANS;
  const double longitudes = (to.x() - from.x()) * RADIANS;
  const double half_chord =
      std::pow(std::sin(latitudes / 2), 2) + (std::cos(from.y() * RADIANS) * std::cos(to.y() * RADIANS) * std::pow(std::sin(longitudes / 2), 2));

  return 2 * EARTH_RADIUS * std::asin(std::sqrt(half_chord));
}

/** @brief Adds `road` to `network`: a stretch between each of its nodes and the next, both ways. */
void add_road(RoadNetwork& network, const miniverse::Way& road) {
  for ( std::size_t next = 1; next < road.node_ids.size(); ++next ) {
    const double metres = metres_between(road.coordinates.at(next - 1), road.coordinates.at(next));

    network[road.node_ids.at(next - 1)].push_back({.to = road.node_ids.at(next), .metres = metres});
    network[road.node_ids.at(next)].push_back({.to = road.node_ids.at(next - 1), .metres = metres});
  }
}

/** @return How far it is from the node `from` to the node `to` by the shortest way through `network` (Dijkstra's); infinite if there is none. */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- from, then to, as a route reads
[[nodiscard]] double shortest_metres(const RoadNetwork& network, std::int64_t from, std::int64_t to) {
  using Reached = std::pair<double, std::int64_t>;  // how far, and where

  std::unordered_map<std::int64_t, double>                           nearest{{from, 0.0}};
  std::priority_queue<Reached, std::vector<Reached>, std::greater<>> frontier;
  frontier.emplace(0.0, from);

  while ( ! frontier.empty() ) {
    const auto [metres, node] = frontier.top();
    frontier.pop();
    if ( node == to ) {
      return metres;
    }

    if ( metres > nearest.at(node) || ! network.contains(node) ) {
      continue;  // reached by a shorter way since, or a node no road leaves
    }

    for ( const Stretch& stretch : network.at(node) ) {
      const double further = metres + stretch.metres;
      const auto [known, is_new] = nearest.try_emplace(stretch.to, further);
      if ( is_new || further < known->second ) {
        known->second = further;
        frontier.emplace(further, stretch.to);
      }
    }
  }

  return std::numeric_limits<double>::infinity();
}

// ==============================================================================
// The roads an uploader puts in the database
// ==============================================================================

/**
 * @return A village's roads, as OpenStreetMap has them: each a way that names its nodes in order, with a position for each.
 * Ways that meet share a node: High Street and Mill Lane meet at node 2.
 */
[[nodiscard]] miniverse::Ways village() {
  const geo::Point south_west(10.000, 50.000);  // node 1
  const geo::Point south(10.004, 50.000);       // node 2
  const geo::Point south_east(10.008, 50.000);  // node 3
  const geo::Point north_west(10.000, 50.003);  // node 4
  const geo::Point north(10.004, 50.003);       // node 5
  const geo::Point north_east(10.008, 50.003);  // node 6

  return {
      {.id = 101,
       .node_ids = {1, 2, 3},
       .coordinates = {south_west, south, south_east},
       .tags = Json(R"({"highway": "primary", "name": "High Street"})")},
      {.id = 102, .node_ids = {2, 5}, .coordinates = {south, north}, .tags = Json(R"({"highway": "residential", "name": "Mill Lane"})")},
      {.id = 103,
       .node_ids = {4, 5, 6},
       .coordinates = {north_west, north, north_east},
       .tags = Json(R"({"highway": "residential", "name": "Church Road"})")},
      {.id = 104, .node_ids = {3, 6}, .coordinates = {south_east, north_east}, .tags = Json(R"({"highway": "residential", "name": "East Road"})")},
      {.id = 105, .node_ids = {1, 5}, .coordinates = {south_west, north}, .tags = Json(R"({"highway": "footway"})")},  // across the green
  };
}

int main(int argc, char** argv) {
  try {
    miniverse::Miniverse world(example::conninfo(argc, argv), miniverse::Layer<Roads>("miniverse_example_roads"));

    // The uploader's part, done once, somewhere else: examples/import_osm.cpp fills a road table from an OpenStreetMap file.
    world.drop_tables();
    world.create_tables();
    world.push<Roads>(village()).get();

    // The worker's part. When it starts: is the table there, and is it a road table? One that is not fails here, with a
    // `miniverse::TablesDiffer` that says how each table differs, and not at the first load.
    world.verify().get();

    // The roads of the area, handed over a chunk at a time, in the order of their ids. The chunks are small here to show
    // several: left out, a chunk is 1000 roads. A callback that returns `false` stops the load.
    //
    // The callback runs on a thread of the miniverse's, one call at a time, while this thread only waits for the future: so
    // what it fills needs no lock.
    const geo::Polygon area = example::box(9.99, 49.99, 10.02, 50.02);
    RoadNetwork        network;
    std::size_t        roads = 0;
    std::size_t        chunks = 0;

    world
        .stream<Roads>(
            area,
            [&network, &roads, &chunks](const miniverse::Ways& chunk) {
              for ( const miniverse::Way& road : chunk ) {
                add_road(network, road);
              }

              roads += chunk.size();
              ++chunks;
            },
            {.chunk_rows = 2}
        )
        .get();  // once this returns, every chunk has been handed over

    std::cout << "a network of " << network.size() << " nodes, from " << roads << " roads in " << chunks << " chunks\n";

    // By the roads it is along High Street, Mill Lane and Church Road; the footway across the green is shorter.
    std::cout << "from the south-west corner to the north-east: " << std::lround(shortest_metres(network, 1, 6)) << " m\n";

    world.drop_tables();

    return EXIT_SUCCESS;
  } catch ( const std::exception& error ) {
    std::cerr << "error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
