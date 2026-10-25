// The thread a push in parts runs on: its steps run in order on it, what a step throws settles the step's future, and
// dropping it runs what was posted first.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

#include "miniverse/world/step_thread.hpp"

using miniverse::detail::StepThread;

namespace {

constexpr std::chrono::milliseconds A_WHILE{20};

}  // namespace

TEST_CASE("step thread: the steps run in the order posted, on one thread that is not the caller's", "[step_thread]") {
  std::vector<int>             order;
  std::vector<std::thread::id> ran_on;

  {
    StepThread                     thread;
    std::vector<std::future<void>> done;
    done.reserve(5);
    for ( int step = 0; step < 5; ++step ) {
      done.push_back(thread.post([&, step] {
        order.push_back(step);
        ran_on.push_back(std::this_thread::get_id());
      }));
    }

    for ( std::future<void>& each : done ) {
      each.get();
    }
  }

  CHECK(order == std::vector<int>{0, 1, 2, 3, 4});
  REQUIRE(ran_on.size() == 5);
  CHECK(ran_on[0] != std::this_thread::get_id());
  CHECK(std::vector<std::thread::id>(5, ran_on[0]) == ran_on);
}

TEST_CASE("step thread: a step that throws settles its future with the error, and the next step still runs", "[step_thread]") {
  StepThread thread;

  std::future<void> failing = thread.post([] { throw std::runtime_error("this step's own error"); });
  std::future<void> next = thread.post([] {});

  CHECK_THROWS_WITH(failing.get(), "this step's own error");
  CHECK_NOTHROW(next.get());
}

TEST_CASE("step thread: one dropped with steps queued runs them all first", "[step_thread]") {
  int               ran = 0;
  std::future<void> last;

  {
    StepThread thread;
    std::ignore = thread.post([] { std::this_thread::sleep_for(A_WHILE); });  // the rest are still queued when the thread is dropped
    std::ignore = thread.post([&] { ++ran; });
    std::ignore = thread.post([&] { ++ran; });
    last = thread.post([&] { ++ran; });
  }

  CHECK(ran == 3);
  CHECK(last.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
}

TEST_CASE("step thread: one assigned over another runs the other's steps first", "[step_thread]") {
  int ran = 0;

  StepThread thread;
  std::ignore = thread.post([] { std::this_thread::sleep_for(A_WHILE); });
  std::ignore = thread.post([&] { ++ran; });

  thread = StepThread();
  CHECK(ran == 1);

  std::future<void> on_the_new_one = thread.post([&] { ++ran; });
  CHECK_NOTHROW(on_the_new_one.get());
  CHECK(ran == 2);
}
