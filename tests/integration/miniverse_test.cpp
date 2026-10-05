// A miniverse against a real PostGIS database: push a small road network, then load it by polygon.
//
// Skipped unless MINIVERSE_TEST_DB holds a libpq connection string to a database with PostGIS enabled. The tests make and drop
// their own tables (miniverse_test_*), so point it at a scratch database.

#include <catch2/catch_test_macros.hpp>

#include <boost/geometry/io/wkt/read.hpp>
#include <boost/geometry/io/wkt/write.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "schemacht/json/json.hpp"
#include "schemacht/postgres/async_client.hpp"
#include "schemacht/query/raw_statement.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/table_name.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;

namespace {

struct TestRoads : miniverse::RoadLayer {};

using World = miniverse::Miniverse<TestRoads>;

[[nodiscard]] std::string test_db() {
  const char* conninfo = std::getenv("MINIVERSE_TEST_DB");
  if ( conninfo == nullptr ) {
    SKIP("MINIVERSE_TEST_DB is not set");
  }

  return conninfo;
}

[[nodiscard]] miniverse::Way way(std::int64_t id, std::vector<std::int64_t> node_ids, const std::string& wkt, std::string tags) {
  return miniverse::Way{
      .id = id, .node_ids = std::move(node_ids), .coordinates = bg::from_wkt<geo::LineString>(wkt), .tags = schemacht::json::Json(std::move(tags))
  };
}

[[nodiscard]] geo::Polygon polygon(const std::string& wkt) { return bg::from_wkt<geo::Polygon>(wkt); }

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

/** @return `WAYS_PER_STATEMENT + 1` ways, one more than a statement holds, with ids from `first_id`, all in the degree from (50, 50). */
[[nodiscard]] miniverse::Ways many_ways(std::int64_t first_id) {
  constexpr std::size_t COUNT = miniverse::RoadLayer::WAYS_PER_STATEMENT + 1;
  miniverse::Ways       ways;

  ways.reserve(COUNT);
  for ( std::size_t i = 0; i < COUNT; ++i ) {
    ways.push_back(way(first_id + static_cast<std::int64_t>(i), {1, 2}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}"));
  }

  return ways;
}

const geo::Polygon FAR_EAST = polygon("POLYGON((50 50,51 50,51 51,50 51,50 50))");

[[nodiscard]] std::vector<std::int64_t> ids(const miniverse::Ways& ways) {
  std::vector<std::int64_t> result;

  result.reserve(ways.size());
  for ( const miniverse::Way& loaded : ways ) {
    result.push_back(loaded.id);
  }

  return result;
}

/** @brief A world with the network in it, its table made fresh and dropped afterwards. */
class Loaded {
 public:
  explicit Loaded(const std::string& conninfo) : _world(conninfo, miniverse::Layer<TestRoads>("miniverse_test_roads")) {
    _world.drop_tables();
    _world.create_tables();
    _world.push<TestRoads>(network()).get();
  }

  [[nodiscard]] World& world() noexcept { return _world; }

 private:
  World _world;

 public:
  Loaded(const Loaded&) = delete;
  Loaded(Loaded&&) = delete;
  Loaded& operator=(const Loaded&) = delete;
  Loaded& operator=(Loaded&&) = delete;
  ~Loaded() {
    // A lost database must not end the run from a destructor, and the next test drops the table first anyway.
    try {
      _world.drop_tables();
    } catch ( ... ) {  // NOLINT(bugprone-empty-catch)
    }
  }
};

// A kind whose setup fails: the table made with it is rolled back.
struct BrokenSetup : miniverse::RoadLayer {
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
  Loaded loaded(test_db());
  World& world = loaded.world();

  CHECK(ids(world.load<TestRoads>(NEAR_ORIGIN).get()) == std::vector<std::int64_t>{1, 3});
  CHECK(ids(world.load<TestRoads>(NORTH_EAST).get()) == std::vector<std::int64_t>{2});
  CHECK(ids(world.load<TestRoads>(EVERYWHERE).get()) == std::vector<std::int64_t>{1, 2, 3, 4});
  CHECK(ids(world.load<TestRoads>(NOWHERE).get()).empty());

  // The triangle's box holds way 4, the triangle does not.
  CHECK(ids(world.load<TestRoads>(TRIANGLE).get()) == std::vector<std::int64_t>{1, 3});
}

TEST_CASE("integration: a loaded way is the way that was pushed", "[integration]") {
  Loaded loaded(test_db());

  const miniverse::Ways ways = loaded.world().load<TestRoads>(NORTH_EAST).get();

  REQUIRE(ways.size() == 1);
  const miniverse::Way& high_street = ways.front();
  CHECK(high_street.id == 2);
  CHECK(high_street.node_ids == std::vector<std::int64_t>{21, 22, 23});
  CHECK(bg::to_wkt(high_street.coordinates) == "LINESTRING(10 10,10.5 10.25,11 11)");
  CHECK(high_street.tags.text() == R"({"name": "High Street", "highway": "residential"})");  // jsonb's own order: shorter keys first
}

TEST_CASE("integration: several loads run at once on one miniverse", "[integration]") {
  Loaded loaded(test_db());
  World& world = loaded.world();

  std::future<miniverse::Ways> south_west = world.load<TestRoads>(NEAR_ORIGIN);
  std::future<miniverse::Ways> north_east = world.load<TestRoads>(NORTH_EAST);

  CHECK(ids(north_east.get()) == std::vector<std::int64_t>{2});
  CHECK(ids(south_west.get()) == std::vector<std::int64_t>{1, 3});
}

TEST_CASE("integration: create_tables makes the spatial index the loads use", "[integration]") {
  Loaded loaded(test_db());

  World& world = loaded.world();

  const auto rows = world.database().execute(IndexNames::bind(world.table_name<TestRoads>().name())).get();

  std::vector<std::string> names;

  names.reserve(rows.size());
  for ( const auto& row : rows ) {
    names.push_back(schemacht::schema::get<"indexname">(row));
  }

  CHECK(names == std::vector<std::string>{"miniverse_test_roads_geom_idx", "miniverse_test_roads_pkey"});
}

TEST_CASE("integration: a stream of roads hands the ways over a chunk at a time, ordered by id", "[integration]") {
  Loaded loaded(test_db());

  // On a pool thread, one call at a time: read here only once the future is done.
  std::vector<std::vector<std::int64_t>> chunks;

  loaded.world().stream<TestRoads>(EVERYWHERE, [&](const miniverse::Ways& chunk) { chunks.push_back(ids(chunk)); }, {.chunk_rows = 3}).get();

  CHECK(chunks == std::vector<std::vector<std::int64_t>>{{1, 2, 3}, {4}});
}

TEST_CASE("integration: a load that fails reports why through its future", "[integration]") {
  Loaded loaded(test_db());
  World& world = loaded.world();
  world.drop_tables();

  std::future<miniverse::Ways> roads = world.load<TestRoads>(EVERYWHERE);

  CHECK_THROWS_AS(roads.get(), schemacht::postgres::QueryError);  // the table is gone
}

TEST_CASE("integration: a push that breaks the table's rules writes nothing and reports why", "[integration]") {
  Loaded loaded(test_db());
  World& world = loaded.world();

  miniverse::Ways again{way(1, {11, 12}, "LINESTRING(0 0,1 1)", "{}"), way(5, {51, 52}, "LINESTRING(0 0,1 1)", "{}")};
  CHECK_THROWS(world.push<TestRoads>(std::move(again)).get());  // way 1 is there already

  CHECK(ids(world.load<TestRoads>(EVERYWHERE).get()) == std::vector<std::int64_t>{1, 2, 3, 4});
}

TEST_CASE("integration: a push of more ways than one statement holds writes them all", "[integration]") {
  Loaded loaded(test_db());
  World& world = loaded.world();

  world.push<TestRoads>(many_ways(1000)).get();

  const miniverse::Ways far_east = world.load<TestRoads>(FAR_EAST).get();
  CHECK(far_east.size() == miniverse::RoadLayer::WAYS_PER_STATEMENT + 1);
  CHECK(far_east.front().id == 1000);
  CHECK(far_east.back().id == 1000 + static_cast<std::int64_t>(miniverse::RoadLayer::WAYS_PER_STATEMENT));
}

TEST_CASE("integration: a push whose later statement breaks the table's rules writes nothing", "[integration]") {
  Loaded loaded(test_db());
  World& world = loaded.world();

  miniverse::Ways ways = many_ways(1000);
  ways.back().id = 1;  // in the second statement: way 1 is there already

  // The error is the second statement's own (a duplicate key), not the commit's "rolled back".
  try {
    world.push<TestRoads>(std::move(ways)).get();
    FAIL("the push succeeded");
  } catch ( const schemacht::postgres::QueryError& error ) {
    CHECK(error.sqlstate() == "23505");
  }

  CHECK(world.load<TestRoads>(FAR_EAST).get().empty());  // the first statement's ways are rolled back too
  CHECK(ids(world.load<TestRoads>(EVERYWHERE).get()) == std::vector<std::int64_t>{1, 2, 3, 4});
}

TEST_CASE("integration: a push in parts is there for everyone once it commits, and for no one before", "[integration]") {
  Loaded loaded(test_db());
  World& world = loaded.world();

  miniverse::PushInParts<TestRoads> parts = world.begin_push<TestRoads>().get();
  parts.add({way(10, {1, 2}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}"), way(11, {2, 3}, "LINESTRING(50.2 50.2,50.3 50.3)", "{}")}).get();
  parts.add({way(12, {3, 4}, "LINESTRING(50.3 50.3,50.4 50.4)", "{}")}).get();
  const std::vector<std::int64_t> before = ids(world.load<TestRoads>(FAR_EAST).get());  // on another of the pool's connections
  parts.commit().get();

  CHECK(before.empty());
  CHECK(ids(world.load<TestRoads>(FAR_EAST).get()) == std::vector<std::int64_t>{10, 11, 12});
}

TEST_CASE("integration: a push in parts that is dropped without a commit writes nothing", "[integration]") {
  Loaded loaded(test_db());
  World& world = loaded.world();

  {
    miniverse::PushInParts<TestRoads> parts = world.begin_push<TestRoads>().get();
    parts.add({way(10, {1, 2}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}")}).get();
  }

  CHECK(world.load<TestRoads>(FAR_EAST).get().empty());
  CHECK(ids(world.load<TestRoads>(EVERYWHERE).get()) == std::vector<std::int64_t>{1, 2, 3, 4});
}

TEST_CASE("integration: a part that fails ends a push in parts, which then writes nothing", "[integration]") {
  Loaded loaded(test_db());
  World& world = loaded.world();

  miniverse::PushInParts<TestRoads> parts = world.begin_push<TestRoads>().get();
  parts.add({way(10, {1, 2}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}")}).get();

  // Way 1 is there already: the part fails with its own error, a duplicate key.
  try {
    parts.add({way(1, {11, 12}, "LINESTRING(0 0,1 1)", "{}")}).get();
    FAIL("the part was added");
  } catch ( const schemacht::postgres::QueryError& error ) {
    CHECK(error.sqlstate() == "23505");
  }

  // The push has ended: every later call fails with that part's error, and an empty part too.
  CHECK_THROWS_AS(parts.add({way(13, {1, 2}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}")}).get(), schemacht::postgres::QueryError);
  CHECK_THROWS_AS(parts.add({}).get(), schemacht::postgres::QueryError);
  CHECK_THROWS_AS(parts.commit().get(), schemacht::postgres::QueryError);
  CHECK(world.load<TestRoads>(FAR_EAST).get().empty());  // not even the first part
}

TEST_CASE("integration: a part whose rows can't be made ends a push in parts too", "[integration]") {
  Loaded loaded(test_db());
  World& world = loaded.world();

  miniverse::PushInParts<TestRoads> parts = world.begin_push<TestRoads>().get();
  parts.add({way(10, {1, 2}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}")}).get();

  // Three node ids for two points: refused before anything is sent. The file would have a hole if the push went on.
  CHECK_THROWS_AS(parts.add({way(11, {1, 2, 3}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}")}).get(), std::invalid_argument);
  CHECK_THROWS_AS(parts.add({way(12, {1, 2}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}")}).get(), std::invalid_argument);
  CHECK_THROWS_AS(parts.commit().get(), std::invalid_argument);
  CHECK(world.load<TestRoads>(FAR_EAST).get().empty());
}

TEST_CASE("integration: a push in parts that was moved from takes no more calls", "[integration]") {
  Loaded loaded(test_db());
  World& world = loaded.world();

  miniverse::PushInParts<TestRoads> first = world.begin_push<TestRoads>().get();
  miniverse::PushInParts<TestRoads> second = std::move(first);
  second.add({way(10, {1, 2}, "LINESTRING(50.1 50.1,50.2 50.2)", "{}")}).get();

  CHECK_THROWS_AS(first.add({}).get(), std::logic_error);  // NOLINT(bugprone-use-after-move) -- what is under test
  CHECK_THROWS_AS(first.commit().get(), std::logic_error);
  second.commit().get();
  CHECK(ids(world.load<TestRoads>(FAR_EAST).get()) == std::vector<std::int64_t>{10});
}

TEST_CASE("integration: a table whose setup fails is rolled back, so it can be made once the setup is fixed", "[integration]") {
  miniverse::Miniverse world(test_db(), miniverse::Layer<BrokenSetup>("miniverse_test_broken"));
  world.drop_tables();

  CHECK_THROWS_AS(world.create_tables(), schemacht::postgres::QueryError);

  const auto rows = world.database().execute(TableExists::bind("miniverse_test_broken")).get();
  REQUIRE(rows.size() == 1);
  CHECK(! schemacht::schema::get<"found">(rows.front()));
}
