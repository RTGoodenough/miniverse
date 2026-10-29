#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "miniverse/geo/column_types.hpp"
#include "miniverse/geo/grid.hpp"
#include "miniverse/geo/operations.hpp"
#include "miniverse/geo/raster.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/internal_schema.hpp"
#include "miniverse/model/raster/tiles.hpp"
#include "miniverse/model/writes.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/prepared.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/query/raw_statement.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/schema/table_name.hpp"

/**
 * @file
 * The model of rasters on a grid: its table's columns, constraints and merge function, and its rows made from a raster
 * and back. The pixel arithmetic is raster/tiles.hpp, and which tiles a location has is raster/reach.hpp.
 */
namespace miniverse {

/** @brief A raster table's columns and its merge function. Its tiles, and which a location has, are in raster/. */
namespace raster {

/**
 * @brief Which tile of the grid a row is (`raster::Tile::id`). One integer key, as `raster2pgsql`'s `rid`: GDAL reads the wrong
 * pixels when the key is composite.
 */
using TileId = schemacht::schema::Field<std::int64_t, "tile_id", schemacht::schema::KeyRole::Primary>;

/** @brief The tile's pixels, a PostGIS `raster` of one band. */
template <geo::Pixel pixel_t>
using Rast = schemacht::schema::Field<geo::Raster<pixel_t>, "rast">;

/**
 * @brief How a push merges a tile onto the one at its grid position: `miniverse_internal.merge_raster(current, incoming)`,
 * made by the table's setup in the library's own schema (`INTERNAL_SCHEMA`) and shared by every raster table in the database.
 * A pixel with data in the new tile wins; one with no data keeps the old pixel. The function is called by its full name, so a
 * push finds it whatever the search path.
 */
struct MergeRaster {
  static constexpr std::string_view FUNCTION = "miniverse_internal.merge_raster";
  static_assert(FUNCTION.starts_with(INTERNAL_SCHEMA), "the merge function lives in the library's own schema");
};

}  // namespace raster

/**
 * @brief The model of rasters of one band of `pixel_t`, such as elevation: one row per tile of the table's grid
 * (`geo::Grid`, its settings), which every source is warped onto. A load gives the pixels in the bounding box of the location
 * (`raster::window`), from the tiles the location reaches into (`raster::Reach`): where the box holds a tile the location does
 * not reach, as the box of an area that runs askew does, the pixels are nodata, though the table may have data there.
 *
 * @code
 * using Elevation = miniverse::Layer<miniverse::RasterModel<std::int16_t>, "elevation">;
 *
 * miniverse::Miniverse world(conninfo, Elevation("srtm"));
 * world.create_table<"elevation">({.pixels_per_degree = 3600, .tile_pixels = 256, .nodata = -32768});  // once
 * miniverse::geo::Raster<std::int16_t> heights = world.load<"elevation">(area).get();
 * @endcode
 *
  * The grid is chosen when the table is made and kept in its raster constraints, which `raster_columns` reads. So GDAL and QGIS
 * open the table as one raster, `Miniverse::table_settings` gives a writer the grid, every push reads it before cutting its
 * raster into tiles, and the constraints refuse a tile of another grid.
 *
 * The pixel type decides the table's size more than anything, since tiles are compressed (lz4) and whole numbers compress
 * where reals hardly do: for heights, 16-bit whole metres.
 */
template <geo::Pixel pixel_t>
struct RasterModel {
  static constexpr std::int32_t LAYOUT = 1;  ///< The first layout recorded.

  using result_type = geo::Raster<pixel_t>;
  /// What a streamed load hands its callback: some of the tiles, each a raster of its own, since a window needs them all.
  using chunk_type = std::vector<geo::Raster<pixel_t>>;
  using writes = miniverse::writes::WithSettings<geo::Grid<pixel_t>>;
  /// The table's layout. Its name, `raster`, is only a placeholder: a layer names its own table (`Layer`).
  using schema_type = schemacht::schema::Schema<"raster", raster::TileId, raster::Rast<pixel_t>>;
  using row_type = schema_type::row_type;

  /**
   * @brief The tiles whose outlines argument 0 meets: those whose boxes meet its box, which the GiST index on
   * `ST_ConvexHull(rast)` finds, and of them those the polygon itself meets.
   */
  static constexpr auto LOAD =
      schemacht::query::select(
          schemacht::query::On<schema_type>::template col<"rast">().template apply<geo::OutlineIntersects>(schemacht::query::arg<0>())
      )
          .project(schemacht::query::On<schema_type>::template col<"rast">());
  using load_statement_type = schemacht::query::Prepared<LOAD>;

  /**
   * @brief A statement's worth of pixels, in whole tiles (at least one): a tile of 1200 by 1200 `std::int16_t` is 2.9 MB, so 11
   * of them. As text, which is how it is sent, a statement is about twice this.
   */
  static constexpr std::size_t PIXEL_BYTES_PER_STATEMENT = std::size_t{32} << 20U;

  /** @brief The grid the table's constraints record, as `raster_columns` reads them: argument 1 is the table's quoted name. */
  using settings_statement_type = schemacht::query::RawStatement<
      "SELECT scale_x, blocksize_x, nodata_values[1] AS nodata FROM raster_columns "
      "JOIN pg_class ON pg_class.relname = r_table_name "
      "JOIN pg_namespace ON pg_namespace.oid = pg_class.relnamespace AND pg_namespace.nspname = r_table_schema "
      "WHERE pg_class.oid = $1::regclass AND r_raster_column = 'rast' "
      "AND scale_x IS NOT NULL AND blocksize_x IS NOT NULL AND nodata_values[1] IS NOT NULL",
      schemacht::query::RawArguments<std::string>, schemacht::schema::Field<double, "scale_x">, schemacht::schema::Field<std::int32_t, "blocksize_x">,
      schemacht::schema::Field<double, "nodata">>;

  /**
   * @return The statements that set the table up. They run in one transaction (`create_table` does so), after the registry's
   * setup, which makes the library's schema and holds the lock under which setups take turns; a failing one makes no table.
   * - The merge function (`raster::MergeRaster`) in that schema, made if it is not there yet, and shared by every raster
   *   table of the database. A push needs USAGE on the schema.
   * - lz4 compression of the tiles, which needs PostgreSQL 14 or newer built with lz4.
   * - The GiST index on `ST_ConvexHull(rast)`.
   * - The raster constraints that record `grid` for `raster_columns`: SRID 4326, pixel and tile size, alignment, one band of
   *   `pixel_t`, the nodata value, in-db bands, and the grid's whole span as extent.
   * @throws std::invalid_argument if `grid` is not a grid: see `geo::Grid`.
   */
  [[nodiscard]] static std::vector<std::string> setup_sql(const schemacht::schema::TableName& table, const geo::Grid<pixel_t>& grid);

  /**
      * @return `raster` cut into the tiles of `grid`, the table's, it covers (`raster::cut`), in batches of
   * `PIXEL_BYTES_PER_STATEMENT`, in tile-id order.
   * @throws std::invalid_argument as `raster::cut`: `grid` is not a grid, or `raster` is not on it, or reaches past the world,
   * or its pixels are not `width * height`, or a pixel with data has the grid's nodata value.
   */
  [[nodiscard]] static std::vector<std::vector<row_type>> to_rows(result_type raster, const geo::Grid<pixel_t>& grid);

  /** @return The write of `rows`: each tile is inserted, or merged onto the one already there (`raster::MergeRaster`). */
  [[nodiscard]] static schemacht::postgres::SchemaStatement<schema_type, std::tuple<>> write_statement(const std::vector<row_type>& rows) {
    return schemacht::postgres::upsert_statement<schema_type, schemacht::postgres::MergeWith<"rast", raster::MergeRaster>>(rows);
  }

  /** @return What `raster::window` makes of the tiles of `rows` that `location` reaches into: the pixels in its bounding box. */
  [[nodiscard]] static result_type from_rows(std::vector<typename load_statement_type::result_type> rows, const geo::Polygon& location);

  /**
   * @return The tiles of `rows` that `location` reaches into (`raster::Reach`), whole and in no particular order. The rows may
   * hold a tile more, one the location only touches, since the database's test counts touching.
   */
  [[nodiscard]] static chunk_type chunk_from_rows(std::vector<typename load_statement_type::result_type> rows, const geo::Polygon& location);

  /**
   * @return The grid the table's constraints record, read back by `settings_statement_type`.
   * @throws std::runtime_error if the table has no grid in its constraints (it was not made by `create_table`), or its pixels
   * are not 1/n degree for a whole number n, or its nodata value is not a `pixel_t`.
   */
  [[nodiscard]] static geo::Grid<pixel_t> settings_from_rows(std::vector<typename settings_statement_type::row_type> rows);
};

extern template struct RasterModel<std::int8_t>;
extern template struct RasterModel<std::uint8_t>;
extern template struct RasterModel<std::int16_t>;
extern template struct RasterModel<std::uint16_t>;
extern template struct RasterModel<std::int32_t>;
extern template struct RasterModel<std::uint32_t>;
extern template struct RasterModel<float>;
extern template struct RasterModel<double>;

}  // namespace miniverse
