// What the layer kinds are at compile time: their statements' text, and that they are kinds.

#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include "miniverse/miniverse.hpp"
#include "schemacht/query/sql_type.hpp"

namespace {

struct Roads : miniverse::RoadLayer<"osm_roads"> {};

struct NotAKind {
  using result_type = int;
};

}  // namespace

static_assert(miniverse::LayerKind<Roads>);
static_assert(miniverse::HasSetupStatement<Roads>);
static_assert(! miniverse::LayerKind<NotAKind>);

static_assert(Roads::schema_type::TABLE_NAME == "osm_roads");
static_assert(
    Roads::load_statement_type::SQL == "SELECT way_id, node_ids, geom, tags FROM osm_roads WHERE ST_Intersects(geom, $1::geometry) ORDER BY way_id"
);
static_assert(Roads::setup_statement_type::SQL == "CREATE INDEX IF NOT EXISTS osm_roads_geom_idx ON osm_roads USING gist (geom)");

static_assert(schemacht::query::sql::type_name<miniverse::geo::LineString>() == "geometry(LineString,4326)");
static_assert(schemacht::query::sql::type_name<miniverse::geo::Polygon>() == "geometry(Polygon,4326)");

TEST_CASE("layer kinds are checked at compile time", "[compile]") { SUCCEED("the static_asserts above compiled"); }
