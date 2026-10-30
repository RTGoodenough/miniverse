// Layers bound to tables while the program runs: the names they accept, and the statements aimed at them. Nothing here
// connects to a database: a miniverse checks its layers before it opens any connection.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <boost/geometry/io/wkt/read.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "miniverse/world/datasets.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;

using miniverse::Layer;
using miniverse::Miniverse;

namespace {

using Roads = miniverse::Layer<miniverse::RoadModel, "roads">;
using Tracks = miniverse::Layer<miniverse::RoadModel, "tracks">;

}  // namespace

TEST_CASE("layer: a table name is checked when the layer is made", "[layer]") {
  CHECK_NOTHROW(Roads("osm_roads"));
  CHECK_NOTHROW(Roads("gis", "osm_roads"));
  CHECK_THROWS_AS(Roads("Roads"), std::invalid_argument);
  CHECK_THROWS_AS(Roads("osm roads; DROP TABLE x"), std::invalid_argument);
}

TEST_CASE("layer: a layer made from no name is in the table called as it is", "[layer]") {
  CHECK(Roads().table().name().quoted() == R"("roads")");
  CHECK(Roads().source() == R"("roads")");
  CHECK(Roads("gis", "osm_roads").source() == R"("gis"."osm_roads")");
  CHECK_THROWS_AS((Layer<miniverse::RoadModel, "Roads">()), std::invalid_argument);  // the name is not a table's
}

TEST_CASE("layer: two layers of a miniverse can't share a table", "[layer]") {
  CHECK_THROWS_WITH(
      (Miniverse<Roads, Tracks>("", Roads("roads"), Tracks("roads"))),
      Catch::Matchers::ContainsSubstring(R"(the layers roads and tracks of the miniverse are both the table "roads")")
  );
}

TEST_CASE("layer: a road layer's statements are aimed at its table", "[layer]") {
  const Roads layer("osm_roads");

  const std::string load =
      std::string(Roads::model_type::load_statement_type::bind(bg::from_wkt<geo::Polygon>("POLYGON((0 0,1 0,1 1,0 0))")).on(layer.table()).sql());

  CHECK(
      load ==
      R"(SELECT "way_id", "node_ids", "geom", "tags" FROM "osm_roads" WHERE ST_Intersects("geom", $1::geometry(Polygon,4326)) ORDER BY "way_id")"
  );
  CHECK(Roads::model_type::setup_sql(layer.table().name()) == std::vector<std::string>{R"(CREATE INDEX ON "osm_roads" USING gist (geom))"});
  CHECK(miniverse::detail::dataset_index_sql(layer.table().name()) == std::vector<std::string>{R"(CREATE INDEX ON "osm_roads" (dataset))"});
  CHECK(
      std::string(miniverse::detail::sweep_statement<Roads::model_type>(layer.table(), std::string("town"), std::int64_t{7}).sql()) ==
      R"(DELETE FROM "osm_roads" WHERE "dataset" = $1 AND "pushed" <> $2)"
  );
  CHECK(
      std::string(miniverse::detail::remove_statement<Roads::model_type>(layer.table(), std::string("town")).sql()) ==
      R"(DELETE FROM "osm_roads" WHERE "dataset" = $1)"
  );
  CHECK(miniverse::detail::lock_key(layer.table().name(), miniverse::Dataset{"town"}) == R"("osm_roads"/town)");
}
