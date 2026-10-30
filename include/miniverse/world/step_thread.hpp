#pragma once

#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "schemacht/util/futures.hpp"
#include "schemacht/util/move_only_function.hpp"

/**
 * @file
 * A thread that runs the steps posted to it, in order: what a push in parts runs on.
 */
namespace miniverse::detail {

/**
 * @brief A thread that runs the steps posted to it one after another, in the order they were posted, and ends once it is
 * dropped and has run every step it was given. For work that arrives in calls, as the parts of a push do; work known up
 * front runs on `on_a_thread`.
 *
 * Dropping it waits for the thread, so it is dropped from another thread than its own: no step may own it. One that was
 * moved from holds no thread.
 */
class StepThread {
 public:
  /** @brief Starts the thread. @throws std::system_error if no thread can be started. */
  StepThread() : _state(std::make_shared<State>()) {
    _state->thread = std::thread([state = _state] { run_steps(*state); });
  }

  /**
   * @return A future that completes once `step` has run, after the steps posted before it, or holds what it threw.
   * @throws std::bad_alloc if the step could not be queued, and then the same from every later post: the step is lost, and the
   * thread takes no step that would run after it, so a commit never runs after a part that was lost.
   */
  template <std::invocable step_t>
  [[nodiscard]] std::future<void> post(step_t step) {
    try {
      return schemacht::util::future_from<void>([&](const schemacht::util::Settler<void>& done) {
        queue([step = std::move(step), done]() mutable {
          std::exception_ptr error;
          try {
            step();
          } catch ( ... ) {
            error = std::current_exception();
          }

          done(error);
        });
      });
    } catch ( ... ) {
      const std::lock_guard lock(_state->mutex);
      _state->lost = std::current_exception();
      throw;
    }
  }

 private:
  struct State {
    std::mutex                                            mutex;
    std::condition_variable                               wake;
    std::deque<schemacht::util::MoveOnlyFunction<void()>> steps;
    bool                                                  closed = false;
    std::exception_ptr                                    lost;  ///< Why a step could not be queued, once one could not.
    std::thread                                           thread;
  };

  std::shared_ptr<State> _state;  // null once moved from; the thread holds its own share

  /** @brief Queues `step` for the thread, unless a step was lost: then its error is thrown again. */
  void queue(schemacht::util::MoveOnlyFunction<void()> step) {
    {
      const std::lock_guard lock(_state->mutex);
      if ( _state->lost ) {
        std::rethrow_exception(_state->lost);
      }

      _state->steps.push_back(std::move(step));
    }

    _state->wake.notify_one();
  }

  /** @brief Ends the thread: no more steps are taken, those queued are run, and the thread is waited for. */
  void end() noexcept {
    if ( ! _state ) {
      return;
    }

    {
      const std::lock_guard lock(_state->mutex);
      _state->closed = true;
    }
    _state->wake.notify_one();

    _state->thread.join();
    _state.reset();
  }

  /** @brief What the thread runs: each step as it comes, until it is closed and none is left. */
  static void run_steps(State& state) {
    for ( ;; ) {
      schemacht::util::MoveOnlyFunction<void()> step;
      {
        std::unique_lock lock(state.mutex);
        state.wake.wait(lock, [&] { return state.closed || ! state.steps.empty(); });
        if ( state.steps.empty() ) {
          return;
        }

        step = std::move(state.steps.front());
        state.steps.pop_front();
      }

      step();
    }
  }

 public:
  StepThread(const StepThread&) = delete;
  StepThread(StepThread&&) noexcept = default;
  StepThread& operator=(const StepThread&) = delete;
  StepThread& operator=(StepThread&& other) noexcept {
    if ( this != &other ) {
      end();
      _state = std::move(other._state);
    }

    return *this;
  }
  ~StepThread() { end(); }
};

}  // namespace miniverse::detail
