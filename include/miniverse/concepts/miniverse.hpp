#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "miniverse/dataset.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/model/writes.hpp"
#include "schemacht/postgres/concepts/statements.hpp"
#include "schemacht/schema/table.hpp"
#include "schemacht/schema/table_name.hpp"

/**
 * @file
 * What a model is: one sort of data (roads, elevation) and how it is stored. A model is a type that is never made; a
 * `Layer` (layer.hpp) gives it a name in the program and a table, and a `Miniverse` holds the layers. Two layers of one model
 * are two names: `Layer<RoadModel, "roads">` and `Layer<RoadModel, "tracks">`.
 *
 * A model says:
 * - `LAYOUT`: a whole number for the layout of its table, raised when the layout changes. A table records the layout it was
 *   made with, and `Miniverse::verify` names a table of another, which is made again and refilled.
 * - `result_type`: what a load gives (`Ways`, say).
 * - `schema_type`: its table's layout, a schemacht `Schema`. The schema's own name is a placeholder; the layer names the table.
 * - `load_statement_type`: a `schemacht::query::Prepared` against `schema_type` whose one argument is a polygon in WGS 84
 *   (with `geo::Intersects`, say), run on the layer's table with `on`.
 * - `writes`: how a push treats the rows already there, one of the three types in model/writes.hpp. It says what else the
 *   model has:
 *   - `writes::Plain`: `setup_sql(table)` and `to_rows(result)`.
 *   - `writes::WithSettings<T>` (a `RasterModel`'s grid): `setup_sql(table, settings)`, run once by
 *     `Miniverse::create_table<"name">(settings)`; `settings_statement_type`, whose one argument is the table's quoted name, and
 *     `settings_from_rows(rows)`, which read the settings back from the table before `to_rows(result, settings)`.
 *   - `writes::Datasets` (a `FeatureModel` or `RoadModel`): `dataset::Name` and `dataset::Stamp` among the schema's columns,
 *     `setup_sql(table)`, and `to_rows(result, dataset, stamp)`, which marks each row with the dataset's name and the push's
 *     stamp. The sweep and the remove of a dataset, and the dataset index, are the library's (world/datasets.hpp).
 * - `setup_sql(table, ...)`: statements run once the table is made, such as its spatial index, written with `table.quoted()`.
 * - `from_rows(rows, location)`: the load's rows as a `result_type`; `to_rows(result, ...)`: the other way, in batches of one
 *   statement each. The model sizes the batches: some tens of megabytes of text at most, since a push holds one at a time.
 * - `write_statement(rows)`: the schemacht insert or upsert that writes one batch, aimed at the layer's table with `on`.
 *
 * A streamed load hands its callback chunks of what a load would give, made by `from_rows` from some of the rows. A model whose
 * `from_rows` needs every row at once (a raster's window is stitched from all its tiles) says what a chunk is instead
 * (`HasOwnChunks`): `chunk_type` and `chunk_from_rows(rows, location)`.
 *
 * A model of your own needs only to meet `LayerModel` to be loaded and pushed like the built-in ones.
 */
namespace miniverse {

/** @brief The rows of `model_t`'s load statement: what `from_rows` takes. */
template <typename model_t>
using LoadedRows = std::vector<typename model_t::load_statement_type::result_type>;

/** @brief The rows of `model_t`'s table as they are written, without a generated column: what `write_statement` takes, one batch. */
template <typename model_t>
using RowsOf = std::vector<typename model_t::schema_type::insert_row_type>;

/** @brief The rows of `model_t`'s table in batches, each the rows of one statement: what `to_rows` makes. */
template <typename model_t>
using RowBatches = std::vector<RowsOf<model_t>>;

/** @brief The settings a model that writes with settings keeps in its table (a raster's grid): its `writes::settings_type`. */
template <typename model_t>
using SettingsOf = typename model_t::writes::settings_type;

/** @brief The rows of `model_t`'s settings statement, bound: what `settings_from_rows` takes. */
template <typename model_t>
using SettingsRows = std::vector<typename decltype(model_t::settings_statement_type::bind(std::declval<const std::string&>()))::result_type>;

/** @brief Whether `model_t` writes its rows as they are (`writes::Plain`), with what that asks of it: see the top of this file. */
template <typename model_t>
concept WritesPlain =
    std::same_as<typename model_t::writes, writes::Plain> && requires(const schemacht::schema::TableName& table, model_t::result_type data) {
      { model_t::setup_sql(table) } -> std::same_as<std::vector<std::string>>;
      { model_t::to_rows(std::move(data)) } -> std::same_as<RowBatches<model_t>>;
    };

/** @brief Whether `model_t`'s table is made with settings that it reads back to write (`writes::WithSettings`): see the top of this file. */
template <typename model_t>
concept WritesSettings = requires {
  typename model_t::writes::settings_type;
} && std::same_as<typename model_t::writes, writes::WithSettings<SettingsOf<model_t>>> && requires(const std::string& quoted_table) {
  { model_t::settings_statement_type::bind(quoted_table) } -> schemacht::postgres::RunnableStatement;
} && requires(const schemacht::schema::TableName& table, const SettingsOf<model_t>& settings, model_t::result_type data, SettingsRows<model_t> rows) {
  { model_t::setup_sql(table, settings) } -> std::same_as<std::vector<std::string>>;
  { model_t::settings_from_rows(std::move(rows)) } -> std::same_as<SettingsOf<model_t>>;
  { model_t::to_rows(std::move(data), settings) } -> std::same_as<RowBatches<model_t>>;
};

/** @brief Whether `model_t` writes its rows under a `Dataset`, which a push replaces (`writes::Datasets`): see the top of this file. */
template <typename model_t>
concept WritesDatasets = std::same_as<typename model_t::writes, writes::Datasets> &&
                         // The columns before the statements, by name and then by type: a statement written against a column
                         // that is not there is a hard error, not an unmet requirement.
                         model_t::schema_type::template has_column<dataset::Name::COLUMN_NAME>() &&
                         model_t::schema_type::template has_column<dataset::Stamp::COLUMN_NAME>() &&
                         std::same_as<typename model_t::schema_type::template field_by_name<dataset::Name::COLUMN_NAME>, dataset::Name> &&
                         std::same_as<typename model_t::schema_type::template field_by_name<dataset::Stamp::COLUMN_NAME>, dataset::Stamp> &&
                         requires(const schemacht::schema::TableName& table, model_t::result_type data, const Dataset& dataset, std::int64_t stamp) {
                           { model_t::setup_sql(table) } -> std::same_as<std::vector<std::string>>;
                           { model_t::to_rows(std::move(data), dataset, stamp) } -> std::same_as<RowBatches<model_t>>;
                         };

/** @brief Whether `model_t` says what a streamed chunk of it is, other than a `result_type`: see the top of this file. */
template <typename model_t>
concept HasOwnChunks = requires(LoadedRows<model_t> rows, const geo::Polygon& location) {
  typename model_t::chunk_type;
  { model_t::chunk_from_rows(std::move(rows), location) } -> std::same_as<typename model_t::chunk_type>;
};

/** @brief Whether `model_t` is a model: see the top of this file. */
template <typename model_t>
concept LayerModel =
    requires {
      typename model_t::result_type;
      typename model_t::schema_type;
      typename model_t::load_statement_type::result_type;
      typename model_t::writes;
      { model_t::LAYOUT } -> std::convertible_to<std::int32_t>;
    } && schemacht::postgres::TextSchema<typename model_t::schema_type> &&
    requires(
        const geo::Polygon& location, const schemacht::schema::Table<typename model_t::schema_type>& schema_table, LoadedRows<model_t> rows,
        const RowsOf<model_t>& written
    ) {
      // `.on` takes only a table of the statement's own schema: for another's it returns void, so this also checks the schema.
      { model_t::load_statement_type::bind(location).on(schema_table) } -> schemacht::postgres::RunnableStatement;
      { model_t::from_rows(std::move(rows), location) } -> std::same_as<typename model_t::result_type>;
      { model_t::write_statement(written).on(schema_table) } -> schemacht::postgres::CommandStatement;
    } &&
    // `writes` names one of the three, and the model has what that one asks.
    (WritesPlain<model_t> || WritesSettings<model_t> || WritesDatasets<model_t>) &&
    // A model that names a `chunk_type` must make chunks of it, or its streams would silently fall back to `from_rows`.
    (! requires { typename model_t::chunk_type; } || HasOwnChunks<model_t>);

/** @brief What a streamed load of `model_t` hands its callback (`ChunkOf`): its `result_type`, or its own `chunk_type`. */
template <typename model_t>
struct ChunkType {
  using type = model_t::result_type;
};

template <HasOwnChunks model_t>
struct ChunkType<model_t> {
  using type = model_t::chunk_type;
};

/** @brief What a streamed load of `model_t` hands its callback: some of what a load would give. */
template <typename model_t>
using ChunkOf = ChunkType<model_t>::type;

/**
 * @brief A function a streamed load of `model_t` hands its chunks to: invocable with a `ChunkOf<model_t>`, returning `void`, or a
 * `bool` where `false` means it wants no more.
 */
template <typename callback_t, typename model_t>
concept ChunkCallback = std::invocable<callback_t&, ChunkOf<model_t>> && (std::same_as<std::invoke_result_t<callback_t&, ChunkOf<model_t>>, void> ||
                                                                          std::same_as<std::invoke_result_t<callback_t&, ChunkOf<model_t>>, bool>);

}  // namespace miniverse
