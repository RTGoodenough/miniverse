// Rasters: heights on a grid the table keeps, pushed, merged, and loaded by polygon.
//
// A raster layer holds one band of pixels, cut into the tiles of a grid that is chosen once, when its table is made. A push
// is merged onto what is there; a load gives the pixels of an area as one raster, and a stream gives its tiles a few at a
// time. GDAL and QGIS open the table as one raster, too.
//
// Run with a libpq connection string to a database with PostGIS and postgis_raster:
//   miniverse_example_elevation "host=localhost dbname=gis user=gis"
// It creates the table miniverse_example_elevation, and drops it when it is done. A raster table's setup also makes the
// schema miniverse_functions, which every raster table of the database shares, and which stays.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <utility>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "support.hpp"

namespace geo = miniverse::geo;

struct Elevation : miniverse::RasterLayer<std::int16_t> {};  // heights in whole metres

using Heights = geo::Raster<std::int16_t>;

constexpr std::int16_t NODATA = -32768;

/**
 * @return A raster on `grid` of `degrees_across` by `degrees_down` degrees from the corner at (`west`, `north`), each pixel
 * with the height `height_of` gives for its column. A raster that is pushed must lie on the grid's pixels, as these do.
 */
[[nodiscard]] Heights surveyed(
    const geo::Grid<std::int16_t>& grid, double west, double north, double degrees_across, double degrees_down,
    std::int16_t (*height_of)(std::size_t column)
) {
  Heights heights{
      .west = west,
      .north = north,
      .pixel_width = 1.0 / grid.pixels_per_degree,
      .pixel_height = 1.0 / grid.pixels_per_degree,
      .width = static_cast<std::size_t>(std::lround(degrees_across * grid.pixels_per_degree)),
      .height = static_cast<std::size_t>(std::lround(degrees_down * grid.pixels_per_degree)),
      .nodata = grid.nodata,
  };

  heights.pixels.reserve(heights.width * heights.height);
  for ( std::size_t row = 0; row < heights.height; ++row ) {
    for ( std::size_t column = 0; column < heights.width; ++column ) {
      heights.pixels.push_back(height_of(column));
    }
  }

  return heights;
}

/** @return The height of the pixel of `heights` that `position` lies in. */
[[nodiscard]] std::int16_t height_at(const Heights& heights, const geo::Point& position) {
  const auto column = static_cast<std::size_t>((position.x() - heights.west) / heights.pixel_width);
  const auto row = static_cast<std::size_t>((heights.north - position.y()) / heights.pixel_height);

  return heights.at(column, row);
}

int main(int argc, char** argv) {
  try {
    miniverse::Miniverse world(example::conninfo(argc, argv), miniverse::Layer<Elevation>("miniverse_example_elevation"));

    // The grid is chosen once, here: 120 pixels to a degree (30 arc seconds), in tiles of half a degree. The table keeps it.
    world.drop_tables();
    world.create_table<Elevation>({.pixels_per_degree = 120, .tile_pixels = 60, .nodata = NODATA});

    // A writer asks the table for its grid, and makes its raster on it. (examples/import_raster.cpp has GDAL warp a file onto it.)
    const geo::Grid<std::int16_t> grid = world.table_settings<Elevation>().get();

    // A survey of 2 by 1 degrees: a ridge that rises to 3000 m in the middle. The push cuts it into the grid's 4 by 2 tiles.
    Heights ridge = surveyed(grid, 7, 47, 2, 1, [](std::size_t column) {
      return static_cast<std::int16_t>(3000 - (25 * std::abs(static_cast<int>(column) - 120)));
    });
    world.push<Elevation>(std::move(ridge)).get();

    // A later survey of half a degree of it, with data only in its western half: the lake there, at 372 m. A push is merged
    // onto the tiles already there: where it has data it wins, and where it has none the old pixel stays.
    Heights lake = surveyed(grid, 7.5, 46.75, 0.5, 0.25, [](std::size_t column) { return column < 30 ? std::int16_t{372} : NODATA; });
    world.push<Elevation>(std::move(lake)).get();

    // A load gives the pixels in an area's box as one raster, stitched from the tiles that cover it.
    const Heights heights = world.load<Elevation>(example::box(7.4, 46.4, 8.1, 46.9)).get();

    std::cout << heights.width << " by " << heights.height << " pixels from longitude " << heights.west << ", latitude " << heights.north << '\n';
    std::cout << "  on the lake:      " << height_at(heights, {7.6, 46.6}) << " m\n";
    std::cout << "  east of the lake: " << height_at(heights, {7.9, 46.6}) << " m (the first survey's)\n";

    // A stream gives the tiles as they are stored, here two at a time: a worker with a large area holds a few tiles, not the
    // area. The callback runs on a thread of the miniverse's, one call at a time, while this thread only waits for the future:
    // so what it fills needs no lock.
    std::size_t  tiles = 0;
    std::int16_t highest = NODATA;

    world
        .stream<Elevation>(
            example::box(7.1, 46.1, 8.9, 46.9),
            [&tiles, &highest](const std::vector<Heights>& chunk) {
              for ( const Heights& tile : chunk ) {
                highest = std::max(highest, *std::ranges::max_element(tile.pixels));
              }

              tiles += chunk.size();
            },
            {.chunk_rows = 2}
        )
        .get();

    std::cout << "the highest of " << tiles << " tiles: " << highest << " m\n";

    world.drop_tables();

    return EXIT_SUCCESS;
  } catch ( const std::exception& error ) {
    std::cerr << "error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
