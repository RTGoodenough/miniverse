// Geometry as a schemacht column type, and a road layer's rows.

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
#include "support/checks.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;

namespace {

// What PostgreSQL prints for SRID=4326;LINESTRING(1 2,3.5 -4) (`geom::text`): hex EWKB in capitals.
constexpr std::string_view LINE_TEXT = "0102000020E610000002000000000000000000F03F00000000000000400000000000000C4000000000000010C0";

using test::rows;

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

  // Written: the dataset and the stamp with each, and no id.
  const auto written = rows(miniverse::RoadLayer::to_rows(miniverse::Ways(ways), miniverse::Dataset{"town"}, 42));
  REQUIRE(written.size() == 2);
  CHECK(schemacht::schema::get<"dataset">(written.front()) == "town");
  CHECK(schemacht::schema::get<"pushed">(written.front()) == 42);
  CHECK(schemacht::schema::get<"node_ids">(written.back()) == std::vector<std::int64_t>{3, 4, 5});

  // Read back, as a load has them: with the ids the database gave the rows.
  std::vector<miniverse::RoadLayer::load_statement_type::result_type> stored;
  for ( std::size_t i = 0; i < written.size(); ++i ) {
    stored.emplace_back(
        miniverse::road::WayId{static_cast<std::int64_t>(100 + i)}, miniverse::road::NodeIds{schemacht::schema::get<"node_ids">(written.at(i))},
        miniverse::road::Geom{schemacht::schema::get<"geom">(written.at(i))}, miniverse::road::Tags{schemacht::schema::get<"tags">(written.at(i))}
    );
  }
  const miniverse::Ways back = miniverse::RoadLayer::from_rows(std::move(stored), {});

  REQUIRE(back.size() == ways.size());
  for ( std::size_t i = 0; i < ways.size(); ++i ) {
    CHECK(back.at(i).id == static_cast<std::int64_t>(100 + i));
    CHECK(back.at(i).node_ids == ways.at(i).node_ids);
    CHECK(bg::to_wkt(back.at(i).coordinates) == bg::to_wkt(ways.at(i).coordinates));
    CHECK(back.at(i).tags == ways.at(i).tags);
  }
}

TEST_CASE("road layer: ways are written in batches of WAYS_PER_STATEMENT, in order", "[road]") {
  miniverse::Ways ways;
  for ( std::size_t i = 0; i < miniverse::RoadLayer::WAYS_PER_STATEMENT + 1; ++i ) {
    const auto n = static_cast<std::int64_t>(i);
    ways.push_back({.id = n, .node_ids = {n, n + 1}, .coordinates = bg::from_wkt<geo::LineString>("LINESTRING(0 0,1 1)"), .tags = {}});
  }

  const auto batches = miniverse::RoadLayer::to_rows(std::move(ways), miniverse::Dataset{"town"}, 42);

  REQUIRE(batches.size() == 2);
  CHECK(batches.front().size() == miniverse::RoadLayer::WAYS_PER_STATEMENT);
  CHECK(batches.back().size() == 1);
  CHECK(
      schemacht::schema::get<"node_ids">(batches.front().back()).front() == static_cast<std::int64_t>(miniverse::RoadLayer::WAYS_PER_STATEMENT - 1)
  );
  CHECK(schemacht::schema::get<"node_ids">(batches.back().front()).front() == static_cast<std::int64_t>(miniverse::RoadLayer::WAYS_PER_STATEMENT));
  CHECK(miniverse::RoadLayer::to_rows({}, miniverse::Dataset{"town"}, 42).empty());
}

TEST_CASE("road layer: a way needs one node id per point", "[road]") {
  miniverse::Ways ways;
  ways.push_back(
      {.id = 7, .node_ids = {1, 2, 3}, .coordinates = bg::from_wkt<geo::LineString>("LINESTRING(0 0,1 1)"), .tags = schemacht::json::Json("{}")}
  );

  CHECK_THROWS_AS(miniverse::RoadLayer::to_rows(std::move(ways), miniverse::Dataset{"town"}, 42), std::invalid_argument);
}
