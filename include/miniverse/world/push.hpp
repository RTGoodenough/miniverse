#pragma once

#include <concepts>
#include <cstdint>
#include <future>
#include <memory>
#include <utility>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/dataset.hpp"
#include "miniverse/world/async.hpp"
#include "miniverse/world/datasets.hpp"
#include "miniverse/world/push_in_parts.hpp"
#include "miniverse/world/settings.hpp"
#include "miniverse/world/step_thread.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/schema/table.hpp"

/**
 * @file
 * Pushes to a layer's table, each on a thread started for it: a whole push in a transaction of its own, or the beginning of
 * one that takes its data in parts (push_in_parts.hpp), which starts the thread the parts then run on. What
 * `Miniverse::push` and `begin_push` do for a layer that is a table. `database` and `table` are used from the threads: both
 * must outlive the future and the push, as the miniverse's do.
 */
namespace miniverse::detail {

/** @return A push of `table` begun on `database`, waited for: the table's settings read first, for a kind with them. */
template <LayerKind kind_t>
[[nodiscard]] std::shared_ptr<Push<kind_t>> begun(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table
) {
  typename Push<kind_t>::ToRows to_rows;
  if constexpr ( HasSettings<kind_t> ) {
    typename kind_t::settings_type settings = kind_t::settings_from_rows(database.execute(settings_statement<kind_t>(table.name())).get());
    to_rows = [settings = std::move(settings)](typename kind_t::result_type data) { return kind_t::to_rows(std::move(data), settings); };

  } else {
    to_rows = [](typename kind_t::result_type data) { return kind_t::to_rows(std::move(data)); };
  }

  return std::make_shared<Push<kind_t>>(Push<kind_t>{.transaction = database.begin().get(), .table = &table, .to_rows = std::move(to_rows)});
}

/**
 * @return A push of `dataset` into `table` begun on `database`, waited for: the dataset locked, a stamp drawn, the sweep set to run before the
 * commit.
 */
template <HasDatasets kind_t>
[[nodiscard]] std::shared_ptr<Push<kind_t>> begun(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table, const Dataset& dataset
) {
  const std::int64_t marked = stamp();

  std::shared_ptr<Push<kind_t>> running = std::make_shared<Push<kind_t>>(Push<kind_t>{
      .transaction = database.begin().get(),
      .table = &table,
      .to_rows = [dataset, marked](typename kind_t::result_type data) { return kind_t::to_rows(std::move(data), dataset, marked); },
      .before_commit = sweep_of<kind_t>(table, dataset, marked),
  });
  lock_dataset(running->transaction, dataset);  // a throw drops the push, and its transaction with it

  return running;
}

/**
 * @return A future that completes once `data` is written to `table`, in a transaction of its own on a thread started for it, or holds the error. See
 * `Miniverse::push`.
 */
template <LayerKind kind_t>
[[nodiscard]] std::future<void> push(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table, typename kind_t::result_type data
) {
  return on_a_thread([&database, &table, data = std::move(data)]() mutable {
    const std::shared_ptr<Push<kind_t>> running = begun<kind_t>(database, table);
    running->add(std::move(data));
    running->commit();
  });
}

/**
 * @return A future that completes once `data` replaces `dataset` in `table`, in a transaction of its own on a thread started for it, or holds the
 * error. See `Miniverse::push`.
 */
template <HasDatasets kind_t>
[[nodiscard]] std::future<void> push(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table, const Dataset& dataset,
    typename kind_t::result_type data
) {
  return on_a_thread([&database, &table, dataset, data = std::move(data)]() mutable {
    const std::shared_ptr<Push<kind_t>> running = begun<kind_t>(database, table, dataset);
    running->add(std::move(data));
    running->commit();
  });
}

/**
 * @return A future for a push in parts of the push `begin` gives: on a thread started for the call, the push is begun and the
 * thread its parts then run on is started. It holds the error instead if either fails.
 */
template <LayerKind kind_t, std::invocable begin_t>
[[nodiscard]] std::future<PushInParts<kind_t>> in_parts(begin_t begin) {
  return on_a_thread([begin = std::move(begin)]() mutable {
    std::shared_ptr<Push<kind_t>> push = begin();

    return PushInParts<kind_t>(StepThread(), std::move(push));
  });
}

/** @return A future for a push of `table` that takes its data in parts, begun on a thread started for the call. See `Miniverse::begin_push`. */
template <LayerKind kind_t>
[[nodiscard]] std::future<PushInParts<kind_t>> begin_push(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table
) {
  return in_parts<kind_t>([&database, &table] { return begun<kind_t>(database, table); });
}

/**
 * @return A future for a push of `dataset` into `table` that takes its data in parts, begun on a thread started for the call. See
 * `Miniverse::begin_push`.
 */
template <HasDatasets kind_t>
[[nodiscard]] std::future<PushInParts<kind_t>> begin_push(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table, const Dataset& dataset
) {
  return in_parts<kind_t>([&database, &table, dataset] { return begun<kind_t>(database, table, dataset); });
}

}  // namespace miniverse::detail
