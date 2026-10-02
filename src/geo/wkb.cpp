#include "miniverse/geo/wkb.hpp"

#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "miniverse/geo/types.hpp"

namespace miniverse::geo::wkb {

namespace {

// The geometry type numbers of WKB, as the low bits of the type word.
enum class Kind : std::uint8_t {
  Point = 1,
  LineString = 2,
  Polygon = 3,
  MultiPolygon = 6,
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

[[nodiscard]] std::string_view kind_name(Kind kind) {
  switch ( kind ) {
    case Kind::Point:
      return "Point";
    case Kind::LineString:
      return "LineString";
    case Kind::Polygon:
      return "Polygon";
    case Kind::MultiPolygon:
      return "MultiPolygon";
  }
  return "geometry";
}

template <Geometry geometry_t>
[[nodiscard]] constexpr Kind kind_of() {
  if constexpr ( std::same_as<geometry_t, Point> ) {
    return Kind::Point;
  } else if constexpr ( std::same_as<geometry_t, LineString> ) {
    return Kind::LineString;
  } else if constexpr ( std::same_as<geometry_t, Polygon> ) {
    return Kind::Polygon;
  } else {
    return Kind::MultiPolygon;
  }
}

[[noreturn]] void malformed(const std::string& what) { throw std::invalid_argument("wkb: " + what); }

/** @brief Reads WKB's numbers from a buffer, in the byte order the current geometry's header names. */
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

/** @brief Reads one geometry's header (byte order, type, SRID) and checks it is a 2D `expected` in `srid` or none. */
void read_header(Reader& reader, Kind expected, std::int32_t srid) {
  reader.set_byte_order(reader.byte());

  const std::uint32_t type = reader.word();
  if ( (type & (Z_FLAG | M_FLAG)) != 0 || (type & KIND_MASK) > LAST_2D_KIND ) {
    malformed("only two-dimensional geometry is read, not one with Z or M values");
  }

  if ( (type & SRID_FLAG) != 0 ) {
    const auto found = static_cast<std::int32_t>(reader.word());
    if ( found != srid ) {
      malformed("the geometry has SRID " + std::to_string(found) + ", not " + std::to_string(srid));
    }
  }

  if ( (type & KIND_MASK) != static_cast<std::uint32_t>(expected) ) {
    malformed("expected a " + std::string(kind_name(expected)) + ", found geometry type " + std::to_string(type & KIND_MASK));
  }
}

[[nodiscard]] Point read_point(Reader& reader) {
  const double lon = reader.real();
  const double lat = reader.real();
  return {lon, lat};
}

template <typename points_t>
void read_points(Reader& reader, points_t& points) {
  const std::size_t size = reader.count(POINT_BYTES);
  points.reserve(size);
  for ( std::size_t i = 0; i < size; ++i ) {
    points.push_back(read_point(reader));
  }
}

[[nodiscard]] Polygon read_polygon(Reader& reader) {
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

template <Geometry geometry_t>
[[nodiscard]] geometry_t read_body(Reader& reader, std::int32_t srid) {
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
    return read_polygon(reader);

  } else {
    constexpr std::size_t SMALLEST_POLYGON = 1 + WORD_BYTES + WORD_BYTES;  // byte order, type and ring count
    const std::size_t     size = reader.count(SMALLEST_POLYGON);
    MultiPolygon          polygons;
    polygons.reserve(size);
    for ( std::size_t i = 0; i < size; ++i ) {
      read_header(reader, Kind::Polygon, srid);  // each part is a whole geometry, with its own byte order
      polygons.push_back(read_polygon(reader));
    }
    return polygons;
  }
}

/** @brief Writes little-endian WKB. */
class Writer {
 public:
  Writer() = default;

  void header(Kind kind, std::optional<std::int32_t> srid) {
    byte(LITTLE_ENDIAN_MARK);
    word(static_cast<std::uint32_t>(kind) | (srid ? SRID_FLAG : 0U));
    if ( srid ) {
      word(static_cast<std::uint32_t>(*srid));
    }
  }

  void point(const Point& position) {
    real(position.x());
    real(position.y());
  }

  /** @brief A count of parts, as WKB writes it: a 32-bit word. */
  void count(std::size_t size) {
    if ( size > UINT32_MAX ) {
      throw std::invalid_argument("wkb: a geometry has more parts than WKB can count");
    }
    word(static_cast<std::uint32_t>(size));
  }

  template <typename points_t>
  void points(const points_t& positions) {
    count(positions.size());
    for ( const Point& position : positions ) {
      point(position);
    }
  }

  void polygon(const Polygon& area) {
    count(area.inners().size() + 1);
    points(area.outer());
    for ( const auto& inner : area.inners() ) {
      points(inner);
    }
  }

  void word(std::uint32_t value) { put<WORD_BYTES>(value); }

  [[nodiscard]] std::vector<std::byte> take() && { return std::move(_bytes); }

 private:
  std::vector<std::byte> _bytes;

  void byte(std::uint8_t value) { _bytes.push_back(std::byte{value}); }
  void real(double value) { put<REAL_BYTES>(std::bit_cast<std::uint64_t>(value)); }

  template <std::size_t size>
  void put(std::uint64_t value) {
    for ( std::size_t i = 0; i < size; ++i ) {
      _bytes.push_back(static_cast<std::byte>((value >> (i * BITS_PER_BYTE)) & BYTE_MASK));
    }
  }

 public:
  Writer(const Writer&) = delete;
  Writer(Writer&&) = delete;
  Writer& operator=(const Writer&) = delete;
  Writer& operator=(Writer&&) = delete;
  ~Writer() = default;
};

}  // namespace

std::vector<std::byte> write(const Point& point, std::int32_t srid) {
  Writer writer;
  writer.header(Kind::Point, srid);
  writer.point(point);
  return std::move(writer).take();
}

std::vector<std::byte> write(const LineString& line, std::int32_t srid) {
  Writer writer;
  writer.header(Kind::LineString, srid);
  writer.points(line);
  return std::move(writer).take();
}

std::vector<std::byte> write(const Polygon& polygon, std::int32_t srid) {
  Writer writer;
  writer.header(Kind::Polygon, srid);
  writer.polygon(polygon);
  return std::move(writer).take();
}

std::vector<std::byte> write(const MultiPolygon& polygons, std::int32_t srid) {
  Writer writer;
  writer.header(Kind::MultiPolygon, srid);
  writer.count(polygons.size());
  for ( const Polygon& polygon : polygons ) {
    writer.header(Kind::Polygon, std::nullopt);  // PostGIS writes the SRID once, on the collection
    writer.polygon(polygon);
  }
  return std::move(writer).take();
}

template <Geometry geometry_t>
geometry_t read(std::span<const std::byte> bytes, std::int32_t srid) {
  Reader reader(bytes);
  read_header(reader, kind_of<geometry_t>(), srid);
  auto geometry = read_body<geometry_t>(reader, srid);

  if ( ! reader.at_end() ) {
    malformed(std::to_string(reader.remaining()) + " bytes are left after the " + std::string(kind_name(kind_of<geometry_t>())));
  }
  return geometry;
}

template Point        read<Point>(std::span<const std::byte> bytes, std::int32_t srid);
template LineString   read<LineString>(std::span<const std::byte> bytes, std::int32_t srid);
template Polygon      read<Polygon>(std::span<const std::byte> bytes, std::int32_t srid);
template MultiPolygon read<MultiPolygon>(std::span<const std::byte> bytes, std::int32_t srid);

}  // namespace miniverse::geo::wkb
