#pragma once

#include "schemacht/json/json.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/prepared.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/schema/table_name.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "miniverse/dataset.hpp"
#include "miniverse/geo/column_types.hpp"
#include "miniverse/geo/concepts/wkb.hpp"
#include "miniverse/geo/operations.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/model/feature.hpp"  // IWYU pragma: export
#include "miniverse/model/writes.hpp"   // IWYU pragma: export

/**
 * @file
 * The model of features of one geometry type: its table's columns and index, and its rows made from
 * features and back.
 */
namespace miniverse {

/** @brief A feature table's own columns; the dataset's are in writes.hpp. */
namespace feature {

/** @brief The feature's id, numbered by the database as rows are written. */
using Id =
    schemacht::schema::Field<std::int64_t, "feature_id", schemacht::schema::KeyRole::Primary, true>;

/** @brief The feature's geometry, in WGS 84. */
template <geo::wkb::Geometry geometry_t>
using Geom = schemacht::schema::Field<geometry_t, "geom">;

/** @brief The feature's tags, one JSON object. */
using Tags = schemacht::schema::Field<schemacht::json::Json, "tags">;

}  // namespace feature

/**
 * @brief The model of features of one geometry type: one row per feature, its geometry a
 * `geometry(<Type>,4326)` with a GiST index, its tags a `jsonb`. A load gives the features that
 * intersect the location, in id order. A layer of your own is one line:
 *
 * @code
 * using Buildings = miniverse::Layer<miniverse::FeatureModel<miniverse::geo::MultiPolygon>,
 * "buildings">;
 *
 * miniverse::Miniverse world(conninfo, Buildings("osm_buildings"));
 * world.create_table<"buildings">();  // once
 * for ( const miniverse::Feature<miniverse::geo::MultiPolygon>& building :
 * world.load<"buildings">(area).get() ) { ... }
 * @endcode
 *
 * A table holds one geometry type; the column refuses another. How a file of mixed polygons and
 * multipolygons is read is the reader's to say (`gdal::read_features`).
 *
 * Features are pushed under a `Dataset`, which a push replaces whole and `Miniverse::remove`
 * deletes. The data's ids are not kept, so a refresh gives a dataset's features new ids. Two
 * datasets that cover the same ground load twice: cut them to each other.
 */
template <geo::wkb::Geometry geometry_t>
struct FeatureModel {
  static constexpr std::int32_t LAYOUT = 2;  ///< Layout 1 called the stamp column `pushed`.

  using writes          = miniverse::writes::Datasets;
  using result_type     = Features<geometry_t>;
  /// The table's layout. Its name, `features`, is only a placeholder: a layer names its own table
  /// (`Layer`).
  using schema_type     = schemacht::schema::Schema<"features",
                                                    feature::Id,
                                                    dataset::Name,
                                                    feature::Geom<geometry_t>,
                                                    feature::Tags,
                                                    dataset::Stamp>;
  using row_type        = schema_type::row_type;
  /// A row as it is written: without the id, which the database numbers.
  using insert_row_type = schema_type::insert_row_type;

  /** @brief The features that intersect argument 0, a polygon, in id order: their id, geometry and
   * tags. */
  static constexpr auto LOAD =
      schemacht::query::select(
          schemacht::query::On<schema_type>::template col<"geom">().template apply<geo::Intersects>(
              schemacht::query::arg<0>()))
          .project(schemacht::query::On<schema_type>::template col<"feature_id">(),
                   schemacht::query::On<schema_type>::template col<"geom">(),
                   schemacht::query::On<schema_type>::template col<"tags">())
          .order_by(schemacht::query::On<schema_type>::template col<"feature_id">().asc());
  using load_statement_type = schemacht::query::Prepared<LOAD>;

  /** @brief A statement's worth of features, unless their points fill it first
   * (`POINTS_PER_STATEMENT`). */
  static constexpr std::size_t FEATURES_PER_STATEMENT = 5000;

  /**
   * @brief A statement's worth of points (a point is 16 bytes, twice that as text). A feature with
   * more points than that is a statement alone.
   */
  static constexpr std::size_t POINTS_PER_STATEMENT = std::size_t{1} << 20U;

  /** @return The GiST index on `geom`, unnamed: PostgreSQL names it. The dataset index is the
   * library's (world/datasets.hpp). */
  [[nodiscard]] static std::vector<std::string> setup_sql(
      const schemacht::schema::TableName& table);

  /**
   * @return `features` as rows of `dataset` marked `stamp`, without their ids, in the order given,
   * in batches of `FEATURES_PER_STATEMENT` and `POINTS_PER_STATEMENT`.
   */
  [[nodiscard]] static std::vector<std::vector<insert_row_type>> to_rows(
      result_type features, const Dataset& dataset, std::int64_t stamp);

  /** @return The insert of `rows`, each a new row. */
  [[nodiscard]] static auto write_statement(const std::vector<insert_row_type>& rows)
  {
    return schemacht::postgres::insert_statement<schema_type>(rows);
  }

  /** @return The features of `rows`, which are already the ones that intersect the location. */
  [[nodiscard]] static result_type from_rows(
      std::vector<typename load_statement_type::result_type> rows,
      const geo::Polygon& /*location*/);
};

extern template struct FeatureModel<geo::Point>;
extern template struct FeatureModel<geo::LineString>;
extern template struct FeatureModel<geo::Polygon>;
extern template struct FeatureModel<geo::MultiLineString>;
extern template struct FeatureModel<geo::MultiPolygon>;

}  // namespace miniverse
