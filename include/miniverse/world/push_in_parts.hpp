#pragma once

#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <utility>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/world/async.hpp"
#include "miniverse/world/write_batches.hpp"
#include "schemacht/schema/table.hpp"
#include "schemacht/util/futures.hpp"

/**
 * @file
 * A push that takes its data in parts inside one transaction: what `Miniverse::begin_push` gives.
 */
namespace miniverse {

/**
 * @brief A push that takes its data in parts, all of them written or none: for data too large to hold at once, such as a file a
 * loader reads a chunk at a time. Made by `Miniverse::begin_push`.
 *
 * @code
 * auto buildings = world.begin_push<Buildings>().get();
 * miniverse::gdal::read_features<miniverse::geo::MultiPolygon>(source, 10000, [&](auto chunk) {
 *   buildings.add(std::move(chunk)).get();
 * });
 * buildings.commit().get();  // now it is all there, for everyone at once
 * @endcode
 *
 * It is one transaction. The parts added are seen by no one else until `commit`, and one dropped without a commit writes
 * nothing. It holds one of the pool's connections from `begin_push` until it ends. A part that fails, for whatever reason,
 * ends the push: it is rolled back, so nothing of it is written, and every later `add`, and the `commit`, fails with that
 * part's error.
 *
 * One call at a time: wait for an `add`'s future before the next `add` or the `commit`. Wait for every `add` before the
 * miniverse ends. One that was moved from fails every call. For a kind with datasets, the push is of one dataset, which it
 * replaces when it commits: the dataset's rows that no part wrote are deleted then, so a commit with no parts leaves the
 * dataset empty, as a push of no data does (see `Miniverse::begin_push`).
 */
template <LayerKind kind_t>
class PushInParts {
 public:
  using ToRows = std::function<RowBatches<kind_t>(typename kind_t::result_type)>;

  /**
   * @brief The push whose transaction, begun on `table`'s database, `state` holds, with what it runs before it commits (a
   * dataset's sweep; nothing, for most kinds), and whose parts' rows `to_rows` makes. What `Miniverse::begin_push` makes one
   * with: a program has no call to.
   */
  PushInParts(std::shared_ptr<detail::PushState> state, const schemacht::schema::Table<typename kind_t::schema_type>& table, ToRows to_rows)
      : _state(std::move(state)), _table(&table), _to_rows(std::move(to_rows)) {}

  /**
   * @return A future that completes once `data`, a part, is written into the push, or holds the error, which ends the push. The
   * rows are made by the kind on the calling thread (for a kind with settings, with the table's, read when the push began),
   * and written in the kind's batches, one statement each.
   */
  [[nodiscard]] std::future<void> add(kind_t::result_type data) {
    return schemacht::util::future_from<void>([&](const schemacht::util::Settler<void>& done) {
      if ( const std::exception_ptr ended = ended_by(); ended ) {
        done.fail(ended);
        return;
      }

      try {
        detail::write_batches<kind_t>(_state, *_table, _to_rows(std::move(data)), done, detail::AfterWrite::GoOn);
      } catch ( ... ) {
        detail::end_push(_state, done, std::current_exception());  // the part's rows could not be made: it can't be left out of the whole
      }
    });
  }

  /** @return A future that completes once every part added is there for everyone, or holds the error (nothing is then written). */
  [[nodiscard]] std::future<void> commit() {
    return schemacht::util::future_from<void>([&](const schemacht::util::Settler<void>& done) {
      if ( const std::exception_ptr ended = ended_by(); ended ) {
        done.fail(ended);
        return;
      }

      detail::commit_push(_state, done);
    });
  }

 private:
  using Table = schemacht::schema::Table<typename kind_t::schema_type>;

  std::shared_ptr<detail::PushState> _state;  // null once moved from
  const Table*                       _table;
  ToRows                             _to_rows;

  /** @return Why this push takes no more calls, if it doesn't: a part of it failed, or it was moved from. */
  [[nodiscard]] std::exception_ptr ended_by() const {
    if ( ! _state ) {
      return std::make_exception_ptr(std::logic_error("this push in parts was moved from: it is the other one now"));
    }

    return _state->failed;
  }

 public:
  PushInParts(const PushInParts&) = delete;  // it is one transaction
  PushInParts(PushInParts&&) noexcept = default;
  PushInParts& operator=(const PushInParts&) = delete;
  PushInParts& operator=(PushInParts&&) noexcept = default;
  ~PushInParts() = default;  // not committed: rolled back
};

}  // namespace miniverse
