// A miniverse of a model of your own needs only world.hpp, and the geo headers the model uses: this
// file includes neither miniverse.hpp nor the built-in models, and world.hpp comes first, so it
// compiles on its own.

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <string>

#include "miniverse/world.hpp"
#include "support/places.hpp"

static_assert(std::same_as<decltype(miniverse::Miniverse(
                               std::string(), miniverse::Layer<test::Places, "places">("places"))),
                           miniverse::Miniverse<miniverse::Layer<test::Places, "places">>>);

TEST_CASE("a model of your own needs only world.hpp", "[compile]")
{
  SUCCEED("this file compiled against world.hpp alone");
}
