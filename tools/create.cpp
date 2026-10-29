#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>

#include "commands.hpp"
#include "layer_of.hpp"
#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/geo/concepts/raster.hpp"
#include "miniverse/geo/grid.hpp"
#include "miniverse/world.hpp"
#include "models.hpp"

namespace tool {

namespace {

/** @return `text` as a pixel of `pixel_t`. @throws std::invalid_argument if it is not one. */
template <geo::Pixel pixel_t>
[[nodiscard]] pixel_t pixel_of(const std::string& text) {
  pixel_t     value{};
  const char* end = text.data() + text.size();  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) -- from_chars takes a range
  const auto [stop, error] = std::from_chars(text.data(), end, value);
  if ( error != std::errc{} || stop != end ) {
    throw std::invalid_argument("'" + text + "' is not a value a pixel of this table can hold");
  }

  return value;
}

/** @return `options` as the grid of a raster of `pixel_t`. @throws std::invalid_argument for options that make none. */
template <geo::Pixel pixel_t>
[[nodiscard]] geo::Grid<pixel_t> grid_of(const GridOptions& options) {
  if ( options.pixels_per_degree.has_value() == options.metres.has_value() ) {
    throw std::invalid_argument("a raster table's grid takes --pixels-per-degree or --metres, one of the two");
  }
  if ( ! options.tile_pixels || ! options.nodata ) {
    throw std::invalid_argument("a raster table's grid takes --tile-pixels and --nodata");
  }

  const geo::Grid<pixel_t> grid{
      .pixels_per_degree = options.metres ? geo::pixels_per_degree_of_metres(*options.metres) : *options.pixels_per_degree,
      .tile_pixels = *options.tile_pixels,
      .nodata = pixel_of<pixel_t>(*options.nodata),
  };
  geo::check_grid(grid);

  return grid;
}

}  // namespace

void create(const Table& table, const GridOptions& grid) {
  with_model(table.model, [&]<typename model_t>(std::type_identity<model_t> /*model*/) {
    if constexpr ( miniverse::WritesSettings<model_t> ) {
      const miniverse::SettingsOf<model_t> made = grid_of<decltype(miniverse::SettingsOf<model_t>::nodata)>(grid);

      miniverse::Miniverse world(table.conninfo, layer_of<model_t>(table.name));
      world.template create_table<"table">(made);

      std::cout << "made the table " << table.name << " (" << table.model << "): " << made.pixels_per_degree << " pixels per degree, about "
                << made.metres_per_pixel() << " m, in tiles of " << made.tile_pixels << " pixels\n";

    } else {
      if ( grid.pixels_per_degree || grid.metres || grid.tile_pixels || grid.nodata ) {
        throw std::invalid_argument("only a raster table has a grid: " + table.model + " takes none");
      }

      miniverse::Miniverse world(table.conninfo, layer_of<model_t>(table.name));
      world.template create_table<"table">();

      std::cout << "made the table " << table.name << " (" << table.model << ")\n";
    }
  });
}

}  // namespace tool
