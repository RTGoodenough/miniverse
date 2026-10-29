#include <chrono>
#include <format>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "commands.hpp"
#include "layer_of.hpp"
#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/world.hpp"
#include "miniverse/world/registry.hpp"
#include "models.hpp"

namespace tool {

void datasets(const Table& table) {
  with_model(table.model, [&]<typename model_t>(std::type_identity<model_t> /*model*/) {
    if constexpr ( miniverse::WritesDatasets<model_t> ) {
      miniverse::Miniverse world(table.conninfo, layer_of<model_t>(table.name));

      world.verify().get();
      const std::vector<miniverse::DatasetInfo> listed = world.template datasets<"table">().get();
      if ( listed.empty() ) {
        std::cout << table.name << ": no datasets\n";
      }
      for ( const miniverse::DatasetInfo& dataset : listed ) {
        std::cout << dataset.name << '\t' << dataset.rows
                  << (dataset.rows == 1 ? " row\t" : " rows\t")
                  // NOLINTNEXTLINE(misc-include-cleaner) -- <chrono> is included
                  << std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(dataset.pushed)) << '\n';
      }

    } else {
      throw std::invalid_argument("a table of " + table.model + " has no datasets to list");
    }
  });
}

}  // namespace tool
