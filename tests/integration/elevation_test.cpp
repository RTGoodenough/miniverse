// An elevation layer against a real PostGIS database: tiles made by PostGIS itself, loaded by polygon and compared pixel for
// pixel, and rasters pushed and loaded back.
//
// Skipped unless MINIVERSE_TEST_DB holds a libpq connection string to a database with PostGIS and postgis_raster enabled. The
// tests make and drop their own table (miniverse_test_elevation), so point it at a scratch database.

#include <catch2/catch_test_macros.hpp>

#include <boost/geometry/io/wkt/read.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "schemacht/postgres/async_client.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/query/raw_statement.hpp"
#include "schemacht/schema/field.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;
namespace sch = schemacht::schema;

namespace {

struct TestElevation : miniverse::ElevationLayer<std::int16_t> {};
struct FineElevation : miniverse::ElevationLayer<std::int16_t> {};

using World = miniverse::Miniverse<TestElevation>;
using Raster = geo::Raster<std::int16_t>;

constexpr std::int16_t NODATA = -32768;

// Four pixels to a degree, in tiles of one degree.
constexpr geo::Grid<std::int16_t> GRID{.pixels_per_degree = 4, .tile_pixels = 4, .nodata = NODATA};

[[nodiscard]] std::string test_db() {
  const char* conninfo = std::getenv("MINIVERSE_TEST_DB");
  if ( conninfo == nullptr ) {
    SKIP("MINIVERSE_TEST_DB is not set");
  }

  return conninfo;
}

[[nodiscard]] geo::Polygon polygon(const std::string& wkt) { return bg::from_wkt<geo::Polygon>(wkt); }

/**
 * Three of the four tiles from longitude 0 to 2 and latitude 2 to 0, made by PostGIS: each pixel is `column * 100 + row`,
 * counted in pixels from (0, 2). The north-east tile, 181 across and 88 down, is missing. 360 tiles span the world.
 */
using InsertTiles = schemacht::query::RawStatement<
    "INSERT INTO miniverse_test_elevation (tile_id, rast) "
    "SELECT r * 360 + c, ST_MapAlgebra(ST_AddBand(ST_MakeEmptyRaster(4, 4, -180 + c, 90 - r, 0.25, -0.25, 0, 0, 4326), '16BSI'::text, 0, -32768), "
    "1, '16BSI', format('(%s + [rast.x] - 1) * 100 + (%s + [rast.y] - 1)', c * 4 - 720, r * 4 - 352)) "
    "FROM (VALUES (180, 88), (180, 89), (181, 89)) AS tiles (c, r) RETURNING tile_id",
    schemacht::query::RawArguments<>, sch::Field<std::int64_t, "tile_id">>;

/** @return The pixel the tiles above have `column` across and `row` down from (0, 2). */
[[nodiscard]] std::int16_t made(std::size_t column, std::size_t row) {
  constexpr std::size_t TILE = 4;
  if ( column >= TILE && row < TILE ) {
    return NODATA;  // the missing tile
  }

  return static_cast<std::int16_t>((column * 100) + row);
}

/** @brief A world with the tiles above in it, its table made fresh and dropped afterwards. */
class Tiled {
 public:
  explicit Tiled(const std::string& conninfo, const schemacht::postgres::Database::Options& options = {})
      : _world(conninfo, options, miniverse::Layer<TestElevation>("miniverse_test_elevation")) {
    _world.drop_tables();
    _world.create_table<TestElevation>(GRID);
    std::ignore = _world.database().execute(InsertTiles::bind()).get();
  }

  [[nodiscard]] World& world() noexcept { return _world; }

 private:
  World _world;

 public:
  Tiled(const Tiled&) = delete;
  Tiled(Tiled&&) = delete;
  Tiled& operator=(const Tiled&) = delete;
  Tiled& operator=(Tiled&&) = delete;
  ~Tiled() {
    // A lost database must not end the run from a destructor, and the next test drops the table first anyway.
    try {
      _world.drop_tables();
    } catch ( ... ) {  // NOLINT(bugprone-empty-catch)
    }
  }
};

using RasterColumns = schemacht::query::RawStatement<
    "SELECT srid, scale_x, scale_y, blocksize_x, blocksize_y, same_alignment, num_bands, pixel_types::text AS pixel_types, "
    "nodata_values::text AS nodata_values, out_db::text AS out_db, ST_AsText(extent) AS extent "
    "FROM raster_columns WHERE r_table_name = $1",
    schemacht::query::RawArguments<std::string>, sch::Field<std::int32_t, "srid">, sch::Field<double, "scale_x">, sch::Field<double, "scale_y">,
    sch::Field<std::int32_t, "blocksize_x">, sch::Field<std::int32_t, "blocksize_y">, sch::Field<bool, "same_alignment">,
    sch::Field<std::int32_t, "num_bands">, sch::Field<std::string, "pixel_types">, sch::Field<std::string, "nodata_values">,
    sch::Field<std::string, "out_db">, sch::Field<std::string, "extent">>;

using SeqScanOff = schemacht::query::RawStatement<
    "SELECT set_config('enable_seqscan', 'off', false) AS setting", schemacht::query::RawArguments<>, sch::Field<std::string, "setting">>;

/** @return The plan PostgreSQL makes for `sql` with the one argument `argument`, a line per row, on the pool's next connection. */
[[nodiscard]] std::string plan_of(schemacht::postgres::Database& database, std::string sql, std::string argument) {
  std::promise<std::string> promise;
  std::future<std::string>  plan = promise.get_future();

  database.pool().run(
      "EXPLAIN " + std::move(sql), {std::move(argument)},
      [&promise](const schemacht::postgres::QueryResult& result, const std::exception_ptr& error) {
        if ( error ) {
          promise.set_exception(error);
          return;
        }

        std::string lines;
        for ( std::size_t row = 0; row < result.row_count(); ++row ) {
          lines += std::string(result.field(row, 0).value_or("")) + "\n";
        }

        promise.set_value(std::move(lines));
      }
  );

  return plan.get();
}

using IndexDefinitions = schemacht::query::RawStatement<
    "SELECT indexdef FROM pg_indexes WHERE tablename = $1 ORDER BY indexname", schemacht::query::RawArguments<std::string>,
    sch::Field<std::string, "indexdef">>;

}  // namespace

TEST_CASE("integration: an elevation load gives the pixels in the polygon's box, nodata where no tile is", "[integration]") {
  Tiled tiled(test_db());

  const Raster heights = tiled.world().load<TestElevation>(polygon("POLYGON((0 0,2 0,2 2,0 2,0 0))")).get();

  CHECK(heights.west == 0);
  CHECK(heights.north == 2);
  CHECK(heights.pixel_width == 0.25);
  CHECK(heights.pixel_height == 0.25);
  CHECK(heights.nodata == NODATA);
  REQUIRE(heights.width == 8);
  REQUIRE(heights.height == 8);
  for ( std::size_t row = 0; row < heights.height; ++row ) {
    for ( std::size_t column = 0; column < heights.width; ++column ) {
      CHECK(heights.at(column, row) == made(column, row));
    }
  }

  // The box, not the polygon: a triangle's pixels are its box's.
  CHECK(tiled.world().load<TestElevation>(polygon("POLYGON((0 0,2 0,0 2,0 0))")).get() == heights);
}

TEST_CASE("integration: an elevation load inside one tile is widened to whole pixels", "[integration]") {
  Tiled tiled(test_db());

  const Raster heights = tiled.world().load<TestElevation>(polygon("POLYGON((0.3 1.3,0.6 1.3,0.6 1.6,0.3 1.6,0.3 1.3))")).get();

  CHECK(heights.west == 0.25);
  CHECK(heights.north == 1.75);
  CHECK(heights.width == 2);
  CHECK(heights.height == 2);
  CHECK(heights.pixels == std::vector<std::int16_t>{101, 201, 102, 202});
}

TEST_CASE("integration: an elevation load where there are no tiles is empty", "[integration]") {
  Tiled tiled(test_db());

  const Raster heights = tiled.world().load<TestElevation>(polygon("POLYGON((30 30,31 30,31 31,30 31,30 30))")).get();

  CHECK(heights.width == 0);
  CHECK(heights.pixels.empty());
}

TEST_CASE("integration: an elevation table records its grid where GDAL and miniverse read it", "[integration]") {
  Tiled  tiled(test_db());
  World& world = tiled.world();

  CHECK(world.settings<TestElevation>().get() == GRID);

  const auto columns = world.database().execute(RasterColumns::bind(world.table_name<TestElevation>().name())).get();
  REQUIRE(columns.size() == 1);
  const auto& row = columns.front();
  CHECK(sch::get<"srid">(row) == 4326);
  CHECK(sch::get<"scale_x">(row) == 0.25);
  CHECK(sch::get<"scale_y">(row) == -0.25);
  CHECK(sch::get<"blocksize_x">(row) == 4);
  CHECK(sch::get<"blocksize_y">(row) == 4);
  CHECK(sch::get<"same_alignment">(row));
  CHECK(sch::get<"num_bands">(row) == 1);
  CHECK(sch::get<"pixel_types">(row) == "{16BSI}");
  CHECK(sch::get<"nodata_values">(row) == "{-32768}");
  CHECK(sch::get<"out_db">(row) == "{f}");
  CHECK(  // the whole world, and a hair more for rounding
      sch::get<"extent">(row) ==
      "POLYGON((-180.000000001 -90.000000001,-180.000000001 90.000000001,180.000000001 90.000000001,180.000000001 -90.000000001,"
      "-180.000000001 -90.000000001))"
  );
}

TEST_CASE("integration: create_table makes the index on the tiles' outlines that the loads use", "[integration]") {
  Tiled  tiled(test_db());
  World& world = tiled.world();

  const auto rows = world.database().execute(IndexDefinitions::bind(world.table_name<TestElevation>().name())).get();

  REQUIRE(rows.size() == 2);
  CHECK(sch::get<"indexdef">(rows.at(0)).ends_with("USING btree (tile_id)"));  // the primary key's (_pkey)
  CHECK(sch::get<"indexdef">(rows.at(1)).ends_with("USING gist (st_convexhull(rast))"));
}

TEST_CASE("integration: a pushed raster is cut into the table's tiles and loaded back", "[integration]") {
  Tiled  tiled(test_db());
  World& world = tiled.world();

  // The missing tile's degree, from (1, 2) to (2, 1).
  Raster pushed{.west = 1, .north = 2, .pixel_width = 0.25, .pixel_height = 0.25, .width = 4, .height = 4, .nodata = NODATA, .pixels = {}};
  for ( std::int16_t pixel = 0; pixel < 16; ++pixel ) {
    pushed.pixels.push_back(pixel);
  }

  world.push<TestElevation>(pushed).get();

  CHECK(world.load<TestElevation>(polygon("POLYGON((1 1,2 1,2 2,1 2,1 1))")).get() == pushed);
}

TEST_CASE("integration: a push onto a tile that is there already writes nothing and reports why", "[integration]") {
  Tiled  tiled(test_db());
  World& world = tiled.world();

  // Two degrees, from (0, 2) to (2, 1): the tile 180 across and 88 down is there, the one 181 across is not.
  Raster pushed{
      .west = 0,
      .north = 2,
      .pixel_width = 0.25,
      .pixel_height = 0.25,
      .width = 8,
      .height = 4,
      .nodata = NODATA,
      .pixels = std::vector<std::int16_t>(32, 7)
  };

  CHECK_THROWS_AS(world.push<TestElevation>(std::move(pushed)).get(), schemacht::postgres::QueryError);

  // Still no tile there: a load inside it finds none.
  CHECK(world.load<TestElevation>(polygon("POLYGON((1.25 1.25,1.75 1.25,1.75 1.75,1.25 1.75,1.25 1.25))")).get().pixels.empty());
}

TEST_CASE("integration: a raster not on the table's grid is not pushed", "[integration]") {
  Tiled tiled(test_db());

  Raster coarse{.west = 1, .north = 2, .pixel_width = 0.5, .pixel_height = 0.5, .width = 1, .height = 1, .nodata = NODATA, .pixels = {1}};

  CHECK_THROWS_AS(tiled.world().push<TestElevation>(std::move(coarse)).get(), std::invalid_argument);
}

TEST_CASE("integration: an elevation load is answered by the index on the tiles' outlines", "[integration]") {
  // One connection, so the setting that rules out a table scan holds for the EXPLAIN that follows it.
  Tiled  tiled(test_db(), {.pool = {.connections = 1}, .read = {}});
  World& world = tiled.world();
  std::ignore = world.database().execute(SeqScanOff::bind()).get();

  const geo::Polygon area = polygon("POLYGON((0 0,1 0,1 1,0 1,0 0))");
  const std::string sql(TestElevation::load_statement_type::bind(area).on(miniverse::Layer<TestElevation>("miniverse_test_elevation").table()).sql());

  const std::string plan = plan_of(world.database(), sql, schemacht::ColumnType<geo::Polygon>::format(area));

  CHECK(plan.contains("Index Scan using miniverse_test_elevation_st_convexhull_idx"));
}

TEST_CASE("integration: a push of several new tiles writes them all", "[integration]") {
  Tiled  tiled(test_db());
  World& world = tiled.world();

  // Three degrees, from (10, 2) to (13, 1): three new tiles in one insert.
  Raster pushed{.west = 10, .north = 2, .pixel_width = 0.25, .pixel_height = 0.25, .width = 12, .height = 4, .nodata = NODATA, .pixels = {}};
  for ( std::int16_t pixel = 0; pixel < 48; ++pixel ) {
    pushed.pixels.push_back(pixel);
  }

  world.push<TestElevation>(pushed).get();

  CHECK(world.load<TestElevation>(polygon("POLYGON((10 1,13 1,13 2,10 2,10 1))")).get() == pushed);
}

TEST_CASE("integration: on a grid whose arithmetic rounds, the world's last column and row of tiles can be written", "[integration]") {
  // 1/3600 has no exact double, so the far edge of the last tile computes a rounding step past 180 and -90.
  miniverse::Miniverse world(test_db(), miniverse::Layer<FineElevation>("miniverse_test_elevation_fine"));
  world.drop_tables();
  world.create_table<FineElevation>({.pixels_per_degree = 3600, .tile_pixels = 1200, .nodata = NODATA});

  // The world's south-east pixel.
  const double pixel = 1.0 / 3600;
  const Raster corner{
      .west = 180 - pixel,
      .north = -90 + pixel,
      .pixel_width = pixel,
      .pixel_height = pixel,
      .width = 1,
      .height = 1,
      .nodata = NODATA,
      .pixels = {42}
  };

  world.push<FineElevation>(corner).get();

  const geo::Polygon around = polygon("POLYGON((179.9999 -89.9999,179.99999 -89.9999,179.99999 -89.99999,179.9999 -89.99999,179.9999 -89.9999))");
  CHECK(world.load<FineElevation>(around).get().pixels.back() == 42);

  world.drop_tables();
}
