#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <utility>

#include "miniverse/concepts/miniverse.hpp"
#include "schemacht/postgres/transaction.hpp"
#include "schemacht/schema/table.hpp"
#include "schemacht/util/futures.hpp"

/**
 * @file
 * Writing a kind's batches into a transaction, one statement at a time, each completion sending the next: what a whole push
 * and each part of a `PushInParts` share, with the state of the push they write into and how a failed batch ends it.
 */
namespace miniverse::detail {

/** @brief What a write does once its last batch is written: commit its transaction, or leave it open for the next part. */
enum class AfterWrite : std::uint8_t {
  Commit,
  GoOn,
};

/** @brief A push while it runs, shared with each write of it: its transaction, and why it ended if a part has failed. */
struct PushState {
  schemacht::postgres::Transaction transaction;
  std::exception_ptr               failed;  ///< Set before the transaction is rolled back; what every later call gets.
};

/** @return The state of a push begun with `transaction`. */
[[nodiscard]] inline std::shared_ptr<PushState> push_begun(schemacht::postgres::Transaction transaction) {
  return std::make_shared<PushState>(PushState{.transaction = std::move(transaction), .failed = nullptr});
}

/**
 * @brief Ends the push `state` is: notes `error` as why, rolls its transaction back, and settles `done` with `error`, the
 * part's own, whatever the rollback says.
 */
inline void end_push(const std::shared_ptr<PushState>& state, const schemacht::util::Settler<void>& done, const std::exception_ptr& error) {
  state->failed = error;  // before the rollback: whoever `done` wakes then finds the push ended, and leaves the transaction be

  try {
    state->transaction.rollback([state, done, error](const std::exception_ptr& /*rolled_back*/) { done.fail(error); });
  } catch ( ... ) {
    done.fail(error);  // the rollback couldn't be sent: dropping the transaction rolls it back
  }
}

/** @brief A write of batches while it runs: its batches, sent one statement at a time inside the push's transaction. */
template <LayerKind kind_t>
struct BatchWrite {
  RowBatches<kind_t>                                            batches;
  std::shared_ptr<PushState>                                    state;
  schemacht::util::Settler<void>                                done;
  const schemacht::schema::Table<typename kind_t::schema_type>* table = nullptr;
  AfterWrite                                                    then = AfterWrite::GoOn;
  std::size_t                                                   sent = 0;  ///< Batches written so far; the next to send.
};

/**
 * @brief Sends `write`'s next batch, whose completion sends the one after, and so on; then commits, or goes on. One batch at a
 * time is made into a statement's text, on a thread of the pool's, and the batch is let go of once it is. A batch that
 * fails, on the server or before it gets there, ends the push.
 */
template <LayerKind kind_t>
void send_next(std::shared_ptr<BatchWrite<kind_t>> write) {
  BatchWrite<kind_t>& running = *write;
  try {
    if ( running.sent == running.batches.size() ) {
      if ( running.then == AfterWrite::Commit ) {
        running.state->transaction.commit(running.done);
      } else {
        running.done(nullptr);
      }

      return;
    }

    const RowsOf<kind_t> batch = std::move(running.batches.at(running.sent));  // the statement copies it, so it ends with this step
    ++running.sent;
    running.state->transaction.execute(
        kind_t::write_statement(batch).on(*running.table), [write](std::uint64_t /*count*/, const std::exception_ptr& error) mutable {
          if ( error ) {
            end_push(write->state, write->done, error);
            return;
          }

          send_next(std::move(write));
        }
    );
  } catch ( ... ) {
    end_push(running.state, running.done, std::current_exception());
  }
}

/**
 * @brief Writes `batches` to `table` inside `state`'s transaction, one statement at a time, and settles `done` with the
 * outcome: once the last is written (and, for `AfterWrite::Commit`, the transaction committed), or with the error of the
 * batch that failed, which ends the push.
 */
template <LayerKind kind_t>
void write_batches(
    const std::shared_ptr<PushState>& state, const schemacht::schema::Table<typename kind_t::schema_type>& table, RowBatches<kind_t> batches,
    const schemacht::util::Settler<void>& done, AfterWrite then
) {
  send_next(
      std::make_shared<BatchWrite<kind_t>>(
          BatchWrite<kind_t>{.batches = std::move(batches), .state = state, .done = done, .table = &table, .then = then}
      )
  );
}

}  // namespace miniverse::detail
