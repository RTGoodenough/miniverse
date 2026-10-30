#pragma once

#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/tables_differ.hpp"
#include "miniverse/world/registry.hpp"
#include "miniverse/world/settings.hpp"
#include "schemacht/postgres/async_client.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/postgres/table_differences.hpp"
#include "schemacht/schema/table.hpp"

/**
 * @file
 * The check of a layer's table for `Miniverse::verify`: against the registry's record of it, then its model's schema and
 * settings. What a `verify` fails with is tables_differ.hpp.
 */
namespace miniverse {

namespace detail {

/**
 * @return The layout the registry records the layer table `name` as made with; nothing for a table it does not know, or a
 * database with no registry, or with one of an older build. Waits for the answer.
 */
[[nodiscard]] inline std::optional<std::int32_t> recorded_layout(schemacht::postgres::Database& database, const std::string& name) {
  try {
    const registry::LayerLayoutRows rows = database.execute(registry::LayerLayout::bind(name)).get();
    if ( rows.empty() ) {
      return std::nullopt;
    }

    return schemacht::schema::get<"layout">(rows.front());
  } catch ( const schemacht::postgres::QueryError& failed ) {
    // undefined_table: a database with no registry; undefined_function: a registry of an older build, without `layer_name`.
    if ( failed.sqlstate() != "42P01" && failed.sqlstate() != "42883" ) {
      throw;
    }

    return std::nullopt;
  }
}

/**
 * @return What is wrong with `table` for a layer of `model_t`, each in words, in the order found; nothing if it is as the model
 * says and the registry records it. One recorded with another layout is told that alone. Else its columns are compared with
 * the model's schema, and, when they match and the model has settings, the settings are read: the differences, or what the
 * settings lack, say enough, so only a table that is otherwise right is told that the registry does not know it. Waits for
 * each statement in turn: for a thread of the call's own.
 * @throws schemacht::postgres::QueryError, or whatever a statement fails with, when a check could not run.
 */
template <LayerModel model_t>
[[nodiscard]] std::vector<std::string> table_problems(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename model_t::schema_type>& table
) {
  const std::optional<std::int32_t> recorded = recorded_layout(database, registry::name_of(table.name()));
  const std::optional<std::string>  unrecorded = registry::layout_problem(recorded, model_t::LAYOUT);
  if ( unrecorded && recorded ) {
    return {*unrecorded};
  }

  std::vector<std::string> problems;
  for ( const schemacht::postgres::TableDifference& difference :
        database.execute(schemacht::postgres::table_differences_statement<typename model_t::schema_type>().on(table)).get() ) {
    problems.push_back(schemacht::postgres::describe(difference));
  }

  // Only a table whose columns match is asked for its settings: the question may not even be answerable of another (no raster
  // type, so no `raster_columns`).
  if constexpr ( WritesSettings<model_t> ) {
    if ( problems.empty() ) {
      auto rows = database.execute(settings_statement<model_t>(table.name())).get();
      try {
        std::ignore = model_t::settings_from_rows(std::move(rows));
      } catch ( const std::exception& unreadable ) {
        problems.emplace_back(unreadable.what());
      }
    }
  }

  if ( problems.empty() && unrecorded ) {
    problems.push_back(*unrecorded);
  }

  return problems;
}

}  // namespace detail

}  // namespace miniverse
