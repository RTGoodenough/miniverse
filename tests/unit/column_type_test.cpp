// Geometry as a schemacht column type, and a road layer's rows.

#include <catch2/catch_test_macros.hpp>

#include <boost/geometry/io/wkt/read.hpp>
#include <boost/geometry/io/wkt/write.hpp>

#include <cstddef>
#include <cstdint>
#include <iterator>
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

/** @return The rows of `batches`, in order: for checks that don't care about the batching. */
[[nodiscard]] std::vector<miniverse::RoadLayer::row_type> rows(std::vector<std::vector<miniverse::RoadLayer::row_type>> batches) {
  std::vector<miniverse::RoadLayer::row_type> all;

  for ( auto& batch : batches ) {
    all.insert(all.end(), std::move_iterator(batch.begin()), std::move_iterator(batch.end()));
  }

  return all;
}

}  // namespace

TEST_CASE("column type: a geometry's text form is hex EWKB", "[column_type]") {
  using Codec = schemacht::postgres::FieldCodec<geo::LineString>;

  const geo::LineString line = Codec::parse(LINE_TEXT);

  CHECK(bg::to_wkt(line) == "LINESTRING(1 2,3.5 -4)");
  CHECK(bg::to_wkt(Codec::parse(Codec::format(line))) == "LINESTRING(1 2,3.5 -4)");
  CHECK(bg::to_wkt(Codec::parse_binary(schemacht::util::parse_hex(LINE_TEXT))) == "LINESTRING(1 2,3.5 -4)");
}

TEST_CASE("road layer: ways become rows and back", "[road]") {
  miniverse::Ways ways;
  ways.push_back(
      {.id = 7,
       .node_ids = {1, 2},
       .coordinates = bg::from_wkt<geo::LineString>("LINESTRING(0 0,1 1)"),
       .tags = schemacht::json::Json(R"({"highway": "primary"})")}
  );
  ways.push_back(
      {.id = 9, .node_ids = {3, 4, 5}, .coordinates = bg::from_wkt<geo::LineString>("LINESTRING(2 2,3 3,4 4)"), .tags = schemacht::json::Json("{}")}
  );

  const miniverse::Ways back = miniverse::RoadLayer::from_rows(rows(miniverse::RoadLayer::to_rows(miniverse::Ways(ways))), {});

  REQUIRE(back.size() == ways.size());
  for ( std::size_t i = 0; i < ways.size(); ++i ) {
    CHECK(back.at(i).id == ways.at(i).id);
    CHECK(back.at(i).node_ids == ways.at(i).node_ids);
    CHECK(bg::to_wkt(back.at(i).coordinates) == bg::to_wkt(ways.at(i).coordinates));
    CHECK(back.at(i).tags == ways.at(i).tags);
  }
}

TEST_CASE("road layer: ways are written in batches of WAYS_PER_STATEMENT, in order", "[road]") {
  miniverse::Ways ways;
  for ( std::size_t i = 0; i < miniverse::RoadLayer::WAYS_PER_STATEMENT + 1; ++i ) {
    ways.push_back({.id = static_cast<std::int64_t>(i), .node_ids = {1, 2}, .coordinates = bg::from_wkt<geo::LineString>("LINESTRING(0 0,1 1)"), .tags = {}});
  }

  const auto batches = miniverse::RoadLayer::to_rows(std::move(ways));

  REQUIRE(batches.size() == 2);
  CHECK(batches.front().size() == miniverse::RoadLayer::WAYS_PER_STATEMENT);
  CHECK(batches.back().size() == 1);
  CHECK(schemacht::schema::get<"way_id">(batches.front().back()) == static_cast<std::int64_t>(miniverse::RoadLayer::WAYS_PER_STATEMENT - 1));
  CHECK(schemacht::schema::get<"way_id">(batches.back().front()) == static_cast<std::int64_t>(miniverse::RoadLayer::WAYS_PER_STATEMENT));
  CHECK(miniverse::RoadLayer::to_rows({}).empty());
}

TEST_CASE("road layer: a way needs one node id per point", "[road]") {
  miniverse::Ways ways;
  ways.push_back(
      {.id = 7, .node_ids = {1, 2, 3}, .coordinates = bg::from_wkt<geo::LineString>("LINESTRING(0 0,1 1)"), .tags = schemacht::json::Json("{}")}
  );

  CHECK_THROWS_AS(miniverse::RoadLayer::to_rows(std::move(ways)), std::invalid_argument);
}
