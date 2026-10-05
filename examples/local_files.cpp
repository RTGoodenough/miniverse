// One worker, two worlds: the same layers from local files, and from the database.
//
// A layer is a table, or is read from a file, and the program says which where it makes the layer. The miniverse is of the
// same type either way, so the worker below takes both: a test or a laptop runs it on files with no database at all, and
// production on the tables the same files were pushed into. Both give the same answer here; what may differ between a file
// and a table is the order of what a load gives (a file's own, where a table's is by id) and how tags are spelt.
//
// This needs the optional components miniverse::gdal and miniverse::osm. Run with a libpq connection string to a database with
// PostGIS and postgis_raster, on PostgreSQL 17 or newer (which `verify` needs for a table):
//   miniverse_example_local_files "host=localhost dbname=gis user=gis"
// It reads examples/data/streets.osm and examples/data/heights.asc. It creates the tables miniverse_example_local_roads and
// miniverse_example_local_heights, and drops them when it is done; and, as every raster table's setup does, the schema
// miniverse_functions, which stays.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>

#include "miniverse/gdal/raster.hpp"
#include "miniverse/miniverse.hpp"
#include "miniverse/osm/ways.hpp"
#include "support.hpp"

namespace geo = miniverse::geo;

struct Roads : miniverse::RoadLayer {};
struct Elevation : miniverse::RasterLayer<std::int16_t> {};  // heights in whole metres

using World = miniverse::Miniverse<Roads, Elevation>;
using Heights = geo::Raster<std::int16_t>;

/** @brief What the worker finds in an area. */
struct Survey {
  std::size_t  roads = 0;
  std::size_t  road_nodes = 0;
  std::int16_t highest = 0;

  [[nodiscard]] bool operator==(const Survey&) const = default;
};

/** @return What `world` holds in `area`. The worker's part: nothing here knows whether a layer is a table or a file. */
[[nodiscard]] Survey survey(World& world, const geo::Polygon& area) {
  world.verify().get();  // the tables are the kinds' own, and the raster file can be read (the road file was, when its layer was made)

  std::future<miniverse::Ways> roads_loading = world.load<Roads>(area);
  std::future<Heights>         heights_loading = world.load<Elevation>(area);
  const miniverse::Ways        roads = roads_loading.get();
  const Heights                heights = heights_loading.get();

  Survey found{.roads = roads.size(), .highest = heights.pixels.empty() ? heights.nodata : *std::ranges::max_element(heights.pixels)};
  for ( const miniverse::Way& road : roads ) {
    found.road_nodes += road.node_ids.size();
  }

  return found;
}

int main(int argc, char** argv) {
  try {
    const miniverse::osm::WaySource     streets{.path = MINIVERSE_EXAMPLE_DATA "/streets.osm", .keys = {"highway"}};
    const miniverse::gdal::RasterSource heights{.path = MINIVERSE_EXAMPLE_DATA "/heights.asc"};
    const geo::Polygon                  area = example::box(6.5, 45.5, 10.5, 50.5);

    // The grid heights are held on, in tables and from files alike: about 7 km to a pixel, in tiles of 8 pixels. The file's
    // own pixels are twice that size, so it is resampled onto the grid, the same way for both worlds.
    const geo::Grid<std::int16_t> grid{.pixels_per_degree = geo::pixels_per_degree_of_metres(7000), .tile_pixels = 8, .nodata = -32768};

    // A world of files, with no connection string: nothing is connected to.
    World files(miniverse::osm::road_file<Roads>(streets), miniverse::gdal::raster_file<Elevation>(heights, grid));
    const Survey from_files = survey(files, area);
    std::cout << "from files:  " << from_files.roads << " roads of " << from_files.road_nodes << " nodes, the highest ground " << from_files.highest
              << " m\n";

    // A world of tables, which an uploader fills from the same files.
    World tables(
        example::conninfo(argc, argv), miniverse::Layer<Roads>("miniverse_example_local_roads"),
        miniverse::Layer<Elevation>("miniverse_example_local_heights")
    );
    tables.drop_tables();
    tables.create_table<Roads>();
    tables.create_table<Elevation>(grid);
    tables.push<Roads>(miniverse::osm::read_ways(streets)).get();
    tables.push<Elevation>(miniverse::gdal::read_raster<std::int16_t>(heights, grid)).get();
    const Survey from_tables = survey(tables, area);

    std::cout << "from tables: " << from_tables.roads << " roads of " << from_tables.road_nodes << " nodes, the highest ground " << from_tables.highest
              << " m\n";
    std::cout << (from_files == from_tables ? "the same from both\n" : "NOT the same\n");

    tables.drop_tables();

    return from_files == from_tables ? EXIT_SUCCESS : EXIT_FAILURE;
  } catch ( const std::exception& error ) {
    std::cerr << "error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
