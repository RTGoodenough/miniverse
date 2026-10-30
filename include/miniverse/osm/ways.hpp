#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "miniverse/model/way.hpp"

/**
 * @file
 * OpenStreetMap files as ways, read with libosmium: a `.osm.pbf` extract, or any other form
 * libosmium opens (`.osm`,
 * `.osm.bz2`), as the `Ways` a `RoadModel` is pushed, or read in its place (road_file.hpp). Each
 * way comes with its node ids, which other formats of the same data drop.
 *
 * @code
 * using Roads = miniverse::Layer<miniverse::RoadModel, "roads">;
 *
 * auto parts = world.begin_push<"roads">(miniverse::Dataset{"region.osm.pbf"}).get();
 * miniverse::osm::read_ways({.path = "region.osm.pbf", .keys = {"highway"}}, 10000,
 * [&](miniverse::Ways roads) { parts.add(std::move(roads)).get();  // a chunk at a time, so the
 * ways are not held whole
 * });
 * parts.commit().get();
 * @endcode
 *
 * - **Which ways**: those with a tag of one of `keys` (`highway` for roads, `railway`, `waterway`),
 * or every way if there are no keys, in the file's order (by id in a sorted file, as OSM's are).
 * Relations are not read.
 * - **A way** has its id, its node ids in order, their positions as a line (longitude and
 * latitude), and its tags as one JSON object of text values (OSM's text is UTF-8; text that is not
 * fails the push). A closed way is a line whose last node is its first again.
 * - **Left out, and counted**: a way with a node the file does not have (an extract cuts ways at
 * its edge), and one of fewer than two nodes.
 * - **Only the map as it is**: a history file (`.osh`), with several versions of each way, is
 * refused.
 * - **Memory**: the position of every node of the file is held while it is read. Read an extract of
 * the area wanted.
 *
 * A file can also be read in place of a table (`road_file`, road_file.hpp).
 *
 * Part of the optional component `miniverse::osm` (CMake: `MINIVERSE_BUILD_OSM`; osm.hpp includes
 * all of it). Errors are `std::runtime_error`: libosmium's own for a file it can't read (a
 * `std::system_error` for one that can't be opened).
 */
namespace miniverse::osm {

/** @brief Which ways to read. */
struct WaySource {
  std::string              path;  ///< The OSM file.
  std::vector<std::string> keys;  ///< Only ways with a tag of one of these keys; empty for every
                                  ///< way.
};

/** @brief How a read went. */
struct WayRead {
  std::size_t ways          = 0;  ///< Handed over.
  std::size_t without_nodes = 0;  ///< Left out, of the ways asked for: one with a node the file
                                  ///< lacks, or of fewer than two.
};

/**
 * @brief Reads `source`'s ways and hands them to `on_chunk`, `chunk_ways` at a time (the last
 * chunk, what is left), on the calling thread.
 * @throws std::invalid_argument if `chunk_ways` is 0.
 * @throws std::runtime_error if the file can't be opened or read, is damaged, or holds a history.
 * Chunks handed over before the error stay handed over. What `on_chunk` throws is passed on.
 */
WayRead read_ways(
    const WaySource& source, std::size_t chunk_ways, const std::function<void(Ways)>& on_chunk);

/** @return Every way of `source`, as above: for a file small enough to hold. */
[[nodiscard]] Ways read_ways(const WaySource& source);

}  // namespace miniverse::osm
