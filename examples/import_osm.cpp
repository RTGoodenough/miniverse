// An OpenStreetMap file into a road layer: all of it, or none of it.
//
// The uploader's side of examples/road_network.cpp. The file's ways are read a chunk at a time and written as one push in
// parts, so they are never all held at once, and workers see none of them until the commit: a file that fails halfway leaves
// nothing. (What is held is the position of every node of the file, 16 bytes each: read an extract of the area wanted.)
// This needs the optional component miniverse::osm, built when libosmium is found.
//
// Run with a libpq connection string to a database with PostGIS: miniverse_example_import_osm "host=localhost dbname=gis user=gis"
// It reads examples/data/streets.osm; a file of your own (.osm.pbf, .osm, .osm.bz2) goes in its place. It creates the table
// miniverse_example_osm_roads, and drops it when it is done; a real uploader would keep it.

#include <cstdlib>
#include <exception>
#include <iostream>
#include <utility>

#include "miniverse/miniverse.hpp"
#include "miniverse/osm/ways.hpp"
#include "support.hpp"

struct Roads : miniverse::RoadLayer {};

int main(int argc, char** argv) {
  try {
    miniverse::Miniverse world(example::conninfo(argc, argv), miniverse::Layer<Roads>("miniverse_example_osm_roads"));

    world.drop_tables();
    world.create_tables();

    // One transaction for the whole file. Each chunk is waited for before the next is read.
    miniverse::PushInParts<Roads> roads = world.begin_push<Roads>().get();

    // The ways with a `highway` tag, which are OpenStreetMap's roads and paths, 10000 at a time.
    const miniverse::osm::WayRead read =
        miniverse::osm::read_ways({.path = MINIVERSE_EXAMPLE_DATA "/streets.osm", .keys = {"highway"}}, 10'000, [&roads](miniverse::Ways chunk) {
          roads.add(std::move(chunk)).get();
        });

    roads.commit().get();  // now they are all there, for everyone at once

    // Left out: a road with a node the file lacks, as an extract has at its edge. The file's building is not a road at all.
    std::cout << read.ways << " roads written, " << read.without_nodes << " left out\n";

    // Each has its id, its tags as the file has them, its line, and the ids of its nodes, which roads that meet share.
    for ( const miniverse::Way& road : world.load<Roads>(example::box(9.99, 49.99, 10.01, 50.01)).get() ) {
      std::cout << "  way " << road.id << ", " << road.node_ids.size() << " nodes: " << road.tags.text() << '\n';
    }

    world.drop_tables();

    return EXIT_SUCCESS;
  } catch ( const std::exception& error ) {
    std::cerr << "error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
