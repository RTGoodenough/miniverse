// Feature layers against a real PostGIS database: a multipolygon, a polygon, a line and a point layer in one miniverse, each
// one line, pushed and loaded by polygon.
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

#include "miniverse/geo/concepts/wkb.hpp"
#include "miniverse/miniverse.hpp"
#include "schemacht/json/json.hpp"
#include "schemacht/postgres/async_client.hpp"
#include "schemacht/query/raw_statement.hpp"
#include "schemacht/schema/field.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;

namespace {

struct Buildings : miniverse::FeatureLayer<geo::MultiPolygon> {};
struct Parks : miniverse::FeatureLayer<geo::Polygon> {};
struct Paths : miniverse::FeatureLayer<geo::LineString> {};
struct Shops : miniverse::FeatureLayer<geo::Point> {};

using World = miniverse::Miniverse<Buildings, Parks, Paths, Shops>;

using IndexNames = schemacht::query::RawStatement<
    "SELECT indexname FROM pg_indexes WHERE tablename = $1 ORDER BY indexname", schemacht::query::RawArguments<std::string>,
    schemacht::schema::Field<std::string, "indexname">>;

[[nodiscard]] std::string test_db() {
  const char* conninfo = std::getenv("MINIVERSE_TEST_DB");
  if ( conninfo == nullptr ) {
    SKIP("MINIVERSE_TEST_DB is not set");
  }

  return conninfo;
}

template <geo::wkb::Geometry geometry_t>
[[nodiscard]] miniverse::Feature<geometry_t> feature(std::int64_t id, const std::string& wkt, std::string tags) {
  return {.id = id, .geometry = bg::from_wkt<geometry_t>(wkt), .tags = schemacht::json::Json(std::move(tags))};
}

template <geo::wkb::Geometry geometry_t>
[[nodiscard]] std::vector<std::int64_t> ids(const miniverse::Features<geometry_t>& features) {
  std::vector<std::int64_t> result;

  result.reserve(features.size());
  for ( const miniverse::Feature<geometry_t>& loaded : features ) {
    result.push_back(loaded.id);
  }

  return result;
}

const geo::Polygon NEAR_ORIGIN = bg::from_wkt<geo::Polygon>("POLYGON((-1 -1,5 -1,5 5,-1 5,-1 -1))");
const geo::Polygon FAR_AWAY = bg::from_wkt<geo::Polygon>("POLYGON((49 49,52 49,52 52,49 52,49 49))");
const geo::Polygon EVERYWHERE = bg::from_wkt<geo::Polygon>("POLYGON((-10 -10,60 -10,60 60,-10 60,-10 -10))");

/** @brief A world of the four layers, each with one feature near the origin and one far away; its tables dropped afterwards. */
class Filled {
 public:
  explicit Filled(const std::string& conninfo)
      : _world(
            conninfo, miniverse::Layer<Buildings>("miniverse_test_buildings"), miniverse::Layer<Parks>("miniverse_test_parks"),
            miniverse::Layer<Paths>("miniverse_test_paths"), miniverse::Layer<Shops>("miniverse_test_shops")
        ) {
    _world.drop_tables();
    _world.create_tables();

    // Started together, then waited for: four layers written at once on one pool.
    std::future<void> buildings = _world.push<Buildings>({
        feature<geo::MultiPolygon>(1, "MULTIPOLYGON(((0 0,4 0,4 4,0 4,0 0),(1 1,1 2,2 2,2 1,1 1)),((4.5 4.5,4.6 4.5,4.6 4.6,4.5 4.5)))", R"({"building": "yes"})"),
        feature<geo::MultiPolygon>(2, "MULTIPOLYGON(((50 50,51 50,51 51,50 50)))", "{}"),
    });
    std::future<void> parks = _world.push<Parks>({
        feature<geo::Polygon>(1, "POLYGON((0 0,3 0,3 3,0 3,0 0),(1 1,1 2,2 2,2 1,1 1))", R"({"leisure": "park"})"),
        feature<geo::Polygon>(2, "POLYGON((50 50,51 50,51 51,50 50))", "{}"),
    });
    std::future<void> paths = _world.push<Paths>({
        feature<geo::LineString>(1, "LINESTRING(0 0,1 1,2 0)", R"({"highway": "path"})"),
        feature<geo::LineString>(2, "LINESTRING(50 50,51 51)", "{}"),
    });
    std::future<void> shops = _world.push<Shops>({
        feature<geo::Point>(1, "POINT(1.5 1.5)", R"({"shop": "bakery", "name": "Crumb"})"),
        feature<geo::Point>(2, "POINT(50.5 50.5)", "{}"),
    });
    buildings.get();
    parks.get();
    paths.get();
    shops.get();
  }

  [[nodiscard]] World& world() noexcept { return _world; }

 private:
  World _world;

 public:
  Filled(const Filled&) = delete;
  Filled(Filled&&) = delete;
  Filled& operator=(const Filled&) = delete;
  Filled& operator=(Filled&&) = delete;
  ~Filled() {
    // A lost database must not end the run from a destructor, and the next test drops the tables first anyway.
    try {
      _world.drop_tables();
    } catch ( ... ) {  // NOLINT(bugprone-empty-catch)
    }
  }
};

}  // namespace

TEST_CASE("integration: a feature load gives the features that intersect the polygon, ordered by id", "[integration]") {
  Filled filled(test_db());
  World& world = filled.world();

  CHECK(ids(world.load<Buildings>(NEAR_ORIGIN).get()) == std::vector<std::int64_t>{1});
  CHECK(ids(world.load<Buildings>(FAR_AWAY).get()) == std::vector<std::int64_t>{2});
  CHECK(ids(world.load<Buildings>(EVERYWHERE).get()) == std::vector<std::int64_t>{1, 2});
  CHECK(ids(world.load<Paths>(NEAR_ORIGIN).get()) == std::vector<std::int64_t>{1});
  CHECK(ids(world.load<Shops>(FAR_AWAY).get()) == std::vector<std::int64_t>{2});

  // The shop near the origin is in the hole of building 1's first part: the building does not hold it, its box does.
  const auto in_the_hole = bg::from_wkt<geo::Polygon>("POLYGON((1.4 1.4,1.6 1.4,1.6 1.6,1.4 1.6,1.4 1.4))");
  CHECK(ids(world.load<Shops>(in_the_hole).get()) == std::vector<std::int64_t>{1});
  CHECK(world.load<Buildings>(in_the_hole).get().empty());
}

TEST_CASE("integration: a loaded feature is the feature that was pushed, of each geometry type", "[integration]") {
  Filled filled(test_db());
  World& world = filled.world();

  const auto buildings = world.load<Buildings>(NEAR_ORIGIN).get();
  const auto parks = world.load<Parks>(NEAR_ORIGIN).get();
  const auto paths = world.load<Paths>(NEAR_ORIGIN).get();
  const auto shops = world.load<Shops>(NEAR_ORIGIN).get();

  REQUIRE(buildings.size() == 1);
  CHECK(bg::to_wkt(buildings.front().geometry) == "MULTIPOLYGON(((0 0,4 0,4 4,0 4,0 0),(1 1,1 2,2 2,2 1,1 1)),((4.5 4.5,4.6 4.5,4.6 4.6,4.5 4.5)))");
  CHECK(buildings.front().tags.text() == R"({"building": "yes"})");
  REQUIRE(parks.size() == 1);
  CHECK(bg::to_wkt(parks.front().geometry) == "POLYGON((0 0,3 0,3 3,0 3,0 0),(1 1,1 2,2 2,2 1,1 1))");
  REQUIRE(paths.size() == 1);
  CHECK(bg::to_wkt(paths.front().geometry) == "LINESTRING(0 0,1 1,2 0)");
  REQUIRE(shops.size() == 1);
  CHECK(bg::to_wkt(shops.front().geometry) == "POINT(1.5 1.5)");
  CHECK(shops.front().tags.text() == R"({"name": "Crumb", "shop": "bakery"})");  // jsonb's own order: shorter keys first
}

TEST_CASE("integration: create_tables makes the spatial index a feature load uses", "[integration]") {
  Filled filled(test_db());
  World& world = filled.world();

  const auto rows = world.database().execute(IndexNames::bind(world.table_name<Shops>().name())).get();

  std::vector<std::string> names;

  names.reserve(rows.size());
  for ( const auto& row : rows ) {
    names.push_back(schemacht::schema::get<"indexname">(row));
  }

  CHECK(names == std::vector<std::string>{"miniverse_test_shops_geom_idx", "miniverse_test_shops_pkey"});
}

TEST_CASE("integration: a feature that is already there fails the push, which writes nothing", "[integration]") {
  Filled filled(test_db());
  World& world = filled.world();

  miniverse::Features<geo::Point> again{feature<geo::Point>(3, "POINT(2 2)", "{}"), feature<geo::Point>(1, "POINT(2 2)", "{}")};

  try {
    world.push<Shops>(std::move(again)).get();
    FAIL("the push succeeded");
  } catch ( const schemacht::postgres::QueryError& error ) {
    CHECK(error.sqlstate() == "23505");  // a duplicate key
  }

  CHECK(ids(world.load<Shops>(EVERYWHERE).get()) == std::vector<std::int64_t>{1, 2});
}
