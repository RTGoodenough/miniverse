#pragma once

#include <concepts>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/geo/types.hpp"
#include "schemacht/postgres/concepts/statements.hpp"
#include "schemacht/schema/table.hpp"
#include "schemacht/schema/table_name.hpp"

/**
 * What a kind of layer is: a kind of data (roads, elevation, ...) and how it is stored. A kind is a type, used only as a name,
 * never made; `Miniverse<Roads, Elevation>` holds one layer of each, and each layer names its table when the miniverse is
 * made (`Layer<Roads>("osm_roads")`). Two datasets of the same kind are two types: `struct Roads : RoadLayer {};` and
 * `struct Tracks : RoadLayer {};`.
 *
 * A kind says:
 * - `result_type`: what a load gives, decoded (`Ways`, say), for the worker to build what it wants from.
 * - `schema_type`: its table's layout, as a schemacht `Schema`. The schema's own name is only a placeholder.
 * - `load_statement_type`: the query that reads what lies in a location, a `schemacht::query::Prepared` written against
 *   `schema_type`, whose one argument is a polygon in WGS 84 (with `geo::Intersects`, say). It runs on the layer's table (`on`).
 * - `setup_sql(table, ...)`: statements that run once the table is made, such as its spatial index, written with the
 *   table's quoted name.
 * - `from_rows(rows, location)`: the load statement's rows made into a `result_type`, and `to_rows(result, ...)` the other
 *   way, for writing: in batches, each the rows of one statement. The kind sizes them, as it knows its rows: a batch should
 *   be some tens of megabytes at most as text, since a push holds one at a time (`RoadLayer::WAYS_PER_STATEMENT`,
 *   `ElevationLayer::PIXEL_BYTES_PER_STATEMENT`).
 * - `write_statement(rows)`: the statement that writes one batch, a schemacht insert (`insert_statement`: a row already there
 *   fails the push) or upsert (`upsert_statement`: it is merged), aimed at the layer's table (`on`). A push runs one per batch,
 *   all in one transaction.
 *
 * The `...` is the kind's settings, if its table is made with any beside its name:
 * - A kind without settings (`HasNoSettings`, a `RoadLayer`) makes its table the same way every time: `setup_sql(table)`,
 *   `to_rows(result)`.
 * - A kind with settings (`HasSettings`, an `ElevationLayer`'s grid) has a `settings_type`. Its table is made with them,
 *   `setup_sql(table, settings)`, once, by `Miniverse::create_table<Kind>(settings)`, and the table keeps them. To write, the
 *   kind reads them back from the table: `settings_statement_type`, a statement whose one argument is the table's quoted
 *   name, and `settings_from_rows(rows)`, its rows made into a `settings_type`; a push does so before `to_rows(result,
 *   settings)`, and `Miniverse::table_settings` gives them to a writer, to warp its source onto. A worker that only loads
 *   never needs them.
 *
 * miniverse's own kinds (`RoadLayer`, `ElevationLayer`) are built this way, and a kind of your own needs only to meet
 * `LayerKind` to be loaded and pushed like them.
 */
namespace miniverse {

/** @brief The rows of `kind_t`'s table: what `write_statement` takes, one batch. */
template <typename kind_t>
using RowsOf = std::vector<typename kind_t::schema_type::row_type>;

/** @brief The rows of `kind_t`'s table in batches, each the rows of one statement: what `to_rows` makes. */
template <typename kind_t>
using RowBatches = std::vector<RowsOf<kind_t>>;

/** @brief The rows of `kind_t`'s settings statement, bound: what `settings_from_rows` takes. */
template <typename kind_t>
using SettingsRows = std::vector<typename decltype(kind_t::settings_statement_type::bind(std::declval<const std::string&>()))::result_type>;

/** @brief Whether `kind_t`'s table is made with its name alone, and its rows from its data alone: see the top of this file. */
template <typename kind_t>
concept HasNoSettings = requires(const schemacht::schema::TableName& table, kind_t::result_type data) {
  { kind_t::setup_sql(table) } -> std::same_as<std::vector<std::string>>;
  { kind_t::to_rows(std::move(data)) } -> std::same_as<RowBatches<kind_t>>;
};

/** @brief Whether `kind_t`'s table is made with settings that it reads back to write: see the top of this file. */
template <typename kind_t>
concept HasSettings = requires(const std::string& quoted_table) {
  typename kind_t::settings_type;
  { kind_t::settings_statement_type::bind(quoted_table) } -> schemacht::postgres::RunnableStatement;
} && requires(
    const schemacht::schema::TableName& table, const kind_t::settings_type& settings, kind_t::result_type data, SettingsRows<kind_t> rows
) {
  { kind_t::setup_sql(table, settings) } -> std::same_as<std::vector<std::string>>;
  { kind_t::settings_from_rows(std::move(rows)) } -> std::same_as<typename kind_t::settings_type>;
  { kind_t::to_rows(std::move(data), settings) } -> std::same_as<RowBatches<kind_t>>;
};

/** @brief Whether `kind_t` is a layer kind: see the top of this file. */
template <typename kind_t>
concept LayerKind =
    requires {
      typename kind_t::result_type;
      typename kind_t::schema_type;
      typename kind_t::load_statement_type::result_type;
    } && schemacht::postgres::TextSchema<typename kind_t::schema_type> &&
    requires(
        const geo::Polygon& location, const schemacht::schema::Table<typename kind_t::schema_type>& schema_table,
        std::vector<typename kind_t::load_statement_type::result_type> rows, const RowsOf<kind_t>& written
    ) {
      // `.on` takes only a table of the statement's own schema: for another's it returns void, so this also checks the schema.
      { kind_t::load_statement_type::bind(location).on(schema_table) } -> schemacht::postgres::RunnableStatement;
      { kind_t::from_rows(std::move(rows), location) } -> std::same_as<typename kind_t::result_type>;
      { kind_t::write_statement(written).on(schema_table) } -> schemacht::postgres::CommandStatement;
    } &&
    (HasNoSettings<kind_t> || HasSettings<kind_t>);

}  // namespace miniverse
