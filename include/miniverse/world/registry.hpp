#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "miniverse/internal_schema.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/query/raw_statement.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/table_name.hpp"

/**
 * @file
 * What a database remembers about its layers, in two tables of `INTERNAL_SCHEMA`: `layers`, which tables are layers and the
 * layout each was made with, for `Miniverse::verify` to tell a table of another layout from one edited by hand; and
 * `datasets`, the datasets each holds and when each was last pushed, for `Miniverse::datasets`. A layer's `kind` is its
 * schema's placeholder name (`features`, `roads`, `raster`), for a reader of the table. The registry's own tables have no
 * layout number: their layout is fixed.
 */
namespace miniverse {

/** @brief A dataset of a layer, as `Miniverse::datasets` lists them. */
struct DatasetInfo {
  std::string                           name;
  std::chrono::system_clock::time_point pushed;    ///< When the push that last wrote it committed, by the server's clock.
  std::uint64_t                         rows = 0;  ///< Counted when listed.

  [[nodiscard]] bool operator==(const DatasetInfo&) const = default;
};

namespace registry {

/**
 * @return The statements that make the schema, its two tables and its function unless they are there, under an advisory lock
 * on the schema's name held until the transaction ends: two of them at once in a new database take turns (`IF NOT EXISTS`
 * alone fails the second). Run first in the transaction that makes or drops a layer's table.
 */
[[nodiscard]] inline std::vector<std::string> setup_sql() {
  const std::string schema(INTERNAL_SCHEMA);

  return {
      "DO $$ BEGIN PERFORM pg_advisory_xact_lock(hashtext('" + schema + "')); END $$",
      "CREATE SCHEMA IF NOT EXISTS " + schema,
      "CREATE TABLE IF NOT EXISTS " + schema +
          ".layers (table_name text PRIMARY KEY, kind text NOT NULL, layout integer NOT NULL, created timestamptz NOT NULL)",
      "CREATE TABLE IF NOT EXISTS " + schema +
          ".datasets (table_name text NOT NULL, dataset text NOT NULL, pushed timestamptz NOT NULL, stamp bigint NOT NULL, "
          "PRIMARY KEY (table_name, dataset))",
      // The rows of a layer's table by dataset: the table is named when the function is called, so the statement is made then.
      // Made only if absent: replacing a function takes its owner, and the role that made the schema need not be this one.
      "DO $do$ BEGIN IF to_regprocedure('" + schema + ".dataset_rows(text)') IS NULL THEN CREATE FUNCTION " + schema +
          ".dataset_rows(layer text) RETURNS TABLE (dataset text, row_count bigint) LANGUAGE plpgsql STABLE AS $fn$ BEGIN RETURN QUERY "
          "EXECUTE format('SELECT dataset, count(*) FROM %s GROUP BY dataset', layer::regclass); END $fn$; END IF; END $do$",
  };
}

/** @return `table` as the registry names it: `schema.table`, or `table` alone for one on the search path, as the layer gave it. */
[[nodiscard]] inline std::string name_of(const schemacht::schema::TableName& table) {
  return table.schema_name().empty() ? table.name() : table.schema_name() + "." + table.name();
}

/** @return The statement that records `table` as a layer of `kind`, made now with `layout`, over any earlier record of it. */
[[nodiscard]] inline schemacht::postgres::UncheckedSql registered(const std::string& table, std::string_view kind, std::int32_t layout) {
  return schemacht::postgres::unchecked_sql(
      "INSERT INTO " + std::string(INTERNAL_SCHEMA) +
          ".layers (table_name, kind, layout, created) VALUES ($1, $2, $3, now()) "
          "ON CONFLICT (table_name) DO UPDATE SET kind = EXCLUDED.kind, layout = EXCLUDED.layout, created = EXCLUDED.created",
      table, std::string(kind), layout
  );
}

/** @return The statement that forgets `table` as a layer. */
[[nodiscard]] inline schemacht::postgres::UncheckedSql layer_forgotten(const std::string& table) {
  return schemacht::postgres::unchecked_sql("DELETE FROM " + std::string(INTERNAL_SCHEMA) + ".layers WHERE table_name = $1", table);
}

/** @return The statement that forgets every dataset of `table`. */
[[nodiscard]] inline schemacht::postgres::UncheckedSql datasets_forgotten(const std::string& table) {
  return schemacht::postgres::unchecked_sql("DELETE FROM " + std::string(INTERNAL_SCHEMA) + ".datasets WHERE table_name = $1", table);
}

/** @return The statement that records `dataset` of `table` as pushed now, by the push stamped `stamp`. */
[[nodiscard]] inline schemacht::postgres::UncheckedSql dataset_pushed(const std::string& table, const std::string& dataset, std::int64_t stamp) {
  return schemacht::postgres::unchecked_sql(
      "INSERT INTO " + std::string(INTERNAL_SCHEMA) +
          ".datasets (table_name, dataset, pushed, stamp) VALUES ($1, $2, now(), $3) "
          "ON CONFLICT (table_name, dataset) DO UPDATE SET pushed = EXCLUDED.pushed, stamp = EXCLUDED.stamp",
      table, dataset, stamp
  );
}

/** @return The statement that forgets `dataset` of `table`. */
[[nodiscard]] inline schemacht::postgres::UncheckedSql dataset_removed(const std::string& table, const std::string& dataset) {
  return schemacht::postgres::unchecked_sql(
      "DELETE FROM " + std::string(INTERNAL_SCHEMA) + ".datasets WHERE table_name = $1 AND dataset = $2", table, dataset
  );
}

/** @brief The layout the layer table named by argument 0 was made with: one row, or none for a table not in the registry. */
using LayerLayout = schemacht::query::RawStatement<
    "SELECT layout FROM miniverse_internal.layers WHERE table_name = $1", schemacht::query::RawArguments<std::string>,
    schemacht::schema::Field<std::int32_t, "layout">>;

/** @brief The rows `LayerLayout` gives: one, or none. */
using LayerLayoutRows = std::vector<LayerLayout::row_type>;

/** @brief The datasets of the layer table named by argument 0, by name, each with when it was last pushed. */
using DatasetsPushed = schemacht::query::RawStatement<
    "SELECT dataset, pushed FROM miniverse_internal.datasets WHERE table_name = $1 ORDER BY dataset", schemacht::query::RawArguments<std::string>,
    schemacht::schema::Field<std::string, "dataset">, schemacht::schema::Field<std::chrono::system_clock::time_point, "pushed">>;

/** @brief How many rows each dataset of the layer table named by argument 0 holds, for the datasets that hold any. */
using DatasetRows = schemacht::query::RawStatement<
    "SELECT dataset, row_count FROM miniverse_internal.dataset_rows($1)", schemacht::query::RawArguments<std::string>,
    schemacht::schema::Field<std::string, "dataset">, schemacht::schema::Field<std::int64_t, "row_count">>;

/**
 * @return What `verify` says of a layer whose registry row holds `found` (none: no row, or no registry at all) when its kind
 * is at layout `wanted`; nothing if they agree.
 */
[[nodiscard]] inline std::optional<std::string> layout_problem(std::optional<std::int32_t> found, std::int32_t wanted) {
  if ( ! found ) {
    return "not in the registry: made by hand, or by an older build; make the table again";
  }

  if ( *found != wanted ) {
    return "made with layout " + std::to_string(*found) + "; this build needs layout " + std::to_string(wanted) +
           ": make the table again and import its data";
  }

  return std::nullopt;
}

}  // namespace registry

}  // namespace miniverse
