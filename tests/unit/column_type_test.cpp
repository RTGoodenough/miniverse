// Geometry as a schemacht column type.

#include <catch2/catch_test_macros.hpp>

#include <boost/geometry/io/wkt/read.hpp>
#include <boost/geometry/io/wkt/write.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "schemacht/json/json.hpp"
#include "schemacht/postgres/field_codec.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/util/hex.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;

namespace {

// What PostgreSQL prints for SRID=4326;LINESTRING(1 2,3.5 -4) (`geom::text`): hex EWKB in capitals.
constexpr std::string_view LINE_TEXT = "0102000020E610000002000000000000000000F03F00000000000000400000000000000C4000000000000010C0";

}  // namespace

TEST_CASE("column type: a geometry's text form is hex EWKB", "[column_type]") {
  using Codec = schemacht::postgres::FieldCodec<geo::LineString>;

  const geo::LineString line = Codec::parse(LINE_TEXT);

  CHECK(bg::to_wkt(line) == "LINESTRING(1 2,3.5 -4)");
  CHECK(bg::to_wkt(Codec::parse(Codec::format(line))) == "LINESTRING(1 2,3.5 -4)");
  CHECK(bg::to_wkt(Codec::parse_binary(schemacht::util::parse_hex(LINE_TEXT))) == "LINESTRING(1 2,3.5 -4)");
}
