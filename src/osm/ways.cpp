#include "miniverse/osm/ways.hpp"

#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <osmium/handler.hpp>
#include <osmium/handler/node_locations_for_ways.hpp>
#include <osmium/index/map/flex_mem.hpp>
#include <osmium/io/any_input.hpp>  // IWYU pragma: keep -- the file formats a Reader opens
#include <osmium/io/reader.hpp>
#include <osmium/osm/entity_bits.hpp>
#include <osmium/osm/location.hpp>
#include <osmium/osm/node_ref.hpp>
#include <osmium/osm/tag.hpp>
#include <osmium/osm/types.hpp>
#include <osmium/osm/way.hpp>
#include <osmium/visitor.hpp>

#include <protozero/exception.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "miniverse/layer/road_layer.hpp"
#include "schemacht/json/json.hpp"

namespace miniverse::osm {

namespace {

/** @brief Where each node of the file is, by its id: filled as the nodes are read, and asked for each way's. */
using Positions = osmium::index::map::FlexMem<osmium::unsigned_object_id_type, osmium::Location>;

// Nodes, for their positions, and ways: not relations. libosmium's own `node | way` makes this same value, in a header where
// the analyzer's complaint that 3 names no one kind of object can't be answered.
// NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) -- a set of bits, which the type is for
constexpr auto NODES_AND_WAYS = static_cast<osmium::osm_entity_bits::type>(
    static_cast<unsigned char>(osmium::osm_entity_bits::node) | static_cast<unsigned char>(osmium::osm_entity_bits::way)
);

/** @return `tags` as one JSON object, each value text, in the file's order. */
[[nodiscard]] schemacht::json::Json json_of(const osmium::TagList& tags) {
  boost::json::object object;
  for ( const osmium::Tag& tag : tags ) {
    object[tag.key()] = tag.value();
  }

  return schemacht::json::Json(boost::json::serialize(object));
}

/** @brief Gathers the ways libosmium reads, those `source` asks for, and hands them over a chunk at a time. */
class Gatherer : public osmium::handler::Handler {
 public:
  Gatherer(const WaySource& source, std::size_t chunk_ways, const std::function<void(Ways)>& on_chunk)
      : _source(&source), _chunk_ways(chunk_ways), _on_chunk(&on_chunk) {}

  /** @brief Called by libosmium with each way, its nodes' positions filled in where the file has them. */
  void way(const osmium::Way& from) {
    if ( ! wanted(from) ) {
      return;
    }

    const osmium::WayNodeList& nodes = from.nodes();
    if ( nodes.size() < 2 || ! std::ranges::all_of(nodes, [](const osmium::NodeRef& node) { return node.location().valid(); }) ) {
      ++_read.without_nodes;
      return;
    }

    Way made{.id = from.id(), .node_ids = {}, .coordinates = {}, .tags = json_of(from.tags())};
    made.node_ids.reserve(nodes.size());
    made.coordinates.reserve(nodes.size());
    for ( const osmium::NodeRef& node : nodes ) {
      made.node_ids.push_back(node.ref());
      made.coordinates.emplace_back(node.lon(), node.lat());
    }

    _chunk.push_back(std::move(made));
    ++_read.ways;
    if ( _chunk.size() == _chunk_ways ) {
      hand_over();
    }
  }

  /** @brief Hands over the ways gathered since the last chunk, if there are any: for the end of the file. */
  void hand_over() {
    if ( ! _chunk.empty() ) {
      (*_on_chunk)(std::exchange(_chunk, {}));
    }
  }

  [[nodiscard]] const WayRead& read() const noexcept { return _read; }

 private:
  const WaySource*                  _source;
  std::size_t                       _chunk_ways;
  const std::function<void(Ways)>*  _on_chunk;
  Ways                              _chunk;
  WayRead                           _read;

  [[nodiscard]] bool wanted(const osmium::Way& way) const {
    return _source->keys.empty() || std::ranges::any_of(_source->keys, [&](const std::string& key) { return way.tags().has_key(key.c_str()); });
  }

 public:
  Gatherer(const Gatherer&) = delete;
  Gatherer(Gatherer&&) = delete;
  Gatherer& operator=(const Gatherer&) = delete;
  Gatherer& operator=(Gatherer&&) = delete;
  ~Gatherer() = default;
};

}  // namespace

WayRead read_ways(const WaySource& source, std::size_t chunk_ways, const std::function<void(Ways)>& on_chunk) {
  if ( chunk_ways == 0 ) {
    throw std::invalid_argument("a chunk of ways holds at least one");
  }

  osmium::io::Reader reader(source.path, NODES_AND_WAYS);
  if ( reader.header().has_multiple_object_versions() ) {
    throw std::runtime_error("'" + source.path + "' holds a history, several versions of each way: only a file of the map as it is can be read");
  }

  // Positions by id, those above zero and (as an editor numbers what it has not uploaded) those below. A way's nodes that
  // the file lacks are left without a position, not an error.
  Positions                                                   positions;
  Positions                                                   negatives;
  osmium::handler::NodeLocationsForWays<Positions, Positions> positioner(positions, negatives);
  positioner.ignore_errors();
  Gatherer gatherer(source, chunk_ways, on_chunk);

  try {
    osmium::apply(reader, positioner, gatherer);
    reader.close();
  } catch ( const protozero::exception& error ) {  // a damaged PBF file: not one of libosmium's own errors, nor a std::runtime_error
    throw std::runtime_error("'" + source.path + "' is damaged: " + error.what());
  }

  gatherer.hand_over();

  return gatherer.read();
}

Ways read_ways(const WaySource& source) {
  Ways all;

  std::ignore = read_ways(source, std::numeric_limits<std::size_t>::max(), [&all](Ways chunk) { all = std::move(chunk); });

  return all;
}

}  // namespace miniverse::osm
