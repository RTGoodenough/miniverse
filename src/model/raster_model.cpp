#include "miniverse/model/raster_model.hpp"

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
#include "miniverse/model/raster/reach.hpp"
#include "miniverse/model/raster/tiles.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/table_name.hpp"

namespace miniverse {

namespace {

// How far the extent constraint reaches past the grid's own edge, in degrees: PostGIS works a tile's far edge out in floating
// point, as its corner plus its width, which can land a rounding step past the edge, and the constraint would then refuse it.
constexpr double EXTENT_MARGIN = 1e-9;

// How far 1 / pixel size may be from a whole number of pixels per degree. raster_columns gives the size rounded to 10 places.
constexpr double PIXELS_PER_DEGREE_TOLERANCE = 0.25;

// The tiles' TOAST compression, set before any tile is written since it applies from then on. pglz, PostgreSQL's default,
// makes whole-number tiles load far slower than lz4.
constexpr std::string_view TILE_COMPRESSION = "lz4";

// The body of `raster::MergeRaster`. ST_Union's LAST runs in C, where an ST_MapAlgebra expression does not.
constexpr std::string_view MERGE_FUNCTION_DEFINITION =
    "(current raster, incoming raster) RETURNS raster LANGUAGE sql IMMUTABLE PARALLEL SAFE "
    "AS $fn$ SELECT ST_Union(tile, 'LAST' ORDER BY n) FROM unnest(ARRAY[current, incoming]) WITH ORDINALITY AS tiles (tile, n) $fn$";

/** @return `value` as the shortest decimal that reads back as the same `double`. */
[[nodiscard]] std::string decimal(double value) { return std::format("{}", value); }

}  // namespace

template <geo::Pixel pixel_t>
std::vector<std::string> RasterModel<pixel_t>::setup_sql(const schemacht::schema::TableName& table, const geo::Grid<pixel_t>& grid) {
  geo::check_grid(grid);

  const std::int64_t tile = grid.tile_pixels;
  const std::string  size = decimal(grid.pixel_degrees());
  const geo::Box     tiles = grid.outline();
  const double       west = tiles.min_corner().x() - EXTENT_MARGIN;
  const double       north = tiles.max_corner().y() + EXTENT_MARGIN;
  const double       east = tiles.max_corner().x() + EXTENT_MARGIN;
  const double       south = tiles.min_corner().y() - EXTENT_MARGIN;
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

  // The schema is there, and setups take turns: the registry's setup, run before this, made it under its lock. The function
  // is made only if absent: replacing it takes its owner, and the role that made it need not be this one.
  return {
      std::format(
          "DO $do$ BEGIN IF to_regprocedure('{0}(raster, raster)') IS NULL THEN CREATE FUNCTION {0}{1}; END IF; END $do$",
          raster::MergeRaster::FUNCTION, MERGE_FUNCTION_DEFINITION
      ),
      std::format("ALTER TABLE {} ALTER COLUMN rast SET COMPRESSION {}", table.quoted(), TILE_COMPRESSION),
      "CREATE INDEX ON " + table.quoted() + " USING gist (ST_ConvexHull(rast))",
      std::move(alter),
  };
}

template <geo::Pixel pixel_t>
std::vector<std::vector<typename RasterModel<pixel_t>::row_type>> RasterModel<pixel_t>::to_rows(result_type raster, const geo::Grid<pixel_t>& grid) {
  std::vector<raster::Tile<pixel_t>> tiles = raster::cut(raster, grid);

  const std::size_t tile_bytes = static_cast<std::size_t>(grid.tile_pixels) * static_cast<std::size_t>(grid.tile_pixels) * sizeof(pixel_t);
  const std::size_t tiles_per_batch = std::max<std::size_t>(1, PIXEL_BYTES_PER_STATEMENT / tile_bytes);

  // By ascending tile id across the batches, as `cut` gives them: see there.
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
geo::Raster<pixel_t> RasterModel<pixel_t>::from_rows(std::vector<typename load_statement_type::result_type> rows, const geo::Polygon& location) {
  return raster::window(chunk_from_rows(std::move(rows), location), location);
}

template <geo::Pixel pixel_t>
std::vector<geo::Raster<pixel_t>> RasterModel<pixel_t>::chunk_from_rows(
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
geo::Grid<pixel_t> RasterModel<pixel_t>::settings_from_rows(std::vector<typename settings_statement_type::row_type> rows) {
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

template struct RasterModel<std::int8_t>;
template struct RasterModel<std::uint8_t>;
template struct RasterModel<std::int16_t>;
template struct RasterModel<std::uint16_t>;
template struct RasterModel<std::int32_t>;
template struct RasterModel<std::uint32_t>;
template struct RasterModel<float>;
template struct RasterModel<double>;

}  // namespace miniverse
