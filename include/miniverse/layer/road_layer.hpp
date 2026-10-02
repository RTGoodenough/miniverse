#pragma once

#include <cstdint>
#include <tuple>
#include <vector>

#include "miniverse/geo/column_types.hpp"
#include "miniverse/geo/types.hpp"
#include "schemacht/json/json.hpp"
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
using Geometry = schemacht::schema::Field<geo::LineString, "geom">;
using Tags = schemacht::schema::Field<schemacht::json::Json, "tags">;

/** @brief One row of a road table. */
using Row = std::tuple<WayId, NodeIds, Geometry, Tags>;

[[nodiscard]] std::vector<Row> to_rows(const Ways& ways);
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
  using schema_type = schemacht::schema::Schema<table, road::WayId, road::NodeIds, road::Geometry, road::Tags>;

  /** @brief The ways that intersect `$1`, a polygon (the index answers `ST_Intersects`). */
  using load_statement_type = schemacht::query::RawStatement<
      schemacht::util::concat_ctstrings<
          "SELECT way_id, node_ids, geom, tags FROM ", table, " WHERE ST_Intersects(geom, $1::geometry) ORDER BY way_id">(),
      schemacht::query::RawArguments<geo::Polygon>, road::WayId, road::NodeIds, road::Geometry, road::Tags>;

  /** @brief The spatial index that `load_statement`'s `ST_Intersects` uses. */
  using setup_statement_type = schemacht::query::RawStatement<
      schemacht::util::concat_ctstrings<"CREATE INDEX IF NOT EXISTS ", table, "_geom_idx ON ", table, " USING gist (geom)">(),
      schemacht::query::RawArguments<>>;

  [[nodiscard]] static auto load_statement(const geo::Polygon& location) { return load_statement_type::bind(location); }

  [[nodiscard]] static std::vector<road::Row> to_rows(const Ways& ways) { return road::to_rows(ways); }
  [[nodiscard]] static Ways                   from_rows(std::vector<road::Row> rows) { return road::from_rows(std::move(rows)); }
};

}  // namespace miniverse
