#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "miniverse/geo/types.hpp"
#include "miniverse/layer.hpp"
#include "miniverse/layer/way.hpp"
#include "miniverse/osm/concepts/ways.hpp"  // IWYU pragma: export
#include "miniverse/osm/ways.hpp"
#include "miniverse/osm/ways_in_memory.hpp"
#include "miniverse/reader.hpp"

/**
 * @file
 * An OpenStreetMap file as a layer: its ways read once (ways.hpp) and held in memory (ways_in_memory.hpp), in place of a table.
 */
namespace miniverse::osm {

/**
 * @brief The reader of ways held in memory as a layer of `kind_t`: what `road_file` makes from a file's ways, and a program can
 * make from ways of its own. A load gives the ways that intersect the location, in the order they were given, with their tags
 * as given. `Miniverse::verify` has nothing to check once it is made.
 */
template <WayKind kind_t>
class RoadFile : public Reader<kind_t> {
 public:
  /**
   * @param name What the ways are called in a message: the file they were read from.
   * @param ways The ways the layer holds.
   */
  RoadFile(std::string name, Ways ways) : _name(std::move(name)), _ways(std::move(ways)) {}

  [[nodiscard]] std::string name() const override { return _name; }

  [[nodiscard]] Ways load(const geo::Polygon& location) const override { return _ways.in(location); }

  void stream(const geo::Polygon& location, std::size_t chunk_rows, const Reader<kind_t>::OnChunk& on_chunk) const override {
    _ways.in(location, chunk_rows, on_chunk);
  }

 private:
  std::string          _name;
  detail::WaysInMemory _ways;

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
 * The whole file is read here, and the ways `source` asks for are held in memory for as long as the layer lives: fine for a
 * town or a state; a country belongs in the database. Ways `read_ways` leaves out are left out here.
 * @throws std::runtime_error as `read_ways`.
 */
template <WayKind kind_t>
[[nodiscard]] Layer<kind_t> road_file(const WaySource& source) {
  return Layer<kind_t>(std::make_shared<RoadFile<kind_t>>("'" + source.path + "'", read_ways(source)));
}

}  // namespace miniverse::osm
