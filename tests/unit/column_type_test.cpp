// Geometry as a schemacht column type, and a road layer's rows.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "schemacht/json/json.hpp"
#include "schemacht/postgres/field_codec.hpp"
#include "support/geometry.hpp"

namespace geo = miniverse::geo;
namespace test = miniverse::test;

namespace {

// What PostgreSQL prints for SRID=4326;LINESTRING(1 2,3.5 -4) (`geom::text`): hex EWKB in capitals.
constexpr std::string_view LINE_TEXT = "0102000020E610000002000000000000000000F03F00000000000000400000000000000C4000000000000010C0";

}  // namespace

TEST_CASE("column type: a geometry's text form is hex EWKB", "[column_type]") {
  using Codec = schemacht::postgres::FieldCodec<geo::LineString>;

  const geo::LineString line = Codec::parse(LINE_TEXT);
  CHECK(test::coordinates(line) == test::Coordinates{{1, 2}, {3.5, -4}});
  CHECK(Codec::parse(Codec::format(line)).size() == 2);

  const std::vector<std::byte> binary = test::bytes(LINE_TEXT);
  CHECK(test::coordinates(Codec::parse_binary(binary)) == test::Coordinates{{1, 2}, {3.5, -4}});
}

TEST_CASE("road layer: ways become rows and back", "[road]") {
  const miniverse::Ways ways{
      miniverse::Way{
          .id = 7, .node_ids = {1, 2}, .coordinates = test::line({{0, 0}, {1, 1}}), .tags = schemacht::json::Json(R"({"highway": "primary"})")
      },
      miniverse::Way{.id = 9, .node_ids = {3, 4, 5}, .coordinates = test::line({{2, 2}, {3, 3}, {4, 4}}), .tags = schemacht::json::Json("{}")},
  };

  const miniverse::Ways back = miniverse::road::from_rows(miniverse::road::to_rows(ways));

  REQUIRE(back.size() == ways.size());
  for ( std::size_t i = 0; i < ways.size(); ++i ) {
    CHECK(back.at(i).id == ways.at(i).id);
    CHECK(back.at(i).node_ids == ways.at(i).node_ids);
    CHECK(test::coordinates(back.at(i).coordinates) == test::coordinates(ways.at(i).coordinates));
    CHECK(back.at(i).tags == ways.at(i).tags);
  }
}
