#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include "commands.hpp"
#include "layer_of.hpp"
#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/world.hpp"
#include "models.hpp"

namespace tool {

void settings(const Table& table)
{
  with_model(table.model, [&]<typename model_t>(std::type_identity<model_t> /*model*/) {
    if constexpr ( miniverse::WritesSettings<model_t> ) {
      miniverse::Miniverse world(table.conninfo, layer_of<model_t>(table.name));

      const miniverse::SettingsOf<model_t> grid = world.template table_settings<"table">().get();

      std::cout << "pixels per degree: " << grid.pixels_per_degree << " (about "
                << grid.metres_per_pixel() << " m)\n"
                << "tile pixels: " << grid.tile_pixels << '\n';

      // In full, so that it can be given to another create; whole numbers of one byte as numbers,
      // not letters.
      using pixel_type = decltype(grid.nodata);
      std::cout << "nodata: " << std::setprecision(std::numeric_limits<pixel_type>::max_digits10)
                << +grid.nodata << '\n';
    }
    else {
      throw std::invalid_argument(
          "only a raster table has settings: a table of " + table.model + " has none");
    }
  });
}

}  // namespace tool
