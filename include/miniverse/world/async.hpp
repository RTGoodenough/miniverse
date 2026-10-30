#pragma once

#include <concepts>
#include <exception>
#include <future>
#include <type_traits>
#include <utility>

#include "schemacht/util/futures.hpp"

/**
 * @file
 * How a miniverse's calls become futures: work run on a thread of its own, and a step guarded so that its throw settles the
 * future.
 *
 * The pool's completions run on its two worker threads and must not wait on a future of the same pool (`get()`), which could
 * leave no worker free to complete it. So a call of several statements, or one whose rows take work to turn into the result
 * (a raster's window is stitched from its tiles), runs on a thread started for the call, `on_a_thread`, where it waits on each
 * statement in turn. Only a stream runs on the workers, since its chunks arrive on them.
 *
 * Errors: what the work finds wrong arrives through the future, never from the call. A call that cannot start throws, as
 * schemacht's do: no thread to run it on (`std::system_error`), no memory for it (`std::bad_alloc`), or a call that makes no
 * sense (`std::logic_error`: a write to a layer that is only read, a push in parts that was moved from).
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
 * @return A future for what `work` gives, run on a thread started for it; it holds what `work` throws instead. The future waits
 * for the thread if it is dropped before it is done.
 * @throws std::system_error if no thread can be started.
 */
template <std::invocable work_t>
[[nodiscard]] std::future<std::invoke_result_t<work_t>> on_a_thread(work_t work) {
  return std::async(std::launch::async, std::move(work));
}

}  // namespace miniverse::detail
