// EWKB, read and written, against what PostGIS 3.5 itself writes: each hex string below is `ST_AsEWKB(...)` (or `ST_AsBinary`)
// of the geometry named beside it.

#include <catch2/catch_test_macros.hpp>

#include <boost/geometry/io/wkt/read.hpp>
#include <boost/geometry/io/wkt/write.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

#include "miniverse/geo/types.hpp"
#include "miniverse/geo/wkb.hpp"
#include "schemacht/util/hex.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;
namespace wkb = miniverse::geo::wkb;

using schemacht::util::format_hex;
using schemacht::util::parse_hex;

namespace {

// SRID=4326;POINT(1.5 -2.25)
constexpr std::string_view POINT = "0101000020e6100000000000000000f83f00000000000002c0";
// SRID=4326;LINESTRING(1 2,3.5 -4), little-endian (NDR) and big-endian (XDR), and as plain WKB (ST_AsBinary: no SRID, so refused)
constexpr std::string_view LINE = "0102000020e610000002000000000000000000f03f00000000000000400000000000000c4000000000000010c0";
constexpr std::string_view LINE_BIG_ENDIAN = "0020000002000010e6000000023ff00000000000004000000000000000400c000000000000c010000000000000";
constexpr std::string_view LINE_PLAIN = "010200000002000000000000000000f03f00000000000000400000000000000c4000000000000010c0";
// SRID=4326;POLYGON((0 0,4 0,4 4,0 4,0 0),(1 1,1 2,2 2,2 1,1 1))
constexpr std::string_view POLYGON_WITH_HOLE =
    "0103000020e61000000200000005000000000000000000000000000000000000000000000000001040000000000000000000000000000010400000000000001040"
    "000000000000000000000000000010400000000000000000000000000000000005000000000000000000f03f000000000000f03f000000000000f03f0000000000"
    "000040000000000000004000000000000000400000000000000040000000000000f03f000000000000f03f000000000000f03f";
// SRID=4326;POLYGON EMPTY
constexpr std::string_view POLYGON_EMPTY = "0103000020e610000000000000";
// SRID=4326;POINT(1 2 3), POINT Z(1 2 3) as ISO WKB, SRID=3857;POINT(1 2), SRID=4326;POINT EMPTY
constexpr std::string_view POINT_Z = "01010000a0e6100000000000000000f03f00000000000000400000000000000840";
constexpr std::string_view POINT_Z_ISO = "01e9030000000000000000f03f00000000000000400000000000000840";
constexpr std::string_view POINT_3857 = "0101000020110f0000000000000000f03f0000000000000040";
constexpr std::string_view POINT_EMPTY = "0101000020e6100000000000000000f87f000000000000f87f";

constexpr std::string_view LINE_WKT = "LINESTRING(1 2,3.5 -4)";
constexpr std::string_view POLYGON_WITH_HOLE_WKT = "POLYGON((0 0,4 0,4 4,0 4,0 0),(1 1,1 2,2 2,2 1,1 1))";

template <wkb::Geometry geometry_t>
[[nodiscard]] geometry_t from_wkt(std::string_view text) {
  return bg::from_wkt<geometry_t>(std::string(text));
}

template <wkb::Geometry geometry_t>
[[nodiscard]] bool refused(std::string_view text) {
  try {
    std::ignore = wkb::read<geometry_t>(parse_hex(text));
  } catch ( const std::invalid_argument& ) {
    return true;
  }

  return false;
}

}  // namespace

TEST_CASE("wkb: a geometry is written as PostGIS writes it", "[wkb]") {
  CHECK(format_hex(wkb::write(geo::Point(1.5, -2.25))) == POINT);
  CHECK(format_hex(wkb::write(from_wkt<geo::LineString>(LINE_WKT))) == LINE);
  CHECK(format_hex(wkb::write(from_wkt<geo::Polygon>(POLYGON_WITH_HOLE_WKT))) == POLYGON_WITH_HOLE);
  CHECK(format_hex(wkb::write(geo::Polygon())) == POLYGON_EMPTY);
}

TEST_CASE("wkb: what PostGIS writes is read back", "[wkb]") {
  CHECK(bg::to_wkt(wkb::read<geo::Point>(parse_hex(POINT))) == "POINT(1.5 -2.25)");
  CHECK(bg::to_wkt(wkb::read<geo::LineString>(parse_hex(LINE))) == LINE_WKT);
  CHECK(bg::to_wkt(wkb::read<geo::LineString>(parse_hex(LINE_BIG_ENDIAN))) == LINE_WKT);
  CHECK(bg::to_wkt(wkb::read<geo::Polygon>(parse_hex(POLYGON_WITH_HOLE))) == POLYGON_WITH_HOLE_WKT);
  CHECK(wkb::read<geo::Polygon>(parse_hex(POLYGON_EMPTY)).outer().empty());
}

TEST_CASE("wkb: what is not a 2D geometry of the expected type in WGS 84 is refused", "[wkb]") {
  CHECK(refused<geo::Point>(POINT_Z));
  CHECK(refused<geo::Point>(POINT_Z_ISO));
  CHECK(refused<geo::Point>(POINT_3857));
  CHECK(refused<geo::LineString>(LINE_PLAIN));  // no SRID
  CHECK(refused<geo::Point>(POINT_EMPTY));
  CHECK(refused<geo::Polygon>(LINE));                                // another type
  CHECK(refused<geo::LineString>(LINE.substr(0, LINE.size() - 2)));  // a byte short
  CHECK(refused<geo::LineString>(std::string(LINE) + "00"));         // a byte left over
  CHECK(refused<geo::LineString>("0302000020e6100000"));             // a byte order that is neither
  CHECK(refused<geo::LineString>("0102000020e6100000ffffffff"));     // a count larger than the bytes left
  CHECK(! refused<geo::Point>(POINT));
}
