// OpenStreetMap files read with libosmium: ways with their node ids, positions and tags, the ways an extract cut left out,
// and a PBF file pushed into a road layer and loaded back.
//
// The files are OSM XML written here as text, and a PBF libosmium makes from it. The last test needs MINIVERSE_TEST_DB (a
// libpq connection string to a scratch database with PostGIS), and is skipped without it.

#include <catch2/catch_message.hpp>
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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "miniverse/osm/ways.hpp"
#include "schemacht/postgres/async_client.hpp"
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

// Ways to tell locations apart by, not in the order of their ids: a square walked round (4), a vee (1), a short way far
// off (3), and a slant whose box overlaps the vee's but which passes it by (2).
constexpr std::string_view STREETS = R"(<?xml version='1.0' encoding='UTF-8'?>
<osm version="0.6" generator="miniverse test">
  <node id="1" version="1" lat="0" lon="0"/>
  <node id="2" version="1" lat="1" lon="1"/>
  <node id="3" version="1" lat="0" lon="2"/>
  <node id="4" version="1" lat="2" lon="0.5"/>
  <node id="5" version="1" lat="0.5" lon="2"/>
  <node id="6" version="1" lat="5" lon="5"/>
  <node id="7" version="1" lat="6" lon="6"/>
  <node id="8" version="1" lat="3" lon="3"/>
  <node id="9" version="1" lat="4" lon="3"/>
  <node id="10" version="1" lat="4" lon="4"/>
  <node id="11" version="1" lat="3" lon="4"/>
  <way id="4" version="1"><nd ref="8"/><nd ref="9"/><nd ref="10"/><nd ref="11"/><nd ref="8"/><tag k="highway" v="service"/></way>
  <way id="1" version="1"><nd ref="1"/><nd ref="2"/><nd ref="3"/><tag k="highway" v="primary"/></way>
  <way id="3" version="1"><nd ref="6"/><nd ref="7"/><tag k="highway" v="track"/></way>
  <way id="2" version="1"><nd ref="4"/><nd ref="5"/><tag k="highway" v="path"/></way>
</osm>
)";

[[nodiscard]] geo::Polygon polygon(const std::string& wkt) { return bg::from_wkt<geo::Polygon>(wkt); }

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

TEST_CASE("osm: a file as a layer loads the ways that intersect a location, in the file's order, with no database", "[osm]") {
  const Files          files;
  miniverse::Miniverse world(osm::road_file<Roads>({.path = files.write("streets.osm", STREETS), .keys = {"highway"}}));
  const auto           in = [&world](const std::string& wkt) { return ids(world.load<Roads>(polygon(wkt)).get()); };

  world.verify().get();

  CHECK(in("POLYGON((-1 -1,9 -1,9 9,-1 9,-1 -1))") == std::vector<std::int64_t>{4, 1, 3, 2});  // as the file has them, not by id
  CHECK(in("POLYGON((0 0,1 0,1 1,0 1,0 0))") == std::vector<std::int64_t>{1});                  // the slant's box overlaps it; it passes by
  CHECK(in("POLYGON((3.4 3.4,3.6 3.4,3.6 3.6,3.4 3.6,3.4 3.4))").empty());                      // inside the square that 4 walks round
  CHECK(in("POLYGON((2 -1,3 -1,3 0,2 0,2 -1))") == std::vector<std::int64_t>{1});              // the vee ends on its corner
  CHECK(in("POLYGON((4.5 4.5,4.5 6.5,6.5 6.5,6.5 4.5,4.5 4.5))") == std::vector<std::int64_t>{3});  // a ring that runs clockwise
  CHECK(in("POLYGON((40 40,41 40,41 41,40 41,40 40))").empty());
}

TEST_CASE("osm: a road file is streamed a chunk at a time, and is read when its layer is made", "[osm]") {
  const Files              files;
  miniverse::Miniverse     world(osm::road_file<Roads>({.path = files.write("streets.osm", STREETS), .keys = {"highway"}}));
  std::vector<std::size_t> sizes;

  world.stream<Roads>(polygon("POLYGON((-1 -1,9 -1,9 9,-1 9,-1 -1))"), [&sizes](const miniverse::Ways& chunk) { sizes.push_back(chunk.size()); }, {.chunk_rows = 3})
      .get();

  CHECK(sizes == std::vector<std::size_t>{3, 1});
  CHECK_THROWS_AS(osm::road_file<Roads>({.path = files.path_of("missing.osm.pbf"), .keys = {}}), std::runtime_error);
  CHECK_THROWS_AS(world.push<Roads>({}).get(), std::logic_error);
}

TEST_CASE("osm: a road file's stream stops when its callback wants no more, and hands over no empty chunk", "[osm]") {
  const Files                         files;
  miniverse::Miniverse                world(osm::road_file<Roads>({.path = files.write("streets.osm", STREETS), .keys = {"highway"}}));
  const geo::Polygon                  everywhere = polygon("POLYGON((-1 -1,9 -1,9 9,-1 9,-1 -1))");
  std::vector<std::vector<std::int64_t>> stopped;
  std::vector<std::vector<std::int64_t>> halves;

  world
      .stream<Roads>(
          everywhere,
          [&stopped](const miniverse::Ways& chunk) {
            stopped.push_back(ids(chunk));

            return false;
          },
          {.chunk_rows = 1}
      )
      .get();
  world.stream<Roads>(everywhere, [&halves](const miniverse::Ways& chunk) { halves.push_back(ids(chunk)); }, {.chunk_rows = 2}).get();

  CHECK(stopped == std::vector<std::vector<std::int64_t>>{{4}});
  CHECK(halves == std::vector<std::vector<std::int64_t>>{{4, 1}, {3, 2}});  // four ways in twos: no third chunk of none
}

TEST_CASE("osm: ways held in memory are found from several threads at once", "[osm]") {
  const Files          files;
  miniverse::Miniverse world(osm::road_file<Roads>({.path = files.write("streets.osm", STREETS), .keys = {"highway"}}));
  const geo::Polygon   near_vee = polygon("POLYGON((0 0,1 0,1 1,0 1,0 0))");
  const geo::Polygon   everywhere = polygon("POLYGON((-1 -1,9 -1,9 9,-1 9,-1 -1))");

  std::vector<std::future<miniverse::Ways>> loads;
  loads.reserve(16);
  for ( int i = 0; i < 16; ++i ) {
    loads.push_back(world.load<Roads>(i % 2 == 0 ? near_vee : everywhere));
  }

  for ( std::size_t i = 0; i < loads.size(); ++i ) {
    CHECK(ids(loads.at(i).get()) == (i % 2 == 0 ? std::vector<std::int64_t>{1} : std::vector<std::int64_t>{4, 1, 3, 2}));
  }
}

TEST_CASE("osm: ways given in the program are a layer too, and one of no points is nowhere", "[osm]") {
  miniverse::Ways ways;
  ways.push_back({.id = 1, .node_ids = {1, 2}, .coordinates = {{0, 0}, {1, 1}}});
  ways.push_back({.id = 2});                                                        // no points at all
  ways.push_back({.id = 3, .node_ids = {3, 3}, .coordinates = {{0.5, 0.2}, {0.5, 0.2}}});  // no length: a point
  miniverse::Miniverse world{miniverse::Layer<Roads>(std::make_shared<osm::RoadFile<Roads>>("three ways", std::move(ways)))};

  CHECK(ids(world.load<Roads>(polygon("POLYGON((-1 -1,2 -1,2 2,-1 2,-1 -1))")).get()) == std::vector<std::int64_t>{1, 3});
  CHECK(ids(world.load<Roads>(polygon("POLYGON((0.4 0.1,0.6 0.1,0.6 0.2,0.4 0.2,0.4 0.1))")).get()) == std::vector<std::int64_t>{3});  // on its edge
  CHECK(world.load<Roads>(geo::Polygon{}).get().empty());
  CHECK_THROWS_WITH(world.push<Roads>({}).get(), ContainsSubstring("three ways"));
}

TEST_CASE("integration: a road file loads as the table it was pushed into", "[osm][integration]") {
  const Files           files;
  const osm::WaySource  source{.path = files.write("streets.osm", STREETS), .keys = {"highway"}};
  miniverse::Miniverse  tables(test_db(), miniverse::Layer<Roads>("miniverse_test_osm_roads"));
  miniverse::Miniverse  from_file(osm::road_file<Roads>(source));
  tables.drop_tables();
  tables.create_tables();
  tables.push<Roads>(osm::read_ways(source)).get();

  for ( const std::string wkt : {
            "POLYGON((-1 -1,9 -1,9 9,-1 9,-1 -1))",
            "POLYGON((0 0,1 0,1 1,0 1,0 0))",
            "POLYGON((3.4 3.4,3.6 3.4,3.6 3.6,3.4 3.6,3.4 3.4))",
            "POLYGON((2 -1,3 -1,3 0,2 0,2 -1))",
            "POLYGON((4.5 4.5,4.5 6.5,6.5 6.5,6.5 4.5,4.5 4.5))",
            "POLYGON((1.2 1.2,1.3 1.2,1.3 1.3,1.2 1.3,1.2 1.2))",  // on the slant
            "POLYGON((0 3,3 3,3 6,0 6,0 3))",                      // an edge of it along a side of the square
            "POLYGON((1 0.2,3 0.2,2 0.4,1 0.2))",                  // a triangle across the vee's east arm
            "POLYGON((4 4,5 4,5 5,4 5,4 4))",                      // a corner of it on a corner of the square, and on the track's end
            "POLYGON((-1 -1,9 -1,9 9,-1 9,-1 -1),(3 3,3 4,4 4,4 3,3 3))",  // the square on the edge of its hole
            "POLYGON((-1 -1,9 -1,9 9,-1 9,-1 -1),(-0.5 -0.5,-0.5 2.5,2.5 2.5,2.5 -0.5,-0.5 -0.5))",  // the vee and the slant in its hole
            "POLYGON((40 40,41 40,41 41,40 41,40 40))",
        } ) {
    INFO(wkt);
    const miniverse::Ways in_table = tables.load<Roads>(polygon(wkt)).get();
    miniverse::Ways       in_file = from_file.load<Roads>(polygon(wkt)).get();
    std::ranges::sort(in_file, {}, &miniverse::Way::id);  // the table's are by id

    REQUIRE(ids(in_file) == ids(in_table));
    for ( std::size_t i = 0; i < in_table.size(); ++i ) {
      CHECK(in_file.at(i).node_ids == in_table.at(i).node_ids);
      CHECK(bg::to_wkt(in_file.at(i).coordinates) == bg::to_wkt(in_table.at(i).coordinates));
    }
  }

  // A location of no points holds nothing, from either. One whose ring is not closed is refused by both: by the file's
  // layer at once, and by PostGIS when it comes to test a way against it.
  geo::Polygon open_ring;
  open_ring.outer() = {{-1, -1}, {9, -1}, {9, 9}, {-1, 9}};
  CHECK(tables.load<Roads>(geo::Polygon{}).get().empty());
  CHECK(from_file.load<Roads>(geo::Polygon{}).get().empty());
  CHECK_THROWS_AS(from_file.load<Roads>(open_ring).get(), std::invalid_argument);
  CHECK_THROWS_AS(tables.load<Roads>(open_ring).get(), schemacht::postgres::QueryError);

  tables.drop_tables();
}
