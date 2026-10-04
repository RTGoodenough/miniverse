// A miniverse checking its tables against its layers' kinds, against a real PostGIS database.
//
// Skipped unless MINIVERSE_TEST_DB holds a libpq connection string to a database with PostGIS and postgis_raster enabled, on
// PostgreSQL 17 or newer. The tests make and drop their own tables (miniverse_test_verify_*), so point it at a scratch database.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <tuple>

#include "miniverse/miniverse.hpp"
#include "schemacht/postgres/statements.hpp"

namespace geo = miniverse::geo;

using Catch::Matchers::ContainsSubstring;
using miniverse::Layer;
using miniverse::Miniverse;

namespace {

struct Roads : miniverse::RoadLayer {};
struct Buildings : miniverse::FeatureLayer<geo::MultiPolygon> {};
struct Elevation : miniverse::RasterLayer<std::int16_t> {};

constexpr geo::Grid<std::int16_t> GRID{.pixels_per_degree = 4, .tile_pixels = 4, .nodata = -32768};

[[nodiscard]] std::string test_db() {
  const char* conninfo = std::getenv("MINIVERSE_TEST_DB");
  if ( conninfo == nullptr ) {
    SKIP("MINIVERSE_TEST_DB is not set");
  }

  return conninfo;
}

/** @brief A world of a road, a building and an elevation table, each made as its kind makes it; dropped afterwards. */
class Made {
 public:
  explicit Made(const std::string& conninfo)
      : _world(
            conninfo, Layer<Roads>("miniverse_test_verify_roads"), Layer<Buildings>("miniverse_test_verify_buildings"),
            Layer<Elevation>("miniverse_test_verify_elevation")
        ) {
    _world.drop_tables();
    _world.create_table<Roads>();
    _world.create_table<Buildings>();
    _world.create_table<Elevation>(GRID);
  }

  [[nodiscard]] Miniverse<Roads, Buildings, Elevation>& world() noexcept { return _world; }

 private:
  Miniverse<Roads, Buildings, Elevation> _world;

 public:
  Made(const Made&) = delete;
  Made(Made&&) = delete;
  Made& operator=(const Made&) = delete;
  Made& operator=(Made&&) = delete;
  ~Made() {
    // A lost database must not end the run from a destructor, and the next test drops the tables first anyway.
    try {
      _world.drop_tables();
    } catch ( ... ) {  // NOLINT(bugprone-empty-catch)
    }
  }
};

/** @return What a `verify` that finds differences says; nothing if it finds none. */
template <miniverse::LayerKind... kind_ts>
[[nodiscard]] std::string complaint_of(Miniverse<kind_ts...>& world) {
  try {
    world.verify().get();
  } catch ( const miniverse::TablesDiffer& error ) {
    return error.what();
  }

  return "";
}

}  // namespace

TEST_CASE("integration: a miniverse whose tables its own create_table made verifies", "[integration]") {
  Made made(test_db());

  // Of one layer, and of none, too.
  Miniverse   only_roads(test_db(), Layer<Roads>("miniverse_test_verify_roads"));
  Miniverse<> of_nothing(test_db());

  CHECK_NOTHROW(made.world().verify().get());
  CHECK_NOTHROW(only_roads.verify().get());
  CHECK_NOTHROW(of_nothing.verify().get());
}

TEST_CASE("integration: a raster table in a schema of its own verifies, and is named with it when it does not", "[integration]") {
  Miniverse world(test_db(), Layer<Elevation>("miniverse_test_verify_schema", "elevation"));
  std::ignore = world.database().execute(schemacht::postgres::unchecked_sql("DROP SCHEMA IF EXISTS miniverse_test_verify_schema CASCADE")).get();
  std::ignore = world.database().execute(schemacht::postgres::unchecked_sql("CREATE SCHEMA miniverse_test_verify_schema")).get();
  world.create_table<Elevation>(GRID);

  CHECK_NOTHROW(world.verify().get());
  CHECK(world.table_settings<Elevation>().get() == GRID);

  world.drop_tables();
  CHECK_THAT(complaint_of(world), ContainsSubstring(R"("miniverse_test_verify_schema"."elevation": )") && ContainsSubstring("is not in the database"));

  std::ignore = world.database().execute(schemacht::postgres::unchecked_sql("DROP SCHEMA miniverse_test_verify_schema CASCADE")).get();
}

TEST_CASE("integration: verify names a table that is not there", "[integration]") {
  Made made(test_db());
  std::ignore = made.world().database().execute(schemacht::postgres::unchecked_sql("DROP TABLE miniverse_test_verify_elevation")).get();

  const std::string complaint = complaint_of(made.world());

  CHECK_THAT(complaint, ContainsSubstring(R"("miniverse_test_verify_elevation": )") && ContainsSubstring("is not in the database"));
  CHECK_THAT(complaint, ! ContainsSubstring("miniverse_test_verify_roads") && ! ContainsSubstring("miniverse_test_verify_buildings"));
}

TEST_CASE("integration: verify says how a table of another kind differs", "[integration]") {
  const Made made(test_db());

  // A road layer, and an elevation layer, each aimed at the buildings' table.
  Miniverse aimed_wrong(test_db(), Layer<Roads>("miniverse_test_verify_buildings"));
  Miniverse raster_aimed_wrong(test_db(), Layer<Elevation>("miniverse_test_verify_buildings"));

  const std::string complaint = complaint_of(aimed_wrong);
  const std::string of_raster = complaint_of(raster_aimed_wrong);

  CHECK_THAT(complaint, ContainsSubstring(R"(the column "way_id")") && ContainsSubstring(R"(the column "node_ids")"));
  CHECK_THAT(complaint, ContainsSubstring("geometry(MultiPolygon,4326), not geometry(LineString,4326)"));

  // Its columns say enough: a table that is no raster table is not also told it has no grid.
  CHECK_THAT(of_raster, ContainsSubstring(R"(the column "rast")") && ! ContainsSubstring("no grid"));
}

TEST_CASE("integration: verify says when a raster table has no grid", "[integration]") {
  Made made(test_db());
  std::ignore = made.world().database().execute(schemacht::postgres::unchecked_sql("DROP TABLE miniverse_test_verify_elevation")).get();
  std::ignore = made.world()
                    .database()
                    .execute(schemacht::postgres::unchecked_sql("CREATE TABLE miniverse_test_verify_elevation (tile_id bigint PRIMARY KEY, rast raster NOT NULL)"))
                    .get();

  const std::string complaint = complaint_of(made.world());

  // Its columns are a raster table's: only the grid, which create_table would have recorded, is missing.
  CHECK_THAT(complaint, ContainsSubstring(R"("miniverse_test_verify_elevation": )") && ContainsSubstring("no grid in its raster constraints"));
  CHECK_THAT(complaint, ! ContainsSubstring("the column"));
}

TEST_CASE("integration: verify names every difference of every layer at once, in the layers' order", "[integration]") {
  const Made made(test_db());

  // Roads aimed at the buildings' table, and an elevation table that is not there.
  Miniverse both_wrong(test_db(), Layer<Roads>("miniverse_test_verify_buildings"), Layer<Elevation>("miniverse_test_verify_nothing"));

  const std::string complaint = complaint_of(both_wrong);

  const auto roads = complaint.find(R"("miniverse_test_verify_buildings": )");
  const auto elevation = complaint.find(R"("miniverse_test_verify_nothing": the table)");
  REQUIRE(roads != std::string::npos);
  REQUIRE(elevation != std::string::npos);
  CHECK(roads < elevation);
  CHECK_THAT(complaint, ContainsSubstring("the miniverse's tables are not as its layers say:"));

  // One by one too, for a program that wants to do more than print them.
  try {
    both_wrong.verify().get();
    FAIL("it verified");
  } catch ( const miniverse::TablesDiffer& error ) {
    REQUIRE(error.differences().size() >= 2);
    CHECK(error.differences().front().starts_with(R"("miniverse_test_verify_buildings": )"));
    CHECK(error.differences().back() == R"("miniverse_test_verify_nothing": the table "miniverse_test_verify_nothing" is not in the database)");
  }
}
