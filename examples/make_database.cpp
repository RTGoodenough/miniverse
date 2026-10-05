// A database made and filled from files, and kept: the roads of an OpenStreetMap extract and the heights of a directory of
// GeoTIFFs, as the two tables `roads` and `elevation`.
//
// The other examples make tables in a database that is there, and drop them again. This one makes the database too, and
// leaves it: something of real size to point workers at, or to open in a viewer (the tables are PostGIS's own, so QGIS
// reads them over a PostGIS connection). This needs the optional components miniverse::gdal and miniverse::osm.
//
// Run with a libpq connection string to any database of the server that is there (`postgres` always is), of keywords or a
// URI, then the new database's name, the OpenStreetMap file, and the directory of GeoTIFFs:
//   miniverse_example_make_database "host=localhost user=gis dbname=postgres" region region.osm.pbf heights/
// The database named in the string is only connected to, to make the new one from: nothing in it is read or changed.
// The user must be allowed to make databases and the extensions postgis and postgis_raster. A database of the name that is
// there already is left alone, and the run fails; so does a second run after one that failed halfway, until what the first
// left is dropped by hand (`dropdb region`). Nothing here drops anything.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <libpq-fe.h>

#include "miniverse/gdal/raster.hpp"
#include "miniverse/miniverse.hpp"
#include "miniverse/osm/ways.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/postgres/statements.hpp"

namespace geo = miniverse::geo;
namespace postgres = schemacht::postgres;

struct Roads : miniverse::RoadLayer {};
struct Elevation : miniverse::RasterLayer<std::int16_t> {};  // heights in whole metres

using World = miniverse::Miniverse<Roads, Elevation>;

namespace {

/**
 * @return `conninfo`, a libpq connection string of either form, as one of keywords without its database: the server, the
 * user and all else, for another database's name to follow. libpq reads the string, so what it takes is what is taken.
 * @throws std::invalid_argument with libpq's message for a string it can't read.
 */
std::string without_database(const std::string& conninfo) {
  char*                                                              message = nullptr;
  const std::unique_ptr<PQconninfoOption, decltype(&PQconninfoFree)> options(PQconninfoParse(conninfo.c_str(), &message), &PQconninfoFree);
  const std::unique_ptr<char, decltype(&PQfreemem)>                  held(message, &PQfreemem);
  if ( ! options ) {
    throw std::invalid_argument(message != nullptr ? message : "the connection string can't be read");
  }

  // Each value in quotes, with the two characters that mean something inside them escaped. The array ends with a keyword of none.
  std::string result;
  for ( const PQconninfoOption* option = options.get(); option->keyword != nullptr; ++option ) {  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if ( option->val == nullptr || std::string_view(option->keyword) == "dbname" ) {
      continue;
    }

    result += std::string(option->keyword) + "='";
    for ( const char letter : std::string_view(option->val) ) {
      if ( letter == '\'' || letter == '\\' ) {
        result += '\\';
      }
      result += letter;
    }
    result += "' ";
  }

  return result;
}

/**
 * @brief Makes the database `name` on the server of `server`, a connection string to any database of it.
 * @return The connection string to the new database.
 * @throws std::invalid_argument unless `name` is of small letters, digits and `_` alone and starts with a letter, which
 * needs no quoting, or if `server` can't be read: either before anything is made. A QueryError if there is such a database
 * already.
 */
std::string make_database(const std::string& server, const std::string& name) {
  const auto plain = [](unsigned char letter) { return std::islower(letter) != 0 || std::isdigit(letter) != 0 || letter == '_'; };
  if ( name.empty() || std::islower(static_cast<unsigned char>(name.front())) == 0 || ! std::ranges::all_of(name, plain) ) {
    throw std::invalid_argument("the database's name must be of small letters, digits and _, and start with a letter");
  }

  std::string made = without_database(server) + "dbname=" + name;

  postgres::Database maintenance(server);
  std::ignore = maintenance.execute(postgres::unchecked_sql("CREATE DATABASE " + name)).get();

  return made;
}

/** @brief Writes the roads and paths of the OpenStreetMap file at `path`, all of them or none. */
void write_roads(World& world, const std::string& path) {
  miniverse::PushInParts<Roads> roads = world.begin_push<Roads>().get();

  const miniverse::osm::WayRead read = miniverse::osm::read_ways({.path = path, .keys = {"highway"}}, 10'000, [&roads](miniverse::Ways chunk) {
    roads.add(std::move(chunk)).get();
  });

  roads.commit().get();

  std::cout << "roads: " << read.ways << " written, " << read.without_nodes << " left out, of " << path << '\n';
}

/**
 * @brief Writes the GeoTIFFs of `directory` in the order of their names, each all of it or none. Where two files overlap, as
 * neighbours of a survey do by a few pixels, the later one's data is what the table has.
 */
void write_heights(World& world, const std::string& directory) {
  std::vector<std::filesystem::path> files;
  for ( const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory) ) {
    if ( entry.is_regular_file() && (entry.path().extension() == ".tif" || entry.path().extension() == ".tiff") ) {
      files.push_back(entry.path());
    }
  }
  std::ranges::sort(files);

  const geo::Grid<std::int16_t> grid = world.table_settings<Elevation>().get();

  std::size_t written = 0;
  for ( const std::filesystem::path& file : files ) {
    miniverse::PushInParts<Elevation> heights = world.begin_push<Elevation>().get();

    // Windows of 2 by 2 tiles, 1024 pixels a side: each is held twice while it is made, and once more as it is sent.
    const miniverse::gdal::RasterRead read =
        miniverse::gdal::read_raster<std::int16_t>({.path = file.string()}, grid, 2, [&heights](geo::Raster<std::int16_t> window) {
          heights.add(std::move(window)).get();
        });

    heights.commit().get();

    std::cout << "elevation: " << ++written << " of " << files.size() << ", " << file.filename().string() << ": " << read.windows << " windows written, "
              << read.without_data << " with no data left out\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::span<char*> args(argv, static_cast<std::size_t>(argc));
  if ( args.size() != 5 ) {
    std::cerr << "usage: " << args[0] << " <connection string to the server> <new database> <file.osm.pbf> <directory of GeoTIFFs>\n";
    return EXIT_FAILURE;
  }

  try {
    World world(make_database(args[1], args[2]), miniverse::Layer<Roads>("roads"), miniverse::Layer<Elevation>("elevation"));

    std::ignore = world.database().execute(postgres::unchecked_sql("CREATE EXTENSION IF NOT EXISTS postgis")).get();
    std::ignore = world.database().execute(postgres::unchecked_sql("CREATE EXTENSION IF NOT EXISTS postgis_raster")).get();

    // The elevation table's grid, chosen once: 3600 pixels to a degree, about 31 m, which is what a survey of one arc second
    // has, so its pixels are the table's. Tiles of 512 pixels suit loads of up to 100 km.
    world.create_table<Roads>();
    world.create_table<Elevation>({.pixels_per_degree = 3600, .tile_pixels = 512, .nodata = -32768});

    write_roads(world, args[3]);
    write_heights(world, args[4]);

    // What the planner knows of the new tables, now and not when the server next gets to it.
    std::ignore = world.database().execute(postgres::unchecked_sql("ANALYZE")).get();

    std::cout << "the database " << args[2] << " is made, and stays\n";

    return EXIT_SUCCESS;
  } catch ( const std::exception& error ) {
    std::cerr << "error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
