#pragma once

#include <cstddef>
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

/** @brief The columns of a raster table, the same in every `RasterLayer` but for the pixel type. */
namespace raster {

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
 * `miniverse_functions`, shared by every raster table in the database, whatever their schemas: it takes nothing from a
 * table. The schema is not named `miniverse`: a role of that name (`"$user"`, first on the default search path) would then
 * make every table it names without a schema in it, not in `public`.
 */
struct MergeRaster {
  static constexpr std::string_view SCHEMA = "miniverse_functions";
  static constexpr std::string_view FUNCTION = "miniverse_functions.merge_raster";
};

}  // namespace raster

/**
 * @brief The layer kind of rasters of one band of `pixel_t`, such as elevation: one row per tile of the table's grid
 * (`geo::Grid`, its settings), which every source is warped onto. A load gives the pixels in the bounding box of the location,
 * from the tiles that cover it (see `from_rows`).
 *
 * @code
 * struct Elevation : miniverse::RasterLayer<std::int16_t> {};
 *
 * miniverse::Miniverse world(conninfo, miniverse::Layer<Elevation>("elevation"));
 * world.create_table<Elevation>({.pixels_per_degree = 3600, .tile_pixels = 256, .nodata = -32768});  // once
 * miniverse::geo::Raster<std::int16_t> heights = world.load<Elevation>(area).get();
 * @endcode
 *
 * The grid is chosen once, when the table is made, and the table keeps it in its raster constraints, which PostGIS's
 * `raster_columns` reads: GDAL and QGIS open the table as one raster, `Miniverse::table_settings` gives the grid to a writer to
 * warp its source onto, and every push reads it before cutting its raster into tiles. The constraints also refuse a tile of
 * another grid.
 */
template <geo::Pixel pixel_t>
struct RasterLayer {
  using result_type = geo::Raster<pixel_t>;
  /// What a streamed load hands its callback: some of the tiles, each a raster of its own, since a window needs them all.
  using chunk_type = std::vector<geo::Raster<pixel_t>>;
  using settings_type = geo::Grid<pixel_t>;
  /// The table's layout. Its name, `raster`, is only a placeholder: a layer names its own table (`Layer`).
  using schema_type = schemacht::schema::Schema<"raster", raster::TileId, raster::Rast<pixel_t>>;
  using row_type = schema_type::row_type;

  /** @brief The tiles whose outline's box meets argument 0's box (the GiST index on `ST_ConvexHull(rast)` answers `&&`). */
  static constexpr auto LOAD =
      schemacht::query::select(
          schemacht::query::On<schema_type>::template col<"rast">().template apply<geo::BoxesIntersect>(schemacht::query::arg<0>())
      )
          .project(schemacht::query::On<schema_type>::template col<"rast">());
  using load_statement_type = schemacht::query::Prepared<LOAD>;

  /**
   * @brief How many bytes of pixels a push writes in one statement, at most, in whole tiles (at least one): a tile of 1200 by
   * 1200 `std::int16_t` is 2.9 MB, so 11 of them. As text, which is how it is sent, a statement is about twice this.
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
   * @return The schema `miniverse_functions` and the function in it a push merges a tile with (`raster::MergeRaster`), each made if
   * missing; the table's spatial index; and its raster constraints, which record `grid` for `raster_columns` (and so GDAL):
   * SRID 4326, the pixel size, the tile size, alignment to the grid, one band of `pixel_t`, the nodata value, no out-db
   * bands, and an extent of the whole world (and a hair more, for rounding), so it never needs widening.
   *
   * The function is replaced by every setup, and outlives a dropped table, as other raster tables share it. The first
   * setup in a database makes the schema, which needs CREATE on the database; pushing needs USAGE on the schema
   * `miniverse_functions`. Two setups at once can conflict on it (`CREATE OR REPLACE FUNCTION` and `CREATE SCHEMA IF NOT
   * EXISTS` are not safe against each other): the second then fails, and can be run again.
   * @throws std::invalid_argument if `grid` is not a grid: see `geo::Grid`.
   */
  [[nodiscard]] static std::vector<std::string> setup_sql(const schemacht::schema::TableName& table, const settings_type& grid);

  /**
   * @return `raster` cut into the tiles of `grid`, the table's, it covers, in batches of `PIXEL_BYTES_PER_STATEMENT`, by tile id.
   * Its pixels equal to its own nodata become the grid's nodata, and a tile with no data at all is left out.
   * @throws std::invalid_argument if `grid` is not a grid (as `setup_sql`), or `raster` is not on it (another pixel size, or
   * not aligned to its pixels), or reaches past the world, or its pixels are not `width * height`, or (when its nodata is not
   * the grid's) a pixel with data has the grid's nodata value.
   */
  [[nodiscard]] static std::vector<std::vector<row_type>> to_rows(result_type raster, const settings_type& grid);

  /** @return The write of `rows`: each tile is inserted, or merged onto the one already there (`raster::MergeRaster`). */
  [[nodiscard]] static schemacht::postgres::SchemaStatement<schema_type, std::tuple<>> write_statement(const std::vector<row_type>& rows) {
    return schemacht::postgres::upsert_statement<schema_type, schemacht::postgres::MergeWith<"rast", raster::MergeRaster>>(rows);
  }

  /**
   * @return The pixels of the tiles `rows` in the bounding box of `location`, widened to whole pixels of the tiles' grid.
   * Pixels no tile covers are the tiles' nodata. With no tiles at all, there is no grid to place pixels on: the result is
   * empty (0 by 0).
   * @throws std::invalid_argument if the tiles don't share a grid (pixel size, alignment) and a nodata value.
   */
  [[nodiscard]] static result_type from_rows(std::vector<typename load_statement_type::result_type> rows, const geo::Polygon& location);

  /**
   * @return The tiles `rows`, each as it is stored, in no particular order: whole tiles, not cut to `location`'s box. A tile is
   * megabytes, so stream them a few at a time (`Miniverse::stream`'s `read.chunk_rows`).
   */
  [[nodiscard]] static chunk_type chunk_from_rows(std::vector<typename load_statement_type::result_type> rows, const geo::Polygon& /*location*/);

  /** @throws std::runtime_error if the table has no grid in its constraints: it was not made by `create_table`. */
  [[nodiscard]] static settings_type settings_from_rows(std::vector<typename settings_statement_type::row_type> rows);
};

extern template struct RasterLayer<std::int8_t>;
extern template struct RasterLayer<std::uint8_t>;
extern template struct RasterLayer<std::int16_t>;
extern template struct RasterLayer<std::uint16_t>;
extern template struct RasterLayer<std::int32_t>;
extern template struct RasterLayer<std::uint32_t>;
extern template struct RasterLayer<float>;
extern template struct RasterLayer<double>;

}  // namespace miniverse
