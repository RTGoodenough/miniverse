#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "miniverse/geo/column_types.hpp"
#include "miniverse/geo/operations.hpp"
#include "miniverse/geo/raster.hpp"
#include "miniverse/geo/types.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/prepared.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/query/raw_statement.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/schema/table_name.hpp"

namespace miniverse {

/** @brief The columns of an elevation table, the same in every `ElevationLayer` but for the pixel type. */
namespace elevation {

/**
 * Which tile of the grid a row is: `tile_row * tiles_across + tile_col`, where tiles are counted east from longitude -180
 * (`tile_col`) and south from latitude 90 (`tile_row`), and `tiles_across` is how many tiles span the world's 360 degrees.
 *
 * One integer key, as `raster2pgsql`'s `rid`: GDAL tells tiles apart by the primary key, and reads the wrong pixels when it
 * is a composite one.
 */
using TileId = schemacht::schema::Field<std::int64_t, "tile_id", schemacht::schema::KeyRole::Primary>;

template <geo::Pixel pixel_t>
using Rast = schemacht::schema::Field<geo::Raster<pixel_t>, "rast">;

/**
 * @brief How a push merges a tile onto the one already at its grid position: `miniverse_functions.merge_raster(current, incoming)`,
 * which the table's setup makes. A pixel with data in the new tile wins; one with no data there keeps the old pixel.
 *
 * The function is called by its full name, so a push finds it whatever its search path. It is one function in the schema
 * `miniverse_functions`, shared by every elevation table in the database, whatever their schemas: it takes nothing from a
 * table. The schema is not named `miniverse`: a role of that name (`"$user"`, first on the default search path) would then
 * make every table it names without a schema in it, not in `public`.
 */
struct MergeRaster {
  static constexpr std::string_view SCHEMA = "miniverse_functions";
  static constexpr std::string_view FUNCTION = "miniverse_functions.merge_raster";
};

}  // namespace elevation

/**
 * @brief The layer kind of elevation, or any other raster of one band of `pixel_t`: one row per tile of the layer's grid
 * (`geo::Grid`, its settings), which every source is warped onto. A load gives the pixels in the bounding box of the location,
 * from the tiles that cover it (see `from_rows`).
 *
 * @code
 * struct Elevation : miniverse::ElevationLayer<std::int16_t> {};
 *
 * const miniverse::geo::Grid<std::int16_t> grid{.pixels_per_degree = 3600, .tile_pixels = 256, .nodata = -32768};
 * miniverse::Miniverse world(conninfo, miniverse::Layer<Elevation>("elevation", grid));
 * world.create_table<Elevation>();  // once
 * miniverse::geo::Raster<std::int16_t> heights = world.load<Elevation>(area).get();
 * @endcode
 *
 * The table keeps its grid in its raster constraints, which PostGIS's `raster_columns` reads, so GDAL and QGIS open the table
 * as one raster, and `Miniverse::table_settings` reads it back from there. The constraints also refuse a tile of another
 * grid, so a layer configured with the wrong grid can't push into the table.
 */
template <geo::Pixel pixel_t>
struct ElevationLayer {
  using result_type = geo::Raster<pixel_t>;
  using settings_type = geo::Grid<pixel_t>;
  /// The table's layout. Its name, `elevation`, is only a placeholder: a layer names its own table (`Layer`).
  using schema_type = schemacht::schema::Schema<"elevation", elevation::TileId, elevation::Rast<pixel_t>>;
  using row_type = schema_type::row_type;

  /** @brief The tiles whose outline's box meets argument 0's box (the GiST index on `ST_ConvexHull(rast)` answers `&&`). */
  static constexpr auto LOAD =
      schemacht::query::select(
          schemacht::query::On<schema_type>::template col<"rast">().template apply<geo::BoxesIntersect>(schemacht::query::arg<0>())
      )
          .project(schemacht::query::On<schema_type>::template col<"rast">());
  using load_statement_type = schemacht::query::Prepared<LOAD>;

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
   * @return The schema `miniverse_functions` and the function in it a push merges a tile with (`elevation::MergeRaster`), each made if
   * missing; the table's spatial index; and its raster constraints, which record `grid` for `raster_columns` (and so GDAL):
   * SRID 4326, the pixel size, the tile size, alignment to the grid, one band of `pixel_t`, the nodata value, no out-db
   * bands, and an extent of the whole world (and a hair more, for rounding), so it never needs widening.
   *
   * The function is replaced by every setup, and outlives a dropped table, as other elevation tables share it (see
   * docs/open-decisions.md). The first setup in a database makes the schema, which needs CREATE on the database; pushing
   * needs USAGE on the schema `miniverse_functions`.
   * @throws std::invalid_argument if `grid` is not a grid: see `geo::Grid`.
   */
  [[nodiscard]] static std::vector<std::string> setup_sql(const schemacht::schema::TableName& table, const settings_type& grid);

  /**
   * @return `raster` cut into the tiles of `grid` it covers. Its pixels equal to its own nodata become the grid's nodata, and a
   * tile with no data at all is left out.
   * @throws std::invalid_argument if `grid` is not a grid (as `setup_sql`), or `raster` is not on it (another pixel size, or
   * not aligned to its pixels), or reaches past the world, or its pixels are not `width * height`, or (when its nodata is not
   * the grid's) a pixel with data has the grid's nodata value.
   */
  [[nodiscard]] static std::vector<row_type> to_rows(result_type raster, const settings_type& grid);

  /** @return The write of `rows`: each tile is inserted, or merged onto the one already there (`elevation::MergeRaster`). */
  [[nodiscard]] static schemacht::postgres::SchemaStatement<schema_type, std::tuple<>> write_statement(const std::vector<row_type>& rows) {
    return schemacht::postgres::upsert_statement<schema_type, schemacht::postgres::MergeWith<"rast", elevation::MergeRaster>>(rows);
  }

  /**
   * @return The pixels of the tiles `rows` in the bounding box of `location`, widened to whole pixels of the tiles' grid.
   * Pixels no tile covers are the tiles' nodata. With no tiles at all, there is no grid to place pixels on: the result is
   * empty (0 by 0).
   * @throws std::invalid_argument if the tiles don't share a grid (pixel size, alignment) and a nodata value.
   */
  [[nodiscard]] static result_type from_rows(std::vector<typename load_statement_type::result_type> rows, const geo::Polygon& location);

  /** @throws std::runtime_error if the table has no grid in its constraints: it was not made by `create_table`. */
  [[nodiscard]] static settings_type settings_from_rows(std::vector<typename settings_statement_type::row_type> rows);
};

extern template struct ElevationLayer<std::int8_t>;
extern template struct ElevationLayer<std::uint8_t>;
extern template struct ElevationLayer<std::int16_t>;
extern template struct ElevationLayer<std::uint16_t>;
extern template struct ElevationLayer<std::int32_t>;
extern template struct ElevationLayer<std::uint32_t>;
extern template struct ElevationLayer<float>;
extern template struct ElevationLayer<double>;

}  // namespace miniverse
