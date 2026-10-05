#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/geo/types.hpp"
#include "miniverse/layer.hpp"
#include "miniverse/layer/road_layer.hpp"
#include "miniverse/osm/concepts/ways.hpp"  // IWYU pragma: export
#include "miniverse/reader.hpp"

/**
 * OpenStreetMap files as ways, read with libosmium: a `.osm.pbf` extract, or any other form libosmium opens (`.osm`,
 * `.osm.bz2`), as the `Ways` a `RoadLayer` is pushed: each way with its node ids, which other formats of the same data drop.
 *
 * @code
 * struct Roads : miniverse::RoadLayer {};
 *
 * miniverse::osm::read_ways({.path = "region.osm.pbf", .keys = {"highway"}}, 10000, [&](miniverse::Ways roads) {
 *   world.push<Roads>(std::move(roads)).get();  // a chunk at a time: the ways are never held whole
 * });
 * @endcode
 *
 * - **Which ways**: those with a tag of one of `keys` (`highway` for roads, `railway`, `waterway`), or every way if there are
 *   no keys; in the file's order, which is by id in a file that is sorted, as OSM's are. Relations are not read.
 * - **A way** has its id, the ids of its nodes in order, their positions as a line (longitude and latitude, as OSM has
 *   them), and its tags as one JSON object of text values, as the file has them (OSM's text is UTF-8; text that is not
 *   fails the push). A closed way, a roundabout or an area's outline, is a line whose last node is its first again.
 * - **Left out, and counted**: of the ways asked for, one with a node the file does not have (an extract cuts ways at its
 *   edge), and one of fewer than two nodes, which is not a line. A file with its ways before its nodes, which no OSM file
 *   is, has every way left out so.
 * - **Only the map as it is**: a history file (`.osh`), with several versions of each way, is refused.
 * - **Memory**: the position of every node of the file is held while it is read, 16 bytes each and for a while twice that,
 *   since a way names its nodes only by id: several gigabytes for a large country. Read an extract of the area wanted.
 *
 * A file can also be a layer itself, read in place of a table (`road_file`, at the end of this file): for a test, or a user
 * with files alone.
 *
 * This is the optional component `miniverse::osm` (CMake: `MINIVERSE_BUILD_OSM`); the rest of miniverse does not need
 * libosmium. Errors are `std::runtime_error`: libosmium's own for a file it can't read (a `std::system_error` for one that
 * can't be opened).
 */
namespace miniverse::osm {

/** @brief Which ways to read. */
struct WaySource {
  std::string              path;  ///< The OSM file.
  std::vector<std::string> keys;  ///< Only ways with a tag of one of these keys; empty for every way.
};

/** @brief How a read went. */
struct WayRead {
  std::size_t ways = 0;           ///< Handed over.
  std::size_t without_nodes = 0;  ///< Left out, of the ways asked for: one with a node the file lacks, or of fewer than two.
};

/**
 * @brief Reads `source`'s ways and hands them to `on_chunk`, `chunk_ways` at a time (the last of what is left), on the
 * calling thread.
 * @throws std::invalid_argument if `chunk_ways` is 0.
 * @throws std::runtime_error if the file can't be opened or read, is damaged, or holds a history. Chunks handed over before
 * an error stay handed over. What `on_chunk` throws is passed on.
 */
WayRead read_ways(const WaySource& source, std::size_t chunk_ways, const std::function<void(Ways)>& on_chunk);

/** @return Every way of `source`, as above: for a file small enough to hold. */
[[nodiscard]] Ways read_ways(const WaySource& source);

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

/**
 * @brief The reader of ways held in memory as a layer of `kind_t`: what `road_file` makes a layer with, from a file's. A load
 * gives the ways that intersect the location, in the order they were given (a table's are by id), their tags as they were
 * given (a table's are as `jsonb` prints them: the same document, another text). There is nothing left to go wrong once it
 * is made, so `Miniverse::verify` finds nothing to say of it.
 */
template <WayKind kind_t>
class RoadFile : public Reader<kind_t> {
 public:
  /** @param name What the ways are called in a message: the file they were read from. */
  RoadFile(std::string name, Ways ways) : _name(std::move(name)), _ways(std::move(ways)) {}

  [[nodiscard]] std::string name() const override { return _name; }

  [[nodiscard]] Ways load(const geo::Polygon& location) const override { return _ways.in(location); }

  void stream(const geo::Polygon& location, std::size_t chunk_rows, const Reader<kind_t>::OnChunk& on_chunk) const override {
    _ways.in(location, chunk_rows, on_chunk);
  }

 private:
  std::string  _name;
  WaysInMemory _ways;

 public:
  RoadFile(const RoadFile&) = delete;
  RoadFile(RoadFile&&) = delete;
  RoadFile& operator=(const RoadFile&) = delete;
  RoadFile& operator=(RoadFile&&) = delete;
  ~RoadFile() override = default;
};

/**
 * @return The layer of `kind_t` that is `source`, an OpenStreetMap file, read in place of a table:
 *
 * @code
 * miniverse::Miniverse world(miniverse::osm::road_file<Roads>({.path = "town.osm.pbf", .keys = {"highway"}}));
 * world.load<Roads>(area).get();  // the file's roads that intersect the area
 * @endcode
 *
 * The whole file is read here, and its ways (those `source` asks for) held in memory for as long as the layer lives: fine for
 * a town or a state, where a country belongs in the database. Ways left out by `read_ways` are left out here.
 * @throws std::runtime_error as `read_ways`, if the file can't be read.
 */
template <WayKind kind_t>
[[nodiscard]] Layer<kind_t> road_file(const WaySource& source) {
  return Layer<kind_t>(std::make_shared<RoadFile<kind_t>>("'" + source.path + "'", read_ways(source)));
}

}  // namespace miniverse::osm
