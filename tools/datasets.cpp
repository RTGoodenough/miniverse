#include <chrono>
#include <format>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "commands.hpp"
#include "kinds.hpp"
#include "layer_of.hpp"
#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/world.hpp"
#include "miniverse/world/registry.hpp"

namespace tool {

void datasets(const Table& table) {
  with_kind(table.kind, [&]<typename kind_t>(std::type_identity<kind_t> /*kind*/) {
    if constexpr ( miniverse::HasDatasets<kind_t> ) {
      miniverse::Miniverse world(table.conninfo, layer_of<kind_t>(table.name));

      world.verify().get();
      const std::vector<miniverse::DatasetInfo> listed = world.template datasets<kind_t>().get();
      if ( listed.empty() ) {
        std::cout << table.name << ": no datasets\n";
      }
      for ( const miniverse::DatasetInfo& dataset : listed ) {
        std::cout << dataset.name << '\t' << dataset.rows << (dataset.rows == 1 ? " row\t" : " rows\t")
                  << std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(dataset.pushed)) << '\n';
      }

    } else {
      throw std::invalid_argument("a table of " + table.kind + " has no datasets to list");
    }
  });
}

}  // namespace tool
