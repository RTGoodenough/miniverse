#pragma once

#include <concepts>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/world/async.hpp"
#include "miniverse/world/step_thread.hpp"
#include "schemacht/postgres/transaction.hpp"
#include "schemacht/schema/table.hpp"

/**
 * @file
 * A push run in steps: the steps, and the handle `Miniverse::begin_push` gives to run each on the push's own thread, which a
 * whole push runs in one go.
 */
namespace miniverse {

namespace detail {

/**
 * @brief A push while it runs: its transaction, how its rows are made, what it runs before it commits, and why it ended if a
 * step of it failed. A step runs on the thread that calls it and waits for each statement, so the push holds one statement's
 * text at a time; a step that fails rolls the transaction back, and every later step fails with its error.
 */
template <LayerKind kind_t>
struct Push {
  using Table = schemacht::schema::Table<typename kind_t::schema_type>;
  using ToRows = std::function<RowBatches<kind_t>(typename kind_t::result_type)>;
  using BeforeCommit = std::function<void(schemacht::postgres::Transaction&)>;

  schemacht::postgres::Transaction transaction;
  const Table*                     table = nullptr;
  ToRows                           to_rows;        ///< A part's data as the kind's batches: with the table's settings, or the dataset and stamp.
  BeforeCommit                     before_commit;  ///< A dataset's sweep; nothing, for most kinds.
  std::exception_ptr               failed;         ///< Why the push ended, if a step failed: what every later step throws.

  /** @brief Writes `data` as rows, one statement per batch, each waited for; a batch is let go of once it is written. */
  void add(typename kind_t::result_type data) {
    step([&] {
      RowBatches<kind_t> batches = to_rows(std::move(data));
      for ( RowsOf<kind_t>& batch : batches ) {
        const RowsOf<kind_t> rows = std::move(batch);

        std::ignore = transaction.execute(kind_t::write_statement(rows).on(*table)).get();
      }
    });
  }

  /** @brief Runs what comes before the commit, then commits: what was added is then there for everyone. */
  void commit() {
    step([&] {
      if ( before_commit ) {
        before_commit(transaction);
      }

      transaction.commit().get();
    });
  }

  /**
   * @brief Ends the push with `error`: the transaction is rolled back and waited for, unless it has ended already (a commit that
   * failed ended it), and every later step throws `error`. What the rollback says is beside the point: the error is the push's.
   */
  void end(const std::exception_ptr& error) noexcept {
    failed = error;

    try {
      if ( transaction.is_open() ) {
        transaction.rollback().wait();
      }
    } catch ( ... ) {  // NOLINT(bugprone-empty-catch) -- the ROLLBACK couldn't be queued: dropping the transaction rolls it back
    }
  }

 private:
  /** @brief Runs `run`, unless a step has failed already; one that throws ends the push with its error. */
  template <std::invocable run_t>
  void step(run_t run) {
    if ( failed ) {
      std::rethrow_exception(failed);
    }

    try {
      run();
    } catch ( ... ) {
      end(std::current_exception());
      throw;
    }
  }
};

}  // namespace detail

/**
 * @brief A push that takes its data in parts, all of them written or none: for data too large to hold at once, such as a file a
 * loader reads a chunk at a time. Made by `Miniverse::begin_push`.
 *
 * @code
 * auto buildings = world.begin_push<Buildings>(miniverse::Dataset{"buildings.gpkg"}).get();
 * miniverse::gdal::read_features<miniverse::geo::MultiPolygon>(source, 10000, [&](auto chunk) {
 *   buildings.add(std::move(chunk)).get();
 * });
 * buildings.commit().get();  // now it is all there, for everyone at once
 * @endcode
 *
 * It is one transaction: the parts are seen by no one else until `commit`, and one dropped without a commit writes nothing.
 * It holds one of the pool's connections from `begin_push` until it ends. A part that fails ends the push: it is rolled
 * back, and every later `add`, and the `commit`, fails with that part's error. A connection lost under the push ends it
 * the same way, with the error of the part that found it gone.
 *
 * The parts are made into rows and written on the push's own thread, in the order they are added: an `add` need not be
 * waited for before the next, or before the `commit`. Each `add` holds its part until the part is written, so a reader far
 * ahead of the writes holds that many parts; one that waits for the previous `add` before it reads the next holds two.
 * Dropping the push runs what was added, ends its thread, and waits for that. Commit or drop the push before the miniverse
 * ends. One that was moved from fails every call. For a kind with datasets the push replaces its dataset when it commits,
 * so a commit with no parts leaves the dataset empty.
 */
template <LayerKind kind_t>
class PushInParts {
 public:
  /** @brief The handle of `push`, a push that has begun, whose steps run on `thread`. */
  PushInParts(detail::StepThread thread, std::shared_ptr<detail::Push<kind_t>> push) : _push(std::move(push)), _thread(std::move(thread)) {}

  /** @return A future that completes once `data`, a part, is written into the push, or holds the error, which ends the push. */
  [[nodiscard]] std::future<void> add(kind_t::result_type data) {
    return step([push = _push, data = std::move(data)]() mutable { push->add(std::move(data)); });
  }

  /** @return A future that completes once every part added is there for everyone, or holds the error (nothing is then written). */
  [[nodiscard]] std::future<void> commit() {
    return step([push = _push] { push->commit(); });
  }

 private:
  std::shared_ptr<detail::Push<kind_t>> _push;    // null once moved from
  detail::StepThread                    _thread;  // after the push: its steps hold the push until they have run

  /** @return A future for `run`, a step of the push, posted to its thread; it holds the error if there is no push (moved from) or the step was lost. */
  template <std::invocable run_t>
  [[nodiscard]] std::future<void> step(run_t run) {
    if ( ! _push ) {
      return detail::failed<void>(std::make_exception_ptr(std::logic_error("this push in parts was moved from: it is the other one now")));
    }

    try {
      return _thread.post(std::move(run));
    } catch ( ... ) {
      return detail::failed<void>(std::current_exception());
    }
  }

 public:
  PushInParts(const PushInParts&) = delete;  // it is one transaction
  PushInParts(PushInParts&&) noexcept = default;
  PushInParts& operator=(const PushInParts&) = delete;
  PushInParts& operator=(PushInParts&&) noexcept = default;
  ~PushInParts() = default;
};

}  // namespace miniverse
