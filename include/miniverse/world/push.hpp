#pragma once

#include <exception>
#include <future>
#include <utility>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/world/async.hpp"
#include "miniverse/world/push_in_parts.hpp"
#include "miniverse/world/settings.hpp"
#include "miniverse/world/write_batches.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/postgres/transaction.hpp"
#include "schemacht/schema/table.hpp"
#include "schemacht/util/futures.hpp"

/**
 * @file
 * Pushes to a layer's table: a whole dataset in one transaction of its own, or the beginning of one that takes its data in
 * parts. For a kind with settings, the table's are read first and the rows are made with them. What `Miniverse::push` and
 * `begin_push` do for a layer that is a table. Each keeps `database` and `table` for the steps that run after it returns:
 * both must outlive the future, as the miniverse's do.
 */
namespace miniverse::detail {

/** @brief Writes `batches` to `table` in one transaction of their own on `database`, and settles `done` with the outcome. */
template <LayerKind kind_t>
void write_rows(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table, RowBatches<kind_t> batches,
    const schemacht::util::Settler<void>& done
) {
  if ( batches.empty() ) {
    done(nullptr);  // nothing to write
    return;
  }

  database.begin([batches = std::move(batches), done,
                  table = &table](schemacht::postgres::Transaction transaction, const std::exception_ptr& error) mutable {
    if ( error ) {
      done(error);
      return;
    }

    guarded(done, [&] { write_batches<kind_t>(push_begun(std::move(transaction)), *table, std::move(batches), done, AfterWrite::Commit); });
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

    guarded(done, [&] { done(PushInParts<kind_t>(std::move(transaction), *table, std::move(to_rows)), nullptr); });
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
