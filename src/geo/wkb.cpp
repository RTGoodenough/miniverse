#include "miniverse/geo/wkb.hpp"

#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/geo/types.hpp"
#include "miniverse/geo/wkb_bytes.hpp"

namespace miniverse::geo::wkb {

namespace {

// The geometry type numbers of WKB, as the low bits of the type word.
enum class Kind : std::uint8_t {
  Point = 1,
  LineString = 2,
  Polygon = 3,
};

// EWKB's flags in the type word's high bits, and the bits left for the type number.
constexpr std::uint32_t Z_FLAG = 0x80000000U;
constexpr std::uint32_t M_FLAG = 0x40000000U;
constexpr std::uint32_t SRID_FLAG = 0x20000000U;
constexpr std::uint32_t KIND_MASK = 0x0FFFFFFFU;

// ISO WKB numbers a type with Z, M or both as 1000, 2000 or 3000 more than the 2D one, so any number past this is not 2D.
constexpr std::uint32_t LAST_2D_KIND = 7;

constexpr std::size_t WORD_BYTES = 4;
constexpr std::size_t POINT_BYTES = 2 * sizeof(double);

/** @brief The points of a line or a ring: Boost.Geometry's `linestring` and `ring` are both a `std::vector` of them. */
using Points = std::vector<Point>;

template <Geometry geometry_t>
[[nodiscard]] constexpr Kind kind_of() {
  if constexpr ( std::same_as<geometry_t, Point> ) {
    return Kind::Point;

  } else if constexpr ( std::same_as<geometry_t, LineString> ) {
    return Kind::LineString;

  } else {
    return Kind::Polygon;
  }
}

// ---- Reading --------------------------------------------------------------------------------------------------------

/** @brief Reads the geometry's header (byte order, type, SRID) and checks it is a 2D `geometry_t` in WGS 84. */
template <Geometry geometry_t>
void read_header(Reader& reader) {
  reader.byte_order();

  const auto type = reader.number<std::uint32_t>();
  if ( (type & (Z_FLAG | M_FLAG)) != 0 || (type & KIND_MASK) > LAST_2D_KIND ) {
    malformed("only two-dimensional geometry is read, not one with Z or M values");
  }

  if ( (type & KIND_MASK) != static_cast<std::uint32_t>(kind_of<geometry_t>()) ) {
    malformed("expected a " + std::string(type_name<geometry_t>().view()) + ", found geometry type " + std::to_string(type & KIND_MASK));
  }

  if ( (type & SRID_FLAG) == 0 ) {
    malformed("the geometry names no SRID, so its coordinates could be in any system (expected " + std::to_string(WGS84_SRID) + ")");
  }

  const auto srid = reader.number<std::int32_t>();
  if ( srid != WGS84_SRID ) {
    malformed("the geometry has SRID " + std::to_string(srid) + ", not " + std::to_string(WGS84_SRID));
  }
}

[[nodiscard]] Point read_point(Reader& reader) {
  const auto lon = reader.number<double>();
  const auto lat = reader.number<double>();

  return {lon, lat};
}

void read_points(Reader& reader, Points& points) {
  const std::size_t size = reader.count(POINT_BYTES);

  points.reserve(size);
  for ( std::size_t i = 0; i < size; ++i ) {
    points.push_back(read_point(reader));
  }
}

template <Geometry geometry_t>
[[nodiscard]] geometry_t read_body(Reader& reader) {
  if constexpr ( std::same_as<geometry_t, Point> ) {
    const Point point = read_point(reader);
    if ( std::isnan(point.x()) && std::isnan(point.y()) ) {
      malformed("an empty point has no position");  // WKB writes POINT EMPTY as two NaNs
    }

    return point;

  } else if constexpr ( std::same_as<geometry_t, LineString> ) {
    LineString line;
    read_points(reader, line);

    return line;

  } else {
    const std::size_t rings = reader.count(WORD_BYTES);  // each ring is at least its own count
    Polygon           polygon;

    if ( rings > 0 ) {
      read_points(reader, polygon.outer());
    }

    polygon.inners().resize(rings > 0 ? rings - 1 : 0);
    for ( auto& inner : polygon.inners() ) {
      read_points(reader, inner);
    }

    return polygon;
  }
}

// ---- Writing ------------------------------------------------------------------------------------------------------

void put_point(Writer& out, const Point& position) {
  out.number(position.x());
  out.number(position.y());
}

void put_points(Writer& out, const Points& positions) {
  out.count(positions.size());
  for ( const Point& position : positions ) {
    put_point(out, position);
  }
}

template <Geometry geometry_t>
void put_body(Writer& out, const geometry_t& geometry) {
  if constexpr ( std::same_as<geometry_t, Point> ) {
    put_point(out, geometry);

  } else if constexpr ( std::same_as<geometry_t, LineString> ) {
    put_points(out, geometry);

  } else {
    if ( geometry.outer().empty() && geometry.inners().empty() ) {
      out.count(0);  // no rings, as PostGIS writes POLYGON EMPTY
      return;
    }

    out.count(geometry.inners().size() + 1);
    put_points(out, geometry.outer());
    for ( const auto& inner : geometry.inners() ) {
      put_points(out, inner);
    }
  }
}

}  // namespace

template <Geometry geometry_t>
std::vector<std::byte> write(const geometry_t& geometry) {
  Writer out;
  out.number(static_cast<std::uint32_t>(kind_of<geometry_t>()) | SRID_FLAG);
  out.number(WGS84_SRID);
  put_body(out, geometry);

  return std::move(out).bytes();
}

template <Geometry geometry_t>
geometry_t read(std::span<const std::byte> bytes) {
  Reader reader(bytes);
  read_header<geometry_t>(reader);
  auto geometry = read_body<geometry_t>(reader);

  if ( ! reader.at_end() ) {
    malformed(std::to_string(reader.remaining()) + " bytes are left after the " + std::string(type_name<geometry_t>().view()));
  }

  return geometry;
}

template std::vector<std::byte> write<Point>(const Point& geometry);
template std::vector<std::byte> write<LineString>(const LineString& geometry);
template std::vector<std::byte> write<Polygon>(const Polygon& geometry);

template Point      read<Point>(std::span<const std::byte> bytes);
template LineString read<LineString>(std::span<const std::byte> bytes);
template Polygon    read<Polygon>(std::span<const std::byte> bytes);

}  // namespace miniverse::geo::wkb
