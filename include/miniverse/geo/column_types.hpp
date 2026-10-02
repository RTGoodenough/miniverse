#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <boost/algorithm/hex.hpp>

#include "miniverse/geo/types.hpp"
#include "miniverse/geo/wkb.hpp"
#include "schemacht/column_type.hpp"
#include "schemacht/util/compile_time.hpp"

/**
 * The geometry types as schemacht column types: a `geo::LineString` can be a `Field`'s type, a raw statement's argument, and
 * inserted and read like a built-in type.
 *
 * Each is a PostGIS `geometry` of its own kind in WGS 84 (`geometry(LineString,4326)`), so the database refuses another kind
 * or SRID in the column. Its text form is hex EWKB, which is what PostGIS prints, and what a `$n::geometry` argument reads;
 * its binary form is EWKB itself. There is no `binary_accepts`: `geometry` comes from an extension, so its type OID differs
 * between databases.
 */
namespace miniverse::geo {

/** @brief The `ColumnType` of a geometry type stored as the PostGIS type `sql_type`. */
template <wkb::Geometry geometry_t, schemacht::util::CTString sql_type>
struct GeometryColumn {
  static constexpr std::string_view SQL_TYPE = sql_type.view();

  /** @throws std::invalid_argument if `text` is not hex EWKB of a 2D `geometry_t` in WGS 84. */
  [[nodiscard]] static geometry_t parse(std::string_view text) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() / 2);
    try {
      boost::algorithm::unhex(text, std::back_inserter(bytes));
    } catch ( const boost::algorithm::hex_decode_error& ) {
      throw std::invalid_argument("geometry: the text is not hex EWKB");
    }
    return wkb::read<geometry_t>(std::as_bytes(std::span(bytes)));
  }

  [[nodiscard]] static std::string format(const geometry_t& geometry) {
    const std::vector<std::byte> bytes = wkb::write(geometry);
    std::string                  text;
    text.reserve(bytes.size() * 2);
    const auto octets = bytes | std::views::transform([](std::byte part) { return std::to_integer<std::uint8_t>(part); });
    // NOLINTNEXTLINE(boost-use-ranges) -- Boost's range overload needs a nested ::iterator, which a transform_view lacks
    boost::algorithm::hex(octets.begin(), octets.end(), std::back_inserter(text));
    return text;
  }

  /** @throws std::invalid_argument if `bytes` are not EWKB of a 2D `geometry_t` in WGS 84. */
  [[nodiscard]] static geometry_t parse_binary(std::span<const std::byte> bytes) { return wkb::read<geometry_t>(bytes); }
};

}  // namespace miniverse::geo

template <>
struct schemacht::ColumnType<miniverse::geo::Point> : miniverse::geo::GeometryColumn<miniverse::geo::Point, "geometry(Point,4326)"> {};

template <>
struct schemacht::ColumnType<miniverse::geo::LineString> : miniverse::geo::GeometryColumn<miniverse::geo::LineString, "geometry(LineString,4326)"> {};

template <>
struct schemacht::ColumnType<miniverse::geo::Polygon> : miniverse::geo::GeometryColumn<miniverse::geo::Polygon, "geometry(Polygon,4326)"> {};

template <>
struct schemacht::ColumnType<miniverse::geo::MultiPolygon>
    : miniverse::geo::GeometryColumn<miniverse::geo::MultiPolygon, "geometry(MultiPolygon,4326)"> {};
