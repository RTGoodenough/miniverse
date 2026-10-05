// The feature kind without a database: its statements, features as rows and back, and how a push's rows are batched.

#include <catch2/catch_test_macros.hpp>

#include <boost/geometry/io/wkt/read.hpp>
#include <boost/geometry/io/wkt/write.hpp>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "schemacht/json/json.hpp"
#include "schemacht/schema/field.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;

namespace {

struct Buildings : miniverse::FeatureLayer<geo::MultiPolygon> {};
struct Paths : miniverse::FeatureLayer<geo::LineString> {};

using Building = miniverse::Feature<geo::MultiPolygon>;
using Path = miniverse::Feature<geo::LineString>;

/** @return A path of `points` points along the equator, with the id `id`. */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- an id and a count of points, in the order every call reads
[[nodiscard]] Path path(std::int64_t id, std::size_t points) {
  Path made;
  made.id = id;

  made.geometry.reserve(points);
  for ( std::size_t i = 0; i < points; ++i ) {
    made.geometry.emplace_back(static_cast<double>(i) * 1e-6, 0.0);
  }

  return made;
}

/** @return The rows of `batches`, in order: for checks that don't care about the batching. */
template <typename row_t>
[[nodiscard]] std::vector<row_t> rows(std::vector<std::vector<row_t>> batches) {
  std::vector<row_t> all;

  for ( auto& batch : batches ) {
    all.insert(all.end(), std::move_iterator(batch.begin()), std::move_iterator(batch.end()));
  }

  return all;
}

}  // namespace

TEST_CASE("feature layer: its statements are aimed at its table, in its geometry type", "[feature]") {
  const miniverse::Layer<Buildings> layer("osm_buildings");

  const std::string load = std::string(
      Buildings::load_statement_type::bind(bg::from_wkt<geo::Polygon>("POLYGON((0 0,1 0,1 1,0 0))")).on(layer.table()).sql()
  );

  CHECK(
      load ==
      R"(SELECT "feature_id", "geom", "tags" FROM "osm_buildings" WHERE ST_Intersects("geom", $1::geometry(Polygon,4326)) ORDER BY "feature_id")"
  );
  CHECK(Buildings::setup_sql(layer.table().name()) == std::vector<std::string>{R"(CREATE INDEX ON "osm_buildings" USING gist (geom))"});
}

TEST_CASE("feature layer: features become rows and back", "[feature]") {
  miniverse::Features<geo::MultiPolygon> buildings;
  buildings.push_back(
      Building{
          .id = 7,
          .geometry = bg::from_wkt<geo::MultiPolygon>("MULTIPOLYGON(((0 0,1 0,1 1,0 0)))"),
          .tags = schemacht::json::Json(R"({"building": "yes"})"),
      }
  );
  buildings.push_back(
      Building{.id = 9, .geometry = bg::from_wkt<geo::MultiPolygon>("MULTIPOLYGON(((2 2,3 2,3 3,2 2)),((5 5,6 5,6 6,5 5)))"), .tags = schemacht::json::Json("{}")}
  );

  const miniverse::Features<geo::MultiPolygon> back = Buildings::from_rows(rows(Buildings::to_rows(miniverse::Features<geo::MultiPolygon>(buildings))), {});

  REQUIRE(back.size() == buildings.size());
  for ( std::size_t i = 0; i < buildings.size(); ++i ) {
    CHECK(back.at(i).id == buildings.at(i).id);
    CHECK(bg::to_wkt(back.at(i).geometry) == bg::to_wkt(buildings.at(i).geometry));
    CHECK(back.at(i).tags == buildings.at(i).tags);
  }
}

TEST_CASE("feature layer: features are written in batches of FEATURES_PER_STATEMENT, in order", "[feature]") {
  miniverse::Features<geo::LineString> paths;
  for ( std::size_t i = 0; i < Paths::FEATURES_PER_STATEMENT + 1; ++i ) {
    paths.push_back(path(static_cast<std::int64_t>(i), 2));
  }

  const auto batches = Paths::to_rows(std::move(paths));

  REQUIRE(batches.size() == 2);
  CHECK(batches.front().size() == Paths::FEATURES_PER_STATEMENT);
  CHECK(batches.back().size() == 1);
  CHECK(schemacht::schema::get<"feature_id">(batches.back().front()) == static_cast<std::int64_t>(Paths::FEATURES_PER_STATEMENT));
  CHECK(Paths::to_rows({}).empty());
}

TEST_CASE("feature layer: a batch ends before its geometries pass POINTS_PER_STATEMENT, and a larger feature is a batch alone", "[feature]") {
  constexpr std::size_t HALF = Paths::POINTS_PER_STATEMENT / 2;

  // Two halves fill a statement exactly; two points more start the next. Then one feature larger than a statement.
  miniverse::Features<geo::LineString> paths;
  paths.push_back(path(1, HALF));
  paths.push_back(path(2, HALF));
  paths.push_back(path(3, 2));
  paths.push_back(path(4, Paths::POINTS_PER_STATEMENT + 1));
  paths.push_back(path(5, 2));

  const auto batches = Paths::to_rows(std::move(paths));

  REQUIRE(batches.size() == 4);
  CHECK(batches.at(0).size() == 2);  // 1 and 2
  CHECK(batches.at(1).size() == 1);  // 3: it would pass the limit with them
  CHECK(batches.at(2).size() == 1);  // 4, alone
  CHECK(batches.at(3).size() == 1);  // 5
  CHECK(schemacht::schema::get<"feature_id">(batches.at(2).front()) == 4);
}

TEST_CASE("feature layer: a first feature larger than a statement is one batch, with no empty one before it", "[feature]") {
  miniverse::Features<geo::LineString> paths;
  paths.push_back(path(1, Paths::POINTS_PER_STATEMENT + 1));

  const auto batches = Paths::to_rows(std::move(paths));

  REQUIRE(batches.size() == 1);
  CHECK(batches.front().size() == 1);
}

TEST_CASE("feature layer: a feature's tags are an empty object unless given", "[feature]") {
  CHECK(miniverse::Feature<geo::Point>().tags.text() == "{}");
  CHECK(miniverse::Way().tags.text() == "{}");
}
