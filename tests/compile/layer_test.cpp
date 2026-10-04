// What is checked at compile time: which types are layer kinds, the kinds a miniverse deduces, and the geometry columns' types.

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "miniverse/geo/operations.hpp"
#include "miniverse/miniverse.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/prepared.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/query/sql_type.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/schema/table_name.hpp"

namespace {

namespace geo = miniverse::geo;
namespace q = schemacht::query;
namespace sch = schemacht::schema;

struct Roads : miniverse::RoadLayer {};

// A kind written by hand, as a user would: places, as points, loaded by polygon. Its conversions are only declared, since
// the concept checks their signatures, not their bodies.
using PlacesSchema = sch::Schema<"places", sch::Field<std::int64_t, "place_id", sch::KeyRole::Primary>, sch::Field<geo::Point, "position">>;

constexpr auto PLACES_IN = q::select(q::On<PlacesSchema>::col<"position">().apply<geo::Intersects>(q::arg<0>()));

struct Places {
  using result_type = std::vector<geo::Point>;
  using settings_type = miniverse::NoSettings;
  using schema_type = PlacesSchema;
  using load_statement_type = q::Prepared<PLACES_IN>;

  static std::vector<std::string>                                        setup_sql(const sch::TableName& table, miniverse::NoSettings settings);
  static std::vector<schema_type::row_type>                              to_rows(result_type places, miniverse::NoSettings settings);
  static schemacht::postgres::SchemaStatement<schema_type, std::tuple<>> write_statement(const std::vector<schema_type::row_type>& rows);
  static result_type from_rows(std::vector<load_statement_type::result_type> rows, const geo::Polygon& location);
};

// Places, but loaded by the roads' query, which is written against another schema.
struct PlacesByRoadQuery : Places {
  using load_statement_type = miniverse::RoadLayer::load_statement_type;

  static result_type from_rows(std::vector<load_statement_type::result_type> rows, const geo::Polygon& location);
};

// Places with settings, but no way to read them back.
struct PlacesWithUnreadSettings : Places {
  using settings_type = int;

  static std::vector<std::string>           setup_sql(const sch::TableName& table, int settings);
  static std::vector<schema_type::row_type> to_rows(result_type places, int settings);
};

struct Elevation : miniverse::ElevationLayer<std::int16_t> {};

struct NotAKind {
  using result_type = int;
};

}  // namespace

static_assert(miniverse::LayerKind<Roads>);
static_assert(miniverse::LayerKind<Places>);
static_assert(miniverse::LayerKind<Elevation>);
static_assert(miniverse::HasSettings<Elevation>);
static_assert(! miniverse::HasSettings<Roads>);
static_assert(! miniverse::LayerKind<PlacesByRoadQuery>);
static_assert(! miniverse::LayerKind<PlacesWithUnreadSettings>);
static_assert(! miniverse::LayerKind<NotAKind>);

// A layer of a kind with settings is made with them; one of a kind without is made from its table alone.
static_assert(std::constructible_from<miniverse::Layer<Roads>, const char*>);
static_assert(std::constructible_from<miniverse::Layer<Elevation>, const char*, miniverse::geo::Grid<std::int16_t>>);
static_assert(! std::constructible_from<miniverse::Layer<Elevation>, const char*>);
static_assert(! std::constructible_from<miniverse::Layer<Elevation>, const char*, const char*>);

// A miniverse's kinds are deduced from its layers.
static_assert(std::same_as<decltype(miniverse::Miniverse(std::string(), miniverse::Layer<Roads>("roads"))), miniverse::Miniverse<Roads>>);
static_assert(std::same_as<
              decltype(miniverse::Miniverse(
                  std::string(), schemacht::postgres::Database::Options(), miniverse::Layer<Roads>("roads"), miniverse::Layer<Places>("places")
              )),
              miniverse::Miniverse<Roads, Places>>);

static_assert(schemacht::query::sql::type_name<miniverse::geo::Point>() == "geometry(Point,4326)");
static_assert(schemacht::query::sql::type_name<miniverse::geo::LineString>() == "geometry(LineString,4326)");
static_assert(schemacht::query::sql::type_name<miniverse::geo::Polygon>() == "geometry(Polygon,4326)");

TEST_CASE("layer kinds are checked at compile time", "[compile]") { SUCCEED("the static_asserts above compiled"); }
