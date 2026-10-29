#include <iostream>
#include <type_traits>

#include "commands.hpp"
#include "layer_of.hpp"
#include "miniverse/world.hpp"
#include "models.hpp"

namespace tool {

void verify(const Table& table) {
  with_model(table.model, [&]<typename model_t>(std::type_identity<model_t> /*model*/) {
    miniverse::Miniverse world(table.conninfo, layer_of<model_t>(table.name));

    world.verify().get();

    std::cout << table.name << ": a table of " << table.model << ", as it should be\n";
  });
}

}  // namespace tool
