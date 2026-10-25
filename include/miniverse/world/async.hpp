#pragma once

#include <concepts>
#include <exception>
#include <future>
#include <type_traits>
#include <utility>
#include <vector>

#include "schemacht/postgres/database.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/util/futures.hpp"

/**
 * @file
 * How a miniverse's calls become futures: a step of one guarded so that its throw settles the future, work run on a thread
 * of its own, and a statement run on the pool whose rows are converted into the future's value.
 */
namespace miniverse::detail {

/** @brief Runs `step`; what it throws settles `done` with the error instead. For the steps of a call that returns a future. */
template <schemacht::util::FutureValue value_t, std::invocable step_t>
void guarded(const schemacht::util::Settler<value_t>& done, step_t step) {
  try {
    step();
  } catch ( ... ) {
    done.fail(std::current_exception());
  }
}

/**
 * @return A future for what `work` gives, run on a thread started for it (a reader's load, a push); it holds what `work`
 * throws instead, or why no thread could be started. The future waits for the thread if it is dropped before it is done.
 */
template <std::invocable work_t>
[[nodiscard]] std::future<std::invoke_result_t<work_t>> on_a_thread(work_t work) {
  try {
    return std::async(std::launch::async, std::move(work));
  } catch ( ... ) {
    std::promise<std::invoke_result_t<work_t>> failed;
    failed.set_exception(std::current_exception());

    return failed.get_future();
  }
}

/** @brief The rows a statement gives. */
template <schemacht::postgres::RunnableStatement statement_t>
using ResultRows = std::vector<typename statement_t::result_type>;

/**
 * @return A future for the rows of the statement `make()` gives, run on `database`, made into a `result_t` by `convert`. It
 * holds the error instead if the statement could not be made or run, or `convert` threw.
 */
template <schemacht::util::FutureValue result_t, std::invocable make_t, std::invocable<ResultRows<std::invoke_result_t<make_t>>> convert_t>
[[nodiscard]] std::future<result_t> execute_as(schemacht::postgres::Database& database, make_t make, convert_t convert) {
  return schemacht::util::future_from<result_t>([&](const schemacht::util::Settler<result_t>& done) {
    guarded(done, [&] {  // the statement may not get made: an argument could not be bound
      database.execute(make(), [done, convert = std::move(convert)](ResultRows<std::invoke_result_t<make_t>> rows, const std::exception_ptr& error) {
        if ( error ) {
          done.fail(error);
          return;
        }

        guarded(done, [&] { done(convert(std::move(rows)), nullptr); });
      });
    });
  });
}

}  // namespace miniverse::detail
