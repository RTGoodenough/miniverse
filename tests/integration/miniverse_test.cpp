// A miniverse against a real PostGIS database: push a small road network, then load it by polygon.
//
// Skipped unless MINIVERSE_TEST_DB holds a libpq connection string to a database with PostGIS enabled. The tests make and drop
// their own tables (miniverse_test_*), so point it at a scratch database.

#include <catch2/catch_test_macros.hpp>

#include <boost/geometry/io/wkt/read.hpp>
#include <boost/geometry/io/wkt/write.hpp>

#include <cstddef>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "miniverse/world/registry.hpp"
#include "schemacht/json/json.hpp"
#include "schemacht/postgres/async_client.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/query/raw_statement.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/table_name.hpp"
#include "support/checks.hpp"
#include "support/database.hpp"
#include "support/geometry.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;

using test::test_db;

namespace {

using TestRoads = miniverse::Layer<miniverse::RoadModel, "roads">;

using World = miniverse::Miniverse<TestRoads>;

[[nodiscard]] miniverse::Way way(std::int64_t id, std::vector<std::int64_t> node_ids, const std::string& wkt, std::string tags) {
  return miniverse::Way{
      .id = id, .node_ids = std::move(node_ids), .coordinates = bg::from_wkt<geo::LineString>(wkt), .tags = schemacht::json::Json(std::move(tags))
  };
}

using test::polygon;

// The places loaded, as closed rings that run counter-clockwise.
const geo::Polygon NEAR_ORIGIN = polygon("POLYGON((-1 -1,2 -1,2 4,-1 4,-1 -1))");
const geo::Polygon NORTH_EAST = polygon("POLYGON((5 5,20 5,20 20,5 20,5 5))");
const geo::Polygon EVERYWHERE = polygon("POLYGON((-1 -1,20 -1,20 20,-1 20,-1 -1))");
const geo::Polygon NOWHERE = polygon("POLYGON((30 30,31 30,31 31,30 31,30 30))");
const geo::Polygon TRIANGLE = polygon("POLYGON((0 0,4 0,0 4,0 0))");

/**
 * The network, in degrees:
 * - 1 near the origin, 3 just north of it, 2 far to the north-east
 * - 4 inside the box of the triangle (0 0, 4 0, 0 4), but beyond its long side: a bounding-box test would take it, ST_Intersects does not
 */
[[nodiscard]] miniverse::Ways network() {
  return {
      way(1, {11, 12}, "LINESTRING(0 0,1 1)", R"({"highway": "primary"})"),
      way(2, {21, 22, 23}, "LINESTRING(10 10,10.5 10.25,11 11)", R"({"highway": "residential", "name": "High Street"})"),
      way(3, {31, 32}, "LINESTRING(0.5 2,0.5 3)", "{}"),
      way(4, {41, 42}, "LINESTRING(3 3,4 2)", R"({"highway": "track"})"),
  };
}

/** @return `WAYS_PER_STATEMENT + 1` ways, one more than a statement holds, with ids from `first_id` and two nodes each, all in the degree from (50, 50). */
[[nodiscard]] miniverse::Ways many_ways(std::int64_t first_id) {
  constexpr std::size_t COUNT = miniverse::RoadModel::WAYS_PER_STATEMENT + 1;
  miniverse::Ways       ways;

  ways.reserve(COUNT);
  for ( std::size_t i = 0; i < COUNT; ++i ) {
    const std::int64_t id = first_id + static_cast<std::int64_t>(i);
    ways.push_back(way(id, {2 * id, (2 * id) + 1}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}"));
  }

  return ways;
}

const miniverse::Dataset NETWORK{"network"};
const miniverse::Dataset FAR_EAST_SURVEY{"far east"};

const geo::Polygon FAR_EAST = polygon("POLYGON((50 50,51 50,51 51,50 51,50 50))");

using test::ids;

/** @brief A world with the network in it as the dataset `NETWORK`, its table made fresh and dropped afterwards: ways 1 to 4. */
[[nodiscard]] test::Fresh<World> with_network(const std::string& conninfo) {
  return test::Fresh<World>(
      [](World& world) {
        world.create_tables();
        world.push<"roads">(NETWORK, network()).get();
      },
      conninfo, TestRoads("miniverse_test_roads")
  );
}

// A model whose setup fails: the table made with it is rolled back.
struct BrokenSetup : miniverse::RoadModel {
  [[nodiscard]] static std::vector<std::string> setup_sql(const schemacht::schema::TableName& /*table*/) {
    return {"CREATE INDEX ON miniverse_no_such_table (geom)"};
  }
};

using TableExists = schemacht::query::RawStatement<
    "SELECT to_regclass($1) IS NOT NULL AS found", schemacht::query::RawArguments<std::string>, schemacht::schema::Field<bool, "found">>;

using IndexNames = schemacht::query::RawStatement<
    "SELECT indexname FROM pg_indexes WHERE tablename = $1 ORDER BY indexname", schemacht::query::RawArguments<std::string>,
    schemacht::schema::Field<std::string, "indexname">>;

}  // namespace

TEST_CASE("integration: a load gives the ways that intersect the polygon, ordered by id", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());
  World&             world = loaded.world();

  CHECK(ids(world.load<"roads">(NEAR_ORIGIN).get()) == std::vector<std::int64_t>{1, 3});
  CHECK(ids(world.load<"roads">(NORTH_EAST).get()) == std::vector<std::int64_t>{2});
  CHECK(ids(world.load<"roads">(EVERYWHERE).get()) == std::vector<std::int64_t>{1, 2, 3, 4});
  CHECK(ids(world.load<"roads">(NOWHERE).get()).empty());

  // The triangle's box holds way 4, the triangle does not.
  CHECK(ids(world.load<"roads">(TRIANGLE).get()) == std::vector<std::int64_t>{1, 3});
}

TEST_CASE("integration: a loaded way is the way that was pushed", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());

  const miniverse::Ways ways = loaded.world().load<"roads">(NORTH_EAST).get();

  REQUIRE(ways.size() == 1);
  const miniverse::Way& high_street = ways.front();
  CHECK(high_street.id == 2);
  CHECK(high_street.node_ids == std::vector<std::int64_t>{21, 22, 23});
  CHECK(bg::to_wkt(high_street.coordinates) == "LINESTRING(10 10,10.5 10.25,11 11)");
  CHECK(high_street.tags.text() == R"({"name": "High Street", "highway": "residential"})");  // jsonb's own order: shorter keys first
}

TEST_CASE("integration: several loads run at once on one miniverse", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());
  World&             world = loaded.world();

  std::future<miniverse::Ways> south_west = world.load<"roads">(NEAR_ORIGIN);
  std::future<miniverse::Ways> north_east = world.load<"roads">(NORTH_EAST);

  CHECK(ids(north_east.get()) == std::vector<std::int64_t>{2});
  CHECK(ids(south_west.get()) == std::vector<std::int64_t>{1, 3});
}

TEST_CASE("integration: create_tables makes the spatial index the loads use", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());

  World& world = loaded.world();

  const auto rows = world.database().execute(IndexNames::bind(world.table_name<"roads">().name())).get();

  std::vector<std::string> names;

  names.reserve(rows.size());
  for ( const auto& row : rows ) {
    names.push_back(schemacht::schema::get<"indexname">(row));
  }

  CHECK(names == std::vector<std::string>{"miniverse_test_roads_dataset_idx", "miniverse_test_roads_geom_idx", "miniverse_test_roads_pkey"});
}

// The registry's records by the key it holds them under: the name the table resolves to, read while the table is there.
using RegistryName = schemacht::query::RawStatement<
    "SELECT miniverse_internal.layer_name($1) AS name", schemacht::query::RawArguments<std::string>, schemacht::schema::Field<std::string, "name">>;

using DatasetRecords = schemacht::query::RawStatement<
    "SELECT dataset FROM miniverse_internal.datasets WHERE table_name = $1 ORDER BY dataset", schemacht::query::RawArguments<std::string>,
    schemacht::schema::Field<std::string, "dataset">>;

using LayerRecord = schemacht::query::RawStatement<
    "SELECT kind, layout FROM miniverse_internal.layers WHERE table_name = $1", schemacht::query::RawArguments<std::string>,
    schemacht::schema::Field<std::string, "kind">, schemacht::schema::Field<std::int32_t, "layout">>;

namespace {

/** @return The name the registry keys `table` under, which exists. */
[[nodiscard]] std::string registry_name(World& world, const std::string& table) {
  return schemacht::schema::get<"name">(world.database().execute(RegistryName::bind(table)).get().front());
}

}  // namespace

TEST_CASE("integration: create_table records the table in the registry, and drop_tables forgets it", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());
  World&             world = loaded.world();

  // Keyed by the name the table resolves to, however the layer spelled it.
  const std::string key = registry_name(world, "miniverse_test_roads");
  CHECK(key.ends_with(".miniverse_test_roads"));
  CHECK(registry_name(world, "public.miniverse_test_roads") == key);

  const auto recorded = world.database().execute(LayerRecord::bind(key)).get();
  REQUIRE(recorded.size() == 1);
  CHECK(schemacht::schema::get<"kind">(recorded.front()) == "roads");
  CHECK(schemacht::schema::get<"layout">(recorded.front()) == TestRoads::model_type::LAYOUT);

  world.drop_tables();
  CHECK(world.database().execute(LayerRecord::bind(key)).get().empty());
  CHECK(world.database().execute(DatasetRecords::bind(key)).get().empty());  // the network's, gone with the table
}

TEST_CASE("integration: the record of a push is written in the push's own transaction", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());
  World&             world = loaded.world();

  // The registry's table is taken away between the part and the commit: the commit fails, and the part is not there.
  miniverse::PushInParts<TestRoads::model_type> parts = world.begin_push<"roads">(FAR_EAST_SURVEY).get();
  parts.add({way(10, {1, 2}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}")}).get();
  std::ignore = world.database().execute(schemacht::postgres::unchecked_sql("ALTER TABLE miniverse_internal.datasets RENAME TO away")).get();
  CHECK_THROWS_AS(parts.commit().get(), schemacht::postgres::QueryError);
  std::ignore = world.database().execute(schemacht::postgres::unchecked_sql("ALTER TABLE miniverse_internal.away RENAME TO datasets")).get();

  CHECK(world.load<"roads">(FAR_EAST).get().empty());
  CHECK(world.database().execute(DatasetRecords::bind(registry_name(world, "miniverse_test_roads"))).get().size() == 1);
}

TEST_CASE("integration: datasets lists each with its rows and when it was last pushed, and remove takes it off the list", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());
  World&             world = loaded.world();

  world.push<"roads">(FAR_EAST_SURVEY, many_ways(1000)).get();
  std::vector<miniverse::DatasetInfo> listed = world.datasets<"roads">().get();
  REQUIRE(listed.size() == 2);  // by name
  CHECK(listed.at(0).name == FAR_EAST_SURVEY.name);
  CHECK(listed.at(0).rows == miniverse::RoadModel::WAYS_PER_STATEMENT + 1);
  CHECK(listed.at(1).name == NETWORK.name);
  CHECK(listed.at(1).rows == 4);
  CHECK(listed.at(0).pushed > listed.at(1).pushed);  // the network was pushed first

  // Pushed again: the same entry, with the newer push's rows and time.
  world.push<"roads">(NETWORK, {way(1, {11, 12}, "LINESTRING(0 0,1 1)", "{}")}).get();
  listed = world.datasets<"roads">().get();
  REQUIRE(listed.size() == 2);
  CHECK(listed.at(1).rows == 1);
  CHECK(listed.at(1).pushed > listed.at(0).pushed);

  CHECK(world.remove<"roads">(FAR_EAST_SURVEY).get() == miniverse::RoadModel::WAYS_PER_STATEMENT + 1);
  listed = world.datasets<"roads">().get();
  REQUIRE(listed.size() == 1);
  CHECK(listed.front().name == NETWORK.name);
}

TEST_CASE("integration: a stream of roads hands the ways over a chunk at a time, ordered by id", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());

  // On a pool thread, one call at a time: read here only once the future is done.
  std::vector<std::vector<std::int64_t>> chunks;

  loaded.world().stream<"roads">(EVERYWHERE, [&](const miniverse::Ways& chunk) { chunks.push_back(ids(chunk)); }, {.chunk_rows = 3}).get();

  CHECK(chunks == std::vector<std::vector<std::int64_t>>{{1, 2, 3}, {4}});
}

TEST_CASE("integration: a load that fails reports why through its future", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());
  World&             world = loaded.world();
  world.drop_tables();

  std::future<miniverse::Ways> roads = world.load<"roads">(EVERYWHERE);

  CHECK_THROWS_AS(roads.get(), schemacht::postgres::QueryError);  // the table is gone
}

TEST_CASE("integration: a push that breaks the table's rules writes nothing and reports why", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());
  World&             world = loaded.world();

  // Two ways of one id in one statement: the database can't update one row twice in a statement.
  miniverse::Ways twice{way(5, {51, 52}, "LINESTRING(0 0,1 1)", "{}"), way(5, {53, 54}, "LINESTRING(0 0,1 1)", R"({"highway": "path"})")};
  try {
    world.push<"roads">(FAR_EAST_SURVEY, std::move(twice)).get();
    FAIL("the push succeeded");
  } catch ( const schemacht::postgres::QueryError& error ) {
    CHECK(error.sqlstate() == "21000");  // cardinality violation
  }

  CHECK(ids(world.load<"roads">(EVERYWHERE).get()) == std::vector<std::int64_t>{1, 2, 3, 4});
}

TEST_CASE("integration: a push of more ways than one statement holds writes them all", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());
  World&             world = loaded.world();

  world.push<"roads">(FAR_EAST_SURVEY, many_ways(1000)).get();

  const miniverse::Ways far_east = world.load<"roads">(FAR_EAST).get();
  CHECK(far_east.size() == miniverse::RoadModel::WAYS_PER_STATEMENT + 1);
  CHECK(far_east.front().id == 1000);
  CHECK(far_east.back().id == 1000 + static_cast<std::int64_t>(miniverse::RoadModel::WAYS_PER_STATEMENT));
  CHECK(far_east.front().node_ids == std::vector<std::int64_t>{2000, 2001});
}

TEST_CASE("integration: a push whose later statement breaks the table's rules writes nothing", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());
  World&             world = loaded.world();

  miniverse::Ways ways = many_ways(1000);
  ways.push_back(ways.back());  // the second statement now holds one way twice

  // The error is the second statement's own (one row updated twice), not the commit's "rolled back".
  try {
    world.push<"roads">(FAR_EAST_SURVEY, std::move(ways)).get();
    FAIL("the push succeeded");
  } catch ( const schemacht::postgres::QueryError& error ) {
    CHECK(error.sqlstate() == "21000");
  }

  CHECK(world.load<"roads">(FAR_EAST).get().empty());  // the first statement's ways are rolled back too
  CHECK(ids(world.load<"roads">(EVERYWHERE).get()) == std::vector<std::int64_t>{1, 2, 3, 4});
}

TEST_CASE("integration: a push in parts is there for everyone once it commits, and for no one before", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());
  World&             world = loaded.world();

  miniverse::PushInParts<TestRoads::model_type> parts = world.begin_push<"roads">(FAR_EAST_SURVEY).get();
  parts.add({way(10, {1, 2}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}"), way(11, {2, 3}, "LINESTRING(50.2 50.2,50.3 50.3)", "{}")}).get();
  parts.add({way(12, {3, 4}, "LINESTRING(50.3 50.3,50.4 50.4)", "{}")}).get();
  const std::vector<std::int64_t> before = ids(world.load<"roads">(FAR_EAST).get());  // on another of the pool's connections
  parts.commit().get();

  CHECK(before.empty());
  CHECK(ids(world.load<"roads">(FAR_EAST).get()) == std::vector<std::int64_t>{10, 11, 12});
}

TEST_CASE("integration: the parts of a push in parts are written in the order added, and need not be waited for", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());
  World&             world = loaded.world();

  miniverse::PushInParts<TestRoads::model_type> parts = world.begin_push<"roads">(FAR_EAST_SURVEY).get();
  std::future<void>                             first = parts.add({way(10, {1, 2}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}")});
  std::future<void>                             second = parts.add({way(11, {2, 3}, "LINESTRING(50.2 50.2,50.3 50.3)", "{}")});
  std::future<void>                             committed = parts.commit();

  committed.get();
  first.get();
  second.get();
  CHECK(ids(world.load<"roads">(FAR_EAST).get()) == std::vector<std::int64_t>{10, 11});
}

TEST_CASE("integration: a push in parts assigned over another drops the other, which then writes nothing", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());
  World&             world = loaded.world();

  miniverse::PushInParts<TestRoads::model_type> parts = world.begin_push<"roads">(FAR_EAST_SURVEY).get();
  parts.add({way(10, {1, 2}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}")}).get();

  parts = world.begin_push<"roads">(miniverse::Dataset{"far_east_again"}).get();  // another dataset: the first push holds its lock
  parts.add({way(11, {2, 3}, "LINESTRING(50.2 50.2,50.3 50.3)", "{}")}).get();
  parts.commit().get();

  CHECK(ids(world.load<"roads">(FAR_EAST).get()) == std::vector<std::int64_t>{11});
}

TEST_CASE("integration: a push in parts that is dropped without a commit writes nothing", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());
  World&             world = loaded.world();

  {
    miniverse::PushInParts<TestRoads::model_type> parts = world.begin_push<"roads">(FAR_EAST_SURVEY).get();
    parts.add({way(10, {1, 2}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}")}).get();
  }

  CHECK(world.load<"roads">(FAR_EAST).get().empty());
  CHECK(ids(world.load<"roads">(EVERYWHERE).get()) == std::vector<std::int64_t>{1, 2, 3, 4});
  CHECK(world.datasets<"roads">().get().size() == 1);  // nor is the dataset recorded
}

TEST_CASE("integration: a part that fails ends a push in parts, which then writes nothing", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());
  World&             world = loaded.world();

  miniverse::PushInParts<TestRoads::model_type> parts = world.begin_push<"roads">(FAR_EAST_SURVEY).get();
  parts.add({way(10, {1, 2}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}")}).get();

  // One id twice in one part: the part fails with its own error, a row updated twice in one statement.
  try {
    parts.add({way(11, {5, 6}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}"), way(11, {7, 8}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}")}).get();
    FAIL("the part was added");
  } catch ( const schemacht::postgres::QueryError& error ) {
    CHECK(error.sqlstate() == "21000");
  }

  // The push has ended: every later call fails with that part's error, and an empty part too.
  CHECK_THROWS_AS(parts.add({way(13, {1, 2}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}")}).get(), schemacht::postgres::QueryError);
  CHECK_THROWS_AS(parts.add({}).get(), schemacht::postgres::QueryError);
  CHECK_THROWS_AS(parts.commit().get(), schemacht::postgres::QueryError);
  CHECK(world.load<"roads">(FAR_EAST).get().empty());  // not even the first part
}

TEST_CASE("integration: a part whose rows can't be made ends a push in parts too", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());
  World&             world = loaded.world();

  miniverse::PushInParts<TestRoads::model_type> parts = world.begin_push<"roads">(FAR_EAST_SURVEY).get();
  parts.add({way(10, {1, 2}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}")}).get();

  // Three node ids for two points: refused before anything is sent. The file would have a hole if the push went on.
  CHECK_THROWS_AS(parts.add({way(11, {1, 2, 3}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}")}).get(), std::invalid_argument);
  CHECK_THROWS_AS(parts.add({way(12, {1, 2}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}")}).get(), std::invalid_argument);
  CHECK_THROWS_AS(parts.commit().get(), std::invalid_argument);
  CHECK(world.load<"roads">(FAR_EAST).get().empty());
}

TEST_CASE("integration: a push in parts that was moved from takes no more calls", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());
  World&             world = loaded.world();

  miniverse::PushInParts<TestRoads::model_type> first = world.begin_push<"roads">(FAR_EAST_SURVEY).get();
  miniverse::PushInParts<TestRoads::model_type> second = std::move(first);
  second.add({way(10, {1, 2}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}")}).get();

  CHECK_THROWS_AS(std::ignore = first.add({}), std::logic_error);  // NOLINT(bugprone-use-after-move) -- what is under test
  CHECK_THROWS_AS(std::ignore = first.commit(), std::logic_error);
  second.commit().get();
  CHECK(ids(world.load<"roads">(FAR_EAST).get()) == std::vector<std::int64_t>{10});
}

TEST_CASE("integration: a push of a dataset again replaces it, and a way pushed again keeps its row with the newer push's nodes", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());
  World&             world = loaded.world();

  // The network again from a newer export: way 1 with a node moved, way 3 with new tags, way 4 gone, a new way 5.
  world
      .push<"roads">(
          NETWORK,
          {
              way(1, {11, 13}, "LINESTRING(0 0,1 1.5)", R"({"highway": "primary"})"),
              way(2, {21, 22, 23}, "LINESTRING(10 10,10.5 10.25,11 11)", R"({"highway": "residential", "name": "High Street"})"),
              way(3, {31, 32}, "LINESTRING(0.5 2,0.5 3)", R"({"highway": "footway"})"),
              way(5, {51, 52}, "LINESTRING(1 1,1.5 1.5)", "{}"),
          }
      )
      .get();

  const miniverse::Ways everywhere = world.load<"roads">(EVERYWHERE).get();
  CHECK(ids(everywhere) == std::vector<std::int64_t>{1, 2, 3, 5});
  REQUIRE(everywhere.size() == 4);
  CHECK(everywhere.at(0).node_ids == std::vector<std::int64_t>{11, 13});
  CHECK(bg::to_wkt(everywhere.at(0).coordinates) == "LINESTRING(0 0,1 1.5)");
  CHECK(everywhere.at(2).tags.text() == R"({"highway": "footway"})");
}

TEST_CASE("integration: a way of thousands of nodes is stored like any other", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());
  World&             world = loaded.world();

  // Across the far-east degree, a node every ten thousandth: far longer than a b-tree entry could hold.
  constexpr std::int64_t    NODES = 5000;
  std::vector<std::int64_t> node_ids;
  geo::LineString           line;
  for ( std::int64_t i = 0; i < NODES; ++i ) {
    node_ids.push_back(90'000 + i);
    line.emplace_back(50.1 + (static_cast<double>(i) / 10'000), 50.5);
  }
  world.push<"roads">(
           FAR_EAST_SURVEY, {miniverse::Way{.id = 9, .node_ids = node_ids, .coordinates = line, .tags = schemacht::json::Json("{}")}}
  ).get();

  const miniverse::Ways far_east = world.load<"roads">(FAR_EAST).get();
  REQUIRE(far_east.size() == 1);
  CHECK(far_east.front().id == 9);
  CHECK(far_east.front().node_ids == node_ids);
}

TEST_CASE("integration: a way two datasets both carry is one row, of the dataset that last pushed it", "[integration]") {
  test::Fresh<World> loaded = with_network(test_db());
  World&             world = loaded.world();

  // A neighbouring export that carries way 2 whole too, and a way of its own.
  const miniverse::Dataset neighbour{"neighbour"};
  world
      .push<"roads">(
          neighbour,
          {
              way(2, {21, 22, 23}, "LINESTRING(10 10,10.5 10.25,11 11)", R"({"highway": "residential", "name": "High Street"})"),
              way(8, {81, 82}, "LINESTRING(11 11,12 12)", "{}"),
          }
      )
      .get();
  CHECK(ids(world.load<"roads">(NORTH_EAST).get()) == std::vector<std::int64_t>{2, 8});  // High Street once, and the neighbour's own way

  // The network pushed again without High Street: it is the neighbour's now, so it stays.
  world.push<"roads">(NETWORK, {way(1, {11, 12}, "LINESTRING(0 0,1 1)", "{}")}).get();
  CHECK(ids(world.load<"roads">(EVERYWHERE).get()) == std::vector<std::int64_t>{1, 2, 8});

  // Removing the neighbour takes High Street with it.
  CHECK(world.remove<"roads">(neighbour).get() == 2);
  CHECK(ids(world.load<"roads">(EVERYWHERE).get()) == std::vector<std::int64_t>{1});
}

TEST_CASE("integration: a table whose setup fails is rolled back, so it can be made once the setup is fixed", "[integration]") {
  miniverse::Miniverse world(test_db(), miniverse::Layer<BrokenSetup, "broken">("miniverse_test_broken"));
  world.drop_tables();

  CHECK_THROWS_AS(world.create_tables(), schemacht::postgres::QueryError);

  const auto rows = world.database().execute(TableExists::bind("miniverse_test_broken")).get();
  REQUIRE(rows.size() == 1);
  CHECK(! schemacht::schema::get<"found">(rows.front()));
}
