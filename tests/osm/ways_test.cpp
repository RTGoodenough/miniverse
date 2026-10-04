// OpenStreetMap files read with libosmium: ways with their node ids, positions and tags, the ways an extract cut left out,
// and a PBF file pushed into a road layer and loaded back.
//
// The files are OSM XML written here as text, and a PBF libosmium makes from it. The last test needs MINIVERSE_TEST_DB (a
// libpq connection string to a scratch database with PostGIS), and is skipped without it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <boost/geometry/io/wkt/read.hpp>
#include <boost/geometry/io/wkt/write.hpp>

#include <osmium/io/any_input.hpp>   // IWYU pragma: keep -- the file formats a Reader opens
#include <osmium/io/any_output.hpp>  // IWYU pragma: keep -- and those a Writer makes
#include <osmium/io/reader.hpp>
#include <osmium/io/writer.hpp>
#include <osmium/memory/buffer.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "miniverse/osm/ways.hpp"
#include "support/files.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;
namespace osm = miniverse::osm;

using Catch::Matchers::ContainsSubstring;
using test::Files;
using test::test_db;

namespace {

// Two roads, a building, a road with a node the file does not have (as an extract leaves one at its edge), a road of one
// node, and a building with a node the file does not have either.
constexpr std::string_view TOWN = R"(<?xml version='1.0' encoding='UTF-8'?>
<osm version="0.6" generator="miniverse test">
  <node id="11" version="1" lat="0" lon="0"/>
  <node id="12" version="1" lat="1" lon="1"/>
  <node id="21" version="1" lat="10" lon="10"/>
  <node id="22" version="1" lat="10.25" lon="10.5"/>
  <node id="23" version="1" lat="11" lon="11"/>
  <node id="31" version="1" lat="5" lon="5"/>
  <way id="1" version="1"><nd ref="11"/><nd ref="12"/><tag k="highway" v="primary"/></way>
  <way id="2" version="1"><nd ref="21"/><nd ref="22"/><nd ref="23"/><tag k="highway" v="residential"/><tag k="name" v="The &quot;High&quot; Street"/></way>
  <way id="3" version="1"><nd ref="11"/><nd ref="21"/><tag k="building" v="yes"/></way>
  <way id="4" version="1"><nd ref="12"/><nd ref="99"/><tag k="highway" v="track"/></way>
  <way id="5" version="1"><nd ref="31"/><tag k="highway" v="path"/></way>
  <way id="6" version="1"><nd ref="11"/><nd ref="98"/><tag k="building" v="shed"/></way>
</osm>
)";

// As an editor saves what it has not uploaded: ids below zero. And a closed way, round a triangle.
constexpr std::string_view DRAFT = R"(<?xml version='1.0' encoding='UTF-8'?>
<osm version="0.6" generator="miniverse test">
  <node id="-3" version="1" lat="2" lon="1"/>
  <node id="-2" version="1" lat="0" lon="2"/>
  <node id="-1" version="1" lat="0" lon="0"/>
  <way id="-7" version="1"><nd ref="-1"/><nd ref="-2"/><nd ref="-3"/><nd ref="-1"/><tag k="junction" v="roundabout"/></way>
</osm>
)";

/** @brief Copies the OSM file `from` to `to`, in the format `to`'s name says: a PBF file made by libosmium itself. */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- from, then to, as a copy reads
void convert(const std::string& from, const std::string& to) {
  osmium::io::Reader reader(from);
  osmium::io::Writer writer(to);
  while ( osmium::memory::Buffer buffer = reader.read() ) {
    writer(std::move(buffer));
  }

  writer.close();
  reader.close();
}

[[nodiscard]] std::vector<std::int64_t> ids(const miniverse::Ways& ways) {
  std::vector<std::int64_t> result;

  result.reserve(ways.size());
  for ( const miniverse::Way& way : ways ) {
    result.push_back(way.id);
  }

  return result;
}

struct Roads : miniverse::RoadLayer {};

}  // namespace

TEST_CASE("osm: the ways with a key are read, with their node ids, positions and tags", "[osm]") {
  const Files       files;
  const std::string path = files.write("town.osm", TOWN);

  miniverse::Ways    roads;
  const osm::WayRead read = osm::read_ways({.path = path, .keys = {"highway"}}, 100, [&](miniverse::Ways chunk) { roads = std::move(chunk); });

  CHECK(read.ways == 2);
  CHECK(read.without_nodes == 2);  // the road with a node that is not in the file, and the road of one node: not the building
  REQUIRE(roads.size() == 2);
  CHECK(roads.at(0).id == 1);
  CHECK(roads.at(0).node_ids == std::vector<std::int64_t>{11, 12});
  CHECK(bg::to_wkt(roads.at(0).coordinates) == "LINESTRING(0 0,1 1)");  // longitude first
  CHECK(roads.at(0).tags.text() == R"({"highway":"primary"})");
  CHECK(roads.at(1).id == 2);
  CHECK(roads.at(1).node_ids == std::vector<std::int64_t>{21, 22, 23});
  CHECK(bg::to_wkt(roads.at(1).coordinates) == "LINESTRING(10 10,10.5 10.25,11 11)");
  CHECK(roads.at(1).tags.text() == R"({"highway":"residential","name":"The \"High\" Street"})");
}

TEST_CASE("osm: without keys every way is read, and with several a way of any of them", "[osm]") {
  const Files       files;
  const std::string path = files.write("town.osm", TOWN);

  CHECK(ids(osm::read_ways({.path = path, .keys = {}})) == std::vector<std::int64_t>{1, 2, 3});
  CHECK(ids(osm::read_ways({.path = path, .keys = {"building"}})) == std::vector<std::int64_t>{3});
  CHECK(ids(osm::read_ways({.path = path, .keys = {"railway", "building", "highway"}})) == std::vector<std::int64_t>{1, 2, 3});
  CHECK(osm::read_ways({.path = path, .keys = {"railway"}}).empty());
}

TEST_CASE("osm: ids below zero are read as any other, and a closed way ends on its first node", "[osm]") {
  const Files files;

  const miniverse::Ways ways = osm::read_ways({.path = files.write("draft.osm", DRAFT), .keys = {}});

  REQUIRE(ways.size() == 1);
  CHECK(ways.front().id == -7);
  CHECK(ways.front().node_ids == std::vector<std::int64_t>{-1, -2, -3, -1});
  CHECK(bg::to_wkt(ways.front().coordinates) == "LINESTRING(0 0,2 0,1 2,0 0)");
}

TEST_CASE("osm: a damaged file is refused", "[osm]") {
  const Files files;

  CHECK_THROWS_AS(osm::read_ways({.path = files.write("damaged.osm.pbf", "this is not a PBF file, whatever its name says"), .keys = {}}), std::runtime_error);
  CHECK_THROWS_AS(osm::read_ways({.path = files.write("damaged.osm", "<osm version=\"0.6\"><way id="), .keys = {}}), std::runtime_error);
}

TEST_CASE("osm: ways are handed over a chunk at a time, in the file's order", "[osm]") {
  const Files       files;
  const std::string path = files.write("town.osm", TOWN);

  std::vector<std::vector<std::int64_t>> chunks;
  const osm::WayRead read = osm::read_ways({.path = path, .keys = {}}, 2, [&](const miniverse::Ways& chunk) { chunks.push_back(ids(chunk)); });

  CHECK(read.ways == 3);
  CHECK(chunks == std::vector<std::vector<std::int64_t>>{{1, 2}, {3}});  // the last chunk is what is left
  CHECK_THROWS_AS(osm::read_ways({.path = path, .keys = {}}, 0, [](const miniverse::Ways&) {}), std::invalid_argument);
}

TEST_CASE("osm: what the callback throws is passed on, and a file that is not there is refused", "[osm]") {
  const Files       files;
  const std::string path = files.write("town.osm", TOWN);

  std::size_t chunks = 0;
  const auto  taking_one = [&](const miniverse::Ways& /*chunk*/) {
    if ( ++chunks == 2 ) {
      throw std::runtime_error("one chunk is enough");
    }
  };

  CHECK_THROWS_WITH(osm::read_ways({.path = path, .keys = {}}, 1, taking_one), ContainsSubstring("is enough"));
  CHECK(chunks == 2);
  CHECK_THROWS_AS(osm::read_ways({.path = files.path_of("nothing.osm.pbf"), .keys = {}}), std::runtime_error);
}

TEST_CASE("osm: a PBF file reads as the XML it was made from", "[osm]") {
  const Files       files;
  const std::string xml = files.write("town.osm", TOWN);
  const std::string pbf = files.path_of("town.osm.pbf");
  convert(xml, pbf);

  const miniverse::Ways from_xml = osm::read_ways({.path = xml, .keys = {"highway"}});
  const miniverse::Ways from_pbf = osm::read_ways({.path = pbf, .keys = {"highway"}});

  REQUIRE(from_pbf.size() == from_xml.size());
  for ( std::size_t i = 0; i < from_xml.size(); ++i ) {
    CHECK(from_pbf.at(i).id == from_xml.at(i).id);
    CHECK(from_pbf.at(i).node_ids == from_xml.at(i).node_ids);
    CHECK(bg::to_wkt(from_pbf.at(i).coordinates) == bg::to_wkt(from_xml.at(i).coordinates));
    CHECK(from_pbf.at(i).tags == from_xml.at(i).tags);
  }
}

TEST_CASE("integration: a PBF file pushed a chunk at a time loads back as it was read", "[osm][integration]") {
  const Files       files;
  const std::string pbf = files.path_of("town.osm.pbf");
  convert(files.write("town.osm", TOWN), pbf);
  const osm::WaySource source{.path = pbf, .keys = {"highway"}};

  miniverse::Miniverse world(test_db(), miniverse::Layer<Roads>("miniverse_test_osm_roads"));
  world.drop_tables();
  world.create_tables();

  const osm::WayRead    read = osm::read_ways(source, 1, [&](miniverse::Ways chunk) { world.push<Roads>(std::move(chunk)).get(); });
  const miniverse::Ways loaded = world.load<Roads>(bg::from_wkt<geo::Polygon>("POLYGON((-1 -1,12 -1,12 12,-1 12,-1 -1))")).get();
  const miniverse::Ways in_file = osm::read_ways(source);

  CHECK(read.ways == 2);
  REQUIRE(loaded.size() == in_file.size());
  for ( std::size_t i = 0; i < in_file.size(); ++i ) {
    CHECK(loaded.at(i).id == in_file.at(i).id);
    CHECK(loaded.at(i).node_ids == in_file.at(i).node_ids);
    CHECK(bg::to_wkt(loaded.at(i).coordinates) == bg::to_wkt(in_file.at(i).coordinates));
  }
  CHECK(loaded.at(0).tags.text() == R"({"highway": "primary"})");  // as jsonb writes it back
  CHECK(loaded.at(1).tags.text() == R"({"name": "The \"High\" Street", "highway": "residential"})");

  world.drop_tables();
}
