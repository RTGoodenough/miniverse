// The feature kind without a database: its statements, features as rows of a dataset and back, and how a push's rows are batched.

#include <catch2/catch_test_macros.hpp>

#include <boost/geometry/io/wkt/read.hpp>
#include <boost/geometry/io/wkt/write.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "schemacht/json/json.hpp"
#include "schemacht/schema/field.hpp"
#include "support/checks.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;

namespace {

struct Buildings : miniverse::FeatureLayer<geo::MultiPolygon> {};
struct Paths : miniverse::FeatureLayer<geo::LineString> {};

using Building = miniverse::Feature<geo::MultiPolygon>;
using Path = miniverse::Feature<geo::LineString>;

const miniverse::Dataset DATASET{"survey"};
constexpr std::int64_t   STAMP = 42;

/** @return A path of `points` points along the equator, numbered `id` in its tags (its id is not written: the database numbers rows). */
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- an id and a count of points, in the order every call reads
[[nodiscard]] Path path(std::int64_t id, std::size_t points) {
  Path made;
  made.id = id;
  made.tags = schemacht::json::Json("{\"n\": " + std::to_string(id) + "}");

  made.geometry.reserve(points);
  for ( std::size_t i = 0; i < points; ++i ) {
    made.geometry.emplace_back(static_cast<double>(i) * 1e-6, 0.0);
  }

  return made;
}

using test::rows;

}  // namespace

TEST_CASE("feature layer: its statements are aimed at its table, in its geometry type", "[feature]") {
  const miniverse::Layer<Buildings> layer("osm_buildings");

  const std::string load =
      std::string(Buildings::load_statement_type::bind(bg::from_wkt<geo::Polygon>("POLYGON((0 0,1 0,1 1,0 0))")).on(layer.table()).sql());

  CHECK(
      load ==
      R"(SELECT "feature_id", "geom", "tags" FROM "osm_buildings" WHERE ST_Intersects("geom", $1::geometry(Polygon,4326)) ORDER BY "feature_id")"
  );
  CHECK(
      Buildings::setup_sql(layer.table().name()) ==
      std::vector<std::string>{R"(CREATE INDEX ON "osm_buildings" USING gist (geom))", R"(CREATE INDEX ON "osm_buildings" (dataset))"}
  );
  CHECK(
      std::string(Buildings::sweep_statement_type::bind(DATASET.name, STAMP).on(layer.table()).sql()) ==
      R"(DELETE FROM "osm_buildings" WHERE "dataset" = $1 AND "pushed" <> $2)"
  );
  CHECK(
      std::string(Buildings::remove_statement_type::bind(DATASET.name).on(layer.table()).sql()) ==
      R"(DELETE FROM "osm_buildings" WHERE "dataset" = $1)"
  );
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
      Building{
          .id = 9,
          .geometry = bg::from_wkt<geo::MultiPolygon>("MULTIPOLYGON(((2 2,3 2,3 3,2 2)),((5 5,6 5,6 6,5 5)))"),
          .tags = schemacht::json::Json("{}")
      }
  );

  // Written: the dataset and the stamp with each, and no id.
  const auto written = rows(Buildings::to_rows(miniverse::Features<geo::MultiPolygon>(buildings), DATASET, STAMP));
  REQUIRE(written.size() == 2);
  CHECK(schemacht::schema::get<"dataset">(written.front()) == "survey");
  CHECK(schemacht::schema::get<"pushed">(written.front()) == STAMP);
  CHECK(bg::to_wkt(schemacht::schema::get<"geom">(written.back())) == bg::to_wkt(buildings.back().geometry));
  CHECK(schemacht::schema::get<"tags">(written.back()) == buildings.back().tags);

  // Read back, as a load has them: with the ids the database gave the rows.
  std::vector<Buildings::load_statement_type::result_type> stored;
  for ( std::size_t i = 0; i < written.size(); ++i ) {
    stored.emplace_back(
        miniverse::feature::Id{static_cast<std::int64_t>(100 + i)},
        miniverse::feature::Geom<geo::MultiPolygon>{schemacht::schema::get<"geom">(written.at(i))},
        miniverse::feature::Tags{schemacht::schema::get<"tags">(written.at(i))}
    );
  }
  const miniverse::Features<geo::MultiPolygon> back = Buildings::from_rows(std::move(stored), {});

  REQUIRE(back.size() == buildings.size());
  for ( std::size_t i = 0; i < buildings.size(); ++i ) {
    CHECK(back.at(i).id == static_cast<std::int64_t>(100 + i));
    CHECK(bg::to_wkt(back.at(i).geometry) == bg::to_wkt(buildings.at(i).geometry));
    CHECK(back.at(i).tags == buildings.at(i).tags);
  }
}

TEST_CASE("feature layer: features are written in batches of FEATURES_PER_STATEMENT, in order", "[feature]") {
  miniverse::Features<geo::LineString> paths;
  for ( std::size_t i = 0; i < Paths::FEATURES_PER_STATEMENT + 1; ++i ) {
    paths.push_back(path(static_cast<std::int64_t>(i), 2));
  }

  const auto batches = Paths::to_rows(std::move(paths), DATASET, STAMP);

  REQUIRE(batches.size() == 2);
  CHECK(batches.front().size() == Paths::FEATURES_PER_STATEMENT);
  CHECK(batches.back().size() == 1);
  CHECK(schemacht::schema::get<"tags">(batches.back().front()).text() == "{\"n\": " + std::to_string(Paths::FEATURES_PER_STATEMENT) + "}");
  CHECK(Paths::to_rows({}, DATASET, STAMP).empty());
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

  const auto batches = Paths::to_rows(std::move(paths), DATASET, STAMP);

  REQUIRE(batches.size() == 4);
  CHECK(batches.at(0).size() == 2);  // 1 and 2
  CHECK(batches.at(1).size() == 1);  // 3: it would pass the limit with them
  CHECK(batches.at(2).size() == 1);  // 4, alone
  CHECK(batches.at(3).size() == 1);  // 5
  CHECK(schemacht::schema::get<"tags">(batches.at(2).front()).text() == R"({"n": 4})");
}

TEST_CASE("feature layer: a first feature larger than a statement is one batch, with no empty one before it", "[feature]") {
  miniverse::Features<geo::LineString> paths;
  paths.push_back(path(1, Paths::POINTS_PER_STATEMENT + 1));

  const auto batches = Paths::to_rows(std::move(paths), DATASET, STAMP);

  REQUIRE(batches.size() == 1);
  CHECK(batches.front().size() == 1);
}

TEST_CASE("feature layer: a feature's tags are an empty object unless given", "[feature]") {
  CHECK(miniverse::Feature<geo::Point>().tags.text() == "{}");
  CHECK(miniverse::Way().tags.text() == "{}");
}
