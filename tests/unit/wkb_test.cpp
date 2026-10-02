// EWKB, read and written, against what PostGIS 3.5 itself writes: each hex string below is `ST_AsEWKB(...)` (or `ST_AsBinary`)
// of the geometry named beside it.

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "miniverse/geo/types.hpp"
#include "miniverse/geo/wkb.hpp"
#include "support/geometry.hpp"

namespace geo = miniverse::geo;
namespace wkb = miniverse::geo::wkb;
namespace test = miniverse::test;

namespace {

// SRID=4326;POINT(1.5 -2.25)
constexpr std::string_view POINT = "0101000020e6100000000000000000f83f00000000000002c0";
// SRID=4326;LINESTRING(1 2,3.5 -4), little-endian (NDR) and big-endian (XDR), and as plain WKB (ST_AsBinary: no SRID)
constexpr std::string_view LINE = "0102000020e610000002000000000000000000f03f00000000000000400000000000000c4000000000000010c0";
constexpr std::string_view LINE_BIG_ENDIAN = "0020000002000010e6000000023ff00000000000004000000000000000400c000000000000c010000000000000";
constexpr std::string_view LINE_PLAIN = "010200000002000000000000000000f03f00000000000000400000000000000c4000000000000010c0";
// SRID=4326;POLYGON((0 0,4 0,4 4,0 4,0 0),(1 1,1 2,2 2,2 1,1 1))
constexpr std::string_view POLYGON_WITH_HOLE =
    "0103000020e61000000200000005000000000000000000000000000000000000000000000000001040000000000000000000000000000010400000000000001040"
    "000000000000000000000000000010400000000000000000000000000000000005000000000000000000f03f000000000000f03f000000000000f03f0000000000"
    "000040000000000000004000000000000000400000000000000040000000000000f03f000000000000f03f000000000000f03f";
// SRID=4326;MULTIPOLYGON(((0 0,1 0,1 1,0 0)),((2 2,3 2,3 3,2 2)))
constexpr std::string_view MULTI_POLYGON =
    "0106000020e6100000020000000103000000010000000400000000000000000000000000000000000000000000000000f03f00000000000000000000000000"
    "00f03f000000000000f03f00000000000000000000000000000000010300000001000000040000000000000000000040000000000000004000000000000008"
    "4000000000000000400000000000000840000000000000084000000000000000400000000000000040";
// SRID=4326;POINT(1 2 3), POINT Z(1 2 3) as ISO WKB, SRID=3857;POINT(1 2), SRID=4326;POINT EMPTY
constexpr std::string_view POINT_Z = "01010000a0e6100000000000000000f03f00000000000000400000000000000840";
constexpr std::string_view POINT_Z_ISO = "01e9030000000000000000f03f00000000000000400000000000000840";
constexpr std::string_view POINT_3857 = "0101000020110f0000000000000000f03f0000000000000040";
constexpr std::string_view POINT_EMPTY = "0101000020e6100000000000000000f87f000000000000f87f";

using test::bytes;
using test::hex;

template <wkb::Geometry geometry_t>
[[nodiscard]] bool refused(std::string_view text) {
  try {
    std::ignore = wkb::read<geometry_t>(bytes(text));
  } catch ( const std::invalid_argument& ) {
    return true;
  }
  return false;
}

}  // namespace

TEST_CASE("wkb: a geometry is written as PostGIS writes it", "[wkb]") {
  CHECK(hex(wkb::write(geo::Point(1.5, -2.25))) == POINT);
  CHECK(hex(wkb::write(test::line({{1, 2}, {3.5, -4}}))) == LINE);

  geo::Polygon with_hole = test::rectangle(0, 0, 4, 4);
  with_hole.inners().push_back(test::points<geo::Polygon::ring_type>({{1, 1}, {1, 2}, {2, 2}, {2, 1}, {1, 1}}));
  CHECK(hex(wkb::write(with_hole)) == POLYGON_WITH_HOLE);

  const geo::MultiPolygon two{
      test::polygon({{0, 0}, {1, 0}, {1, 1}, {0, 0}}),
      test::polygon({{2, 2}, {3, 2}, {3, 3}, {2, 2}}),
  };
  CHECK(hex(wkb::write(two)) == MULTI_POLYGON);
}

TEST_CASE("wkb: what PostGIS writes is read back", "[wkb]") {
  const auto point = wkb::read<geo::Point>(bytes(POINT));
  CHECK(point.x() == 1.5);
  CHECK(point.y() == -2.25);

  const test::Coordinates line{{1, 2}, {3.5, -4}};
  CHECK(test::coordinates(wkb::read<geo::LineString>(bytes(LINE))) == line);
  CHECK(test::coordinates(wkb::read<geo::LineString>(bytes(LINE_BIG_ENDIAN))) == line);
  CHECK(test::coordinates(wkb::read<geo::LineString>(bytes(LINE_PLAIN))) == line);

  const std::vector<test::Coordinates> with_hole{
      {{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}},
      {{1, 1}, {1, 2}, {2, 2}, {2, 1}, {1, 1}},
  };
  CHECK(test::rings(wkb::read<geo::Polygon>(bytes(POLYGON_WITH_HOLE))) == with_hole);

  const auto two = wkb::read<geo::MultiPolygon>(bytes(MULTI_POLYGON));
  REQUIRE(two.size() == 2);
  CHECK(test::rings(two.front()) == std::vector<test::Coordinates>{{{0, 0}, {1, 0}, {1, 1}, {0, 0}}});
  CHECK(test::rings(two.back()) == std::vector<test::Coordinates>{{{2, 2}, {3, 2}, {3, 3}, {2, 2}}});
}

TEST_CASE("wkb: what is not a 2D geometry of the expected type and SRID is refused", "[wkb]") {
  CHECK(refused<geo::Point>(POINT_Z));
  CHECK(refused<geo::Point>(POINT_Z_ISO));
  CHECK(refused<geo::Point>(POINT_3857));
  CHECK(refused<geo::Point>(POINT_EMPTY));
  CHECK(refused<geo::Polygon>(LINE));                                // another type
  CHECK(refused<geo::LineString>(LINE.substr(0, LINE.size() - 2)));  // a byte short
  CHECK(refused<geo::LineString>(std::string(LINE) + "00"));         // a byte left over
  CHECK(refused<geo::LineString>("0302000020e6100000"));             // a byte order that is neither
  CHECK(refused<geo::LineString>("0102000020e6100000ffffffff"));     // a count larger than the bytes left
  CHECK(! refused<geo::Point>(POINT));

  // Another SRID is read when it is the one asked for.
  CHECK(wkb::read<geo::Point>(bytes(POINT_3857), 3857).y() == 2);
}
