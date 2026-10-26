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
#include "miniverse/layer/dataset_columns.hpp"  // IWYU pragma: export
#include "miniverse/layer/way.hpp"              // IWYU pragma: export
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
 * The layer kind of roads, OpenStreetMap ways: its table's columns and index, and its rows made from ways and back.
 */
namespace miniverse {

/** @brief A road table's own columns; the dataset's are in dataset_columns.hpp. */
namespace road {

/** @brief The way's own id, the source's (OpenStreetMap's way id): the table's key (see `RoadLayer`). */
using WayId = schemacht::schema::Field<std::int64_t, "way_id", schemacht::schema::KeyRole::Primary>;

/** @brief The way's nodes, in order. */
using NodeIds = schemacht::schema::Field<std::vector<std::int64_t>, "node_ids">;

/** @brief The way as a line through its nodes. */
using Geom = schemacht::schema::Field<geo::LineString, "geom">;

/** @brief The way's tags, one JSON object. */
using Tags = schemacht::schema::Field<schemacht::json::Json, "tags">;

}  // namespace road

/**
 * @brief The layer kind of roads: one row per way, keyed by the way's own id, its geometry a `geometry(LineString,4326)` with a
 * GiST index. A load gives the ways that intersect the location, in id order: one network, whatever exports it was pushed from.
 *
 * @code
 * struct Roads : miniverse::RoadLayer {};
 * miniverse::Miniverse world(conninfo, miniverse::Layer<Roads>("osm_roads"));
 * @endcode
 *
 * Ways are pushed under a `Dataset`, which a push replaces whole and `Miniverse::remove` deletes. A way is its id, as in
 * OpenStreetMap: a way two exports both carry is one row, of the dataset that last pushed it, and a way pushed again keeps its
 * row, with the line, nodes and tags of the later push.
 *
 * This asks of the data that way ids and node ids are each one space across the layer, OpenStreetMap's, stable and not reused
 * for another place: data of the layer's own takes ids from a range OpenStreetMap does not use, and a producer that cannot
 * promise that gets a layer of its own. An export holds its ways whole (`osm::read_ways` drops a way with a missing node). A
 * way that a neighbouring export took over, and this export then dropped, stays until that neighbour is pushed again: its row
 * is the neighbour's, and only the neighbour's sweep deletes it.
 */
struct RoadLayer {
  static constexpr std::int32_t LAYOUT = 1;  ///< The first layout recorded.

  using result_type = Ways;
  /// The table's layout. Its name, `roads`, is only a placeholder: a layer names its own table (`Layer`).
  using schema_type = schemacht::schema::Schema<"roads", road::WayId, dataset::Name, road::NodeIds, road::Geom, road::Tags, dataset::Pushed>;
  using row_type = schema_type::row_type;
  /// A row as it is written: every column, the way's id among them.
  using insert_row_type = schema_type::insert_row_type;

  /** @brief The ways that intersect argument 0, a polygon, in id order: their id, nodes, line and tags. */
  static constexpr auto LOAD =
      schemacht::query::select(schemacht::query::On<schema_type>::col<"geom">().apply<geo::Intersects>(schemacht::query::arg<0>()))
          .project(
              schemacht::query::On<schema_type>::col<"way_id">(), schemacht::query::On<schema_type>::col<"node_ids">(),
              schemacht::query::On<schema_type>::col<"geom">(), schemacht::query::On<schema_type>::col<"tags">()
          )
          .order_by(schemacht::query::On<schema_type>::col<"way_id">().asc());
  using load_statement_type = schemacht::query::Prepared<LOAD>;

  /// The sweep and the remove of a dataset: dataset_columns.hpp.
  using sweep_statement_type = dataset::Sweep<schema_type>;
  using remove_statement_type = dataset::Remove<schema_type>;

  /** @brief A statement's worth of ways: a way is some hundreds of bytes to a few kilobytes as text. */
  static constexpr std::size_t WAYS_PER_STATEMENT = 5000;

  /** @return The GiST index on `geom` and the dataset index (`dataset::index_sql`), both unnamed: PostgreSQL names them. */
  [[nodiscard]] static std::vector<std::string> setup_sql(const schemacht::schema::TableName& table);

  /**
   * @return `ways` as rows of `dataset` marked `stamp`, in the order given, in batches of `WAYS_PER_STATEMENT`.
   * @throws std::invalid_argument for a way whose node ids and coordinates differ in number.
   */
  [[nodiscard]] static std::vector<std::vector<insert_row_type>> to_rows(Ways ways, const Dataset& dataset, std::int64_t stamp);

  /**
   * @return The write of `rows`: a way whose id is not in the table is a new row; one whose id is there updates that row
   * (dataset, nodes, line, tags and stamp). Two ways of one id in one statement fail it.
   */
  [[nodiscard]] static schemacht::postgres::SchemaStatement<schema_type, std::tuple<>> write_statement(const std::vector<insert_row_type>& rows) {
    return schemacht::postgres::upsert_statement<schema_type>(rows);
  }

  /** @return The ways of `rows`, which are already the ones that intersect the location. */
  [[nodiscard]] static Ways from_rows(std::vector<load_statement_type::result_type> rows, const geo::Polygon& /*location*/);
};

}  // namespace miniverse
