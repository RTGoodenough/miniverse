#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "miniverse/dataset.hpp"
#include "miniverse/geo/column_types.hpp"
#include "miniverse/geo/operations.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/layer/way.hpp"  // IWYU pragma: export
#include "schemacht/json/json.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/query/command.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/prepared.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/schema/table_name.hpp"

/**
 * @file
 * The layer kind of roads, OpenStreetMap ways: its table's columns and index, and its rows made from ways and back.
 */
namespace miniverse {

/** @brief The columns of a road table, the same in every `RoadLayer`. */
namespace road {

/** @brief The way's id: the database's own, numbered as rows are written, never the data's. */
using WayId = schemacht::schema::Field<std::int64_t, "way_id", schemacht::schema::KeyRole::Primary, true>;

/** @brief The name of the `Dataset` the way was last pushed under: what a push replaces, and `Miniverse::remove` removes. */
using DatasetName = schemacht::schema::Field<std::string, "dataset">;

/** @brief The way's nodes, in order: unique, since a way is its nodes (a push of the same nodes updates the row). */
using NodeIds = schemacht::schema::Field<std::vector<std::int64_t>, "node_ids", schemacht::schema::KeyRole::Unique>;

using Geom = schemacht::schema::Field<geo::LineString, "geom">;
using Tags = schemacht::schema::Field<schemacht::json::Json, "tags">;

/** @brief The stamp of the push that last wrote the row: a push's sweep deletes its dataset's rows that carry another. */
using Pushed = schemacht::schema::Field<std::int64_t, "pushed">;

}  // namespace road

/**
 * @brief The layer kind of roads: one row per way, its geometry a `geometry(LineString,4326)` with a GiST index. A load gives
 * the ways that intersect the location, ordered by id: one network, whatever exports it was pushed from.
 *
 * @code
 * struct Roads : miniverse::RoadLayer {};
 * miniverse::Miniverse world(conninfo, miniverse::Layer<Roads>("osm_roads"));
 * @endcode
 *
 * Ways are written under a `Dataset`, the export they came from, which a push replaces whole and `Miniverse::remove` removes.
 * A way is its nodes: the table holds one row per sequence of node ids, so a way that two exports both carry (one that
 * crosses the line between them, whole on each side) is one row, of the dataset that last pushed it, and a way pushed again
 * with the same nodes keeps its id, whatever its id in the export was. The ids a load gives are the database's own; the
 * export's way ids are not kept, and need mean nothing. What this asks of the data: that node ids are one space across the
 * layer, stable over time and never reused for another place (a producer that can't promise that gets a layer of its own),
 * and that an export holds its ways whole (`osm::read_ways` drops a way with a missing node). A way a neighbouring export
 * took over and then dropped lingers until that neighbour is pushed again.
 */
struct RoadLayer {
  using result_type = Ways;
  /// The table's layout. Its name, `roads`, is only a placeholder: a layer names its own table (`Layer`).
  using schema_type = schemacht::schema::Schema<"roads", road::WayId, road::DatasetName, road::NodeIds, road::Geom, road::Tags, road::Pushed>;
  using row_type = schema_type::row_type;
  /// A row as it is written: without the id, which the database numbers.
  using insert_row_type = schema_type::insert_row_type;

  /**
   * @brief The ways that intersect argument 0, a polygon, by id (the GiST index answers `ST_Intersects`): id, nodes, line and
   * tags, not the columns a push keeps for itself.
   */
  static constexpr auto LOAD =
      schemacht::query::select(schemacht::query::On<schema_type>::col<"geom">().apply<geo::Intersects>(schemacht::query::arg<0>()))
          .project(
              schemacht::query::On<schema_type>::col<"way_id">(), schemacht::query::On<schema_type>::col<"node_ids">(),
              schemacht::query::On<schema_type>::col<"geom">(), schemacht::query::On<schema_type>::col<"tags">()
          )
          .order_by(schemacht::query::On<schema_type>::col<"way_id">().asc());
  using load_statement_type = schemacht::query::Prepared<LOAD>;

  /** @brief The delete of the dataset named by argument 0's rows that a push, whose stamp is argument 1, did not write. */
  static constexpr auto SWEEP = schemacht::query::delete_where(
      schemacht::query::On<schema_type>::col<"dataset">() == schemacht::query::arg<0>() &&
      schemacht::query::On<schema_type>::col<"pushed">() != schemacht::query::arg<1>()
  );
  using sweep_statement_type = schemacht::query::Prepared<SWEEP>;

  /** @brief The delete of every row of the dataset named by argument 0. */
  static constexpr auto REMOVE = schemacht::query::delete_where(schemacht::query::On<schema_type>::col<"dataset">() == schemacht::query::arg<0>());
  using remove_statement_type = schemacht::query::Prepared<REMOVE>;

  /** @brief How many ways a push writes in one statement: a way is some hundreds of bytes to a few kilobytes as text. */
  static constexpr std::size_t WAYS_PER_STATEMENT = 5000;

  /**
   * @return The spatial index that the load's `ST_Intersects` uses, and the index on `dataset` that a push's sweep and a
   * remove use. Neither is named: PostgreSQL names them (`<table>_geom_idx`, `<table>_dataset_idx`), shortening and numbering
   * the name as needed, so they never clash with another relation's.
   */
  [[nodiscard]] static std::vector<std::string> setup_sql(const schemacht::schema::TableName& table);

  /**
   * @return `ways` as rows of `dataset`, each marked `stamp`, in batches of `WAYS_PER_STATEMENT`, in the order given. Their ids
   * are left behind: the database numbers the rows.
   * @throws std::invalid_argument for a way whose node ids and coordinates differ in number.
   */
  [[nodiscard]] static std::vector<std::vector<insert_row_type>> to_rows(Ways ways, const Dataset& dataset, std::int64_t stamp);

  /**
   * @return The write of `rows`: a way whose nodes are not in the table is a new row, numbered by the database; one whose nodes
   * are there already updates that row (its dataset, line, tags and stamp), which keeps its id. Two ways of the same nodes in
   * one statement fail it: the error is the database's.
   */
  [[nodiscard]] static schemacht::postgres::SchemaStatement<schema_type, std::tuple<>> write_statement(const std::vector<insert_row_type>& rows) {
    return schemacht::postgres::upsert_statement<schema_type, schemacht::postgres::ConflictOn<"node_ids">>(rows);
  }

  /** @return The ways of `rows`, which are already the ones that intersect the location. */
  [[nodiscard]] static Ways from_rows(std::vector<load_statement_type::result_type> rows, const geo::Polygon& /*location*/);
};

}  // namespace miniverse
