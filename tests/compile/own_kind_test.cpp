// A miniverse of a kind of your own needs only world.hpp, and the geo headers the kind uses: this file includes neither
// miniverse.hpp nor the built-in kinds, and world.hpp comes first, so it compiles on its own.

#include "miniverse/world.hpp"

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <string>

#include "support/places.hpp"

static_assert(std::same_as<decltype(miniverse::Miniverse(std::string(), miniverse::Layer<test::Places>("places"))), miniverse::Miniverse<test::Places>>);

TEST_CASE("a kind of your own needs only world.hpp", "[compile]") { SUCCEED("this file compiled against world.hpp alone"); }
