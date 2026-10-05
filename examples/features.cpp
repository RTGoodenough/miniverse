// Features: a layer of your own in one line, filled, and loaded by polygon.
//
// A feature is a geometry with an id and tags. A layer holds the features of one geometry type in a table of its own, which
// the program names when it starts. Every load and push gives a future, so several layers are written and loaded at once.
//
// Run with a libpq connection string to a database with PostGIS: miniverse_example_features "host=localhost dbname=gis user=gis"
// It creates the tables miniverse_example_cafes and miniverse_example_parks, and drops them when it is done.

#include <boost/geometry/algorithms/within.hpp>  // IWYU pragma: keep

#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <string>

#include "miniverse/miniverse.hpp"
#include "schemacht/json/json.hpp"
#include "schemacht/postgres/async_client.hpp"
#include "support.hpp"

namespace geo = miniverse::geo;
using schemacht::json::Json;

// A kind of layer is a type, used as a name: two datasets of points would be two of these.
struct Cafes : miniverse::FeatureLayer<geo::Point> {};
struct Parks : miniverse::FeatureLayer<geo::Polygon> {};

int main(int argc, char** argv) {
  try {
    miniverse::Miniverse world(
        example::conninfo(argc, argv), miniverse::Layer<Cafes>("miniverse_example_cafes"), miniverse::Layer<Parks>("miniverse_example_parks")
    );

    world.drop_tables();
    world.create_tables();  // each with its spatial index

    // Started together, then waited for: the two layers are written at once, on the miniverse's pool of connections.
    // Positions are longitude, then latitude.
    std::future<void> cafes_written = world.push<Cafes>({
        {.id = 1, .geometry = geo::Point(10.004, 50.003), .tags = Json(R"({"name": "The Bandstand", "outdoor_seating": true})")},
        {.id = 2, .geometry = geo::Point(10.011, 50.002), .tags = Json(R"({"name": "Corner Cup"})")},
        {.id = 3, .geometry = geo::Point(10.250, 50.100)},  // across town, and without tags
    });
    std::future<void> parks_written = world.push<Parks>({
        {
            .id = 7,
            .geometry = example::box(10.000, 50.000, 10.010, 50.006),
            .tags = Json(R"({"name": "Old Common", "leisure": "park"})"),
        },
    });
    cafes_written.get();
    parks_written.get();

    // What each layer holds in an area: the features that intersect it, by id. Loaded at once, too.
    const geo::Polygon neighbourhood = example::box(9.990, 49.990, 10.020, 50.010);

    std::future<miniverse::Features<geo::Point>>   cafes_loading = world.load<Cafes>(neighbourhood);
    std::future<miniverse::Features<geo::Polygon>> parks_loading = world.load<Parks>(neighbourhood);
    const miniverse::Features<geo::Point>          cafes = cafes_loading.get();
    const miniverse::Features<geo::Polygon>        parks = parks_loading.get();

    std::cout << cafes.size() << " cafes and " << parks.size() << " park in the neighbourhood:\n";

    // The geometries are Boost.Geometry's own types, so its algorithms take them as they are. Tags are JSON text, for the
    // JSON library of your choice.
    for ( const miniverse::Feature<geo::Point>& cafe : cafes ) {
      std::cout << "  cafe " << cafe.id << ' ' << cafe.tags.text();
      for ( const miniverse::Feature<geo::Polygon>& park : parks ) {
        if ( boost::geometry::within(cafe.geometry, park.geometry) ) {  // NOLINT(misc-include-cleaner)
          std::cout << " is in park " << park.id;
        }
      }

      std::cout << '\n';
    }

    // A push writes all of its features or none. An error comes through the future, as here: cafe 1 is already there.
    try {
      world.push<Cafes>({{.id = 4, .geometry = geo::Point(10.006, 50.001)}, {.id = 1, .geometry = geo::Point(10.004, 50.003)}}).get();
    } catch ( const schemacht::postgres::QueryError& ) {
      std::cout << "a push with a cafe that is already there is refused whole: " << world.load<Cafes>(neighbourhood).get().size() << " cafes still\n";
    }

    world.drop_tables();

    return EXIT_SUCCESS;
  } catch ( const std::exception& error ) {
    std::cerr << "error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
