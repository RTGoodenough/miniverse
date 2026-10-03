#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "miniverse/geo/types.hpp"
#include "miniverse/geo/wkb.hpp"
#include "schemacht/schema/column_type.hpp"
#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/hex.hpp"

/**
 * The geometry types as schemacht column types: a `geo::LineString` can be a `Field`'s type, a query's argument, and inserted
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
