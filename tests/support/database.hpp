#pragma once

// The scratch database the integration tests run on: its connection string, and a world whose tables are made fresh for a
// test and dropped afterwards.

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstdlib>
#include <string>
#include <utility>

namespace test {

/** @return The scratch database's connection string (MINIVERSE_TEST_DB); a test that asks for it is skipped without one. */
[[nodiscard]] inline std::string test_db() {
  const char* conninfo = std::getenv("MINIVERSE_TEST_DB");
  if ( conninfo == nullptr ) {
    SKIP("MINIVERSE_TEST_DB is not set");
  }

  return conninfo;
}

/**
 * @brief A world whose tables are made fresh and dropped afterwards: the world is made with `args` (what a `Miniverse`
 * constructor takes), its tables are dropped, and `fill` runs on it (to make the tables, and push what the test loads); when
 * this ends, the tables are dropped again. Returned from a factory as a prvalue, so it need not be movable.
 */
template <typename world_t>
  requires requires(world_t& world) { world.drop_tables(); }
class Fresh {
 public:
  template <std::invocable<world_t&> fill_t, typename... args_t>
    requires std::constructible_from<world_t, args_t...>
  explicit Fresh(fill_t fill, args_t&&... args) : _world(std::forward<args_t>(args)...) {
    _world.drop_tables();
    fill(_world);
  }

  [[nodiscard]] world_t& world() noexcept { return _world; }

 private:
  world_t _world;

 public:
  Fresh(const Fresh&) = delete;
  Fresh(Fresh&&) = delete;
  Fresh& operator=(const Fresh&) = delete;
  Fresh& operator=(Fresh&&) = delete;
  ~Fresh() {
    // A lost database must not end the run from a destructor, and the next test drops the tables first anyway.
    try {
      _world.drop_tables();
    } catch ( ... ) {  // NOLINT(bugprone-empty-catch)
    }
  }
};

}  // namespace test
