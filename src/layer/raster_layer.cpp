#include "miniverse/layer/raster_layer.hpp"

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "miniverse/geo/grid.hpp"
#include "miniverse/geo/raster.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/layer/raster/reach.hpp"
#include "miniverse/layer/raster/tiles.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/table_name.hpp"

namespace miniverse {

namespace {

// How far the extent constraint reaches past the grid's own edge, in degrees. A tile's far edge, computed in floating point as
// its corner plus its width, can land a rounding step past the edge, and the constraint would then refuse it.
constexpr double EXTENT_MARGIN = 1e-9;

// How far 1 / pixel size may be from a whole number of pixels per degree. raster_columns gives the size rounded to 10 places.
constexpr double PIXELS_PER_DEGREE_TOLERANCE = 0.25;

// Taken first by every setup, and held until its transaction ends: PostgreSQL lets no two transactions make one schema, or
// replace one function, at the same time (the second fails), so setups take turns. The key is the schema's name, hashed.
constexpr std::string_view SETUP_LOCK = "DO $$ BEGIN PERFORM pg_advisory_xact_lock(hashtext('miniverse_functions')); END $$";

// How a tile's bytes are compressed where they are stored. pglz, which a server uses unless its `default_toast_compression`
// says otherwise, makes 16-bit heights a little smaller than lz4 does, but they then load some 75% slower than uncompressed,
// where lz4 costs some 10% for half the size (measured 2026-10-04 on USGS 30 m tiles). Set before any tile is written: it
// applies to values stored from then on.
constexpr std::string_view TILE_COMPRESSION = "lz4";

// The tile `incoming` merged onto `current`, two tiles of one grid position: a pixel with data in `incoming` wins, and one with
// no data there keeps `current`'s. ST_Union's LAST runs in C: about 4 times as fast as an ST_MapAlgebra expression on a tile
// of 1200 by 1200 pixels.
constexpr std::string_view MERGE_FUNCTION_DEFINITION =
    "(current raster, incoming raster) RETURNS raster LANGUAGE sql IMMUTABLE PARALLEL SAFE "
    "AS $$ SELECT ST_Union(tile, 'LAST' ORDER BY n) FROM unnest(ARRAY[current, incoming]) WITH ORDINALITY AS tiles (tile, n) $$";

/** @return `value` as the shortest decimal that reads back as the same `double`. */
[[nodiscard]] std::string decimal(double value) { return std::format("{}", value); }

}  // namespace

template <geo::Pixel pixel_t>
std::vector<std::string> RasterLayer<pixel_t>::setup_sql(const schemacht::schema::TableName& table, const settings_type& grid) {
  geo::check_grid(grid);

  const std::int64_t ppd = grid.pixels_per_degree;
  const std::int64_t tile = grid.tile_pixels;
  const std::string  size = decimal(1.0 / static_cast<double>(ppd));
  const double       west = geo::GRID_WEST - EXTENT_MARGIN;
  const double       north = geo::GRID_NORTH + EXTENT_MARGIN;
  const double       east = geo::GRID_WEST + (static_cast<double>(grid.tiles_across() * tile) / static_cast<double>(ppd)) + EXTENT_MARGIN;
  const double       south = geo::GRID_NORTH - (static_cast<double>(grid.tiles_down() * tile) / static_cast<double>(ppd)) - EXTENT_MARGIN;
  const std::string  world =
      std::format("SRID=4326;POLYGON(({0} {1},{0} {2},{3} {2},{3} {1},{0} {1}))", decimal(west), decimal(south), decimal(north), decimal(east));

  // Each as AddRasterConstraints writes it, which is the form raster_columns parses.
  const std::vector<std::string> constraints{
      "enforce_srid_rast CHECK (ST_SRID(rast) = 4326)",
      std::format("enforce_scalex_rast CHECK (round(ST_ScaleX(rast)::numeric, 10) = round({}, 10))", size),
      std::format("enforce_scaley_rast CHECK (round(ST_ScaleY(rast)::numeric, 10) = round(-{}, 10))", size),
      std::format("enforce_width_rast CHECK (ST_Width(rast) = {})", tile),
      std::format("enforce_height_rast CHECK (ST_Height(rast) = {})", tile),
      std::format("enforce_same_alignment_rast CHECK (ST_SameAlignment(rast, ST_MakeEmptyRaster(1, 1, -180, 90, {0}, -{0}, 0, 0, 4326)))", size),
      "enforce_num_bands_rast CHECK (ST_NumBands(rast) = 1)",
      std::format("enforce_pixel_types_rast CHECK (_raster_constraint_pixel_types(rast) = '{{{}}}'::text[])", geo::PixelType<pixel_t>::NAME),
      std::format(
          "enforce_nodata_values_rast CHECK (_raster_constraint_nodata_values(rast) = '{{{:.10f}}}'::numeric[])", static_cast<double>(grid.nodata)
      ),
      "enforce_out_db_rast CHECK (_raster_constraint_out_db(rast) = '{f}'::boolean[])",
      std::format("enforce_max_extent_rast CHECK (ST_Envelope(rast) @ '{}'::geometry)", world),
  };

  std::string      alter = "ALTER TABLE " + table.quoted();
  std::string_view separator = " ";
  for ( const std::string& constraint : constraints ) {
    alter += separator;
    alter += "ADD CONSTRAINT " + constraint;
    separator = ", ";
  }

  return {
      std::string(SETUP_LOCK),
      std::format("CREATE SCHEMA IF NOT EXISTS {}", raster::MergeRaster::SCHEMA),
      std::format("CREATE OR REPLACE FUNCTION {}{}", raster::MergeRaster::FUNCTION, MERGE_FUNCTION_DEFINITION),
      std::format("ALTER TABLE {} ALTER COLUMN rast SET COMPRESSION {}", table.quoted(), TILE_COMPRESSION),
      "CREATE INDEX ON " + table.quoted() + " USING gist (ST_ConvexHull(rast))",
      std::move(alter),
  };
}

template <geo::Pixel pixel_t>
std::vector<std::vector<typename RasterLayer<pixel_t>::row_type>> RasterLayer<pixel_t>::to_rows(result_type raster, const settings_type& grid) {
  std::vector<raster::Tile<pixel_t>> tiles = raster::cut(raster, grid);  // checks the grid first

  const std::size_t tile_bytes = static_cast<std::size_t>(grid.tile_pixels) * static_cast<std::size_t>(grid.tile_pixels) * sizeof(pixel_t);
  const std::size_t tiles_per_batch = std::max<std::size_t>(1, PIXEL_BYTES_PER_STATEMENT / tile_bytes);

  // In ascending tile_id order, across batches too, as `cut` gives them.
  std::vector<std::vector<row_type>> batches;
  for ( raster::Tile<pixel_t>& tile : tiles ) {
    if ( batches.empty() || batches.back().size() == tiles_per_batch ) {
      batches.emplace_back();
    }

    batches.back().emplace_back(raster::TileId{tile.id}, raster::Rast<pixel_t>{std::move(tile.pixels)});
  }

  return batches;
}

template <geo::Pixel pixel_t>
geo::Raster<pixel_t> RasterLayer<pixel_t>::from_rows(std::vector<typename load_statement_type::result_type> rows, const geo::Polygon& location) {
  return raster::window(chunk_from_rows(std::move(rows), location), location);
}

template <geo::Pixel pixel_t>
std::vector<geo::Raster<pixel_t>> RasterLayer<pixel_t>::chunk_from_rows(
    std::vector<typename load_statement_type::result_type> rows, const geo::Polygon& location
) {
  const raster::Reach reach(location);

  chunk_type tiles;

  tiles.reserve(rows.size());
  for ( auto& row : rows ) {
    if ( geo::Raster<pixel_t>& tile = schemacht::schema::get<"rast">(row); reach.into(tile.outline()) ) {
      tiles.push_back(std::move(tile));
    }
  }

  return tiles;
}

template <geo::Pixel pixel_t>
geo::Grid<pixel_t> RasterLayer<pixel_t>::settings_from_rows(std::vector<typename settings_statement_type::row_type> rows) {
  if ( rows.size() != 1 ) {
    throw std::runtime_error("the table has no grid in its raster constraints: make it with Miniverse::create_table");
  }

  const double scale = schemacht::schema::get<"scale_x">(rows.front());
  const double nodata = schemacht::schema::get<"nodata">(rows.front());
  const double pixels_per_degree = 1.0 / scale;
  if ( ! std::isfinite(pixels_per_degree) || pixels_per_degree < 1 ||
       std::abs(pixels_per_degree - std::round(pixels_per_degree)) > PIXELS_PER_DEGREE_TOLERANCE ) {
    throw std::runtime_error("the table's pixels are " + decimal(scale) + " degrees wide, not 1/n degree for a whole number n");
  }

  if constexpr ( std::integral<pixel_t> ) {
    if ( nodata < static_cast<double>(std::numeric_limits<pixel_t>::lowest()) || nodata > static_cast<double>(std::numeric_limits<pixel_t>::max()) ||
         nodata != std::round(nodata) ) {
      throw std::runtime_error("the table's nodata value " + decimal(nodata) + " is not a " + std::string(geo::PixelType<pixel_t>::NAME));
    }
  }

  return {
      .pixels_per_degree = static_cast<std::int32_t>(std::round(pixels_per_degree)),
      .tile_pixels = schemacht::schema::get<"blocksize_x">(rows.front()),
      .nodata = static_cast<pixel_t>(nodata),
  };
}

template struct RasterLayer<std::int8_t>;
template struct RasterLayer<std::uint8_t>;
template struct RasterLayer<std::int16_t>;
template struct RasterLayer<std::uint16_t>;
template struct RasterLayer<std::int32_t>;
template struct RasterLayer<std::uint32_t>;
template struct RasterLayer<float>;
template struct RasterLayer<double>;

}  // namespace miniverse
