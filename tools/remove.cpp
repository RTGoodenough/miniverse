#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "commands.hpp"
#include "kinds.hpp"
#include "layer_of.hpp"
#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/dataset.hpp"
#include "miniverse/world.hpp"

namespace tool {

void remove(const Table& table, const std::string& dataset) {
  with_kind(table.kind, [&]<typename kind_t>(std::type_identity<kind_t> /*kind*/) {
    if constexpr ( miniverse::HasDatasets<kind_t> ) {
      miniverse::Miniverse world(table.conninfo, layer_of<kind_t>(table.name));

      world.verify().get();
      const std::uint64_t removed = world.template remove<kind_t>(miniverse::Dataset{dataset}).get();
      if ( removed == 0 ) {
        throw std::runtime_error(table.name + " has no dataset '" + dataset + "': nothing is removed");
      }

      std::cout << table.name << ": the dataset '" << dataset << "' is removed, " << removed << (removed == 1 ? " row\n" : " rows\n");

    } else {
      throw std::invalid_argument("a table of " + table.kind + " has no datasets to remove");
    }
  });
}

}  // namespace tool
