// The raster kind without a database: the table's setup, its rows made from a raster in batches and back into the window
// a load asks for, and its grid read back. Cutting and stitching are raster_tiles_test.cpp; which tiles a location has,
// raster_reach_test.cpp.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/table_name.hpp"
#include "support/rasters.hpp"

namespace geo = miniverse::geo;

using test::GRID;
using test::NODATA;
using test::numbered;
using test::polygon;

namespace {

struct Elevation : miniverse::RasterLayer<std::int16_t> {};

using Raster = geo::Raster<std::int16_t>;
using LoadedRow = Elevation::load_statement_type::result_type;
using SettingsRow = Elevation::settings_statement_type::row_type;

/** @return The rows of `batches`, in order: for checks that don't care about the batching. */
[[nodiscard]] std::vector<Elevation::row_type> rows(std::vector<std::vector<Elevation::row_type>> batches) {
  std::vector<Elevation::row_type> all;

  for ( auto& batch : batches ) {
    all.insert(all.end(), std::move_iterator(batch.begin()), std::move_iterator(batch.end()));
  }

  return all;
}

/** @return The tiles of `rows`, as a load reads them. */
[[nodiscard]] std::vector<LoadedRow> loaded(std::vector<Elevation::row_type> rows) {
  std::vector<LoadedRow> tiles;

  tiles.reserve(rows.size());
  for ( Elevation::row_type& row : rows ) {
    tiles.emplace_back(miniverse::raster::Rast<std::int16_t>{std::move(schemacht::schema::get<"rast">(row))});
  }

  return tiles;
}

}  // namespace

TEST_CASE(
    "raster layer: the table's setup is a lock, the shared merge function, its compression, its index and the constraints that record its grid",
    "[raster_layer]"
) {
  const schemacht::schema::TableName table("srtm");

  const std::vector<std::string> setup = Elevation::setup_sql(table, GRID);

  REQUIRE(setup.size() == 6);
  CHECK(setup.at(0).find("pg_advisory_xact_lock") != std::string::npos);  // first: setups take turns at what follows
  CHECK(setup.at(1) == "CREATE SCHEMA IF NOT EXISTS miniverse_functions");
  CHECK(setup.at(2).starts_with("CREATE OR REPLACE FUNCTION miniverse_functions.merge_raster("));  // what it does: tests/integration
  CHECK(
      std::vector<std::string>(setup.begin() + 3, setup.end()) ==
      std::vector<std::string>{
          R"(ALTER TABLE "srtm" ALTER COLUMN rast SET COMPRESSION lz4)",
          R"(CREATE INDEX ON "srtm" USING gist (ST_ConvexHull(rast)))",
          R"(ALTER TABLE "srtm" ADD CONSTRAINT enforce_srid_rast CHECK (ST_SRID(rast) = 4326))"
          R"(, ADD CONSTRAINT enforce_scalex_rast CHECK (round(ST_ScaleX(rast)::numeric, 10) = round(0.25, 10)))"
          R"(, ADD CONSTRAINT enforce_scaley_rast CHECK (round(ST_ScaleY(rast)::numeric, 10) = round(-0.25, 10)))"
          R"(, ADD CONSTRAINT enforce_width_rast CHECK (ST_Width(rast) = 4))"
          R"(, ADD CONSTRAINT enforce_height_rast CHECK (ST_Height(rast) = 4))"
          R"(, ADD CONSTRAINT enforce_same_alignment_rast CHECK (ST_SameAlignment(rast, ST_MakeEmptyRaster(1, 1, -180, 90, 0.25, -0.25, 0, 0, 4326))))"
          R"(, ADD CONSTRAINT enforce_num_bands_rast CHECK (ST_NumBands(rast) = 1))"
          R"(, ADD CONSTRAINT enforce_pixel_types_rast CHECK (_raster_constraint_pixel_types(rast) = '{16BSI}'::text[]))"
          R"(, ADD CONSTRAINT enforce_nodata_values_rast CHECK (_raster_constraint_nodata_values(rast) = '{-32768.0000000000}'::numeric[]))"
          R"(, ADD CONSTRAINT enforce_out_db_rast CHECK (_raster_constraint_out_db(rast) = '{f}'::boolean[]))"
          R"(, ADD CONSTRAINT enforce_max_extent_rast CHECK (ST_Envelope(rast) @ )"
          R"('SRID=4326;POLYGON((-180.000000001 -90.000000001,-180.000000001 90.000000001,180.000000001 90.000000001,)"
          R"(180.000000001 -90.000000001,-180.000000001 -90.000000001))'::geometry))",
      }
  );
}

TEST_CASE("raster layer: the extent covers whole tiles, past the world's edge when tiles don't divide it", "[raster_layer]") {
  // 360 * 4 = 1440 pixels across is 41.14 tiles of 35, so 42 tiles: 1470 pixels, to longitude 187.5. 720 down is 21 tiles: 735, to -93.75.
  const std::string sql =
      Elevation::setup_sql(schemacht::schema::TableName("srtm"), {.pixels_per_degree = 4, .tile_pixels = 35, .nodata = NODATA}).back();

  CHECK(sql.ends_with(
      "'SRID=4326;POLYGON((-180.000000001 -93.750000001,-180.000000001 90.000000001,187.500000001 90.000000001,"
      "187.500000001 -93.750000001,-180.000000001 -93.750000001))'::geometry)"
  ));
}

TEST_CASE("raster layer: a grid that is not one is refused", "[raster_layer]") {
  const schemacht::schema::TableName table("srtm");

  CHECK_THROWS_AS(Elevation::setup_sql(table, {.pixels_per_degree = 0, .tile_pixels = 4, .nodata = NODATA}), std::invalid_argument);
  CHECK_THROWS_AS(Elevation::setup_sql(table, {.pixels_per_degree = 36001, .tile_pixels = 4, .nodata = NODATA}), std::invalid_argument);
  CHECK_NOTHROW(Elevation::setup_sql(table, {.pixels_per_degree = 36000, .tile_pixels = 4, .nodata = NODATA}));
  CHECK_THROWS_AS(Elevation::setup_sql(table, {.pixels_per_degree = 4, .tile_pixels = 0, .nodata = NODATA}), std::invalid_argument);
  CHECK_THROWS_AS(Elevation::setup_sql(table, {.pixels_per_degree = 4, .tile_pixels = 65536, .nodata = NODATA}), std::invalid_argument);
  CHECK_THROWS_AS(miniverse::RasterLayer<float>::setup_sql(table, {.pixels_per_degree = 4, .tile_pixels = 4, .nodata = NAN}), std::invalid_argument);
}

TEST_CASE("raster layer: tiles are written in batches of PIXEL_BYTES_PER_STATEMENT, by tile id", "[raster_layer]") {
  // Tiles of 2048 by 2048 int16 are 8 MB each, so a statement holds 4. Five tiles across from the world's north-west corner.
  constexpr geo::Grid<std::int16_t> COARSE_TILES{.pixels_per_degree = 3600, .tile_pixels = 2048, .nodata = NODATA};
  constexpr std::size_t             TILE = 2048;
  Raster                            raster{
      .west = -180,
      .north = 90,
      .pixel_width = 1.0 / 3600,
      .pixel_height = 1.0 / 3600,
      .width = 5 * TILE,
      .height = TILE,
      .nodata = NODATA,
      .pixels = {}
  };
  raster.pixels.assign(raster.width * raster.height, 1);

  const auto batches = Elevation::to_rows(std::move(raster), COARSE_TILES);

  REQUIRE(batches.size() == 2);
  REQUIRE(batches.front().size() == 4);
  REQUIRE(batches.back().size() == 1);
  CHECK(schemacht::schema::get<"tile_id">(batches.front().front()) == 0);
  CHECK(schemacht::schema::get<"tile_id">(batches.front().back()) == 3);
  CHECK(schemacht::schema::get<"tile_id">(batches.back().front()) == 4);

  // Four small tiles are one batch.
  CHECK(Elevation::to_rows(numbered(0.5, 1, 6, 6), GRID).size() == 1);
}

TEST_CASE("raster layer: a grid that is not one is refused before a raster is cut with it", "[raster_layer]") {
  CHECK_THROWS_AS(Elevation::to_rows(numbered(0, 1, 2, 2), {.pixels_per_degree = 4, .tile_pixels = 0, .nodata = NODATA}), std::invalid_argument);
}

TEST_CASE("raster layer: a streamed chunk is its tiles, each whole", "[raster_layer]") {
  // Two tiles side by side, from (0, 1) and from (1, 1).
  const std::vector<Raster> tiles = Elevation::chunk_from_rows(
      loaded(rows(Elevation::to_rows(numbered(0, 1, 8, 4), GRID))), polygon("POLYGON((0.5 0.2,1.5 0.2,1.5 0.8,0.5 0.8,0.5 0.2))")
  );

  REQUIRE(tiles.size() == 2);
  CHECK(tiles.front().west == 0);
  CHECK(tiles.back().west == 1);
  CHECK(tiles.back().width == 4);
  CHECK(tiles.back().at(0, 0) == 400);  // the raster's column 4, row 0
}

TEST_CASE("raster layer: a load of a location that reaches no tile is as empty as where there is no tile at all", "[raster_layer]") {
  // Two tiles side by side, from (0, 1) and from (1, 1), as the database finds them for a location that touches them.
  const std::vector<LoadedRow> tiles = loaded(rows(Elevation::to_rows(numbered(0, 1, 8, 4), GRID)));

  CHECK(Elevation::chunk_from_rows(tiles, {}).empty());  // a location of no points
  CHECK(Elevation::from_rows(tiles, polygon("POLYGON((2.0000001 0.2,2.5 0.2,2.5 0.8,2.0000001 0.8,2.0000001 0.2))")) == Raster{});
}

namespace {

// A strip a fiftieth of a degree wide from the south west to the north east, half a degree north of the diagonal: its box
// is nearly all of nine tiles three by three from (0, 3), and it crosses five of them.
const std::string STRIP = "POLYGON((0.1 0.6,0.12 0.58,2.42 2.88,2.4 2.9,0.1 0.6))";

}  // namespace

TEST_CASE("raster layer: a load of an area that runs askew is its box, with no data in the tiles it does not reach", "[raster_layer]") {
  const Raster heights = Elevation::from_rows(loaded(rows(Elevation::to_rows(numbered(0, 3, 12, 12), GRID))), polygon(STRIP));

  // The box, widened to whole pixels: from 0 to 2.5 across, and from 3 down to 0.5.
  CHECK(heights.west == 0);
  CHECK(heights.north == 3);
  CHECK(heights.width == 10);
  CHECK(heights.height == 10);

  CHECK(heights.value_at({1.1, 1.6}) == 405);           // in a tile the strip crosses, though not on the strip: column 4, row 5
  CHECK(heights.value_at({2.1, 0.9}) == std::nullopt);  // in the box, in a tile the strip does not reach
  CHECK(heights.value_at({0.6, 2.4}) == std::nullopt);
}

TEST_CASE("raster layer: the grid is read back from raster_columns", "[raster_layer]") {
  // raster_columns gives the pixel size rounded to 10 places, as the constraint compares it.
  CHECK(
      Elevation::settings_from_rows({SettingsRow{0.0002777778, 256, -32768}}) ==
      geo::Grid<std::int16_t>{.pixels_per_degree = 3600, .tile_pixels = 256, .nodata = NODATA}
  );
  // The finest grid allowed, 0.1 arc seconds, still reads back.
  CHECK(Elevation::settings_from_rows({SettingsRow{0.0000277778, 256, -32768}}).pixels_per_degree == 36000);
  CHECK_THROWS_AS(Elevation::settings_from_rows({}), std::runtime_error);
  CHECK_THROWS_AS(Elevation::settings_from_rows({SettingsRow{0.3, 256, -32768}}), std::runtime_error);  // 3.33 pixels per degree
  CHECK_THROWS_AS(Elevation::settings_from_rows({SettingsRow{0.25, 256, 1.5}}), std::runtime_error);    // not an int16
}

TEST_CASE("raster layer: the load is aimed at the layer's table and reads tiles as raster WKB", "[raster_layer]") {
  const miniverse::Layer<Elevation> layer("srtm");

  const std::string load = std::string(Elevation::load_statement_type::bind(polygon("POLYGON((0 0,1 0,1 1,0 0))")).on(layer.table()).sql());

  CHECK(load == R"(SELECT ST_AsBinary("rast") AS "rast" FROM "srtm" WHERE ST_Intersects(ST_ConvexHull("rast"), $1::geometry(Polygon,4326)))");
}
