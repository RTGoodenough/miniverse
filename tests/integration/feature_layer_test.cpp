// Feature layers against a real PostGIS database: a multipolygon, a polygon, a line and a point layer in one miniverse, each
// one line, pushed and loaded by polygon.
//
// Skipped unless MINIVERSE_TEST_DB holds a libpq connection string to a database with PostGIS enabled. The tests make and drop
// their own tables (miniverse_test_*), so point it at a scratch database.

#include <catch2/catch_test_macros.hpp>

#include <boost/geometry/io/wkt/read.hpp>
#include <boost/geometry/io/wkt/write.hpp>

#include <cstdint>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "miniverse/geo/concepts/wkb.hpp"
#include "miniverse/miniverse.hpp"
#include "schemacht/json/json.hpp"
#include "schemacht/postgres/async_client.hpp"
#include "schemacht/query/raw_statement.hpp"
#include "schemacht/schema/field.hpp"
#include "support/checks.hpp"
#include "support/database.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;

using test::test_db;

namespace {

struct Buildings : miniverse::FeatureLayer<geo::MultiPolygon> {};
struct Parks : miniverse::FeatureLayer<geo::Polygon> {};
struct Paths : miniverse::FeatureLayer<geo::LineString> {};
struct Shops : miniverse::FeatureLayer<geo::Point> {};

using World = miniverse::Miniverse<Buildings, Parks, Paths, Shops>;

using IndexNames = schemacht::query::RawStatement<
    "SELECT indexname FROM pg_indexes WHERE tablename = $1 ORDER BY indexname", schemacht::query::RawArguments<std::string>,
    schemacht::schema::Field<std::string, "indexname">>;

template <geo::wkb::Geometry geometry_t>
[[nodiscard]] miniverse::Feature<geometry_t> feature(std::int64_t id, const std::string& wkt, std::string tags) {
  return {.id = id, .geometry = bg::from_wkt<geometry_t>(wkt), .tags = schemacht::json::Json(std::move(tags))};
}

using test::ids;

const miniverse::Dataset SURVEY{"survey-2026"};
const miniverse::Dataset OTHER{"another-survey"};

const geo::Polygon NEAR_ORIGIN = bg::from_wkt<geo::Polygon>("POLYGON((-1 -1,5 -1,5 5,-1 5,-1 -1))");
const geo::Polygon FAR_AWAY = bg::from_wkt<geo::Polygon>("POLYGON((49 49,52 49,52 52,49 52,49 49))");
const geo::Polygon EVERYWHERE = bg::from_wkt<geo::Polygon>("POLYGON((-10 -10,60 -10,60 60,-10 60,-10 -10))");

/**
 * @brief A world of the four layers, each with one feature near the origin and one far away, pushed as the dataset `SURVEY`;
 * its tables dropped afterwards. The ids a load gives are the database's: 1 and 2, in the order pushed.
 */
[[nodiscard]] test::Fresh<World> with_survey(const std::string& conninfo) {
  return test::Fresh<World>(
      [](World& world) {
        world.create_tables();

        // Started together, then waited for: four layers written at once on one pool.
        std::future<void> buildings = world.push<Buildings>(
            SURVEY,
            {
                feature<geo::MultiPolygon>(
                    1, "MULTIPOLYGON(((0 0,4 0,4 4,0 4,0 0),(1 1,1 2,2 2,2 1,1 1)),((4.5 4.5,4.6 4.5,4.6 4.6,4.5 4.5)))", R"({"building": "yes"})"
                ),
                feature<geo::MultiPolygon>(2, "MULTIPOLYGON(((50 50,51 50,51 51,50 50)))", "{}"),
            }
        );
        std::future<void> parks = world.push<Parks>(
            SURVEY, {
                        feature<geo::Polygon>(1, "POLYGON((0 0,3 0,3 3,0 3,0 0),(1 1,1 2,2 2,2 1,1 1))", R"({"leisure": "park"})"),
                        feature<geo::Polygon>(2, "POLYGON((50 50,51 50,51 51,50 50))", "{}"),
                    }
        );
        std::future<void> paths = world.push<Paths>(
            SURVEY, {
                        feature<geo::LineString>(1, "LINESTRING(0 0,1 1,2 0)", R"({"highway": "path"})"),
                        feature<geo::LineString>(2, "LINESTRING(50 50,51 51)", "{}"),
                    }
        );
        std::future<void> shops = world.push<Shops>(
            SURVEY, {
                        feature<geo::Point>(1, "POINT(1.5 1.5)", R"({"shop": "bakery", "name": "Crumb"})"),
                        feature<geo::Point>(2, "POINT(50.5 50.5)", "{}"),
                    }
        );
        buildings.get();
        parks.get();
        paths.get();
        shops.get();
      },
      conninfo, miniverse::Layer<Buildings>("miniverse_test_buildings"), miniverse::Layer<Parks>("miniverse_test_parks"),
      miniverse::Layer<Paths>("miniverse_test_paths"), miniverse::Layer<Shops>("miniverse_test_shops")
  );
}

}  // namespace

TEST_CASE("integration: a feature load gives the features that intersect the polygon, ordered by id", "[integration]") {
  test::Fresh<World> filled = with_survey(test_db());
  World&             world = filled.world();

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
  test::Fresh<World> filled = with_survey(test_db());
  World&             world = filled.world();

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

TEST_CASE("integration: create_tables makes the spatial index a feature load uses, and the dataset index a push's sweep uses", "[integration]") {
  test::Fresh<World> filled = with_survey(test_db());
  World&             world = filled.world();

  const auto rows = world.database().execute(IndexNames::bind(world.table_name<Shops>().name())).get();

  std::vector<std::string> names;

  names.reserve(rows.size());
  for ( const auto& row : rows ) {
    names.push_back(schemacht::schema::get<"indexname">(row));
  }

  CHECK(names == std::vector<std::string>{"miniverse_test_shops_dataset_idx", "miniverse_test_shops_geom_idx", "miniverse_test_shops_pkey"});
}

namespace {

template <geo::wkb::Geometry geometry_t>
[[nodiscard]] std::vector<std::string> wkts(const miniverse::Features<geometry_t>& features) {
  std::vector<std::string> result;

  result.reserve(features.size());
  for ( const miniverse::Feature<geometry_t>& loaded : features ) {
    result.push_back(bg::to_wkt(loaded.geometry));
  }

  return result;
}

}  // namespace

TEST_CASE("integration: a push of a dataset again replaces it: what the new data lacks is gone, and the ids are new", "[integration]") {
  test::Fresh<World> filled = with_survey(test_db());
  World&             world = filled.world();

  // Shop 1 again as it was, shop 2 gone, a new shop 3: the data's ids mean nothing, the geometries do.
  world.push<Shops>(SURVEY, {feature<geo::Point>(1, "POINT(1.5 1.5)", R"({"shop": "bakery"})"), feature<geo::Point>(3, "POINT(2 2)", "{}")}).get();

  const auto shops = world.load<Shops>(EVERYWHERE).get();
  CHECK(wkts(shops) == std::vector<std::string>{"POINT(1.5 1.5)", "POINT(2 2)"});
  CHECK(ids(shops) == std::vector<std::int64_t>{3, 4});  // numbered on: a feature has nothing to say it is the one pushed before
  CHECK(shops.front().tags.text() == R"({"shop": "bakery"})");

  // Pushed empty, the dataset is gone; the other layers are untouched.
  world.push<Shops>(SURVEY, {}).get();
  CHECK(world.load<Shops>(EVERYWHERE).get().empty());
  CHECK(ids(world.load<Paths>(EVERYWHERE).get()) == std::vector<std::int64_t>{1, 2});
}

TEST_CASE("integration: two datasets of one layer are pushed, replaced and removed apart, and loaded as one", "[integration]") {
  test::Fresh<World> filled = with_survey(test_db());
  World&             world = filled.world();

  world.push<Shops>(OTHER, {feature<geo::Point>(1, "POINT(3 3)", R"({"shop": "books"})")}).get();
  CHECK(wkts(world.load<Shops>(EVERYWHERE).get()) == std::vector<std::string>{"POINT(1.5 1.5)", "POINT(50.5 50.5)", "POINT(3 3)"});

  world.push<Shops>(SURVEY, {feature<geo::Point>(1, "POINT(1.5 1.5)", "{}")}).get();  // the survey's far shop goes; the other dataset stays
  CHECK(wkts(world.load<Shops>(EVERYWHERE).get()) == std::vector<std::string>{"POINT(3 3)", "POINT(1.5 1.5)"});

  CHECK(world.remove<Shops>(OTHER).get() == 1);
  CHECK(wkts(world.load<Shops>(EVERYWHERE).get()) == std::vector<std::string>{"POINT(1.5 1.5)"});

  CHECK(world.remove<Shops>(miniverse::Dataset{"never pushed"}).get() == 0);  // nothing to remove is not an error
  CHECK(wkts(world.load<Shops>(EVERYWHERE).get()) == std::vector<std::string>{"POINT(1.5 1.5)"});
}

TEST_CASE("integration: a push in parts replaces the dataset when it commits, and a part that fails leaves it as it was", "[integration]") {
  test::Fresh<World> filled = with_survey(test_db());
  World&             world = filled.world();

  {
    miniverse::PushInParts<Shops> shops = world.begin_push<Shops>(SURVEY).get();
    shops.add({feature<geo::Point>(1, "POINT(1.5 1.5)", "{}")}).get();
    CHECK(ids(world.load<Shops>(EVERYWHERE).get()) == std::vector<std::int64_t>{1, 2});  // nothing has changed yet
    shops.add({feature<geo::Point>(7, "POINT(7 7)", "{}")}).get();
    shops.commit().get();
  }
  CHECK(wkts(world.load<Shops>(EVERYWHERE).get()) == std::vector<std::string>{"POINT(1.5 1.5)", "POINT(7 7)"});

  {
    miniverse::PushInParts<Shops> shops = world.begin_push<Shops>(SURVEY).get();
    shops.add({feature<geo::Point>(1, "POINT(8 8)", "{}")}).get();

    try {
      shops.add({feature<geo::Point>(2, "POINT(9 9)", "not json")}).get();
      FAIL("the part was written");
    } catch ( const schemacht::postgres::QueryError& error ) {
      CHECK(error.sqlstate() == "22P02");  // invalid text representation: not JSON
    }

    CHECK_THROWS_AS(shops.commit().get(), schemacht::postgres::QueryError);
  }
  CHECK(wkts(world.load<Shops>(EVERYWHERE).get()) == std::vector<std::string>{"POINT(1.5 1.5)", "POINT(7 7)"});
}

TEST_CASE("integration: a push that fails leaves the dataset as it was, and a push of no data empties it", "[integration]") {
  test::Fresh<World> filled = with_survey(test_db());
  World&             world = filled.world();

  // Whole: the invalid JSON fails the statement, and the old rows stay.
  CHECK_THROWS_AS(world.push<Shops>(SURVEY, {feature<geo::Point>(1, "POINT(8 8)", "not json")}).get(), schemacht::postgres::QueryError);
  CHECK(wkts(world.load<Shops>(EVERYWHERE).get()) == std::vector<std::string>{"POINT(1.5 1.5)", "POINT(50.5 50.5)"});

  // In parts, committed with no part: as a push of no data, the dataset is emptied.
  world.begin_push<Shops>(SURVEY).get().commit().get();
  CHECK(world.load<Shops>(EVERYWHERE).get().empty());
}

TEST_CASE("integration: two pushes of one dataset at once take turns, so one of them is what remains", "[integration]") {
  test::Fresh<World> filled = with_survey(test_db());
  World&             world = filled.world();

  // Each on a thread of its own, both as the survey: without the lock, each would sweep only what the other had committed
  // before it began, and both would remain.
  std::thread first([&world] { world.push<Shops>(SURVEY, {feature<geo::Point>(1, "POINT(3 3)", R"({"push": "first"})")}).get(); });
  std::thread second([&world] { world.push<Shops>(SURVEY, {feature<geo::Point>(1, "POINT(4 4)", R"({"push": "second"})")}).get(); });
  first.join();
  second.join();

  const auto shops = world.load<Shops>(EVERYWHERE).get();
  REQUIRE(shops.size() == 1);
  CHECK((bg::to_wkt(shops.front().geometry) == "POINT(3 3)" || bg::to_wkt(shops.front().geometry) == "POINT(4 4)"));
}
