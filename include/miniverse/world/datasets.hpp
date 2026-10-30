#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/dataset.hpp"
#include "miniverse/model/writes.hpp"
#include "miniverse/world/async.hpp"
#include "miniverse/world/registry.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/postgres/transaction.hpp"
#include "schemacht/query/command.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/prepared.hpp"
#include "schemacht/schema/concepts/schema.hpp"
#include "schemacht/schema/table.hpp"
#include "schemacht/schema/table_name.hpp"
#include "schemacht/util/futures.hpp"

/**
 * @file
 * What a model with datasets (`writes::Datasets`) adds to a push, and removing and listing datasets: the index on the dataset
 * column, the stamp a push marks its rows with, the lock that keeps two pushes of one dataset from running at once, the sweep
 * that deletes the dataset's rows a push did not write (and then records the push in the registry), the delete of a dataset
 * whole, and the list of a table's datasets.
 */
namespace miniverse::detail {

/**
 * @return What runs after a model with datasets' own setup: the index on `dataset` that the sweep and the remove use. It is not
 * named: PostgreSQL names it after the table (`roads_dataset_idx` for `roads`), shortening and numbering the name as needed, so
 * the name is unique.
 */
[[nodiscard]] inline std::vector<std::string> dataset_index_sql(const schemacht::schema::TableName& table) {
  return {"CREATE INDEX ON " + table.quoted() + " (dataset)"};
}

/** @return The key of the advisory lock a push or a remove of `dataset` in `table` holds while it runs: a dataset name in two tables is two locks. */
[[nodiscard]] inline std::string lock_key(const schemacht::schema::TableName& table, const Dataset& dataset) {
  return table.quoted() + '/' + dataset.name;
}

/** @brief The delete of the rows of `schema_t` of the dataset named by argument 0 that the push whose stamp is argument 1 did not write. */
template <schemacht::schema::IsSchema schema_t>
constexpr auto SWEEP = schemacht::query::delete_where(
    schemacht::query::On<schema_t>::template col<dataset::Name::COLUMN_NAME>() == schemacht::query::arg<0>() &&
    schemacht::query::On<schema_t>::template col<dataset::Pushed::COLUMN_NAME>() != schemacht::query::arg<1>()
);

/** @brief The delete of every row of `schema_t` of the dataset named by argument 0. */
template <schemacht::schema::IsSchema schema_t>
constexpr auto REMOVE =
    schemacht::query::delete_where(schemacht::query::On<schema_t>::template col<dataset::Name::COLUMN_NAME>() == schemacht::query::arg<0>());

/** @return The sweep of `table`: the delete of the rows of the dataset named `dataset_name` that the push stamped `stamp` did not write. */
template <WritesDatasets model_t>
[[nodiscard]] auto sweep_statement(
    const schemacht::schema::Table<typename model_t::schema_type>& table, const std::string& dataset_name, std::int64_t stamp
) {
  return schemacht::query::Prepared<SWEEP<typename model_t::schema_type>>::bind(dataset_name, stamp).on(table);
}

/** @return The delete of every row of the dataset named `dataset_name` from `table`. */
template <WritesDatasets model_t>
[[nodiscard]] auto remove_statement(const schemacht::schema::Table<typename model_t::schema_type>& table, const std::string& dataset_name) {
  return schemacht::query::Prepared<REMOVE<typename model_t::schema_type>>::bind(dataset_name).on(table);
}

/** @return A stamp for one push of a dataset: 64 random bits, all but surely unlike any other push's. */
[[nodiscard]] std::int64_t stamp();

/**
 * @brief Takes an advisory lock on `dataset` in `table` (`lock_key`) in `transaction`, held until it ends, and
 * waits for it: two pushes of one dataset then run one after the other. Run at the same time, each would sweep only what the
 * other had committed before it began, and both exports would stay in the table. Pushes of different datasets, or of one
 * dataset's name into different tables, run side by side.
 */
inline void lock_dataset(schemacht::postgres::Transaction& transaction, const schemacht::schema::TableName& table, const Dataset& dataset) {
  std::ignore = transaction.execute(schemacht::postgres::unchecked_sql("SELECT pg_advisory_xact_lock(hashtext($1))", lock_key(table, dataset))).get();
}

/**
 * @return What a push of `dataset` runs before it commits: the sweep, which deletes the dataset's rows in `table` whose
 * stamp is not `stamp`, this push's, and then the record of the push in the registry. A row the push wrote again (a
 * `RoadModel`'s way, by its id) is kept, with the new stamp.
 */
template <WritesDatasets model_t>
[[nodiscard]] std::function<void(schemacht::postgres::Transaction&)> sweep_of(
    const schemacht::schema::Table<typename model_t::schema_type>& table, const Dataset& dataset, std::int64_t stamp
) {
  return [&table, name = dataset.name, stamp](schemacht::postgres::Transaction& transaction) {
    std::ignore = transaction.execute(sweep_statement<model_t>(table, name, stamp)).get();
    std::ignore = transaction.execute(registry::dataset_pushed(registry::name_of(table.name()), name, stamp)).get();
  };
}

/**
 * @return A future for how many rows of `dataset` were deleted from `table`, or the error: the delete and the registry's
 * forgetting of the dataset in one transaction, under the dataset's lock, so that it takes turns with a push of the dataset,
 * on a thread started for it. See `Miniverse::remove`.
 */
template <WritesDatasets model_t>
[[nodiscard]] std::future<std::uint64_t> remove(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename model_t::schema_type>& table, const Dataset& dataset
) {
  return on_a_thread([&database, &table, dataset] {
    schemacht::postgres::Transaction transaction = database.begin().get();
    lock_dataset(transaction, table.name(), dataset);
    const std::uint64_t removed = transaction.execute(remove_statement<model_t>(table, dataset.name)).get();
    std::ignore = transaction.execute(registry::dataset_removed(registry::name_of(table.name()), dataset.name)).get();
    transaction.commit().get();

    return removed;
  });
}

/**
 * @return A future for the datasets of `table`, by name, each with when it was last pushed and its rows counted now, read on
 * a thread started for it. See `Miniverse::datasets`.
 */
template <WritesDatasets model_t>
[[nodiscard]] std::future<std::vector<DatasetInfo>> datasets(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename model_t::schema_type>& table
) {
  return on_a_thread([&database, &table] {
    const std::string name = registry::name_of(table.name());

    std::vector<DatasetInfo> listed;
    for ( auto& row : database.execute(registry::DatasetsPushed::bind(name)).get() ) {
      listed.push_back({.name = std::move(schemacht::schema::get<"dataset">(row)), .pushed = schemacht::schema::get<"pushed">(row), .rows = 0});
    }

    for ( const auto& row : database.execute(registry::DatasetRows::bind(name)).get() ) {
      for ( DatasetInfo& dataset : listed ) {
        if ( dataset.name == schemacht::schema::get<"dataset">(row) ) {
          dataset.rows = static_cast<std::uint64_t>(schemacht::schema::get<"row_count">(row));
        }
      }
    }

    return listed;
  });
}

}  // namespace miniverse::detail
