// Layers bound to tables while the program runs: the names they accept, and the statements aimed at them. Nothing here
// connects to a database: a miniverse checks its layers before it opens any connection.

#include <catch2/catch_test_macros.hpp>

#include <boost/geometry/io/wkt/read.hpp>

#include <stdexcept>
#include <string>
#include <vector>

#include "miniverse/miniverse.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;

using miniverse::Layer;
using miniverse::Miniverse;

namespace {

struct Roads : miniverse::RoadLayer {};
struct Tracks : miniverse::RoadLayer {};

}  // namespace

TEST_CASE("layer: a table name is checked when the layer is made", "[layer]") {
  CHECK_NOTHROW(Layer<Roads>("osm_roads"));
  CHECK_NOTHROW(Layer<Roads>("gis", "osm_roads"));
  CHECK_THROWS_AS(Layer<Roads>("Roads"), std::invalid_argument);
  CHECK_THROWS_AS(Layer<Roads>("osm roads; DROP TABLE x"), std::invalid_argument);
}

TEST_CASE("layer: two layers of a miniverse can't share a table", "[layer]") {
  CHECK_THROWS_AS((Miniverse<Roads, Tracks>("", Layer<Roads>("roads"), Layer<Tracks>("roads"))), std::invalid_argument);
}

TEST_CASE("layer: a road layer's statements are aimed at its table", "[layer]") {
  const Layer<Roads> layer("osm_roads");

  const std::string load =
      std::string(Roads::load_statement_type::bind(bg::from_wkt<geo::Polygon>("POLYGON((0 0,1 0,1 1,0 0))")).on(layer.table()).sql());

  CHECK(
      load ==
      R"(SELECT "way_id", "node_ids", "geom", "tags" FROM "osm_roads" WHERE ST_Intersects("geom", $1::geometry(Polygon,4326)) ORDER BY "way_id")"
  );
  CHECK(
      Roads::setup_sql(layer.table().name(), miniverse::NoSettings()) == std::vector<std::string>{R"(CREATE INDEX ON "osm_roads" USING gist (geom))"}
  );
}
