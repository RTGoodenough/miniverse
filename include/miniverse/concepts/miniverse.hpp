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
 * - `settings_type`: what a layer of it is configured with, beside its table's name (an elevation's grid), or `NoSettings`.
 *   The table is made with them, and rows are written with them.
 * - `schema_type`: its table's layout, as a schemacht `Schema`. The schema's own name is only a placeholder.
 * - `load_statement_type`: the query that reads what lies in a location, a `schemacht::query::Prepared` written against
 *   `schema_type`, whose one argument is a polygon in WGS 84 (with `geo::Intersects`, say). It runs on the layer's table (`on`).
 * - `setup_sql(table, settings)`: statements that run once the table is made, such as its spatial index, written with the
 *   table's quoted name.
 * - `from_rows(rows, location)`: the load statement's rows made into a `result_type`, and `to_rows(result, settings)` the other
 *   way, for writing.
 * - `write_statement(rows)`: the statement that writes those rows, a schemacht insert (`insert_statement`: a row already there
 *   fails the push) or upsert (`upsert_statement`: it is merged), aimed at the layer's table (`on`).
 *
 * A kind with settings also says how to read its table's back from the database (`HasSettings`), to check a layer's against.
 *
 * miniverse's own kinds (`RoadLayer`, `ElevationLayer`) are built this way, and a kind of your own needs only to meet
 * `LayerKind` to be loaded and pushed like them.
 */
namespace miniverse {

struct NoSettings;  // layer.hpp

/** @brief The rows of `kind_t`'s settings statement, bound: what `settings_from_rows` takes. */
template <typename kind_t>
using SettingsRows = std::vector<typename decltype(kind_t::settings_statement_type::bind(std::declval<const std::string&>()))::result_type>;

/**
 * @brief Whether `kind_t` has settings, and says how to read them back: `settings_statement_type`, a statement whose one
 * argument is the table's quoted name, and `settings_from_rows(rows)`, its rows made into a `settings_type`.
 */
template <typename kind_t>
concept HasSettings = ! std::same_as<typename kind_t::settings_type, NoSettings> && requires(const std::string& table) {
  { kind_t::settings_statement_type::bind(table) } -> schemacht::postgres::RunnableStatement;
} && requires(SettingsRows<kind_t> rows) {
  { kind_t::settings_from_rows(std::move(rows)) } -> std::same_as<typename kind_t::settings_type>;
};

/** @brief Whether `kind_t` is a layer kind: see the top of this file. */
template <typename kind_t>
concept LayerKind =
    requires {
      typename kind_t::result_type;
      typename kind_t::settings_type;
      typename kind_t::schema_type;
      typename kind_t::load_statement_type::result_type;
    } && schemacht::postgres::TextSchema<typename kind_t::schema_type> &&
    requires(
        const geo::Polygon& location, const schemacht::schema::TableName& table,
        const schemacht::schema::Table<typename kind_t::schema_type>& schema_table, kind_t::result_type data, const kind_t::settings_type& settings,
        std::vector<typename kind_t::load_statement_type::result_type> rows, const std::vector<typename kind_t::schema_type::row_type>& written
    ) {
      // `.on` takes only a table of the statement's own schema: for another's it returns void, so this also checks the schema.
      { kind_t::load_statement_type::bind(location).on(schema_table) } -> schemacht::postgres::RunnableStatement;
      { kind_t::setup_sql(table, settings) } -> std::same_as<std::vector<std::string>>;
      { kind_t::to_rows(std::move(data), settings) } -> std::same_as<std::vector<typename kind_t::schema_type::row_type>>;
      { kind_t::from_rows(std::move(rows), location) } -> std::same_as<typename kind_t::result_type>;
      { kind_t::write_statement(written).on(schema_table) } -> schemacht::postgres::CommandStatement;
    } &&
    // Either no settings, or settings it can read back. `HasSettings` tests for `NoSettings` too, as `table_settings` asks it alone.
    (std::same_as<typename kind_t::settings_type, NoSettings> || HasSettings<kind_t>);

}  // namespace miniverse
