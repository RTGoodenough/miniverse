#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "miniverse/geo/column_types.hpp"
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

/** @brief A road: an OpenStreetMap way, as a line through its nodes. */
struct Way {
  std::int64_t              id = 0;
  std::vector<std::int64_t> node_ids;     ///< The way's nodes, in order: one per point of `coordinates`.
  geo::LineString           coordinates;  ///< Where the nodes are.
  schemacht::json::Json     tags{"{}"};   ///< The way's tags, as one JSON object (`{"highway": "primary", ...}`): empty unless given.
};

/** @brief The roads a load gives: ordered by id from a table, and in a file's own order from a file. */
using Ways = std::vector<Way>;

/** @brief The columns of a road table, the same in every `RoadLayer`. */
namespace road {

using WayId = schemacht::schema::Field<std::int64_t, "way_id", schemacht::schema::KeyRole::Primary>;
using NodeIds = schemacht::schema::Field<std::vector<std::int64_t>, "node_ids">;
using Geom = schemacht::schema::Field<geo::LineString, "geom">;
using Tags = schemacht::schema::Field<schemacht::json::Json, "tags">;

}  // namespace road

/**
 * @brief The layer kind of roads: one row per way, its geometry a `geometry(LineString,4326)` with a GiST index. A load gives
 * the ways that intersect the location, ordered by id.
 *
 * @code
 * struct Roads : miniverse::RoadLayer {};
 * miniverse::Miniverse world(conninfo, miniverse::Layer<Roads>("osm_roads"));
 * @endcode
 */
struct RoadLayer {
  using result_type = Ways;
  /// The table's layout. Its name, `roads`, is only a placeholder: a layer names its own table (`Layer`).
  using schema_type = schemacht::schema::Schema<"roads", road::WayId, road::NodeIds, road::Geom, road::Tags>;
  using row_type = schema_type::row_type;

  /** @brief The ways that intersect argument 0, a polygon, by id (the GiST index answers `ST_Intersects`). */
  static constexpr auto LOAD =
      schemacht::query::select(schemacht::query::On<schema_type>::col<"geom">().apply<geo::Intersects>(schemacht::query::arg<0>()))
          .order_by(schemacht::query::On<schema_type>::col<"way_id">().asc());
  using load_statement_type = schemacht::query::Prepared<LOAD>;

  /** @brief How many ways a push writes in one statement: a way is some hundreds of bytes to a few kilobytes as text. */
  static constexpr std::size_t WAYS_PER_STATEMENT = 5000;

  /**
   * @return The spatial index that the load's `ST_Intersects` uses. It is not named: PostgreSQL names it (`<table>_geom_idx`),
   * shortening and numbering the name as needed, so it never clashes with another relation's.
   */
  [[nodiscard]] static std::vector<std::string> setup_sql(const schemacht::schema::TableName& table);

  /**
   * @return `ways` as rows, in batches of `WAYS_PER_STATEMENT`, in the order given.
   * @throws std::invalid_argument for a way whose node ids and coordinates differ in number.
   */
  [[nodiscard]] static std::vector<std::vector<row_type>> to_rows(Ways ways);

  /** @return The insert of `rows`: a way that is already there fails the push, which then writes nothing. */
  [[nodiscard]] static schemacht::postgres::SchemaStatement<schema_type, std::tuple<>> write_statement(const std::vector<row_type>& rows) {
    return schemacht::postgres::insert_statement<schema_type>(rows);
  }

  /** @return The ways of `rows`, which are already the ones that intersect the location. */
  [[nodiscard]] static Ways from_rows(std::vector<row_type> rows, const geo::Polygon& /*location*/);
};

}  // namespace miniverse
