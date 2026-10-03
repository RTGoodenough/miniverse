#pragma once

#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

#include "miniverse/geo/column_types.hpp"
#include "miniverse/geo/operations.hpp"
#include "miniverse/geo/types.hpp"
#include "schemacht/json/json.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/prepared.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/query/raw_statement.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/util/compile_time.hpp"

namespace miniverse {

/** @brief A road: an OpenStreetMap way, as a line through its nodes. */
struct Way {
  std::int64_t              id = 0;
  std::vector<std::int64_t> node_ids;     ///< The way's nodes, in order: one per point of `coordinates`.
  geo::LineString           coordinates;  ///< Where the nodes are.
  schemacht::json::Json     tags;         ///< The way's tags, as one JSON object (`{"highway": "primary", ...}`).
};

/** @brief The roads a load gives, ordered by id. */
using Ways = std::vector<Way>;

/** @brief The columns of a road table, the same in every `RoadLayer`. */
namespace road {

using WayId = schemacht::schema::Field<std::int64_t, "way_id", schemacht::schema::KeyRole::Primary>;
using NodeIds = schemacht::schema::Field<std::vector<std::int64_t>, "node_ids">;
using Geom = schemacht::schema::Field<geo::LineString, "geom">;
using Tags = schemacht::schema::Field<schemacht::json::Json, "tags">;

/** @brief One row of a road table. */
using Row = std::tuple<WayId, NodeIds, Geom, Tags>;

/** @throws std::invalid_argument for a way whose node ids and coordinates differ in number. */
[[nodiscard]] std::vector<Row> to_rows(Ways ways);
[[nodiscard]] Ways             from_rows(std::vector<Row> rows);

}  // namespace road

/**
 * @brief The layer kind of roads stored in the table `table`: one row per way, its geometry a `geometry(LineString,4326)`
 * with a GiST index. A load gives the ways that intersect the location, ordered by id.
 *
 * @code
 * struct Roads : miniverse::RoadLayer<"osm_roads"> {};
 * @endcode
 */
template <schemacht::util::CTString table>
struct RoadLayer {
  using result_type = Ways;
  using schema_type = schemacht::schema::Schema<table, road::WayId, road::NodeIds, road::Geom, road::Tags>;

  /** @brief The ways that intersect argument 0, a polygon, by id (the GiST index answers `ST_Intersects`). */
  static constexpr auto LOAD =
      schemacht::query::select(schemacht::query::On<schema_type>::template col<"geom">().template apply<geo::Intersects>(schemacht::query::arg<0>()))
          .order_by(schemacht::query::On<schema_type>::template col<"way_id">().asc());
  using load_statement_type = schemacht::query::Prepared<LOAD>;

  /**
   * @brief The spatial index that the load's `ST_Intersects` uses. It is not named: PostgreSQL names it (`<table>_geom_idx`),
   * shortening and numbering the name as needed, so it never clashes with another relation's.
   */
  using setup_statement_type = schemacht::query::RawStatement<
      schemacht::util::concat_ctstrings<"CREATE INDEX ON ", table, " USING gist (geom)">(), schemacht::query::RawArguments<>>;

  [[nodiscard]] static std::vector<road::Row> to_rows(Ways ways) { return road::to_rows(std::move(ways)); }
  [[nodiscard]] static Ways                   from_rows(std::vector<road::Row> rows) { return road::from_rows(std::move(rows)); }
};

}  // namespace miniverse
