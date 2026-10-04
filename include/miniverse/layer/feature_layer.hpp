#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "miniverse/geo/column_types.hpp"
#include "miniverse/geo/concepts/wkb.hpp"
#include "miniverse/geo/operations.hpp"
#include "miniverse/geo/types.hpp"
#include "schemacht/json/json.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/prepared.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/schema/table_name.hpp"

namespace miniverse {

/** @brief A feature: one thing of the world as a geometry, with an id and tags. */
template <geo::wkb::Geometry geometry_t>
struct Feature {
  std::int64_t          id = 0;
  geometry_t            geometry{};  ///< Where it is, in WGS 84 longitude and latitude.
  schemacht::json::Json tags{"{}"};  ///< What it is, as one JSON object (`{"building": "yes", ...}`): empty unless given.
};

/** @brief The features a load gives, ordered by id. */
template <geo::wkb::Geometry geometry_t>
using Features = std::vector<Feature<geometry_t>>;

/** @brief The columns of a feature table, the same in every `FeatureLayer` but for the geometry type. */
namespace feature {

using Id = schemacht::schema::Field<std::int64_t, "feature_id", schemacht::schema::KeyRole::Primary>;

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
 * A table holds one geometry type, which the column refuses any other of: a dataset that mixes polygons and multipolygons
 * is stored as `MultiPolygon`, each single polygon as a multipolygon of one.
 */
template <geo::wkb::Geometry geometry_t>
struct FeatureLayer {
  using result_type = Features<geometry_t>;
  /// The table's layout. Its name, `features`, is only a placeholder: a layer names its own table (`Layer`).
  using schema_type = schemacht::schema::Schema<"features", feature::Id, feature::Geom<geometry_t>, feature::Tags>;
  using row_type = schema_type::row_type;

  /** @brief The features that intersect argument 0, a polygon, by id (the GiST index answers `ST_Intersects`). */
  static constexpr auto LOAD =
      schemacht::query::select(schemacht::query::On<schema_type>::template col<"geom">().template apply<geo::Intersects>(schemacht::query::arg<0>()))
          .order_by(schemacht::query::On<schema_type>::template col<"feature_id">().asc());
  using load_statement_type = schemacht::query::Prepared<LOAD>;

  /**
   * @brief How much a push writes in one statement, at most: this many features, or fewer if their geometries reach this many
   * points first (a point is 16 bytes, and twice that as text). One feature with more points than that is a statement alone.
   */
  static constexpr std::size_t FEATURES_PER_STATEMENT = 5000;
  static constexpr std::size_t POINTS_PER_STATEMENT = std::size_t{1} << 20U;

  /**
   * @return The spatial index that the load's `ST_Intersects` uses. It is not named: PostgreSQL names it (`<table>_geom_idx`),
   * shortening and numbering the name as needed, so it never clashes with another relation's.
   */
  [[nodiscard]] static std::vector<std::string> setup_sql(const schemacht::schema::TableName& table);

  /** @return `features` as rows, in batches of `FEATURES_PER_STATEMENT` and `POINTS_PER_STATEMENT`, in the order given. */
  [[nodiscard]] static std::vector<std::vector<row_type>> to_rows(result_type features);

  /** @return The insert of `rows`: a feature that is already there fails the push, which then writes nothing. */
  [[nodiscard]] static schemacht::postgres::SchemaStatement<schema_type, std::tuple<>> write_statement(const std::vector<row_type>& rows) {
    return schemacht::postgres::insert_statement<schema_type>(rows);
  }

  /** @return The features of `rows`, which are already the ones that intersect the location. */
  [[nodiscard]] static result_type from_rows(std::vector<row_type> rows, const geo::Polygon& /*location*/);
};

extern template struct FeatureLayer<geo::Point>;
extern template struct FeatureLayer<geo::LineString>;
extern template struct FeatureLayer<geo::Polygon>;
extern template struct FeatureLayer<geo::MultiLineString>;
extern template struct FeatureLayer<geo::MultiPolygon>;

}  // namespace miniverse
