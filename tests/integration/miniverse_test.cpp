// A miniverse against a real PostGIS database: push a small road network, then load it by polygon.
//
// Skipped unless MINIVERSE_TEST_DB holds a libpq connection string to a database with PostGIS enabled. The tests make and drop
// their own tables (miniverse_test_*), so point it at a scratch database.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "schemacht/json/json.hpp"
#include "schemacht/query/raw_statement.hpp"
#include "schemacht/schema/field.hpp"
#include "support/geometry.hpp"

namespace geo = miniverse::geo;
namespace test = miniverse::test;

namespace {

struct TestRoads : miniverse::RoadLayer<"miniverse_test_roads"> {};

using World = miniverse::Miniverse<TestRoads>;

[[nodiscard]] std::string test_db() {
  const char* conninfo = std::getenv("MINIVERSE_TEST_DB");
  if ( conninfo == nullptr ) {
    SKIP("MINIVERSE_TEST_DB is not set");
  }
  return conninfo;
}

[[nodiscard]] miniverse::Way way(std::int64_t id, std::vector<std::int64_t> node_ids, geo::LineString coordinates, std::string tags) {
  return miniverse::Way{
      .id = id, .node_ids = std::move(node_ids), .coordinates = std::move(coordinates), .tags = schemacht::json::Json(std::move(tags))
  };
}

/**
 * The network, in degrees:
 * - 1 near the origin, 3 just north of it, 2 far to the north-east
 * - 4 inside the box of the triangle (0 0, 4 0, 0 4), but beyond its long side: a bounding-box test would take it, ST_Intersects does not
 */
[[nodiscard]] miniverse::Ways network() {
  return {
      way(1, {11, 12}, test::line({{0, 0}, {1, 1}}), R"({"highway": "primary"})"),
      way(2, {21, 22, 23}, test::line({{10, 10}, {10.5, 10.25}, {11, 11}}), R"({"highway": "residential", "name": "High Street"})"),
      way(3, {31, 32}, test::line({{0.5, 2}, {0.5, 3}}), "{}"),
      way(4, {41, 42}, test::line({{3, 3}, {4, 2}}), R"({"highway": "track"})"),
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
  explicit Loaded(const std::string& conninfo) : _world(conninfo) {
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
  ~Loaded() { _world.drop_tables(); }
};

using IndexNames = schemacht::query::RawStatement<
    "SELECT indexname FROM pg_indexes WHERE tablename = $1 ORDER BY indexname", schemacht::query::RawArguments<std::string>,
    schemacht::schema::Field<std::string, "indexname">>;

}  // namespace

TEST_CASE("integration: a load gives the ways that intersect the polygon, ordered by id", "[integration]") {
  Loaded loaded(test_db());
  World& world = loaded.world();

  CHECK(ids(world.load<TestRoads>(test::rectangle(-1, -1, 2, 4)).get()) == std::vector<std::int64_t>{1, 3});
  CHECK(ids(world.load<TestRoads>(test::rectangle(5, 5, 20, 20)).get()) == std::vector<std::int64_t>{2});
  CHECK(ids(world.load<TestRoads>(test::rectangle(-1, -1, 20, 20)).get()) == std::vector<std::int64_t>{1, 2, 3, 4});
  CHECK(ids(world.load<TestRoads>(test::rectangle(30, 30, 31, 31)).get()).empty());

  // The triangle's box holds way 4, the triangle does not.
  CHECK(ids(world.load<TestRoads>(test::polygon({{0, 0}, {4, 0}, {0, 4}, {0, 0}})).get()) == std::vector<std::int64_t>{1, 3});
}

TEST_CASE("integration: a loaded way is the way that was pushed", "[integration]") {
  Loaded loaded(test_db());

  const miniverse::Ways ways = loaded.world().load<TestRoads>(test::rectangle(5, 5, 20, 20)).get();

  REQUIRE(ways.size() == 1);
  const miniverse::Way& high_street = ways.front();
  CHECK(high_street.id == 2);
  CHECK(high_street.node_ids == std::vector<std::int64_t>{21, 22, 23});
  CHECK(test::coordinates(high_street.coordinates) == test::Coordinates{{10, 10}, {10.5, 10.25}, {11, 11}});
  CHECK(high_street.tags.text() == R"({"name": "High Street", "highway": "residential"})");  // jsonb's own order: shorter keys first
}

TEST_CASE("integration: several loads run at once on one miniverse", "[integration]") {
  Loaded loaded(test_db());
  World& world = loaded.world();

  std::future<miniverse::Ways> south_west = world.load<TestRoads>(test::rectangle(-1, -1, 2, 4));
  std::future<miniverse::Ways> north_east = world.load<TestRoads>(test::rectangle(5, 5, 20, 20));

  CHECK(ids(north_east.get()) == std::vector<std::int64_t>{2});
  CHECK(ids(south_west.get()) == std::vector<std::int64_t>{1, 3});
}

TEST_CASE("integration: create_tables makes the spatial index the loads use", "[integration]") {
  Loaded loaded(test_db());

  const auto rows = loaded.world().database().execute(IndexNames::bind("miniverse_test_roads")).get();

  std::vector<std::string> names;
  names.reserve(rows.size());
  for ( const auto& row : rows ) {
    names.push_back(schemacht::schema::get<"indexname">(row));
  }
  CHECK(names == std::vector<std::string>{"miniverse_test_roads_geom_idx", "miniverse_test_roads_pkey"});
}

TEST_CASE("integration: a push that breaks the table's rules writes nothing and reports why", "[integration]") {
  Loaded loaded(test_db());
  World& world = loaded.world();

  const miniverse::Ways again{way(1, {11, 12}, test::line({{0, 0}, {1, 1}}), "{}"), way(5, {51, 52}, test::line({{0, 0}, {1, 1}}), "{}")};
  CHECK_THROWS(world.push<TestRoads>(again).get());  // way 1 is there already

  CHECK(ids(world.load<TestRoads>(test::rectangle(-1, -1, 20, 20)).get()) == std::vector<std::int64_t>{1, 2, 3, 4});
}
