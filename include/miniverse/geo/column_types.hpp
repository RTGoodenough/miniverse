#pragma once

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "miniverse/geo/raster.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/geo/wkb.hpp"
#include "schemacht/query/sql_type.hpp"
#include "schemacht/schema/column_type.hpp"
#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/hex.hpp"

/**
 * The geometry types, and rasters (below), as schemacht column types: a `geo::LineString` can be a `Field`'s type, a query's argument, and inserted
 * and read like a built-in type.
 *
 * Each is a PostGIS `geometry` of its own kind in WGS 84 (`geometry(LineString,4326)`), so the database refuses another kind
 * or SRID in the column. Its text form is hex EWKB, which is what PostGIS prints, and what a `$n::geometry` argument reads;
 * its binary form is EWKB itself. There is no `binary_accepts`: `geometry` comes from an extension, so its type OID differs
 * between databases.
 */
template <miniverse::geo::wkb::Geometry geometry_t>
struct schemacht::ColumnType<geometry_t> {
  static constexpr auto             SQL_TYPE_TEXT = util::concat_ctstrings<"geometry(", miniverse::geo::wkb::type_name<geometry_t>(), ",4326)">();
  static constexpr std::string_view SQL_TYPE = SQL_TYPE_TEXT.view();

  /** @throws std::invalid_argument if `text` is not hex EWKB of a 2D `geometry_t` in WGS 84. */
  [[nodiscard]] static geometry_t parse(std::string_view text) { return miniverse::geo::wkb::read<geometry_t>(util::parse_hex(text)); }

  [[nodiscard]] static std::string format(const geometry_t& geometry) { return util::format_hex(miniverse::geo::wkb::write(geometry)); }

  /** @throws std::invalid_argument if `bytes` are not EWKB of a 2D `geometry_t` in WGS 84. */
  [[nodiscard]] static geometry_t parse_binary(std::span<const std::byte> bytes) { return miniverse::geo::wkb::read<geometry_t>(bytes); }
};

/**
 * A raster of one band as a schemacht column type: a `raster` column, read through `ST_AsBinary` (PostGIS gives `raster` no
 * binary output, so it is selected as `ST_AsBinary("rast") AS "rast"`, a `bytea` of raster WKB).
 *
 * Its text form, as read, is that `bytea`'s: `\x` and hex. As written (an insert, a `$n::raster` argument), it is hex raster
 * WKB, which is what the `raster` type reads. There is no `binary_accepts`, as for geometry.
 */
template <miniverse::geo::Pixel pixel_t>
struct schemacht::ColumnType<miniverse::geo::Raster<pixel_t>> {
  using raster_type = miniverse::geo::Raster<pixel_t>;

  static constexpr std::string_view SQL_TYPE = "raster";

  /** @throws std::invalid_argument if `text` is not a `bytea` of raster WKB as `miniverse::geo::wkb::read_raster` reads it. */
  [[nodiscard]] static raster_type parse(std::string_view text) {
    constexpr std::string_view BYTEA_PREFIX = "\\x";
    if ( ! text.starts_with(BYTEA_PREFIX) ) {
      throw std::invalid_argument("a raster is read as ST_AsBinary's bytea, written \\x and hex");
    }

    return miniverse::geo::wkb::read_raster<pixel_t>(util::parse_hex(text.substr(BYTEA_PREFIX.size())));
  }

  [[nodiscard]] static std::string format(const raster_type& raster) { return util::format_hex(miniverse::geo::wkb::write(raster)); }

  /** @throws std::invalid_argument if `bytes` are not raster WKB as `miniverse::geo::wkb::read_raster` reads it. */
  [[nodiscard]] static raster_type parse_binary(std::span<const std::byte> bytes) { return miniverse::geo::wkb::read_raster<pixel_t>(bytes); }
};

/** @brief A raster column is selected as `ST_AsBinary("rast") AS "rast"`: see `ColumnType<Raster>` above. */
template <miniverse::geo::Pixel pixel_t>
class schemacht::query::sql::SelectExpression<miniverse::geo::Raster<pixel_t>> {
 public:
  static constexpr std::string_view FUNCTION = "ST_AsBinary";
};
