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

/** @return A stamp for one push of a dataset: 64 random bits, all but surely unlike any other push's. */
[[nodiscard]] std::int64_t stamp();

/**
 * @brief Takes an advisory lock on `dataset`'s name in `transaction`, held until it ends, and waits for it: two pushes of one
 * dataset then run one after the other. Run at the same time, each would sweep only what the other had committed before it
 * began, and both exports would stay in the table. Pushes of different datasets run side by side.
 */
inline void lock_dataset(schemacht::postgres::Transaction& transaction, const Dataset& dataset) {
  std::ignore = transaction.execute(schemacht::postgres::unchecked_sql("SELECT pg_advisory_xact_lock(hashtext($1))", dataset.name)).get();
}

/**
 * @return What a push of `dataset` runs before it commits: the kind's sweep, which deletes the dataset's rows in `table` whose
 * stamp is not `stamp`, this push's. A row the push wrote again (a `RoadLayer`'s way, by its id) is kept, with the new stamp.
 */
template <HasDatasets kind_t>
[[nodiscard]] std::function<void(schemacht::postgres::Transaction&)> sweep_of(
    const schemacht::schema::Table<typename kind_t::schema_type>& table, const Dataset& dataset, std::int64_t stamp
) {
  return [&table, name = dataset.name, stamp](schemacht::postgres::Transaction& transaction) {
    std::ignore = transaction.execute(kind_t::sweep_statement_type::bind(name, stamp).on(table)).get();
  };
}

/** @return A future for how many rows of `dataset` were deleted from `table`, or the error. See `Miniverse::remove`. */
template <HasDatasets kind_t>
[[nodiscard]] std::future<std::uint64_t> remove(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table, const Dataset& dataset
) {
  return schemacht::util::future_from<std::uint64_t>([&](const schemacht::util::Settler<std::uint64_t>& done) {
    guarded(done, [&] {
      database.execute(kind_t::remove_statement_type::bind(dataset.name).on(table), [done](std::uint64_t removed, const std::exception_ptr& error) {
        done(removed, error);
      });
    });
  });
}

}  // namespace miniverse::detail
