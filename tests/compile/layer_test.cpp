// What is checked at compile time: which types are models, which are layers, the layers a miniverse deduces and finds by
// name, and the geometry columns' types.

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstdint>
#include <future>
#include <memory>
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

using Road = miniverse::RoadModel;

// The feature model of each geometry type.
using Shop = miniverse::FeatureModel<geo::Point>;
using Path = miniverse::FeatureModel<geo::LineString>;
using Park = miniverse::FeatureModel<geo::Polygon>;
using River = miniverse::FeatureModel<geo::MultiLineString>;
using Building = miniverse::FeatureModel<geo::MultiPolygon>;

// Places, but loaded by the roads' query, which is written against another schema.
struct PlacesByRoadQuery : Places {
  using load_statement_type = miniverse::RoadModel::load_statement_type;

  static result_type from_rows(std::vector<load_statement_type::result_type> rows, const geo::Polygon& location);
};

// Places whose table is made with a setting, but with no way to read it back from the table.
struct PlacesWithUnreadSettings : Places {
  using writes = miniverse::writes::WithSettings<int>;

  static std::vector<std::string>                        setup_sql(const sch::TableName& table, int settings);
  static std::vector<std::vector<schema_type::row_type>> to_rows(result_type places, int settings);
};

// Places that say their rows belong to datasets, but whose table has no column to say which.
struct PlacesWithUnnamedDatasets : Places {
  using writes = miniverse::writes::Datasets;

  static std::vector<std::vector<schema_type::row_type>> to_rows(result_type places, const miniverse::Dataset& dataset, std::int64_t stamp);
};

// Places that name a chunk of their own, but don't make one: their streams would fall back to from_rows unnoticed.
struct PlacesWithUnmadeChunks : Places {
  using chunk_type = int;
};

using Height = miniverse::RasterModel<std::int16_t>;

struct NotAModel {
  using result_type = int;
};

// The layers: a model under a name.
using Roads = miniverse::Layer<Road, "roads">;
using Tracks = miniverse::Layer<Road, "tracks">;
using Buildings = miniverse::Layer<Building, "buildings">;
using Elevation = miniverse::Layer<Height, "elevation">;
using Towns = miniverse::Layer<Places, "towns">;

}  // namespace

static_assert(miniverse::LayerModel<Road>);
static_assert(miniverse::LayerModel<Shop> && miniverse::LayerModel<Path> && miniverse::LayerModel<Park>);
static_assert(miniverse::LayerModel<River> && miniverse::LayerModel<Building>);
static_assert(miniverse::WritesDatasets<Building> && ! miniverse::WritesPlain<Building> && ! miniverse::WritesSettings<Building>);
static_assert(miniverse::WritesPlain<Places> && ! miniverse::WritesDatasets<Places>);
static_assert(std::same_as<Building::result_type, std::vector<miniverse::Feature<geo::MultiPolygon>>>);
static_assert(miniverse::LayerModel<Places>);
static_assert(miniverse::LayerModel<Height>);
static_assert(miniverse::WritesSettings<Height> && ! miniverse::WritesDatasets<Height>);
static_assert(std::same_as<miniverse::SettingsOf<Height>, geo::Grid<std::int16_t>>);
static_assert(! miniverse::WritesSettings<Road>);
static_assert(! miniverse::LayerModel<PlacesByRoadQuery>);
static_assert(! miniverse::LayerModel<PlacesWithUnreadSettings>);
static_assert(! miniverse::LayerModel<PlacesWithUnnamedDatasets> && ! miniverse::WritesDatasets<PlacesWithUnnamedDatasets>);
static_assert(! miniverse::LayerModel<PlacesWithUnmadeChunks>);
static_assert(! miniverse::LayerModel<NotAModel>);

// A streamed load hands over chunks: some of what a load gives, or for a raster some of its tiles.
static_assert(std::same_as<miniverse::ChunkOf<Road>, miniverse::Ways>);
static_assert(std::same_as<miniverse::ChunkOf<Building>, miniverse::Features<geo::MultiPolygon>>);
static_assert(miniverse::HasOwnChunks<Height> && ! miniverse::HasOwnChunks<Road>);
static_assert(std::same_as<miniverse::ChunkOf<Height>, std::vector<geo::Raster<std::int16_t>>>);

// Its callback takes a chunk, and returns nothing or whether it wants more.
static_assert(miniverse::ChunkCallback<void (*)(miniverse::Ways), Road>);
static_assert(miniverse::ChunkCallback<bool (*)(const miniverse::Ways&), Road>);
static_assert(! miniverse::ChunkCallback<int (*)(miniverse::Ways), Road>);
static_assert(! miniverse::ChunkCallback<void (*)(geo::Raster<std::int16_t>), Road>);

// A layer is a model under a name; a model alone, or a layer under another name, is not that layer.
static_assert(miniverse::IsLayer<Roads> && miniverse::IsLayer<Elevation> && miniverse::IsLayer<Towns>);
static_assert(! miniverse::IsLayer<Road> && ! miniverse::IsLayer<Places> && ! miniverse::IsLayer<NotAModel>);
static_assert(std::same_as<Roads::model_type, Road> && Roads::NAME.view() == "roads");
static_assert(! std::same_as<Roads, Tracks>);

// A push in parts is of one model, and is handed on, not copied: it is one transaction.
static_assert(std::movable<miniverse::PushInParts<Road>> && ! std::copyable<miniverse::PushInParts<Road>>);
static_assert(
    std::same_as<
        decltype(std::declval<miniverse::Miniverse<Roads, Elevation>&>().begin_push<"elevation">()), std::future<miniverse::PushInParts<Height>>>
);

// A layer is made from its table's name alone, whether or not its model has settings: the table keeps those; or from no name, for
// a table called as the layer is.
static_assert(std::constructible_from<Roads, const char*>);
static_assert(std::constructible_from<Elevation, const char*>);
static_assert(std::constructible_from<Elevation, const char*, const char*>);
static_assert(std::default_initializable<Elevation>);
static_assert(! std::constructible_from<Elevation, const char*, miniverse::geo::Grid<std::int16_t>>);

// A miniverse's layers are deduced from its arguments, and found by name.
static_assert(std::same_as<decltype(miniverse::Miniverse(std::string(), Roads("roads"))), miniverse::Miniverse<Roads>>);
static_assert(std::same_as<
              decltype(miniverse::Miniverse(std::string(), schemacht::postgres::Database::Options(), Roads("roads"), Towns("places"))),
              miniverse::Miniverse<Roads, Towns>>);
static_assert(miniverse::Miniverse<Roads, Towns>::holds<"roads">() && miniverse::Miniverse<Roads, Towns>::holds<"towns">());
static_assert(! miniverse::Miniverse<Roads, Towns>::holds<"tracks">());
static_assert(std::same_as<miniverse::Miniverse<Roads, Towns>::ModelOf<"towns">, Places>);
static_assert(
    std::same_as<decltype(std::declval<miniverse::Miniverse<Roads, Towns>&>().load<"roads">(geo::Polygon())), std::future<miniverse::Ways>>
);

// So are those of a miniverse with no database, whose layers have readers; one of no layers at all is not made that way.
static_assert(std::constructible_from<Roads, std::shared_ptr<const miniverse::Reader<Road>>>);
static_assert(std::same_as<decltype(miniverse::Miniverse(Roads("roads"), Towns("places"))), miniverse::Miniverse<Roads, Towns>>);

static_assert(schemacht::query::sql::type_name<miniverse::geo::Point>() == "geometry(Point,4326)");
static_assert(schemacht::query::sql::type_name<miniverse::geo::LineString>() == "geometry(LineString,4326)");
static_assert(schemacht::query::sql::type_name<miniverse::geo::Polygon>() == "geometry(Polygon,4326)");
static_assert(schemacht::query::sql::type_name<miniverse::geo::MultiLineString>() == "geometry(MultiLineString,4326)");
static_assert(schemacht::query::sql::type_name<miniverse::geo::MultiPolygon>() == "geometry(MultiPolygon,4326)");

TEST_CASE("models are checked at compile time", "[compile]") { SUCCEED("the static_asserts above compiled"); }
