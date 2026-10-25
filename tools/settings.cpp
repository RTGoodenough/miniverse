#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include "commands.hpp"
#include "kinds.hpp"
#include "layer_of.hpp"
#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/world.hpp"

namespace tool {

void settings(const Table& table) {
  with_kind(table.kind, [&]<typename kind_t>(std::type_identity<kind_t> /*kind*/) {
    if constexpr ( miniverse::HasSettings<kind_t> ) {
      miniverse::Miniverse world(table.conninfo, layer_of<kind_t>(table.name));

      const typename kind_t::settings_type grid = world.template table_settings<kind_t>().get();

      std::cout << "pixels per degree: " << grid.pixels_per_degree << " (about " << grid.metres_per_pixel() << " m)\n"
                << "tile pixels: " << grid.tile_pixels << '\n';

      // In full, so that it can be given to another create; whole numbers of one byte as numbers, not letters.
      using pixel_type = decltype(grid.nodata);
      std::cout << "nodata: " << std::setprecision(std::numeric_limits<pixel_type>::max_digits10) << +grid.nodata << '\n';

    } else {
      throw std::invalid_argument("only a raster table has settings: a table of " + table.kind + " has none");
    }
  });
}

}  // namespace tool
