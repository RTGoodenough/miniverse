// EWKB, read and written, against what PostGIS 3.5 itself writes: each hex string below is `ST_AsEWKB(...)` (or `ST_AsBinary`)
// of the geometry named beside it.

#include <catch2/catch_test_macros.hpp>

#include <boost/geometry/io/wkt/read.hpp>
#include <boost/geometry/io/wkt/write.hpp>

#include <cstddef>
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
// SRID=4326;MULTILINESTRING((1 2,3.5 -4),(0 0,1 1,2 0)), little-endian and big-endian: its members name no SRID
constexpr std::string_view LINES =
    "0105000020e610000002000000010200000002000000000000000000f03f00000000000000400000000000000c4000000000000010c0010200000003000000000000"
    "00000000000000000000000000000000000000f03f000000000000f03f00000000000000400000000000000000";
constexpr std::string_view LINES_BIG_ENDIAN =
    "0020000005000010e6000000020000000002000000023ff00000000000004000000000000000400c000000000000c010000000000000000000000200000003000000"
    "000000000000000000000000003ff00000000000003ff000000000000040000000000000000000000000000000";
// SRID=4326;MULTILINESTRING EMPTY
constexpr std::string_view LINES_EMPTY = "0105000020e610000000000000";
// SRID=4326;MULTIPOLYGON(((0 0,4 0,4 4,0 4,0 0),(1 1,1 2,2 2,2 1,1 1)),((10 10,11 10,11 11,10 10))), little-endian and big-endian
constexpr std::string_view POLYGONS =
    "0106000020e610000002000000010300000002000000050000000000000000000000000000000000000000000000000010400000000000000000000000000000"
    "10400000000000001040000000000000000000000000000010400000000000000000000000000000000005000000000000000000f03f000000000000f03f0000"
    "00000000f03f0000000000000040000000000000004000000000000000400000000000000040000000000000f03f000000000000f03f000000000000f03f0103"
    "00000001000000040000000000000000002440000000000000244000000000000026400000000000002440000000000000264000000000000026400000000000"
    "0024400000000000002440";
constexpr std::string_view POLYGONS_BIG_ENDIAN =
    "0020000006000010e600000002000000000300000002000000050000000000000000000000000000000040100000000000000000000000000000401000000000"
    "000040100000000000000000000000000000401000000000000000000000000000000000000000000000000000053ff00000000000003ff00000000000003ff0"
    "00000000000040000000000000004000000000000000400000000000000040000000000000003ff00000000000003ff00000000000003ff00000000000000000"
    "00000300000001000000044024000000000000402400000000000040260000000000004024000000000000402600000000000040260000000000004024000000"
    "0000004024000000000000";
// SRID=4326;MULTIPOLYGON EMPTY, and SRID=4326;MULTIPOLYGON(EMPTY): no polygons, and one polygon without rings
constexpr std::string_view POLYGONS_EMPTY = "0106000020e610000000000000";
constexpr std::string_view POLYGONS_OF_EMPTY = "0106000020e610000001000000010300000000000000";
// SRID=4326;MULTILINESTRING Z((1 2 3,4 5 6)), MULTILINESTRING((1 2,3.5 -4)) as plain WKB (no SRID), the same in SRID 3857,
// SRID=4326;MULTIPOINT((1 2))
constexpr std::string_view LINES_Z =
    "01050000a0e610000001000000010200008002000000000000000000f03f00000000000000400000000000000840000000000000104000000000000014400000"
    "000000001840";
constexpr std::string_view LINES_PLAIN = "010500000001000000010200000002000000000000000000f03f00000000000000400000000000000c4000000000000010c0";
constexpr std::string_view LINES_3857 = "0105000020110f000001000000010200000002000000000000000000f03f00000000000000400000000000000c4000000000000010c0";
constexpr std::string_view POINTS = "0104000020e6100000010000000101000000000000000000f03f0000000000000040";
// The start of a SRID=4326 multilinestring of one member, and of two, for members made by hand below.
constexpr std::string_view ONE_LINE_OF = "0105000020e610000001000000";
constexpr std::string_view TWO_LINES_OF = "0105000020e610000002000000";
// What follows a linestring's byte order, type and SRID (9 bytes, 18 hex characters): its count and points.
constexpr std::size_t LINE_HEADER_HEX = 18;

constexpr std::string_view LINE_WKT = "LINESTRING(1 2,3.5 -4)";
constexpr std::string_view POLYGON_WITH_HOLE_WKT = "POLYGON((0 0,4 0,4 4,0 4,0 0),(1 1,1 2,2 2,2 1,1 1))";
constexpr std::string_view LINES_WKT = "MULTILINESTRING((1 2,3.5 -4),(0 0,1 1,2 0))";
constexpr std::string_view POLYGONS_WKT = "MULTIPOLYGON(((0 0,4 0,4 4,0 4,0 0),(1 1,1 2,2 2,2 1,1 1)),((10 10,11 10,11 11,10 10)))";

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
  CHECK(format_hex(wkb::write(from_wkt<geo::MultiLineString>(LINES_WKT))) == LINES);
  CHECK(format_hex(wkb::write(geo::MultiLineString())) == LINES_EMPTY);
  CHECK(format_hex(wkb::write(from_wkt<geo::MultiPolygon>(POLYGONS_WKT))) == POLYGONS);
  CHECK(format_hex(wkb::write(geo::MultiPolygon())) == POLYGONS_EMPTY);
}

TEST_CASE("wkb: what PostGIS writes is read back", "[wkb]") {
  CHECK(bg::to_wkt(wkb::read<geo::Point>(parse_hex(POINT))) == "POINT(1.5 -2.25)");
  CHECK(bg::to_wkt(wkb::read<geo::LineString>(parse_hex(LINE))) == LINE_WKT);
  CHECK(bg::to_wkt(wkb::read<geo::LineString>(parse_hex(LINE_BIG_ENDIAN))) == LINE_WKT);
  CHECK(bg::to_wkt(wkb::read<geo::Polygon>(parse_hex(POLYGON_WITH_HOLE))) == POLYGON_WITH_HOLE_WKT);
  CHECK(wkb::read<geo::Polygon>(parse_hex(POLYGON_EMPTY)).outer().empty());
  CHECK(bg::to_wkt(wkb::read<geo::MultiLineString>(parse_hex(LINES))) == LINES_WKT);
  CHECK(bg::to_wkt(wkb::read<geo::MultiLineString>(parse_hex(LINES_BIG_ENDIAN))) == LINES_WKT);
  CHECK(wkb::read<geo::MultiLineString>(parse_hex(LINES_EMPTY)).empty());
  CHECK(bg::to_wkt(wkb::read<geo::MultiPolygon>(parse_hex(POLYGONS))) == POLYGONS_WKT);
  CHECK(bg::to_wkt(wkb::read<geo::MultiPolygon>(parse_hex(POLYGONS_BIG_ENDIAN))) == POLYGONS_WKT);
  CHECK(wkb::read<geo::MultiPolygon>(parse_hex(POLYGONS_EMPTY)).empty());
}

TEST_CASE("wkb: a member of a multi-geometry need name no SRID, and one that does must name WGS 84", "[wkb]") {
  // LINE is a whole linestring, SRID 4326 and all, so here it is a member that names its SRID; then the same in SRID 3857.
  const std::string with_srid = std::string(ONE_LINE_OF) + std::string(LINE);
  const std::string other_srid = std::string(ONE_LINE_OF) + "0102000020110f0000" + std::string(LINE.substr(LINE_HEADER_HEX));

  CHECK(bg::to_wkt(wkb::read<geo::MultiLineString>(parse_hex(with_srid))) == "MULTILINESTRING((1 2,3.5 -4))");
  CHECK(refused<geo::MultiLineString>(other_srid));
}

TEST_CASE("wkb: an empty polygon in a multipolygon is written and read as PostGIS does", "[wkb]") {
  const geo::MultiPolygon of_empty{geo::Polygon()};

  const auto back = wkb::read<geo::MultiPolygon>(parse_hex(POLYGONS_OF_EMPTY));

  CHECK(format_hex(wkb::write(of_empty)) == POLYGONS_OF_EMPTY);
  REQUIRE(back.size() == 1);
  CHECK(back.front().outer().empty());
}

TEST_CASE("wkb: each member of a multi-geometry is read in its own byte order", "[wkb]") {
  // A little-endian whole, whose first member is big-endian and whose second is little-endian again.
  const std::string mixed = std::string(TWO_LINES_OF) + "0000000002" + std::string(LINE_BIG_ENDIAN.substr(LINE_HEADER_HEX)) + "0102000000" +
                            std::string(LINE.substr(LINE_HEADER_HEX));

  CHECK(bg::to_wkt(wkb::read<geo::MultiLineString>(parse_hex(mixed))) == "MULTILINESTRING((1 2,3.5 -4),(1 2,3.5 -4))");
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

TEST_CASE("wkb: a multi-geometry is refused as a single one is, and for a member of another type", "[wkb]") {
  CHECK(refused<geo::MultiLineString>(LINES_Z));
  CHECK(refused<geo::MultiLineString>(LINES_PLAIN));  // no SRID
  CHECK(refused<geo::MultiLineString>(LINES_3857));
  CHECK(refused<geo::MultiPolygon>(LINES));        // another type
  CHECK(refused<geo::MultiLineString>(POINTS));    // a multipoint
  CHECK(refused<geo::LineString>(LINES));          // a multi-geometry is not its member's type
  CHECK(refused<geo::MultiLineString>(std::string(ONE_LINE_OF) + "0101000000000000000000f03f0000000000000040"));  // a point as its member
  CHECK(refused<geo::MultiLineString>(std::string(ONE_LINE_OF) + "0102000080" + std::string(LINE.substr(LINE_HEADER_HEX))));  // only its member has Z
  CHECK(refused<geo::MultiLineString>(LINES.substr(0, LINES.size() - 2)));                                        // a byte short
  CHECK(refused<geo::MultiLineString>(std::string(LINES) + "00"));                                                // a byte left over
  CHECK(refused<geo::MultiLineString>("0105000020e6100000ffffffff"));                                             // a count larger than the bytes left
  CHECK(! refused<geo::MultiPolygon>(POLYGONS));
}
