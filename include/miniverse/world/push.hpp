#pragma once

#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <utility>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/dataset.hpp"
#include "miniverse/world/async.hpp"
#include "miniverse/world/datasets.hpp"
#include "miniverse/world/push_in_parts.hpp"
#include "miniverse/world/settings.hpp"
#include "miniverse/world/write_batches.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/postgres/transaction.hpp"
#include "schemacht/schema/table.hpp"
#include "schemacht/util/futures.hpp"

/**
 * @file
 * Pushes to a layer's table: all of a push's data in one transaction of its own, or the beginning of one that takes its data
 * in parts. For a kind with settings, the table's are read first and the rows are made with them; for a kind with datasets,
 * the dataset is locked, the rows are stamped, and the dataset's rows the push did not write are swept away before the commit
 * (datasets.hpp), so that the push replaces the dataset. What `Miniverse::push` and `begin_push` do for a layer that is a
 * table. Each keeps `database` and `table` for the steps that run after it returns: both must outlive the future, as the
 * miniverse's do.
 */
namespace miniverse::detail {

/**
 * @brief Writes `batches` to `table` in one transaction of their own on `database`, running `before_commit` (if any) once they
 * are written, and settles `done` with the outcome. With no batches and nothing to run before the commit, nothing is sent.
 */
template <LayerKind kind_t>
void write_rows(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table, RowBatches<kind_t> batches,
    const schemacht::util::Settler<void>& done, BeforeCommit before_commit = {}
) {
  if ( batches.empty() && ! before_commit ) {
    done(nullptr);  // nothing to write
    return;
  }

  database.begin([batches = std::move(batches), done, table = &table,
                  before_commit = std::move(before_commit)](schemacht::postgres::Transaction transaction, const std::exception_ptr& error) mutable {
    if ( error ) {
      done(error);
      return;
    }

    guarded(done, [&] {
      write_batches<kind_t>(push_begun(std::move(transaction), std::move(before_commit)), *table, std::move(batches), done, AfterWrite::Commit);
    });
  });
}

/** @brief Begins a transaction on `database`, and settles `done` with the push in parts of `table` it is, whose rows `to_rows` makes. */
template <LayerKind kind_t>
void begin_parts(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table,
    typename PushInParts<kind_t>::ToRows to_rows, const schemacht::util::Settler<PushInParts<kind_t>>& done
) {
  database.begin([to_rows = std::move(to_rows), done,
                  table = &table](schemacht::postgres::Transaction transaction, const std::exception_ptr& error) mutable {
    if ( error ) {
      done.fail(error);
      return;
    }

    guarded(done, [&] { done(PushInParts<kind_t>(push_begun(std::move(transaction)), *table, std::move(to_rows)), nullptr); });
  });
}

/**
 * @return A future that completes once `data` is written to `table`, or holds the error. The rows are made by the kind: for
 * a kind with settings, with the table's, read first, on a thread of the pool's; for a kind without, on the calling thread.
 * See `Miniverse::push`.
 */
template <LayerKind kind_t>
[[nodiscard]] std::future<void> push(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table, typename kind_t::result_type data
) {
  return schemacht::util::future_from<void>([&](const schemacht::util::Settler<void>& done) {
    if constexpr ( HasSettings<kind_t> ) {
      guarded(done, [&] {
        database.execute(
            settings_statement<kind_t>(table.name()),
            [&database, &table, done, data = std::move(data)](SettingsRows<kind_t> rows, const std::exception_ptr& error) mutable {
              if ( error ) {
                done(error);
                return;
              }

              guarded(done, [&] {
                write_rows<kind_t>(database, table, kind_t::to_rows(std::move(data), kind_t::settings_from_rows(std::move(rows))), done);
              });
            }
        );
      });

    } else {
      guarded(done, [&] { write_rows<kind_t>(database, table, kind_t::to_rows(std::move(data)), done); });
    }
  });
}

/**
 * @return A future that completes once `data` replaces `dataset` in `table`, or holds the error. The rows are made by the kind
 * on the calling thread and stamped; then, in one transaction, the dataset is locked, the rows written, and the dataset's
 * other rows swept. See `Miniverse::push`.
 */
template <HasDatasets kind_t>
[[nodiscard]] std::future<void> push(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table, const Dataset& dataset,
    typename kind_t::result_type data
) {
  return schemacht::util::future_from<void>([&](const schemacht::util::Settler<void>& done) {
    guarded(done, [&] {
      const std::int64_t marked = stamp();
      RowBatches<kind_t> batches = kind_t::to_rows(std::move(data), dataset, marked);

      database.begin([batches = std::move(batches), done, table = &table, dataset,
                      marked](schemacht::postgres::Transaction transaction, const std::exception_ptr& error) mutable {
        if ( error ) {
          done(error);
          return;
        }

        guarded(done, [&] {
          const std::shared_ptr<PushState> state = push_begun(std::move(transaction), sweep_of<kind_t>(*table, dataset, marked));
          lock_dataset(state, dataset, [state, table, batches = std::move(batches), done](const std::exception_ptr& locking) mutable {
            if ( locking ) {
              end_push(state, done, locking);
              return;
            }

            write_batches<kind_t>(state, *table, std::move(batches), done, AfterWrite::Commit);
          });
        });
      });
    });
  });
}

/**
 * @return A future for a push of `dataset` into `table` that takes its data in parts, and replaces the dataset when it commits:
 * its transaction is begun and the dataset locked. It holds the error instead if either fails. See `Miniverse::begin_push`.
 */
template <HasDatasets kind_t>
[[nodiscard]] std::future<PushInParts<kind_t>> begin_push(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table, const Dataset& dataset
) {
  return schemacht::util::future_from<PushInParts<kind_t>>([&](const schemacht::util::Settler<PushInParts<kind_t>>& done) {
    guarded(done, [&] {
      const std::int64_t marked = stamp();

      database.begin([table = &table, dataset, marked, done](schemacht::postgres::Transaction transaction, const std::exception_ptr& error) mutable {
        if ( error ) {
          done.fail(error);
          return;
        }

        guarded(done, [&] {
          const std::shared_ptr<PushState> state = push_begun(std::move(transaction), sweep_of<kind_t>(*table, dataset, marked));
          lock_dataset(state, dataset, [state, table, dataset, marked, done](const std::exception_ptr& locking) {
            if ( locking ) {
              done.fail(locking);  // the transaction goes with the state, which rolls it back
              return;
            }

            guarded(done, [&] {
              done(
                  PushInParts<kind_t>(
                      state, *table,
                      [dataset, marked](typename kind_t::result_type data) { return kind_t::to_rows(std::move(data), dataset, marked); }
                  ),
                  nullptr
              );
            });
          });
        });
      });
    });
  });
}

/**
 * @return A future for a push of `table` that takes its data in parts. For a kind with settings, the table's are read first,
 * once, and every part's rows are made with them. It holds the error instead if they can't be read, or no transaction
 * begun. See `Miniverse::begin_push`.
 */
template <LayerKind kind_t>
[[nodiscard]] std::future<PushInParts<kind_t>> begin_push(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table
) {
  return schemacht::util::future_from<PushInParts<kind_t>>([&](const schemacht::util::Settler<PushInParts<kind_t>>& done) {
    if constexpr ( HasSettings<kind_t> ) {
      guarded(done, [&] {
        database.execute(
            settings_statement<kind_t>(table.name()), [&database, &table, done](SettingsRows<kind_t> rows, const std::exception_ptr& error) {
              if ( error ) {
                done.fail(error);
                return;
              }

              guarded(done, [&] {
                begin_parts<kind_t>(
                    database, table,
                    [settings = kind_t::settings_from_rows(std::move(rows))](typename kind_t::result_type data) {
                      return kind_t::to_rows(std::move(data), settings);
                    },
                    done
                );
              });
            }
        );
      });

    } else {
      guarded(done, [&] {
        begin_parts<kind_t>(database, table, [](typename kind_t::result_type data) { return kind_t::to_rows(std::move(data)); }, done);
      });
    }
  });
}

}  // namespace miniverse::detail
