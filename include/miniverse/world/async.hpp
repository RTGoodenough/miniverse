#pragma once

#include <concepts>
#include <exception>
#include <future>
#include <type_traits>
#include <utility>

#include "schemacht/util/futures.hpp"

/**
 * @file
 * How a miniverse's calls become futures: work run on a thread of its own, a step guarded so that its throw settles the
 * future, and a future that holds an error.
 *
 * The pool's completions run on its two worker threads and must not wait on a future of the same pool (`get()`), which could
 * leave no worker free to complete it. So a call of several statements, or one whose rows take work to turn into the result
 * (a raster's window is stitched from its tiles), runs on a thread started for the call, `on_a_thread`, where it waits on each
 * statement in turn. Only a stream runs on the workers, since its chunks arrive on them.
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

/** @return A future that holds `error`. */
template <schemacht::util::FutureValue value_t>
[[nodiscard]] std::future<value_t> failed(const std::exception_ptr& error) {
  std::promise<value_t> promise;
  promise.set_exception(error);

  return promise.get_future();
}

/**
 * @return A future for what `work` gives, run on a thread started for it; it holds what `work` throws instead, or why no
 * thread could be started. The future waits for the thread if it is dropped before it is done.
 */
template <std::invocable work_t>
[[nodiscard]] std::future<std::invoke_result_t<work_t>> on_a_thread(work_t work) {
  try {
    return std::async(std::launch::async, std::move(work));
  } catch ( ... ) {
    return failed<std::invoke_result_t<work_t>>(std::current_exception());
  }
}

}  // namespace miniverse::detail
