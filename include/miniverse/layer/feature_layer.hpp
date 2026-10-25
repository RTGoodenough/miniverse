#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "miniverse/dataset.hpp"
#include "miniverse/geo/column_types.hpp"
#include "miniverse/geo/concepts/wkb.hpp"
#include "miniverse/geo/operations.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/layer/dataset_columns.hpp"  // IWYU pragma: export
#include "miniverse/layer/feature.hpp"          // IWYU pragma: export
#include "schemacht/json/json.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/prepared.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/schema/table_name.hpp"

/**
 * @file
 * The layer kind of features of one geometry type: its table's columns and index, and its rows made from features and back.
 */
namespace miniverse {

/** @brief The columns of a feature table that are its own (the dataset's are dataset_columns.hpp), the same in every `FeatureLayer` but for the geometry type. */
namespace feature {

/** @brief The feature's id: the database's own, numbered as rows are written, never the data's. */
using Id = schemacht::schema::Field<std::int64_t, "feature_id", schemacht::schema::KeyRole::Primary, true>;

template <geo::wkb::Geometry geometry_t>
using Geom = schemacht::schema::Field<geometry_t, "geom">;

using Tags = schemacht::schema::Field<schemacht::json::Json, "tags">;

}  // namespace feature

/**
 * @brief The layer kind of features of one geometry type: one row per feature, its geometry a `geometry(<Type>,4326)` with a
 * GiST index, its tags a `jsonb`. A load gives the features that intersect the location, ordered by id. A layer of your own
 * is one line:
 *
 * @code
 * struct Buildings : miniverse::FeatureLayer<miniverse::geo::MultiPolygon> {};
 *
 * miniverse::Miniverse world(conninfo, miniverse::Layer<Buildings>("osm_buildings"));
 * world.create_table<Buildings>();  // once
 * for ( const miniverse::Feature<miniverse::geo::MultiPolygon>& building : world.load<Buildings>(area).get() ) { ... }
 * @endcode
 *
 * A table holds one geometry type, which the column refuses any other of: a file that mixes polygons and multipolygons is
 * stored as `MultiPolygon`, each single polygon as a multipolygon of one.
 *
 * Features are written under a `Dataset`, the export they came from, which a push replaces whole and `Miniverse::remove`
 * removes. A feature's id is the database's own: the data's ids are not kept, and a refresh of a dataset gives its features
 * new ids. Nothing tells a feature of one dataset from one of another, so two datasets that cover the same ground load
 * twice: cut them to each other.
 */
template <geo::wkb::Geometry geometry_t>
struct FeatureLayer {
  using result_type = Features<geometry_t>;
  /// The table's layout. Its name, `features`, is only a placeholder: a layer names its own table (`Layer`).
  using schema_type = schemacht::schema::Schema<"features", feature::Id, dataset::Name, feature::Geom<geometry_t>, feature::Tags, dataset::Pushed>;
  using row_type = schema_type::row_type;
  /// A row as it is written: without the id, which the database numbers.
  using insert_row_type = schema_type::insert_row_type;

  /**
   * @brief The features that intersect argument 0, a polygon, by id (the GiST index answers `ST_Intersects`): id, geometry and
   * tags, not the columns a push keeps for itself.
   */
  static constexpr auto LOAD =
      schemacht::query::select(schemacht::query::On<schema_type>::template col<"geom">().template apply<geo::Intersects>(schemacht::query::arg<0>()))
          .project(
              schemacht::query::On<schema_type>::template col<"feature_id">(), schemacht::query::On<schema_type>::template col<"geom">(),
              schemacht::query::On<schema_type>::template col<"tags">()
          )
          .order_by(schemacht::query::On<schema_type>::template col<"feature_id">().asc());
  using load_statement_type = schemacht::query::Prepared<LOAD>;

  /// The sweep and the remove of a dataset (dataset_columns.hpp): what a push runs before it commits, and `Miniverse::remove` runs.
  using sweep_statement_type = dataset::Sweep<schema_type>;
  using remove_statement_type = dataset::Remove<schema_type>;

  /**
   * @brief How much a push writes in one statement, at most: this many features, or fewer if their geometries reach this many
   * points first (a point is 16 bytes, and twice that as text). One feature with more points than that is a statement alone.
   */
  static constexpr std::size_t FEATURES_PER_STATEMENT = 5000;
  static constexpr std::size_t POINTS_PER_STATEMENT = std::size_t{1} << 20U;

  /**
   * @return The spatial index that the load's `ST_Intersects` uses, and the index on `dataset` (`dataset::index_sql`). Neither
   * is named: PostgreSQL names them (`<table>_geom_idx`, `<table>_dataset_idx`), shortening and numbering the name as needed,
   * so they never clash with another relation's.
   */
  [[nodiscard]] static std::vector<std::string> setup_sql(const schemacht::schema::TableName& table);

  /**
   * @return `features` as rows of `dataset`, each marked `stamp`, in batches of `FEATURES_PER_STATEMENT` and
   * `POINTS_PER_STATEMENT`, in the order given. Their ids are left behind: the database numbers the rows.
   */
  [[nodiscard]] static std::vector<std::vector<insert_row_type>> to_rows(result_type features, const Dataset& dataset, std::int64_t stamp);

  /** @return The insert of `rows`: every one a new row, numbered by the database. */
  [[nodiscard]] static schemacht::postgres::SchemaStatement<schema_type, std::tuple<>> write_statement(const std::vector<insert_row_type>& rows) {
    return schemacht::postgres::insert_statement<schema_type>(rows);
  }

  /** @return The features of `rows`, which are already the ones that intersect the location. */
  [[nodiscard]] static result_type from_rows(std::vector<typename load_statement_type::result_type> rows, const geo::Polygon& /*location*/);
};

extern template struct FeatureLayer<geo::Point>;
extern template struct FeatureLayer<geo::LineString>;
extern template struct FeatureLayer<geo::Polygon>;
extern template struct FeatureLayer<geo::MultiLineString>;
extern template struct FeatureLayer<geo::MultiPolygon>;

}  // namespace miniverse
