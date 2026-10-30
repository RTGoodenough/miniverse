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
#include "schemacht/util/compile_time.hpp"

/**
 * @file
 * What a database remembers about its layers, in two tables of `INTERNAL_SCHEMA`: `layers`, which tables are layers and the
 * layout each was made with, for `Miniverse::verify` to tell a table of another layout from one edited by hand; and
 * `datasets`, the datasets each holds and when each was last pushed, for `Miniverse::datasets`. Both name a table by the
 * schema and table it resolves to (`layer_name`, the registry's function: `public.roads`), whatever a layer spelled, so
 * `roads` and `public.roads` are one layer. The column `kind` of `layers` holds the model's schema placeholder name
 * (`features`, `roads`, `raster`), for a reader of the table. The registry's own tables have no layout number: their layout
 * is fixed.
 */
namespace miniverse {

namespace registry {

/** @brief The name of the table argument 0 resolves to, `schema.table`, as the registry keys it: SQL to put around `$1`. */
inline constexpr schemacht::util::CTString LAYER_NAME_OF_ARGUMENT = schemacht::util::concat_ctstrings<INTERNAL_SCHEMA_NAME, ".layer_name($1)">();

/**
 * @return The statements that make the schema, its two tables and its functions unless they are there, under an advisory lock
 * on the schema's name held until the transaction ends: two of them at once in a new database take turns (`IF NOT EXISTS`
 * alone fails the second). Run first in the transaction that makes a layer's table.
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
      // The functions are made only if absent: replacing a function takes its owner, and the role that made the schema need
      // not be this one. The name a table resolves to, `schema.table`; null for one that is not there.
      "DO $do$ BEGIN IF to_regprocedure('" + schema + ".layer_name(text)') IS NULL THEN CREATE FUNCTION " + schema +
          ".layer_name(layer text) RETURNS text LANGUAGE sql STABLE AS $fn$ SELECT n.nspname || '.' || c.relname FROM pg_class c "
          "JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.oid = to_regclass(layer) $fn$; END IF; END $do$",
      // The rows of a layer's table by dataset: the table is named when the function is called, so the statement is made then.
      "DO $do$ BEGIN IF to_regprocedure('" + schema + ".dataset_rows(text)') IS NULL THEN CREATE FUNCTION " + schema +
          ".dataset_rows(layer text) RETURNS TABLE (dataset text, row_count bigint) LANGUAGE plpgsql STABLE AS $fn$ BEGIN RETURN QUERY "
          "EXECUTE format('SELECT dataset, count(*) FROM %s GROUP BY dataset', layer::regclass); END $fn$; END IF; END $do$",
  };
}

/** @return `table` as a layer spelled it, `schema.table` or `table` alone: what the registry's `layer_name` resolves. */
[[nodiscard]] inline std::string name_of(const schemacht::schema::TableName& table) {
  return table.schema_name().empty() ? table.name() : table.schema_name() + "." + table.name();
}

/** @return The statement that records `table`, which exists, as a layer of `model`, made now with `layout`, over any earlier record of it. */
[[nodiscard]] inline schemacht::postgres::UncheckedSql registered(const std::string& table, std::string_view model, std::int32_t layout) {
  return schemacht::postgres::unchecked_sql(
      "INSERT INTO " + std::string(INTERNAL_SCHEMA) + ".layers (table_name, kind, layout, created) VALUES (" +
          std::string(LAYER_NAME_OF_ARGUMENT.view()) +
          ", $2, $3, now()) "
          "ON CONFLICT (table_name) DO UPDATE SET kind = EXCLUDED.kind, layout = EXCLUDED.layout, created = EXCLUDED.created",
      table, std::string(model), layout
  );
}

/** @return The statement that forgets `table`, which still exists, as a layer. */
[[nodiscard]] inline schemacht::postgres::UncheckedSql layer_forgotten(const std::string& table) {
  return schemacht::postgres::unchecked_sql(
      "DELETE FROM " + std::string(INTERNAL_SCHEMA) + ".layers WHERE table_name = " + std::string(LAYER_NAME_OF_ARGUMENT.view()), table
  );
}

/** @return The statement that forgets every dataset of `table`, which still exists. */
[[nodiscard]] inline schemacht::postgres::UncheckedSql datasets_forgotten(const std::string& table) {
  return schemacht::postgres::unchecked_sql(
      "DELETE FROM " + std::string(INTERNAL_SCHEMA) + ".datasets WHERE table_name = " + std::string(LAYER_NAME_OF_ARGUMENT.view()), table
  );
}

/** @return The statement that records `dataset` of `table` as pushed now, by the push stamped `stamp`. */
[[nodiscard]] inline schemacht::postgres::UncheckedSql dataset_pushed(const std::string& table, const std::string& dataset, std::int64_t stamp) {
  return schemacht::postgres::unchecked_sql(
      "INSERT INTO " + std::string(INTERNAL_SCHEMA) + ".datasets (table_name, dataset, pushed, stamp) VALUES (" +
          std::string(LAYER_NAME_OF_ARGUMENT.view()) +
          ", $2, now(), $3) "
          "ON CONFLICT (table_name, dataset) DO UPDATE SET pushed = EXCLUDED.pushed, stamp = EXCLUDED.stamp",
      table, dataset, stamp
  );
}

/** @return The statement that forgets `dataset` of `table`. */
[[nodiscard]] inline schemacht::postgres::UncheckedSql dataset_removed(const std::string& table, const std::string& dataset) {
  return schemacht::postgres::unchecked_sql(
      "DELETE FROM " + std::string(INTERNAL_SCHEMA) + ".datasets WHERE table_name = " + std::string(LAYER_NAME_OF_ARGUMENT.view()) +
          " AND dataset = $2",
      table, dataset
  );
}

/** @brief Whether the registry is there, with its functions: one row, `present`. */
using Present = schemacht::query::RawStatement<
    schemacht::util::concat_ctstrings<"SELECT to_regprocedure('", INTERNAL_SCHEMA_NAME, ".layer_name(text)') IS NOT NULL AS present">(),
    schemacht::query::RawArguments<>, schemacht::schema::Field<bool, "present">>;

/** @brief The layout the layer table named by argument 0 was made with: one row, or none for a table not in the registry. */
using LayerLayout = schemacht::query::RawStatement<
    schemacht::util::concat_ctstrings<"SELECT layout FROM ", INTERNAL_SCHEMA_NAME, ".layers WHERE table_name = ", LAYER_NAME_OF_ARGUMENT>(),
    schemacht::query::RawArguments<std::string>, schemacht::schema::Field<std::int32_t, "layout">>;

/** @brief The rows `LayerLayout` gives: one, or none. */
using LayerLayoutRows = std::vector<LayerLayout::row_type>;

/** @brief The datasets of the layer table named by argument 0, by name, each with when it was last pushed. */
using DatasetsPushed = schemacht::query::RawStatement<
    schemacht::util::concat_ctstrings<
        "SELECT dataset, pushed FROM ", INTERNAL_SCHEMA_NAME, ".datasets WHERE table_name = ", LAYER_NAME_OF_ARGUMENT, " ORDER BY dataset">(),
    schemacht::query::RawArguments<std::string>, schemacht::schema::Field<std::string, "dataset">,
    schemacht::schema::Field<std::chrono::system_clock::time_point, "pushed">>;

/** @brief How many rows each dataset of the layer table named by argument 0 holds, for the datasets that hold any. */
using DatasetRows = schemacht::query::RawStatement<
    schemacht::util::concat_ctstrings<"SELECT dataset, row_count FROM ", INTERNAL_SCHEMA_NAME, ".dataset_rows($1)">(),
    schemacht::query::RawArguments<std::string>, schemacht::schema::Field<std::string, "dataset">,
    schemacht::schema::Field<std::int64_t, "row_count">>;

/**
 * @return What `verify` says of a layer whose registry row holds `found` (none: no row, or no registry at all) when its model
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
