#include <iostream>
#include <type_traits>

#include "commands.hpp"
#include "kinds.hpp"
#include "layer_of.hpp"
#include "miniverse/world.hpp"

namespace tool {

void verify(const Table& table) {
  with_kind(table.kind, [&]<typename kind_t>(std::type_identity<kind_t> /*kind*/) {
    miniverse::Miniverse world(table.conninfo, layer_of<kind_t>(table.name));

    world.verify().get();

    std::cout << table.name << ": a table of " << table.kind << ", as it should be\n";
  });
}

}  // namespace tool
