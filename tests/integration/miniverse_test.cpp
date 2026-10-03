// A miniverse against a real PostGIS database: push a small road network, then load it by polygon.
//
// Skipped unless MINIVERSE_TEST_DB holds a libpq connection string to a database with PostGIS enabled. The tests make and drop
// their own tables (miniverse_test_*), so point it at a scratch database.

#include <catch2/catch_test_macros.hpp>

#include <boost/geometry/io/wkt/read.hpp>
#include <boost/geometry/io/wkt/write.hpp>

#include <cstdint>
#include <cstdlib>
#include <future>
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

// A kind whose setup fails: a table it makes is dropped again.
struct BrokenSetup : miniverse::RoadLayer {
  [[nodiscard]] static std::vector<std::string> setup_sql(const schemacht::schema::TableName& /*table*/, miniverse::NoSettings /*settings*/) {
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

TEST_CASE("integration: a table whose setup fails is dropped again, so it can be made once the setup is fixed", "[integration]") {
  miniverse::Miniverse world(test_db(), miniverse::Layer<BrokenSetup>("miniverse_test_broken"));
  world.drop_tables();

  CHECK_THROWS_AS(world.create_tables(), schemacht::postgres::QueryError);

  const auto rows = world.database().execute(TableExists::bind("miniverse_test_broken")).get();
  REQUIRE(rows.size() == 1);
  CHECK(! schemacht::schema::get<"found">(rows.front()));
}
