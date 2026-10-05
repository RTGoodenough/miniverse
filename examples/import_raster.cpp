// A raster file into a raster layer: a GeoTIFF, or anything else GDAL opens, warped onto the table's grid.
//
// The file may be in any coordinate system and of any pixel size: each pixel of the table's grid is made from the file's
// pixels around it. It is read a window of whole tiles at a time and written as one push in parts: all of it, or none. This
// needs the optional component miniverse::gdal, built when GDAL is found.
//
// Run with a libpq connection string to a database with PostGIS and postgis_raster:
//   miniverse_example_import_raster "host=localhost dbname=gis user=gis"
// It reads examples/data/heights.asc, 2 by 1 degrees of 8 pixels to a degree; a file of your own goes in its place. It
// creates the table miniverse_example_heights, and drops it when it is done; a real uploader would keep it. A raster table's
// setup also makes the schema miniverse_functions, which every raster table of the database shares, and which stays.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <utility>

#include "miniverse/gdal/raster.hpp"
#include "miniverse/miniverse.hpp"
#include "support.hpp"

namespace geo = miniverse::geo;

struct Elevation : miniverse::RasterLayer<std::int16_t> {};  // heights in whole metres

using Heights = geo::Raster<std::int16_t>;

int main(int argc, char** argv) {
  try {
    miniverse::Miniverse world(example::conninfo(argc, argv), miniverse::Layer<Elevation>("miniverse_example_heights"));

    // The table's grid, chosen once: about 7 km to a pixel, which is 16 pixels to a degree, the nearest whole number, in
    // tiles of 8 pixels. Every file is warped onto it, whatever its own pixels, so files of different sources lie pixel on pixel.
    world.drop_tables();
    world.create_table<Elevation>({.pixels_per_degree = geo::pixels_per_degree_of_metres(7000), .tile_pixels = 8, .nodata = -32768});

    // A writer that did not make the table asks it for its grid.
    const geo::Grid<std::int16_t> grid = world.table_settings<Elevation>().get();
    std::cout << "the table's grid: " << grid.pixels_per_degree << " pixels to a degree, " << std::lround(grid.metres_per_pixel()) << " m each\n";

    // One transaction for the whole file, in windows of 2 by 2 tiles. Each window is waited for before the next is read.
    miniverse::PushInParts<Elevation> heights = world.begin_push<Elevation>().get();

    const miniverse::gdal::RasterRead read = miniverse::gdal::read_raster<std::int16_t>(
        {.path = MINIVERSE_EXAMPLE_DATA "/heights.asc", .resampling = miniverse::gdal::Resampling::Bilinear}, grid, 2,
        [&heights](Heights window) { heights.add(std::move(window)).get(); }
    );

    heights.commit().get();  // now it is all there, for everyone at once

    std::cout << read.windows << " windows written, " << read.without_data << " with no data left out\n";

    // The file's area, loaded on the table's grid. Its south-east corner is not surveyed: where a file has no data, the
    // table has the grid's nodata.
    const Heights loaded = world.load<Elevation>(example::box(7, 46, 9, 47)).get();

    std::cout << loaded.width << " by " << loaded.height << " pixels, the highest " << *std::ranges::max_element(loaded.pixels) << " m, "
              << std::ranges::count(loaded.pixels, loaded.nodata) << " with no data\n";

    world.drop_tables();

    return EXIT_SUCCESS;
  } catch ( const std::exception& error ) {
    std::cerr << "error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
