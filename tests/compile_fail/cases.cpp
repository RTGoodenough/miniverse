// Each block must fail to compile, with the message tests/CMakeLists.txt expects for it. Compiled one case at a time.

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/prepared.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/schema/table_name.hpp"

using Roads = miniverse::Layer<miniverse::RoadModel, "roads">;
using Elevation = miniverse::Layer<miniverse::RasterModel<std::int16_t>, "elevation">;
using Shops = miniverse::Layer<miniverse::FeatureModel<miniverse::geo::Point>, "shops">;

// A model written by hand, whole but for `writes`: what each case below adds to it is the one thing wrong with it.
struct Spots {
  static constexpr std::int32_t LAYOUT = 1;

  using result_type = std::vector<miniverse::geo::Point>;
  using schema_type = schemacht::schema::Schema<
      "spots", schemacht::schema::Field<std::int64_t, "spot_id", schemacht::schema::KeyRole::Primary>,
      schemacht::schema::Field<miniverse::geo::Point, "position">>;
  static constexpr auto LOAD =
      schemacht::query::select(schemacht::query::On<schema_type>::col<"position">().apply<miniverse::geo::Intersects>(schemacht::query::arg<0>()));
  using load_statement_type = schemacht::query::Prepared<LOAD>;

  static std::vector<std::string>                                        setup_sql(const schemacht::schema::TableName& table);
  static std::vector<std::vector<schema_type::row_type>>                 to_rows(result_type spots);
  static schemacht::postgres::SchemaStatement<schema_type, std::tuple<>> write_statement(const std::vector<schema_type::row_type>& rows);
  static result_type from_rows(std::vector<load_statement_type::result_type> rows, const miniverse::geo::Polygon& location);
};

#if defined(CASE_LOAD_LAYER_NOT_HELD)
void load_tracks(miniverse::Miniverse<Roads>& world) { std::ignore = world.load<"tracks">(miniverse::geo::Polygon()); }
#endif

#if defined(CASE_STREAM_LAYER_NOT_HELD)
void stream_tracks(miniverse::Miniverse<Roads>& world) {
  std::ignore = world.stream<"tracks">(miniverse::geo::Polygon(), [](const miniverse::Ways& /*chunk*/) {});
}
#endif

#if defined(CASE_PUSH_LAYER_NOT_HELD)
void push_tracks(miniverse::Miniverse<Roads>& world) { std::ignore = world.push<"tracks">(miniverse::Ways()); }
#endif

#if defined(CASE_PUSH_NEEDS_DATASET)
void push_shops(miniverse::Miniverse<Shops>& world) { std::ignore = world.push<"shops">(miniverse::Features<miniverse::geo::Point>()); }
#endif

#if defined(CASE_PUSH_TAKES_NO_DATASET)
void push_heights(miniverse::Miniverse<Elevation>& world) {
  std::ignore = world.push<"elevation">(miniverse::Dataset{"srtm"}, miniverse::geo::Raster<std::int16_t>());
}
#endif

#if defined(CASE_CREATE_TABLES_NEED_SETTINGS)
void make(miniverse::Miniverse<Elevation>& world) { world.create_tables(); }
#endif

#if defined(CASE_NO_LAYERS)
void open_nothing(const std::string& conninfo) { miniverse::Miniverse<> world(conninfo); }
#endif

#if defined(CASE_NO_LAYOUT)
struct Unnumbered : miniverse::RoadModel {
  static constexpr const char* LAYOUT = "one";  // a layout is a whole number
};
void open(const std::string& conninfo) { miniverse::Miniverse world(conninfo, miniverse::Layer<Unnumbered, "roads">()); }
#endif

#if defined(CASE_NO_WRITES)
void open(const std::string& conninfo) { miniverse::Miniverse world(conninfo, miniverse::Layer<Spots, "spots">()); }
#endif

#if defined(CASE_DATASETS_WITHOUT_COLUMNS)
struct Replaced : Spots {
  using writes = miniverse::writes::Datasets;  // but no `dataset` or `pushed` column in the schema

  static std::vector<std::vector<schema_type::row_type>> to_rows(result_type spots, const miniverse::Dataset& dataset, std::int64_t stamp);
};
void open(const std::string& conninfo) { miniverse::Miniverse world(conninfo, miniverse::Layer<Replaced, "spots">()); }
#endif

#if defined(CASE_DUPLICATE_NAME)
void open(const std::string& conninfo) { miniverse::Miniverse world(conninfo, Roads("roads"), Roads("more_roads")); }
#endif

#if defined(CASE_DUPLICATE_NAME_ACROSS_MODELS)
void open(const std::string& conninfo) {
  miniverse::Miniverse world(conninfo, Roads("roads"), miniverse::Layer<miniverse::RasterModel<std::int16_t>, "roads">("heights"));
}
#endif

#if defined(CASE_NOT_A_LAYER)
void open(const std::string& conninfo) { miniverse::Miniverse<miniverse::RoadModel> world(conninfo, miniverse::RoadModel()); }
#endif
