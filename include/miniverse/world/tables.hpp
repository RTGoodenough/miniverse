#pragma once

#include <string>
#include <utility>
#include <vector>

#include "miniverse/concepts/miniverse.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/postgres/transaction.hpp"
#include "schemacht/schema/table.hpp"

/**
 * @file
 * Making and dropping a layer's table: the one part of a miniverse that waits rather than returning a future, for tests and
 * a program's first start. What `Miniverse::create_table` and `drop_tables` do for a layer that is a table.
 */
namespace miniverse::detail {

/**
 * @brief Makes `table` as the kind's schema says and runs `setup` on it, in one transaction, and waits for it. Each statement
 * is waited for in turn, so the error is the failing statement's own; a throw drops the transaction, which rolls it back.
 */
template <LayerKind kind_t>
void make_table(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table, std::vector<std::string> setup
) {
  schemacht::postgres::Transaction transaction = database.begin().get();
  transaction.execute(schemacht::postgres::create_table_statement<typename kind_t::schema_type>().on(table)).get();
  for ( std::string& sql : setup ) {
    transaction.execute(schemacht::postgres::unchecked_sql(std::move(sql))).get();
  }

  transaction.commit().get();
}

/** @brief Drops `table` if it exists, and waits for it. */
template <LayerKind kind_t>
void drop_table(schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table) {
  database.execute(schemacht::postgres::drop_table_statement<typename kind_t::schema_type>().on(table)).get();
}

}  // namespace miniverse::detail
