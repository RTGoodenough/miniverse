#include "miniverse/layer/raster_layer.hpp"

#include <boost/geometry/algorithms/envelope.hpp>  // IWYU pragma: keep
#include <boost/geometry/geometries/box.hpp>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "miniverse/geo/raster.hpp"
#include "miniverse/geo/types.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/table_name.hpp"

namespace miniverse {

namespace {

// The grid's corner, and the world's size in degrees.
constexpr double       WEST = -180;
constexpr double       NORTH = 90;
constexpr std::int64_t DEGREES_ACROSS = 360;
constexpr std::int64_t DEGREES_DOWN = 180;

constexpr std::int32_t MAX_TILE_PIXELS = std::numeric_limits<std::uint16_t>::max();  // raster WKB's width and height

// raster_columns gives the pixel size rounded to 10 places, from which a finer grid can't be told apart: 0.1 arc seconds.
constexpr std::int32_t MAX_PIXELS_PER_DEGREE = 36000;

// How far the extent constraint reaches past the grid's own edge, in degrees. A tile's far edge, computed in floating point as
// its corner plus its width, can land a rounding step past the edge, and the constraint would then refuse it.
constexpr double EXTENT_MARGIN = 1e-9;

// How far a position may be from a pixel's edge, in pixels, and a pixel size from another, relatively, and still count as on it.
constexpr double ALIGNMENT_TOLERANCE = 1e-6;
constexpr double SIZE_TOLERANCE = 1e-9;

// How far 1 / pixel size may be from a whole number of pixels per degree. raster_columns gives the size rounded to 10 places.
constexpr double PIXELS_PER_DEGREE_TOLERANCE = 0.25;

// The tile `incoming` merged onto `current`, two tiles of one grid position: a pixel with data in `incoming` wins, and one with
// no data there keeps `current`'s. ST_Union's LAST runs in C: about 4 times as fast as an ST_MapAlgebra expression on a tile
// of 1200 by 1200 pixels.
constexpr std::string_view MERGE_FUNCTION_DEFINITION =
    "(current raster, incoming raster) RETURNS raster LANGUAGE sql IMMUTABLE PARALLEL SAFE "
    "AS $$ SELECT ST_Union(tile, 'LAST' ORDER BY n) FROM unnest(ARRAY[current, incoming]) WITH ORDINALITY AS tiles (tile, n) $$";

/** @brief Where a raster's north-west pixel lies, in pixels, in a frame that another raster shares. */
struct PixelAt {
  std::int64_t column = 0;
  std::int64_t row = 0;
};

/** @return `value` as a whole number, if it is one to within `ALIGNMENT_TOLERANCE`. */
[[nodiscard]] std::optional<std::int64_t> whole(double value) {
  const double nearest = std::round(value);
  if ( std::abs(value - nearest) > ALIGNMENT_TOLERANCE ) {
    return std::nullopt;
  }

  return static_cast<std::int64_t>(nearest);
}

/** @return `value` rounded down, or to the nearest whole number if it is one already but for rounding error. */
[[nodiscard]] std::int64_t floor_snapped(double value) { return whole(value).value_or(static_cast<std::int64_t>(std::floor(value))); }

/** @return `value` rounded up, or to the nearest whole number if it is one already but for rounding error. */
[[nodiscard]] std::int64_t ceil_snapped(double value) { return whole(value).value_or(static_cast<std::int64_t>(std::ceil(value))); }

[[nodiscard]] bool same_size(double size, double other) { return std::abs(size - other) <= SIZE_TOLERANCE * std::abs(other); }

[[nodiscard]] std::int64_t ceil_div(std::int64_t dividend, std::int64_t divisor) { return (dividend + divisor - 1) / divisor; }

/** @return `value` as the shortest decimal that reads back as the same `double`. */
[[nodiscard]] std::string decimal(double value) { return std::format("{}", value); }

template <geo::Pixel pixel_t>
void check_grid(const geo::Grid<pixel_t>& grid) {
  if ( grid.pixels_per_degree < 1 || grid.pixels_per_degree > MAX_PIXELS_PER_DEGREE ) {
    throw std::invalid_argument("a grid has 1 to 36000 pixels per degree, not " + std::to_string(grid.pixels_per_degree));
  }

  if ( grid.tile_pixels < 1 || grid.tile_pixels > MAX_TILE_PIXELS ) {
    throw std::invalid_argument("a grid's tiles are 1 to 65535 pixels a side, not " + std::to_string(grid.tile_pixels));
  }

  if constexpr ( std::floating_point<pixel_t> ) {
    if ( ! std::isfinite(grid.nodata) ) {
      throw std::invalid_argument("a grid's nodata value must be a finite number");
    }
  }
}

/**
 * @brief Copies the pixels of `source` that have data into `target`, where they overlap: `source` at `source_at` and
 * `target` at `target_at` in one frame of pixels.
 * @return How many pixels were copied.
 */
template <geo::Pixel pixel_t>
std::size_t paste(const geo::Raster<pixel_t>& source, PixelAt source_at, geo::Raster<pixel_t>& target, PixelAt target_at) {
  const std::int64_t west = std::max(source_at.column, target_at.column);
  const std::int64_t east =
      std::min(source_at.column + static_cast<std::int64_t>(source.width), target_at.column + static_cast<std::int64_t>(target.width));
  const std::int64_t north = std::max(source_at.row, target_at.row);
  const std::int64_t south =
      std::min(source_at.row + static_cast<std::int64_t>(source.height), target_at.row + static_cast<std::int64_t>(target.height));
  std::size_t pasted = 0;

  for ( std::int64_t row = north; row < south; ++row ) {
    for ( std::int64_t column = west; column < east; ++column ) {
      const pixel_t value = source.at(static_cast<std::size_t>(column - source_at.column), static_cast<std::size_t>(row - source_at.row));
      if ( value == source.nodata ) {
        continue;
      }

      target.pixels.at((static_cast<std::size_t>(row - target_at.row) * target.width) + static_cast<std::size_t>(column - target_at.column)) = value;
      ++pasted;
    }
  }

  return pasted;
}

}  // namespace

template <geo::Pixel pixel_t>
std::vector<std::string> RasterLayer<pixel_t>::setup_sql(const schemacht::schema::TableName& table, const settings_type& grid) {
  check_grid(grid);

  const std::int64_t ppd = grid.pixels_per_degree;
  const std::int64_t tile = grid.tile_pixels;
  const std::string  size = decimal(1.0 / static_cast<double>(ppd));
  const double       west = WEST - EXTENT_MARGIN;
  const double       north = NORTH + EXTENT_MARGIN;
  const double       east = WEST + (static_cast<double>(ceil_div(DEGREES_ACROSS * ppd, tile) * tile) / static_cast<double>(ppd)) + EXTENT_MARGIN;
  const double       south = NORTH - (static_cast<double>(ceil_div(DEGREES_DOWN * ppd, tile) * tile) / static_cast<double>(ppd)) - EXTENT_MARGIN;
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
      std::format("CREATE SCHEMA IF NOT EXISTS {}", raster::MergeRaster::SCHEMA),
      std::format("CREATE OR REPLACE FUNCTION {}{}", raster::MergeRaster::FUNCTION, MERGE_FUNCTION_DEFINITION),
      "CREATE INDEX ON " + table.quoted() + " USING gist (ST_ConvexHull(rast))",
      std::move(alter),
  };
}

template <geo::Pixel pixel_t>
std::vector<std::vector<typename RasterLayer<pixel_t>::row_type>> RasterLayer<pixel_t>::to_rows(result_type raster, const settings_type& grid) {
  check_grid(grid);  // the table's grid, as read back: cheap to check again
  raster.check_pixel_count();

  // A pixel equal to the grid's nodata would be written as having no data, so it would be lost.
  if ( raster.nodata != grid.nodata && std::ranges::find(raster.pixels, grid.nodata) != raster.pixels.end() ) {
    throw std::invalid_argument(
        "a pixel of the raster has the value " + decimal(static_cast<double>(grid.nodata)) +
        ", which is the grid's nodata: it would be lost. Give the raster the grid's nodata, or change the pixel"
    );
  }

  const auto   ppd = static_cast<std::int64_t>(grid.pixels_per_degree);
  const double size = 1.0 / static_cast<double>(ppd);
  if ( ! same_size(raster.pixel_width, size) || ! same_size(raster.pixel_height, size) ) {
    throw std::invalid_argument(
        "the raster's pixels are " + decimal(raster.pixel_width) + " by " + decimal(raster.pixel_height) + " degrees, not the grid's " + decimal(size)
    );
  }

  const std::optional<std::int64_t> first_column = whole((raster.west - WEST) * static_cast<double>(ppd));
  const std::optional<std::int64_t> first_row = whole((NORTH - raster.north) * static_cast<double>(ppd));
  if ( ! first_column || ! first_row ) {
    throw std::invalid_argument("the raster's corner is not on a corner of the grid's pixels");
  }

  const PixelAt raster_at{.column = *first_column, .row = *first_row};
  const auto    width = static_cast<std::int64_t>(raster.width);
  const auto    height = static_cast<std::int64_t>(raster.height);
  if ( raster_at.column < 0 || raster_at.row < 0 || raster_at.column + width > DEGREES_ACROSS * ppd || raster_at.row + height > DEGREES_DOWN * ppd ) {
    throw std::invalid_argument("the raster reaches past the world: longitude -180 to 180, latitude -90 to 90");
  }

  const std::int64_t                 tile = grid.tile_pixels;
  const std::int64_t                 tiles_across = ceil_div(DEGREES_ACROSS * ppd, tile);
  const std::size_t                  tiles_per_batch = std::max<std::size_t>(1, PIXEL_BYTES_PER_STATEMENT / (static_cast<std::size_t>(tile * tile) * sizeof(pixel_t)));
  std::vector<std::vector<row_type>> batches;

  // In ascending tile_id order, across batches too: two pushes that overlap then lock their tiles in the same order within
  // their transactions (in practice: a statement writes its array of rows in order), so they wait for each other and can't deadlock.
  for ( std::int64_t tile_row = raster_at.row / tile; tile_row * tile < raster_at.row + height; ++tile_row ) {
    for ( std::int64_t tile_column = raster_at.column / tile; tile_column * tile < raster_at.column + width; ++tile_column ) {
      geo::Raster<pixel_t> cut{
          .west = WEST + (static_cast<double>(tile_column * tile) / static_cast<double>(ppd)),
          .north = NORTH - (static_cast<double>(tile_row * tile) / static_cast<double>(ppd)),
          .pixel_width = size,
          .pixel_height = size,
          .width = static_cast<std::size_t>(tile),
          .height = static_cast<std::size_t>(tile),
          .nodata = grid.nodata,
          .pixels = std::vector<pixel_t>(static_cast<std::size_t>(tile * tile), grid.nodata),
      };

      if ( paste(raster, raster_at, cut, PixelAt{.column = tile_column * tile, .row = tile_row * tile}) == 0 ) {
        continue;  // no data to write
      }

      if ( batches.empty() || batches.back().size() == tiles_per_batch ) {
        batches.emplace_back();
      }

      batches.back().emplace_back(raster::TileId{(tile_row * tiles_across) + tile_column}, raster::Rast<pixel_t>{std::move(cut)});
    }
  }

  return batches;
}

template <geo::Pixel pixel_t>
geo::Raster<pixel_t> RasterLayer<pixel_t>::from_rows(std::vector<typename load_statement_type::result_type> rows, const geo::Polygon& location) {
  // Boost declares return_envelope in a detail header; algorithms/envelope.hpp is the public one.
  const auto box = boost::geometry::return_envelope<boost::geometry::model::box<geo::Point>>(location);  // NOLINT(misc-include-cleaner)

  if ( rows.empty() || box.min_corner().x() > box.max_corner().x() ) {
    return {};
  }

  // The window, in pixels of the first tile's frame: the box, widened to whole pixels, and at least one pixel each way.
  const geo::Raster<pixel_t>& first = schemacht::schema::get<"rast">(rows.front());
  const std::int64_t          west = floor_snapped((box.min_corner().x() - first.west) / first.pixel_width);
  const std::int64_t          east = std::max(ceil_snapped((box.max_corner().x() - first.west) / first.pixel_width), west + 1);
  const std::int64_t          north = floor_snapped((first.north - box.max_corner().y()) / first.pixel_height);
  const std::int64_t          south = std::max(ceil_snapped((first.north - box.min_corner().y()) / first.pixel_height), north + 1);

  geo::Raster<pixel_t> window{
      .west = first.west + (static_cast<double>(west) * first.pixel_width),
      .north = first.north - (static_cast<double>(north) * first.pixel_height),
      .pixel_width = first.pixel_width,
      .pixel_height = first.pixel_height,
      .width = static_cast<std::size_t>(east - west),
      .height = static_cast<std::size_t>(south - north),
      .nodata = first.nodata,
      .pixels = std::vector<pixel_t>(static_cast<std::size_t>((east - west) * (south - north)), first.nodata),
  };

  for ( const auto& row : rows ) {
    const geo::Raster<pixel_t>& tile = schemacht::schema::get<"rast">(row);
    if ( ! same_size(tile.pixel_width, first.pixel_width) || ! same_size(tile.pixel_height, first.pixel_height) || tile.nodata != first.nodata ) {
      throw std::invalid_argument("the tiles loaded don't share a pixel size and a nodata value");
    }

    const std::optional<std::int64_t> column = whole((tile.west - first.west) / first.pixel_width);
    const std::optional<std::int64_t> tile_row = whole((first.north - tile.north) / first.pixel_height);
    if ( ! column || ! tile_row ) {
      throw std::invalid_argument("the tiles loaded are not aligned to one grid of pixels");
    }

    paste(tile, PixelAt{.column = *column, .row = *tile_row}, window, PixelAt{.column = west, .row = north});
  }

  return window;
}

template <geo::Pixel pixel_t>
std::vector<geo::Raster<pixel_t>> RasterLayer<pixel_t>::chunk_from_rows(
    std::vector<typename load_statement_type::result_type> rows, const geo::Polygon& /*location*/
) {
  chunk_type tiles;

  tiles.reserve(rows.size());
  for ( auto& row : rows ) {
    tiles.push_back(std::move(schemacht::schema::get<"rast">(row)));
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
