// What the layer kinds are at compile time: their statements' text, and that they are kinds.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

#include "miniverse/geo/operations.hpp"
#include "miniverse/miniverse.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/prepared.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/query/raw_statement.hpp"
#include "schemacht/query/sql_type.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/schema.hpp"

namespace {

namespace geo = miniverse::geo;
namespace q = schemacht::query;
namespace sch = schemacht::schema;

struct Roads : miniverse::RoadLayer<"osm_roads"> {};

// A kind written by hand, as a user would: places, as points, loaded by polygon. Its conversions are only declared, since
// the concept checks their signatures, not their bodies.
using PlacesSchema = sch::Schema<"places", sch::Field<std::int64_t, "place_id", sch::KeyRole::Primary>, sch::Field<geo::Point, "position">>;

constexpr auto PLACES_IN = q::select(q::On<PlacesSchema>::col<"position">().apply<geo::Intersects>(q::arg<0>()));

struct Places {
  using result_type = std::vector<geo::Point>;
  using schema_type = PlacesSchema;
  using load_statement_type = q::Prepared<PLACES_IN>;
  using setup_statement_type = q::RawStatement<"CREATE INDEX ON places USING gist (position)", q::RawArguments<>>;

  static std::vector<schema_type::row_type> to_rows(result_type places);
  static result_type                        from_rows(std::vector<load_statement_type::result_type> rows);
};

struct NotAKind {
  using result_type = int;
};

}  // namespace

static_assert(miniverse::LayerKind<Roads>);
static_assert(miniverse::LayerKind<Places>);
static_assert(! miniverse::LayerKind<NotAKind>);

static_assert(Roads::schema_type::TABLE_NAME == "osm_roads");
static_assert(
    Roads::load_statement_type::SQL ==
    R"(SELECT "way_id", "node_ids", "geom", "tags" FROM "osm_roads" WHERE ST_Intersects("geom", $1::geometry(Polygon,4326)) ORDER BY "way_id")"
);
static_assert(Roads::setup_statement_type::SQL == "CREATE INDEX ON osm_roads USING gist (geom)");

static_assert(schemacht::query::sql::type_name<miniverse::geo::Point>() == "geometry(Point,4326)");
static_assert(schemacht::query::sql::type_name<miniverse::geo::LineString>() == "geometry(LineString,4326)");
static_assert(schemacht::query::sql::type_name<miniverse::geo::Polygon>() == "geometry(Polygon,4326)");

TEST_CASE("layer kinds are checked at compile time", "[compile]") { SUCCEED("the static_asserts above compiled"); }
