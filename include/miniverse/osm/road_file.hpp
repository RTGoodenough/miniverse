#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "miniverse/geo/types.hpp"
#include "miniverse/layer.hpp"
#include "miniverse/model/way.hpp"
#include "miniverse/osm/concepts/ways.hpp"  // IWYU pragma: export
#include "miniverse/osm/ways.hpp"
#include "miniverse/osm/ways_in_memory.hpp"
#include "miniverse/reader.hpp"

/**
 * @file
 * An OpenStreetMap file as a layer: its ways read once (ways.hpp) and held in memory
 * (ways_in_memory.hpp), in place of a table.
 */
namespace miniverse::osm {

/**
 * @brief The reader of ways held in memory as a layer of `model_t`: what `road_file` makes from a
 * file's ways, and a program can make from ways of its own, which is why it is public where the
 * GDAL file readers are not. A load gives the ways that intersect the location, in the order they
 * were given, with their tags as given. `Miniverse::verify` has nothing to check once it is made.
 */
template <ReadsWays model_t>
class RoadFile : public Reader<model_t>
{
 public:
  /**
   * @param name What the ways are called in a message: the file they were read from.
   * @param ways The ways the layer holds.
   */
  RoadFile(std::string name, Ways ways) : _name(std::move(name)), _ways(std::move(ways)) {}

  [[nodiscard]] std::string name() const override { return _name; }

  [[nodiscard]] Ways load(const geo::Polygon& location) const override
  {
    return _ways.in(location);
  }

  void stream(const geo::Polygon&             location,
              std::size_t                     chunk_rows,
              const Reader<model_t>::OnChunk& on_chunk) const override
  {
    _ways.in(location, chunk_rows, on_chunk);
  }

 private:
  std::string          _name;
  detail::WaysInMemory _ways;

 public:
  RoadFile(const RoadFile&)            = delete;
  RoadFile(RoadFile&&)                 = delete;
  RoadFile& operator=(const RoadFile&) = delete;
  RoadFile& operator=(RoadFile&&)      = delete;
  ~RoadFile() override                 = default;
};

/**
 * @return The layer `layer_t`, of a model of ways, that is `source`, an OpenStreetMap file, read in
 * place of a table:
 *
 * @code
 * using Roads = miniverse::Layer<miniverse::RoadModel, "roads">;
 *
 * miniverse::Miniverse world(miniverse::osm::road_file<Roads>({.path = "town.osm.pbf", .keys =
 * {"highway"}})); world.load<"roads">(area).get();  // the file's roads that intersect the area
 * @endcode
 *
 * The whole file is read here, and the ways `source` asks for are held in memory for as long as the
 * layer lives: fine for a town or a state; a country belongs in the database. Ways `read_ways`
 * leaves out are left out here.
 * @throws std::runtime_error as `read_ways`.
 */
template <IsLayer layer_t>
  requires ReadsWays<typename layer_t::model_type>
[[nodiscard]] layer_t road_file(const WaySource& source)
{
  return layer_t(std::make_shared<RoadFile<typename layer_t::model_type>>("'" + source.path + "'",
                                                                          read_ways(source)));
}

}  // namespace miniverse::osm
