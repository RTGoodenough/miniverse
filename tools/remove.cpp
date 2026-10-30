#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "commands.hpp"
#include "layer_of.hpp"
#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/dataset.hpp"
#include "miniverse/world.hpp"
#include "models.hpp"

namespace tool {

void remove(const Table& table, const std::string& dataset)
{
  with_model(table.model, [&]<typename model_t>(std::type_identity<model_t> /*model*/) {
    if constexpr ( miniverse::WritesDatasets<model_t> ) {
      miniverse::Miniverse world(table.conninfo, layer_of<model_t>(table.name));

      world.verify().get();
      const std::uint64_t removed =
          world.template remove<"table">(miniverse::Dataset{dataset}).get();
      if ( removed == 0 ) {
        throw std::runtime_error(
            table.name + " has no dataset '" + dataset + "': nothing is removed");
      }

      std::cout << table.name << ": the dataset '" << dataset << "' is removed, " << removed
                << (removed == 1 ? " row\n" : " rows\n");
    }
    else {
      throw std::invalid_argument("a table of " + table.model + " has no datasets to remove");
    }
  });
}

}  // namespace tool
