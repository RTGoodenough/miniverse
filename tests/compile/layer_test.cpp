// What is checked at compile time: which types are layer kinds, the kinds a miniverse deduces, and the geometry columns' types.

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstdint>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/query/sql_type.hpp"
#include "schemacht/schema/table_name.hpp"
#include "support/places.hpp"

namespace {

namespace geo = miniverse::geo;
namespace sch = schemacht::schema;

using test::Places;

struct Roads : miniverse::RoadLayer {};

// A feature layer of each geometry type: each is one line.
struct Shops : miniverse::FeatureLayer<geo::Point> {};
struct Paths : miniverse::FeatureLayer<geo::LineString> {};
struct Parks : miniverse::FeatureLayer<geo::Polygon> {};
struct Rivers : miniverse::FeatureLayer<geo::MultiLineString> {};
struct Buildings : miniverse::FeatureLayer<geo::MultiPolygon> {};

// Places, but loaded by the roads' query, which is written against another schema.
struct PlacesByRoadQuery : Places {
  using load_statement_type = miniverse::RoadLayer::load_statement_type;

  static result_type from_rows(std::vector<load_statement_type::result_type> rows, const geo::Polygon& location);
};

// Places whose table is made with a setting, but with no way to read it back from the table.
struct PlacesWithUnreadSettings : Places {
  using settings_type = int;

  static std::vector<std::string>           setup_sql(const sch::TableName& table, int settings);
  static std::vector<std::vector<schema_type::row_type>> to_rows(result_type places, int settings);
};

// Places that name a chunk of their own, but don't make one: their streams would fall back to from_rows unnoticed.
struct PlacesWithUnmadeChunks : Places {
  using chunk_type = int;
};

struct Elevation : miniverse::RasterLayer<std::int16_t> {};

struct NotAKind {
  using result_type = int;
};

}  // namespace

static_assert(miniverse::LayerKind<Roads>);
static_assert(miniverse::LayerKind<Shops> && miniverse::LayerKind<Paths> && miniverse::LayerKind<Parks>);
static_assert(miniverse::LayerKind<Rivers> && miniverse::LayerKind<Buildings>);
static_assert(miniverse::HasNoSettings<Buildings>);
static_assert(std::same_as<Buildings::result_type, std::vector<miniverse::Feature<geo::MultiPolygon>>>);
static_assert(miniverse::LayerKind<Places>);
static_assert(miniverse::LayerKind<Elevation>);
static_assert(miniverse::HasSettings<Elevation>);
static_assert(! miniverse::HasSettings<Roads>);
static_assert(! miniverse::LayerKind<PlacesByRoadQuery>);
static_assert(! miniverse::LayerKind<PlacesWithUnreadSettings>);
static_assert(! miniverse::LayerKind<PlacesWithUnmadeChunks>);
static_assert(! miniverse::LayerKind<NotAKind>);

// A streamed load hands over chunks: some of what a load gives, or for a raster some of its tiles.
static_assert(std::same_as<miniverse::ChunkOf<Roads>, miniverse::Ways>);
static_assert(std::same_as<miniverse::ChunkOf<Buildings>, miniverse::Features<geo::MultiPolygon>>);
static_assert(miniverse::HasOwnChunks<Elevation> && ! miniverse::HasOwnChunks<Roads>);
static_assert(std::same_as<miniverse::ChunkOf<Elevation>, std::vector<geo::Raster<std::int16_t>>>);

// Its callback takes a chunk, and returns nothing or whether it wants more.
static_assert(miniverse::ChunkCallback<void (*)(miniverse::Ways), Roads>);
static_assert(miniverse::ChunkCallback<bool (*)(const miniverse::Ways&), Roads>);
static_assert(! miniverse::ChunkCallback<int (*)(miniverse::Ways), Roads>);
static_assert(! miniverse::ChunkCallback<void (*)(geo::Raster<std::int16_t>), Roads>);

// A push in parts is of one kind, and is handed on, not copied: it is one transaction.
static_assert(std::movable<miniverse::PushInParts<Roads>> && ! std::copyable<miniverse::PushInParts<Roads>>);
static_assert(std::same_as<decltype(std::declval<miniverse::Miniverse<Roads, Elevation>&>().begin_push<Elevation>()), std::future<miniverse::PushInParts<Elevation>>>);

// A layer is made from its table's name alone, whether or not its kind has settings: the table keeps those.
static_assert(std::constructible_from<miniverse::Layer<Roads>, const char*>);
static_assert(std::constructible_from<miniverse::Layer<Elevation>, const char*>);
static_assert(std::constructible_from<miniverse::Layer<Elevation>, const char*, const char*>);
static_assert(! std::constructible_from<miniverse::Layer<Elevation>, const char*, miniverse::geo::Grid<std::int16_t>>);

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
static_assert(schemacht::query::sql::type_name<miniverse::geo::MultiLineString>() == "geometry(MultiLineString,4326)");
static_assert(schemacht::query::sql::type_name<miniverse::geo::MultiPolygon>() == "geometry(MultiPolygon,4326)");

TEST_CASE("layer kinds are checked at compile time", "[compile]") { SUCCEED("the static_asserts above compiled"); }
