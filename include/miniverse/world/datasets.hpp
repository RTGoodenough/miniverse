#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <string>
#include <tuple>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/dataset.hpp"
#include "miniverse/world/async.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/postgres/transaction.hpp"
#include "schemacht/schema/table.hpp"
#include "schemacht/util/futures.hpp"

/**
 * @file
 * What a kind with datasets adds to a push, and removing a dataset: the stamp a push marks its rows with, the lock that keeps
 * two pushes of one dataset from running at once, the sweep that deletes the dataset's rows a push did not write, and the
 * delete of a dataset whole.
 */
namespace miniverse::detail {

/** @return A stamp for one push of a dataset: 64 random bits, so that no two pushes of a dataset share one. */
[[nodiscard]] std::int64_t stamp();

/**
 * @brief Takes the lock on `dataset` in `transaction`, a push's, and waits for it. Two pushes of one dataset then run one after
 * the other, whatever their tables: at once, each would sweep only what the other had committed before it began, and both
 * exports would be left in the table. The lock is an advisory lock on the name's hash, held until the transaction ends; pushes
 * of different datasets run side by side (two names of one hash wait for each other, which costs nothing but the wait).
 */
inline void lock_dataset(schemacht::postgres::Transaction& transaction, const Dataset& dataset) {
  std::ignore = transaction.execute(schemacht::postgres::unchecked_sql("SELECT pg_advisory_xact_lock(hashtext($1))", dataset.name)).get();
}

/**
 * @return What a push of `dataset` runs before it commits: the kind's sweep, which deletes the dataset's rows in `table` that
 * the push, whose rows carry `stamp`, did not write, and waits for it. So the push replaces the dataset: a row of an older
 * push of it goes, and a row written again (for a `RoadLayer`, a way of the same nodes) stays, with its id.
 */
template <HasDatasets kind_t>
[[nodiscard]] std::function<void(schemacht::postgres::Transaction&)> sweep_of(
    const schemacht::schema::Table<typename kind_t::schema_type>& table, const Dataset& dataset, std::int64_t stamp
) {
  return [&table, name = dataset.name, stamp](schemacht::postgres::Transaction& transaction) {
    std::ignore = transaction.execute(kind_t::sweep_statement_type::bind(name, stamp).on(table)).get();
  };
}

/** @return A future that completes once every row of `dataset` is deleted from `table`, or holds the error. See `Miniverse::remove`. */
template <HasDatasets kind_t>
[[nodiscard]] std::future<void> remove(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table, const Dataset& dataset
) {
  return schemacht::util::future_from<void>([&](const schemacht::util::Settler<void>& done) {
    guarded(done, [&] {
      database.execute(
          kind_t::remove_statement_type::bind(dataset.name).on(table),
          [done](std::uint64_t /*removed*/, const std::exception_ptr& error) { done(error); }
      );
    });
  });
}

}  // namespace miniverse::detail
