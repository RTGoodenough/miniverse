#pragma once

#include <string>
#include <utility>
#include <vector>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/world/datasets.hpp"
#include "miniverse/world/registry.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/postgres/transaction.hpp"
#include "schemacht/schema/table.hpp"

/**
 * @file
 * Making and dropping a layer's table, and recording it in the registry (registry.hpp): the one part of a miniverse that
 * waits rather than returning a future, for tests and a program's first start. What `Miniverse::create_table` and
 * `drop_tables` do for a layer that is a table.
 */
namespace miniverse::detail {

/** @brief Runs the registry's setup in `transaction`, each statement waited for: the schema and its tables are there after it. */
inline void make_registry(schemacht::postgres::Transaction& transaction) {
  for ( std::string& sql : registry::setup_sql() ) {
    transaction.execute(schemacht::postgres::unchecked_sql(std::move(sql))).get();
  }
}

/**
 * @brief Makes `table` as the model's schema says, runs `setup` on it, then the dataset index for a model with datasets, and
 * records it in the registry with the model's layout, in one transaction, and waits for it. Each statement is waited for in
 * turn, so the error is the failing statement's own; a throw drops the transaction, which rolls it back.
 */
template <LayerModel model_t>
void make_table(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename model_t::schema_type>& table, std::vector<std::string> setup
) {
  if constexpr ( WritesDatasets<model_t> ) {
    for ( std::string& sql : dataset_index_sql(table.name()) ) {
      setup.push_back(std::move(sql));
    }
  }

  schemacht::postgres::Transaction transaction = database.begin().get();
  make_registry(transaction);
  transaction.execute(schemacht::postgres::create_table_statement<typename model_t::schema_type>().on(table)).get();
  for ( std::string& sql : setup ) {
    transaction.execute(schemacht::postgres::unchecked_sql(std::move(sql))).get();
  }
  transaction.execute(registry::registered(registry::name_of(table.name()), model_t::schema_type::TABLE_NAME, model_t::LAYOUT)).get();

  transaction.commit().get();
}

/** @brief Drops `table` if it exists and forgets it and its datasets in the registry, in one transaction, and waits for it. */
template <LayerModel model_t>
void drop_table(schemacht::postgres::Database& database, const schemacht::schema::Table<typename model_t::schema_type>& table) {
  schemacht::postgres::Transaction transaction = database.begin().get();
  make_registry(transaction);  // a drop in a database with no registry yet
  transaction.execute(schemacht::postgres::drop_table_statement<typename model_t::schema_type>().on(table)).get();
  transaction.execute(registry::datasets_forgotten(registry::name_of(table.name()))).get();
  transaction.execute(registry::layer_forgotten(registry::name_of(table.name()))).get();

  transaction.commit().get();
}

}  // namespace miniverse::detail
