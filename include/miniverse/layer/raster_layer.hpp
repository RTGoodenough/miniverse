#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
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

/**
 * @brief Which tiles of a grid a location has: those it reaches into. A load of a raster layer reads these and no others,
 * from a table or from a file, so a long thin area that runs askew costs its own tiles, not all the tiles of its box.
 *
 * A tile the location only touches, at an edge or a corner, is not one of them: no pixel of the location is in it. Touching
 * is told to a billionth of a tile's size, since a tile's edge, worked out in degrees, comes a rounding step to one side of
 * where a location that ends on it was written: a location that reaches no further into a tile than that is not in it. So a
 * location of no area, a point or a line, that lies along the line between tiles has no tile at all.
 */
class Reach {
 public:
  /** @param location A polygon in WGS 84. Its rings may run either way round. */
  explicit Reach(geo::Polygon location);

  /** @return Whether the location reaches into `tile`, a tile's outline. */
  [[nodiscard]] bool into(const geo::Box& tile) const;

  Reach(const Reach&) = default;
  Reach(Reach&&) = default;
  Reach& operator=(const Reach&) = default;
  Reach& operator=(Reach&&) = default;
  ~Reach() = default;

 private:
  geo::Polygon _location;
};

}  // namespace raster

/**
 * @brief The layer kind of rasters of one band of `pixel_t`, such as elevation: one row per tile of the table's grid
 * (`geo::Grid`, its settings), which every source is warped onto. A load gives the pixels in the bounding box of the location,
 * from the tiles the location reaches into (`raster::Reach`; see `from_rows`): where the box holds a tile the location does
 * not reach, as the box of an area that runs askew does, the pixels are nodata, though the table may have data there.
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
 *
 * The pixel type is the layer's to choose, and it decides the table's size more than anything: tiles are stored compressed
 * (lz4), which halves whole numbers and hardly touches reals. Measured on 30 m heights, 16-bit whole metres take a third to
 * a sixth of the space of 32-bit reals and load a third faster; 32-bit whole decimetres, where a metre is too coarse, take
 * about half the reals' space.
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
   * @return A lock that setups take turns by; the schema `miniverse_functions` and the function in it a push merges a tile with
   * (`raster::MergeRaster`), each made if missing; lz4 as the compression of the tiles where they are stored; the table's
   * spatial index; and its raster constraints, which record `grid` for `raster_columns` (and so GDAL):
   * SRID 4326, the pixel size, the tile size, alignment to the grid, one band of `pixel_t`, the nodata value, no out-db
   * bands, and an extent of the whole world (and a hair more, for rounding), so it never needs widening.
   *
   * The function is replaced by every setup, and outlives a dropped table, as other raster tables share it. The first
   * setup in a database makes the schema, which needs CREATE on the database; pushing needs USAGE on the schema
   * `miniverse_functions`. Two setups at once take turns: PostgreSQL lets no two transactions make one schema or replace
   * one function at the same time, so each first takes an advisory lock, held until its transaction ends (`create_table`
   * runs them in one), and the second waits for the first. Run outside a transaction, each statement is one of its own, and
   * the lock is let go at once: run them all in one.
   *
   * The compression needs PostgreSQL 14 or newer, built with lz4, as the usual packages and images are: on one that is not,
   * that statement fails, and `create_table` then makes no table. Tiles of 16-bit heights come to about half their size, and
   * load about a tenth slower than uncompressed (as measured); tiles of reals hardly compress at all.
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

  /** @return What `window` makes of the tiles of `rows` that `location` reaches into: the pixels in its bounding box. */
  [[nodiscard]] static result_type from_rows(std::vector<typename load_statement_type::result_type> rows, const geo::Polygon& location);

  /**
   * @return The pixels of `tiles`, rasters of one grid, in the bounding box of `location`, widened to whole pixels of that
   * grid. Pixels no tile covers are the tiles' nodata. With no tiles at all, there is no grid to place pixels on: the result
   * is empty (0 by 0). What a load is, whether its tiles come from a table or from a reader of a file. It takes the tiles
   * it is given: which tiles a location has is `chunk_from_rows`'s to say, and a file reader's.
   * @throws std::invalid_argument if the tiles don't share a grid (pixel size, alignment) and a nodata value.
   */
  [[nodiscard]] static result_type window(std::span<const geo::Raster<pixel_t>> tiles, const geo::Polygon& location);

  /**
   * @return The tiles of `rows` that `location` reaches into (`raster::Reach`), each as it is stored, in no particular order:
   * whole tiles, not cut to the location. The rows may hold a tile more, which the location only touches: the database's
   * test counts touching. A tile is megabytes, so stream them a few at a time (`Miniverse::stream`'s `read.chunk_rows`).
   */
  [[nodiscard]] static chunk_type chunk_from_rows(std::vector<typename load_statement_type::result_type> rows, const geo::Polygon& location);

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
