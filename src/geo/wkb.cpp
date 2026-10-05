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
  MultiLineString = 5,
  MultiPolygon = 6,
};

// Where a geometry is: on its own, or a member of a multi-geometry. A member need name no SRID of its own: it has the whole's.
enum class Place : std::uint8_t {
  Whole,
  Member,
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
constexpr std::size_t MEMBER_BYTES = 1 + WORD_BYTES + WORD_BYTES;  // a member is at least its byte order, type and count

/** @brief The points of a line or a ring: Boost.Geometry's `linestring` and `ring` are both a `std::vector` of them. */
using Points = std::vector<Point>;

template <Geometry geometry_t>
[[nodiscard]] constexpr Kind kind_of() {
  if constexpr ( std::same_as<geometry_t, Point> ) {
    return Kind::Point;

  } else if constexpr ( std::same_as<geometry_t, LineString> ) {
    return Kind::LineString;

  } else if constexpr ( std::same_as<geometry_t, Polygon> ) {
    return Kind::Polygon;

  } else if constexpr ( std::same_as<geometry_t, MultiLineString> ) {
    return Kind::MultiLineString;

  } else {
    static_assert(std::same_as<geometry_t, MultiPolygon>, "a new geometry type needs its WKB type number here");

    return Kind::MultiPolygon;
  }
}

// ---- Reading --------------------------------------------------------------------------------------------------------

/**
 * @brief Reads the geometry's header (byte order, type, SRID) and checks it is a 2D `geometry_t` in WGS 84. A whole geometry
 * must name its SRID; a member of a multi-geometry need not.
 */
template <Geometry geometry_t>
void read_header(Reader& reader, Place place) {
  reader.byte_order();

  const auto type = reader.number<std::uint32_t>();
  if ( (type & (Z_FLAG | M_FLAG)) != 0 || (type & KIND_MASK) > LAST_2D_KIND ) {
    malformed("only two-dimensional geometry is read, not one with Z or M values");
  }

  if ( (type & KIND_MASK) != static_cast<std::uint32_t>(kind_of<geometry_t>()) ) {
    malformed("expected a " + std::string(type_name<geometry_t>().view()) + ", found geometry type " + std::to_string(type & KIND_MASK));
  }

  if ( (type & SRID_FLAG) == 0 ) {
    if ( place == Place::Whole ) {
      malformed("the geometry names no SRID, so its coordinates could be in any system (expected " + std::to_string(WGS84_SRID) + ")");
    }

    return;
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

  } else if constexpr ( std::same_as<geometry_t, Polygon> ) {
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

  } else {  // a multi-geometry: a count, then each member as a geometry of its own, in a byte order of its own
    using member_t = geometry_t::value_type;

    const std::size_t members = reader.count(MEMBER_BYTES);
    geometry_t        multi;

    multi.reserve(members);
    for ( std::size_t i = 0; i < members; ++i ) {
      read_header<member_t>(reader, Place::Member);
      multi.push_back(read_body<member_t>(reader));
    }

    return multi;
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

  } else if constexpr ( std::same_as<geometry_t, Polygon> ) {
    if ( geometry.outer().empty() && geometry.inners().empty() ) {
      out.count(0);  // no rings, as PostGIS writes POLYGON EMPTY
      return;
    }

    out.count(geometry.inners().size() + 1);
    put_points(out, geometry.outer());
    for ( const auto& inner : geometry.inners() ) {
      put_points(out, inner);
    }

  } else {  // a multi-geometry: each member with a header of its own, without an SRID, as PostGIS writes them
    using member_t = geometry_t::value_type;

    out.count(geometry.size());
    for ( const member_t& member : geometry ) {
      out.byte(std::byte{Reader::LITTLE_ENDIAN_MARK});
      out.number(static_cast<std::uint32_t>(kind_of<member_t>()));
      put_body(out, member);
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
  read_header<geometry_t>(reader, Place::Whole);
  auto geometry = read_body<geometry_t>(reader);

  if ( ! reader.at_end() ) {
    malformed(std::to_string(reader.remaining()) + " bytes are left after the " + std::string(type_name<geometry_t>().view()));
  }

  return geometry;
}

template std::vector<std::byte> write<Point>(const Point& geometry);
template std::vector<std::byte> write<LineString>(const LineString& geometry);
template std::vector<std::byte> write<Polygon>(const Polygon& geometry);
template std::vector<std::byte> write<MultiLineString>(const MultiLineString& geometry);
template std::vector<std::byte> write<MultiPolygon>(const MultiPolygon& geometry);

template Point      read<Point>(std::span<const std::byte> bytes);
template LineString read<LineString>(std::span<const std::byte> bytes);
template Polygon    read<Polygon>(std::span<const std::byte> bytes);
template MultiLineString read<MultiLineString>(std::span<const std::byte> bytes);
template MultiPolygon    read<MultiPolygon>(std::span<const std::byte> bytes);

}  // namespace miniverse::geo::wkb
