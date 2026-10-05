// The raster kind without a database: the table's setup, a raster cut into the grid's tiles, and tiles stitched into the
// window a load asks for.

#include <catch2/catch_test_macros.hpp>

#include <boost/geometry/io/wkt/read.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/table_name.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;

namespace {

struct Elevation : miniverse::RasterLayer<std::int16_t> {};

using Raster = geo::Raster<std::int16_t>;
using LoadedRow = Elevation::load_statement_type::result_type;
using SettingsRow = Elevation::settings_statement_type::row_type;

constexpr std::int16_t NODATA = -32768;

// Four pixels to a degree, in tiles of one degree.
constexpr geo::Grid<std::int16_t> GRID{.pixels_per_degree = 4, .tile_pixels = 4, .nodata = NODATA};

[[nodiscard]] geo::Polygon polygon(const std::string& wkt) { return bg::from_wkt<geo::Polygon>(wkt); }

/** @return `width` by `height` pixels of the grid from (`west`, `north`), each `column * 100 + row`. */
[[nodiscard]] Raster numbered(double west, double north, std::size_t width, std::size_t height) {
  Raster raster{
      .west = west, .north = north, .pixel_width = 0.25, .pixel_height = 0.25, .width = width, .height = height, .nodata = NODATA, .pixels = {}
  };

  for ( std::size_t row = 0; row < height; ++row ) {
    for ( std::size_t column = 0; column < width; ++column ) {
      raster.pixels.push_back(static_cast<std::int16_t>((column * 100) + row));
    }
  }

  return raster;
}

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
  CHECK(setup.at(0).contains("pg_advisory_xact_lock"));  // first: setups take turns at what follows
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
  CHECK_THROWS_AS(
      miniverse::RasterLayer<float>::setup_sql(table, {.pixels_per_degree = 4, .tile_pixels = 4, .nodata = NAN}), std::invalid_argument
  );
}

TEST_CASE("raster layer: a raster is cut into the grid's tiles it covers", "[raster_layer]") {
  // 6 by 6 pixels from (0.5, 1): columns 722 to 727 of the grid and rows 356 to 361, so tiles 180 and 181 across, 89 and 90 down.
  const auto batches = Elevation::to_rows(numbered(0.5, 1, 6, 6), GRID);

  // 360 tiles span the world, so tile 180 across and 89 down is 89 * 360 + 180. Four small tiles are one batch.
  REQUIRE(batches.size() == 1);
  const std::vector<Elevation::row_type>& rows = batches.front();
  REQUIRE(rows.size() == 4);
  CHECK(schemacht::schema::get<"tile_id">(rows.front()) == (89 * 360) + 180);

  const Raster& tile = schemacht::schema::get<"rast">(rows.front());
  CHECK(tile.west == 0);
  CHECK(tile.north == 1);
  CHECK(tile.width == 4);
  CHECK(tile.height == 4);
  CHECK(tile.pixel_width == 0.25);
  CHECK(tile.at(0, 0) == NODATA);  // west of the raster
  CHECK(tile.at(2, 0) == 0);       // the raster's first pixel
  CHECK(tile.at(3, 3) == 103);

  CHECK(schemacht::schema::get<"tile_id">(rows.back()) == (90 * 360) + 181);
  CHECK(schemacht::schema::get<"rast">(rows.back()).at(0, 0) == 204);  // the raster's column 2, row 4
}

TEST_CASE("raster layer: a raster's own nodata becomes the grid's, and a tile with no data is left out", "[raster_layer]") {
  Raster raster = numbered(0.5, 1, 2, 1);
  raster.nodata = 0;  // the first pixel has no data, and now the second too: the one tile they are in is left out
  raster.pixels.at(1) = 0;

  CHECK(Elevation::to_rows(raster, GRID).empty());

  raster.pixels.at(1) = 7;
  const std::vector<Elevation::row_type> tiles = rows(Elevation::to_rows(raster, GRID));

  REQUIRE(tiles.size() == 1);
  CHECK(schemacht::schema::get<"rast">(tiles.front()).at(2, 0) == NODATA);
  CHECK(schemacht::schema::get<"rast">(tiles.front()).at(3, 0) == 7);
}

TEST_CASE("raster layer: tiles are written in batches of PIXEL_BYTES_PER_STATEMENT, by tile id", "[raster_layer]") {
  // Tiles of 2048 by 2048 int16 are 8 MB each, so a statement holds 4. Five tiles across from the world's north-west corner.
  constexpr geo::Grid<std::int16_t> COARSE_TILES{.pixels_per_degree = 3600, .tile_pixels = 2048, .nodata = NODATA};
  constexpr std::size_t             TILE = 2048;
  Raster                            raster{
                                 .west = -180, .north = 90, .pixel_width = 1.0 / 3600, .pixel_height = 1.0 / 3600, .width = 5 * TILE, .height = TILE, .nodata = NODATA, .pixels = {}
  };
  raster.pixels.assign(raster.width * raster.height, 1);

  const auto batches = Elevation::to_rows(std::move(raster), COARSE_TILES);

  REQUIRE(batches.size() == 2);
  REQUIRE(batches.front().size() == 4);
  REQUIRE(batches.back().size() == 1);
  CHECK(schemacht::schema::get<"tile_id">(batches.front().front()) == 0);
  CHECK(schemacht::schema::get<"tile_id">(batches.front().back()) == 3);
  CHECK(schemacht::schema::get<"tile_id">(batches.back().front()) == 4);
}

TEST_CASE("raster layer: a pixel with data equal to the grid's nodata is refused, not lost", "[raster_layer]") {
  Raster raster = numbered(0.5, 1, 2, 1);
  raster.nodata = 0;
  raster.pixels.at(1) = NODATA;  // has data in this raster, but would have none on the grid

  CHECK_THROWS_AS(Elevation::to_rows(raster, GRID), std::invalid_argument);
}

TEST_CASE("raster layer: a raster not on the grid is not cut", "[raster_layer]") {
  Raster other_size = numbered(0, 1, 2, 2);
  other_size.pixel_width = other_size.pixel_height = 0.5;
  Raster wrong_count = numbered(0, 1, 2, 2);
  wrong_count.pixels.pop_back();

  CHECK_THROWS_AS(Elevation::to_rows(other_size, GRID), std::invalid_argument);
  CHECK_THROWS_AS(Elevation::to_rows(numbered(0.1, 1, 2, 2), GRID), std::invalid_argument);      // between pixels
  CHECK_THROWS_AS(Elevation::to_rows(numbered(-180.25, 1, 2, 2), GRID), std::invalid_argument);  // west of the world
  CHECK_THROWS_AS(Elevation::to_rows(numbered(179.75, 1, 2, 2), GRID), std::invalid_argument);   // east of it
  CHECK_THROWS_AS(Elevation::to_rows(std::move(wrong_count), GRID), std::invalid_argument);
}

TEST_CASE("raster layer: a grid that is not one is refused before a raster is cut with it", "[raster_layer]") {
  CHECK_THROWS_AS(Elevation::to_rows(numbered(0, 1, 2, 2), {.pixels_per_degree = 4, .tile_pixels = 0, .nodata = NODATA}), std::invalid_argument);
  CHECK_THROWS_AS(Elevation::to_rows(numbered(0, 1, 2, 2), {.pixels_per_degree = 0, .tile_pixels = 4, .nodata = NODATA}), std::invalid_argument);
}

TEST_CASE("raster layer: tiles stitched over a raster's own box give the raster back", "[raster_layer]") {
  const Raster raster = numbered(0.5, 1, 6, 6);

  CHECK(Elevation::from_rows(loaded(rows(Elevation::to_rows(raster, GRID))), polygon("POLYGON((0.5 -0.5,2 -0.5,2 1,0.5 1,0.5 -0.5))")) == raster);
}

TEST_CASE("raster layer: a box is widened to whole pixels, and pixels no tile covers are nodata", "[raster_layer]") {
  const std::vector<LoadedRow> tiles = loaded(rows(Elevation::to_rows(numbered(0, 1, 4, 4), GRID)));  // the one tile from (0, 1) to (1, 0)

  // Inside the tile, off its pixels' edges: pixels 1 to 2 across (0.25 to 0.75) and 1 to 2 down (latitude 0.75 to 0.25).
  const Raster inside = Elevation::from_rows(tiles, polygon("POLYGON((0.3 0.3,0.6 0.3,0.6 0.6,0.3 0.6,0.3 0.3))"));
  CHECK(inside.west == 0.25);
  CHECK(inside.north == 0.75);
  CHECK(inside.width == 2);
  CHECK(inside.height == 2);
  CHECK(inside.pixels == std::vector<std::int16_t>{101, 201, 102, 202});

  // Half a degree past the tile to the west: two pixels of nodata, then the tile's first column.
  const Raster past = Elevation::from_rows(tiles, polygon("POLYGON((-0.5 0.75,0.25 0.75,0.25 1,-0.5 1,-0.5 0.75))"));
  CHECK(past.west == -0.5);
  CHECK(past.width == 3);
  CHECK(past.height == 1);
  CHECK(past.pixels == std::vector<std::int16_t>{NODATA, NODATA, 0});

  // A point is one pixel.
  CHECK(Elevation::from_rows(tiles, polygon("POLYGON((0.3 0.6,0.3 0.6,0.3 0.6,0.3 0.6))")).pixels == std::vector<std::int16_t>{101});
}

TEST_CASE("raster layer: with no tiles there is no grid, so the window is empty", "[raster_layer]") {
  const Raster empty = Elevation::from_rows({}, polygon("POLYGON((0 0,1 0,1 1,0 1,0 0))"));

  CHECK(empty.width == 0);
  CHECK(empty.height == 0);
  CHECK(empty.pixels.empty());
}

TEST_CASE("raster layer: a streamed chunk is its tiles, each whole", "[raster_layer]") {
  // Two tiles side by side, from (0, 1) and from (1, 1).
  const std::vector<Raster> tiles =
      Elevation::chunk_from_rows(loaded(rows(Elevation::to_rows(numbered(0, 1, 8, 4), GRID))), polygon("POLYGON((0.5 0.2,1.5 0.2,1.5 0.8,0.5 0.8,0.5 0.2))"));

  REQUIRE(tiles.size() == 2);
  CHECK(tiles.front().west == 0);
  CHECK(tiles.back().west == 1);
  CHECK(tiles.back().width == 4);
  CHECK(tiles.back().at(0, 0) == 400);  // the raster's column 4, row 0
}

TEST_CASE("raster layer: a tile the location's box does not meet is not the location's, however near it ends", "[raster_layer]") {
  // Two tiles side by side, from (0, 1) and from (1, 1), as an index that rounds its boxes outward would find them both.
  const auto tiles_in = [](const std::string& wkt) {
    return Elevation::chunk_from_rows(loaded(rows(Elevation::to_rows(numbered(0, 1, 8, 4), GRID))), polygon(wkt)).size();
  };

  CHECK(tiles_in("POLYGON((1 0.2,1.5 0.2,1.5 0.8,1 0.8,1 0.2))") == 2);                          // touching the west tile's edge: both
  CHECK(tiles_in("POLYGON((1.0000001 0.2,1.5 0.2,1.5 0.8,1.0000001 0.8,1.0000001 0.2))") == 1);  // a hair past it: the east one alone
  CHECK(tiles_in("POLYGON((2.0000001 0.2,2.5 0.2,2.5 0.8,2.0000001 0.8,2.0000001 0.2))") == 0);
  CHECK(tiles_in("POLYGON((0.5 1,1.5 1,1.5 2,0.5 2,0.5 1))") == 2);                              // touching both from the north
  CHECK(Elevation::chunk_from_rows(loaded(rows(Elevation::to_rows(numbered(0, 1, 8, 4), GRID))), {}).empty());  // a location of no points

  // And a load of it is then as empty as where there is no tile at all.
  CHECK(
      Elevation::from_rows(loaded(rows(Elevation::to_rows(numbered(0, 1, 8, 4), GRID))), polygon("POLYGON((2.0000001 0.2,2.5 0.2,2.5 0.8,2.0000001 0.8,2.0000001 0.2))")) ==
      Raster{}
  );
}

TEST_CASE("raster layer: tiles that don't share a grid are not stitched", "[raster_layer]") {
  std::vector<LoadedRow> tiles = loaded(rows(Elevation::to_rows(numbered(0, 1, 8, 4), GRID)));
  REQUIRE(tiles.size() == 2);
  std::get<0>(tiles.back()).value.west += 0.1;

  CHECK_THROWS_AS(Elevation::from_rows(tiles, polygon("POLYGON((0 0,2 0,2 1,0 1,0 0))")), std::invalid_argument);
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

  CHECK(load == R"(SELECT ST_AsBinary("rast") AS "rast" FROM "srtm" WHERE "rast" && $1::geometry(Polygon,4326))");
}
