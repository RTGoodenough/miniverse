// A vector file into a feature layer: a GeoPackage, a shapefile, GeoJSON, or anything else GDAL opens.
//
// The file's features become a layer's: its geometries transformed to longitude and latitude, its other fields as tags. A
// file that mixes polygons and multipolygons, as most do, is read as multipolygons. It is read a chunk at a time and written
// as one push in parts: all of it, or none. This needs the optional component miniverse::gdal, built when GDAL is found.
//
// Run with a libpq connection string to a database with PostGIS: miniverse_example_import_vector "host=localhost dbname=gis user=gis"
// It reads examples/data/buildings.geojson; a file of polygons of your own goes in its place. It creates the table
// miniverse_example_buildings, and drops it when it is done; a real uploader would keep it.

#include <cstdlib>
#include <exception>
#include <iostream>
#include <utility>

#include "miniverse/gdal/vector.hpp"
#include "miniverse/miniverse.hpp"
#include "support.hpp"

namespace geo = miniverse::geo;

struct Buildings : miniverse::FeatureLayer<geo::MultiPolygon> {};

using Building = miniverse::Feature<geo::MultiPolygon>;

int main(int argc, char** argv) {
  try {
    miniverse::Miniverse world(example::conninfo(argc, argv), miniverse::Layer<Buildings>("miniverse_example_buildings"));

    world.drop_tables();
    world.create_tables();

    // One transaction for the whole file. Each chunk is waited for before the next is read, so one chunk is held at a time.
    miniverse::PushInParts<Buildings> buildings = world.begin_push<Buildings>().get();

    const miniverse::gdal::VectorRead read = miniverse::gdal::read_features<geo::MultiPolygon>(
        {.path = MINIVERSE_EXAMPLE_DATA "/buildings.geojson"}, 10'000,
        [&buildings](miniverse::Features<geo::MultiPolygon> chunk) { buildings.add(std::move(chunk)).get(); }
    );

    buildings.commit().get();  // now they are all there, for everyone at once

    std::cout << read.features << " buildings written, " << read.without_geometry << " without a geometry left out\n";

    // The buildings of the village: all but the file's barn, which is out of this area. Each has the file's id for it, its
    // fields as tags, and its polygons: a single polygon is a multipolygon of one.
    std::cout << "in the village:\n";
    for ( const Building& building : world.load<Buildings>(example::box(10, 50, 10.01, 50.01)).get() ) {
      std::cout << "  building " << building.id << ", " << building.geometry.size() << (building.geometry.size() == 1 ? " part: " : " parts: ")
                << building.tags.text() << '\n';
    }

    world.drop_tables();

    return EXIT_SUCCESS;
  } catch ( const std::exception& error ) {
    std::cerr << "error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
