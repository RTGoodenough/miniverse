#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "miniverse/dataset.hpp"
#include "miniverse/geo/types.hpp"
#include "schemacht/postgres/concepts/statements.hpp"
#include "schemacht/schema/table.hpp"
#include "schemacht/schema/table_name.hpp"

/**
 * @file
 * What a kind of layer is: a kind of data (roads, elevation) and how it is stored. A kind is a type used only as a name;
 * `Miniverse<Roads, Elevation>` holds one layer of each. Two layers of one kind are two types: `struct Roads : RoadLayer {};`
 * and `struct Tracks : RoadLayer {};`.
 *
 * A kind says:
 * - `result_type`: what a load gives (`Ways`, say).
 * - `schema_type`: its table's layout, a schemacht `Schema`. The schema's own name is a placeholder; the layer names the table.
 * - `load_statement_type`: a `schemacht::query::Prepared` against `schema_type` whose one argument is a polygon in WGS 84
 *   (with `geo::Intersects`, say), run on the layer's table with `on`.
 * - `setup_sql(table, ...)`: statements run once the table is made, such as its spatial index, written with `table.quoted()`.
 * - `from_rows(rows, location)`: the load's rows as a `result_type`; `to_rows(result, ...)`: the other way, in batches of one
 *   statement each. The kind sizes the batches: some tens of megabytes of text at most, since a push holds one at a time.
 * - `write_statement(rows)`: the schemacht insert or upsert that writes one batch, aimed at the layer's table with `on`.
 *
 * The `...` is what a kind's writes need beside the data; a kind is exactly one of three:
 * - Without settings (`HasNoSettings`): `setup_sql(table)`, `to_rows(result)`.
 * - With settings (`HasSettings`, a `RasterLayer`'s grid): `settings_type`; `setup_sql(table, settings)`, run once by
 *   `Miniverse::create_table<Kind>(settings)`; `settings_statement_type`, whose one argument is the table's quoted name, and
 *   `settings_from_rows(rows)`, which read the settings back from the table before `to_rows(result, settings)`.
 * - With datasets (`HasDatasets`, a `FeatureLayer` or `RoadLayer`): rows are written under a `Dataset` and a push replaces
 *   it. `to_rows(result, dataset, stamp)` marks each row with the dataset's name and the push's stamp; `sweep_statement_type`,
 *   bound to both and run before the commit, deletes the dataset's rows the push did not write; `remove_statement_type`,
 *   bound to the name, deletes the dataset. Ids are the database's own: `to_rows` leaves them out (`schema_type::insert_row_type`).
 *
 * A streamed load hands its callback chunks of what a load would give, made by `from_rows` from some of the rows. A kind whose
 * `from_rows` needs every row at once (a raster's window is stitched from all its tiles) says what a chunk is instead
 * (`HasOwnChunks`): `chunk_type` and `chunk_from_rows(rows, location)`.
 *
 * A kind of your own needs only to meet `LayerKind` to be loaded and pushed like the built-in ones.
 */
namespace miniverse {

/** @brief The rows of `kind_t`'s load statement: what `from_rows` takes. */
template <typename kind_t>
using LoadedRows = std::vector<typename kind_t::load_statement_type::result_type>;

/** @brief The rows of `kind_t`'s table as they are written, without a generated column: what `write_statement` takes, one batch. */
template <typename kind_t>
using RowsOf = std::vector<typename kind_t::schema_type::insert_row_type>;

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
} && requires(const schemacht::schema::TableName& table, const kind_t::settings_type& settings, kind_t::result_type data, SettingsRows<kind_t> rows) {
  { kind_t::setup_sql(table, settings) } -> std::same_as<std::vector<std::string>>;
  { kind_t::settings_from_rows(std::move(rows)) } -> std::same_as<typename kind_t::settings_type>;
  { kind_t::to_rows(std::move(data), settings) } -> std::same_as<RowBatches<kind_t>>;
};

/** @brief Whether `kind_t` writes its rows under a `Dataset`, which a push replaces: see the top of this file. */
template <typename kind_t>
concept HasDatasets = requires(
    const schemacht::schema::TableName& table, kind_t::result_type data, const Dataset& dataset, std::int64_t stamp,
    const schemacht::schema::Table<typename kind_t::schema_type>& schema_table
) {
  { kind_t::setup_sql(table) } -> std::same_as<std::vector<std::string>>;
  { kind_t::to_rows(std::move(data), dataset, stamp) } -> std::same_as<RowBatches<kind_t>>;
  { kind_t::sweep_statement_type::bind(dataset.name, stamp).on(schema_table) } -> schemacht::postgres::CommandStatement;
  { kind_t::remove_statement_type::bind(dataset.name).on(schema_table) } -> schemacht::postgres::CommandStatement;
};

/** @brief Whether `kind_t` says what a streamed chunk of it is, other than a `result_type`: see the top of this file. */
template <typename kind_t>
concept HasOwnChunks = requires(LoadedRows<kind_t> rows, const geo::Polygon& location) {
  typename kind_t::chunk_type;
  { kind_t::chunk_from_rows(std::move(rows), location) } -> std::same_as<typename kind_t::chunk_type>;
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
        const geo::Polygon& location, const schemacht::schema::Table<typename kind_t::schema_type>& schema_table, LoadedRows<kind_t> rows,
        const RowsOf<kind_t>& written
    ) {
      // `.on` takes only a table of the statement's own schema: for another's it returns void, so this also checks the schema.
      { kind_t::load_statement_type::bind(location).on(schema_table) } -> schemacht::postgres::RunnableStatement;
      { kind_t::from_rows(std::move(rows), location) } -> std::same_as<typename kind_t::result_type>;
      { kind_t::write_statement(written).on(schema_table) } -> schemacht::postgres::CommandStatement;
    } &&
    // Exactly one of the three: no push has both settings and a dataset to give `to_rows`.
    (int{HasNoSettings<kind_t>} + int{HasSettings<kind_t>} + int{HasDatasets<kind_t>} == 1) &&
    // A kind that names a `chunk_type` must make chunks of it, or its streams would silently fall back to `from_rows`.
    (! requires { typename kind_t::chunk_type; } || HasOwnChunks<kind_t>);

/** @brief What a streamed load of `kind_t` hands its callback (`ChunkOf`): its `result_type`, or its own `chunk_type`. */
template <typename kind_t>
struct ChunkType {
  using type = kind_t::result_type;
};

template <HasOwnChunks kind_t>
struct ChunkType<kind_t> {
  using type = kind_t::chunk_type;
};

/** @brief What a streamed load of `kind_t` hands its callback: some of what a load would give. */
template <typename kind_t>
using ChunkOf = ChunkType<kind_t>::type;

/**
 * @brief A function a streamed load of `kind_t` hands its chunks to: invocable with a `ChunkOf<kind_t>`, returning `void`, or a
 * `bool` where `false` means it wants no more.
 */
template <typename callback_t, typename kind_t>
concept ChunkCallback = std::invocable<callback_t&, ChunkOf<kind_t>> && (std::same_as<std::invoke_result_t<callback_t&, ChunkOf<kind_t>>, void> ||
                                                                         std::same_as<std::invoke_result_t<callback_t&, ChunkOf<kind_t>>, bool>);

}  // namespace miniverse
