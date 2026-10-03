#include "miniverse/geo/wkb.hpp"

#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "miniverse/geo/types.hpp"

namespace miniverse::geo::wkb {

namespace {

// The geometry type numbers of WKB, as the low bits of the type word.
enum class Kind : std::uint8_t {
  Point = 1,
  LineString = 2,
  Polygon = 3,
};

constexpr std::uint8_t BIG_ENDIAN_MARK = 0;
constexpr std::uint8_t LITTLE_ENDIAN_MARK = 1;

// EWKB's flags in the type word's high bits, and the bits left for the type number.
constexpr std::uint32_t Z_FLAG = 0x80000000U;
constexpr std::uint32_t M_FLAG = 0x40000000U;
constexpr std::uint32_t SRID_FLAG = 0x20000000U;
constexpr std::uint32_t KIND_MASK = 0x0FFFFFFFU;

// ISO WKB numbers a type with Z, M or both as 1000, 2000 or 3000 more than the 2D one, so any number past this is not 2D.
constexpr std::uint32_t LAST_2D_KIND = 7;

constexpr std::size_t   BITS_PER_BYTE = 8;
constexpr std::size_t   WORD_BYTES = 4;
constexpr std::size_t   REAL_BYTES = 8;
constexpr std::size_t   POINT_BYTES = 2 * REAL_BYTES;
constexpr std::uint64_t BYTE_MASK = 0xFFU;

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

[[noreturn]] void malformed(const std::string& what) { throw std::invalid_argument("wkb: " + what); }

// ---- Reading --------------------------------------------------------------------------------------------------------

/** @brief Reads WKB's numbers from a buffer, in the byte order the geometry's header names. */
class Reader {
 public:
  explicit Reader(std::span<const std::byte> bytes) : _bytes(bytes) {}

  [[nodiscard]] bool at_end() const noexcept { return _at == _bytes.size(); }

  [[nodiscard]] std::size_t remaining() const noexcept { return _bytes.size() - _at; }

  void set_byte_order(std::byte mark) {
    const auto value = std::to_integer<std::uint8_t>(mark);
    if ( value != LITTLE_ENDIAN_MARK && value != BIG_ENDIAN_MARK ) {
      malformed("byte order mark " + std::to_string(value) + " is neither 0 (big-endian) nor 1 (little-endian)");
    }

    _little = value == LITTLE_ENDIAN_MARK;
  }

  [[nodiscard]] std::byte byte() { return take(1).front(); }

  [[nodiscard]] std::uint32_t word() { return static_cast<std::uint32_t>(unsigned_of(take(WORD_BYTES))); }

  [[nodiscard]] double real() { return std::bit_cast<double>(unsigned_of(take(REAL_BYTES))); }

  /** @return A count of items of `item_bytes` each, checked to fit in what is left, so a corrupt count can't allocate gigabytes. */
  [[nodiscard]] std::size_t count(std::size_t item_bytes) {
    const std::size_t items = word();
    if ( items > remaining() / item_bytes ) {
      malformed("a count of " + std::to_string(items) + " is more than the " + std::to_string(remaining()) + " bytes left could hold");
    }

    return items;
  }

 private:
  std::span<const std::byte> _bytes;
  std::size_t                _at = 0;
  bool                       _little = true;

  [[nodiscard]] std::span<const std::byte> take(std::size_t size) {
    if ( size > remaining() ) {
      malformed("the geometry ends early");
    }

    const std::span<const std::byte> taken = _bytes.subspan(_at, size);
    _at += size;

    return taken;
  }

  [[nodiscard]] std::uint64_t unsigned_of(std::span<const std::byte> bytes) const {
    std::uint64_t value = 0;
    std::size_t   shift = 0;

    for ( const std::byte part : bytes ) {
      const auto digit = std::to_integer<std::uint64_t>(part);
      if ( _little ) {
        value |= digit << shift;
        shift += BITS_PER_BYTE;
      } else {
        value = (value << BITS_PER_BYTE) | digit;
      }
    }

    return value;
  }

 public:
  Reader(const Reader&) = delete;
  Reader(Reader&&) = delete;
  Reader& operator=(const Reader&) = delete;
  Reader& operator=(Reader&&) = delete;
  ~Reader() = default;
};

/** @brief Reads the geometry's header (byte order, type, SRID) and checks it is a 2D `geometry_t` in WGS 84. */
template <Geometry geometry_t>
void read_header(Reader& reader) {
  reader.set_byte_order(reader.byte());

  const std::uint32_t type = reader.word();
  if ( (type & (Z_FLAG | M_FLAG)) != 0 || (type & KIND_MASK) > LAST_2D_KIND ) {
    malformed("only two-dimensional geometry is read, not one with Z or M values");
  }

  if ( (type & KIND_MASK) != static_cast<std::uint32_t>(kind_of<geometry_t>()) ) {
    malformed("expected a " + std::string(type_name<geometry_t>().view()) + ", found geometry type " + std::to_string(type & KIND_MASK));
  }

  if ( (type & SRID_FLAG) == 0 ) {
    malformed("the geometry names no SRID, so its coordinates could be in any system (expected " + std::to_string(WGS84_SRID) + ")");
  }

  const auto srid = static_cast<std::int32_t>(reader.word());
  if ( srid != WGS84_SRID ) {
    malformed("the geometry has SRID " + std::to_string(srid) + ", not " + std::to_string(WGS84_SRID));
  }
}

[[nodiscard]] Point read_point(Reader& reader) {
  const double lon = reader.real();
  const double lat = reader.real();

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

// ---- Writing: little-endian, appended to `out` ----------------------------------------------------------------------

template <std::size_t size>
void put(std::vector<std::byte>& out, std::uint64_t value) {
  for ( std::size_t i = 0; i < size; ++i ) {
    out.push_back(static_cast<std::byte>((value >> (i * BITS_PER_BYTE)) & BYTE_MASK));
  }
}

void put_word(std::vector<std::byte>& out, std::uint32_t value) { put<WORD_BYTES>(out, value); }

/** @brief A count of parts, as WKB writes it: a 32-bit word. */
void put_count(std::vector<std::byte>& out, std::size_t size) {
  if ( size > UINT32_MAX ) {
    throw std::invalid_argument("wkb: a geometry has more parts than WKB can count");
  }

  put_word(out, static_cast<std::uint32_t>(size));
}

void put_point(std::vector<std::byte>& out, const Point& position) {
  put<REAL_BYTES>(out, std::bit_cast<std::uint64_t>(position.x()));
  put<REAL_BYTES>(out, std::bit_cast<std::uint64_t>(position.y()));
}

void put_points(std::vector<std::byte>& out, const Points& positions) {
  put_count(out, positions.size());
  for ( const Point& position : positions ) {
    put_point(out, position);
  }
}

template <Geometry geometry_t>
void put_body(std::vector<std::byte>& out, const geometry_t& geometry) {
  if constexpr ( std::same_as<geometry_t, Point> ) {
    put_point(out, geometry);

  } else if constexpr ( std::same_as<geometry_t, LineString> ) {
    put_points(out, geometry);

  } else {
    if ( geometry.outer().empty() && geometry.inners().empty() ) {
      put_count(out, 0);  // no rings, as PostGIS writes POLYGON EMPTY
      return;
    }

    put_count(out, geometry.inners().size() + 1);
    put_points(out, geometry.outer());
    for ( const auto& inner : geometry.inners() ) {
      put_points(out, inner);
    }
  }
}

}  // namespace

template <Geometry geometry_t>
std::vector<std::byte> write(const geometry_t& geometry) {
  std::vector<std::byte> out;
  out.push_back(std::byte{LITTLE_ENDIAN_MARK});
  put_word(out, static_cast<std::uint32_t>(kind_of<geometry_t>()) | SRID_FLAG);
  put_word(out, static_cast<std::uint32_t>(WGS84_SRID));
  put_body(out, geometry);

  return out;
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
