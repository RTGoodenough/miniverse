# AI Disclosure

The code and docs were written with the help of an AI assistant (Claude).

# miniverse

miniverse is a C++20 library for programs that work on a region of the world at a time. Data is uploaded once into
layers of a PostGIS database (roads, buildings, elevation); worker programs then load whatever lies in a polygon, into
structures of their own. The same layers can be read from local files instead of the database, with the same worker
code, for tests or for a machine without a database.

It is built on schemacht, a sibling library, which describes tables as C++ types and turns queries into SQL at compile time.

## What it does

- **Layer kinds:** features of one geometry type with JSON tags (`FeatureLayer<Geometry>`), roads as OpenStreetMap ways
  with their node ids (`RoadLayer`), and rasters of one band on a fixed grid (`RasterLayer<Pixel>`). A kind of your own
  is a type that names its table's columns, its query and its result.
- **Loading by polygon:** `load` gives everything in an area as one value; `stream` hands it to a callback a chunk at a
  time. Every call returns a `std::future`, so several layers load at once.
- **Pushing:** `push` writes in one transaction; `begin_push` takes large data in parts, all written or none. Vector
  layers are pushed as datasets, the exports they came from: a push of a dataset replaces it, and `remove` deletes one.
  Raster pushes merge onto the tiles already there.
- **Files as layers:** a GeoPackage, shapefile or GeoJSON (`gdal::feature_file`), a GeoTIFF or anything else GDAL opens
  (`gdal::raster_file`), an OpenStreetMap extract (`osm::road_file`), or a reader you write (`Reader<Kind>`).
- **Importing files into tables:** `gdal::read_features`, `gdal::read_raster` (warped onto the table's grid) and
  `osm::read_ways`, each a chunk at a time.
- **Checks:** `verify` compares every table with its kind before a program starts work.
- **GDAL datasets:** a loaded raster as a GDAL dataset in memory (`gdal::dataset_of`), for GDAL's own tools or to write
  as a GeoTIFF.

## Requirements

- A C++20 compiler (built with g++ 13; clang 18 parses it, through clang-tidy) and CMake 3.25 or newer.
- libpq 17 or newer, and a PostgreSQL server with the `postgis` and `postgis_raster` extensions. `verify` needs
  PostgreSQL 17; raster tables need a PostgreSQL 14 or newer built with lz4, as the usual packages and images are.
- Boost headers, 1.74 or newer (1.75 for the OSM component). Boost.Geometry supplies the geometry types.
- schemacht: installed, or checked out beside this repository (`MINIVERSE_SCHEMACHT_DIR` points elsewhere).
- Optional, for `miniverse::gdal`: GDAL 3.7 or newer, built with GEOS.
- Optional, for `miniverse::osm`: libosmium and protozero headers, zlib, expat, bzip2, threads.

Components are built when their dependencies are found (`MINIVERSE_BUILD_GDAL` and `MINIVERSE_BUILD_OSM`: `AUTO`,
`ON` or `OFF`).

## Building, testing and installing

```sh
cmake -S . -B build
cmake --build build -j6
ctest --test-dir build
cmake --install build --prefix /opt/miniverse
```

The tests that need a database skip themselves unless `MINIVERSE_TEST_DB` holds a connection string. Point it at a
scratch database that has PostGIS and postgis_raster enabled; the tests create and drop their own tables in it:

```sh
MINIVERSE_TEST_DB="postgresql://user:password@localhost:5432/scratch" ctest --test-dir build
```

## Using it from another project

Installed:

```cmake
find_package(miniverse REQUIRED COMPONENTS gdal osm)   # the components only if you use them
target_link_libraries(worker PRIVATE miniverse::miniverse miniverse::gdal miniverse::osm)
```

configured with `-DCMAKE_PREFIX_PATH=/opt/miniverse`. Or built as part of your project:

```cmake
set(MINIVERSE_SCHEMACHT_DIR "${CMAKE_SOURCE_DIR}/external/schemacht" CACHE PATH "")
add_subdirectory(external/miniverse)
target_link_libraries(worker PRIVATE miniverse::miniverse)
```

## A first program

A worker that loads the roads and the heights of an area. It is given a world of two tables, or a world of two files;
the worker's code is the same. From [examples/local_files.cpp](examples/local_files.cpp):

```cpp
struct Roads : miniverse::RoadLayer {};
struct Elevation : miniverse::RasterLayer<std::int16_t> {};  // heights in whole metres

using World = miniverse::Miniverse<Roads, Elevation>;

Survey survey(World& world, const geo::Polygon& area) {
  world.verify().get();

  std::future<miniverse::Ways> roads_loading = world.load<Roads>(area);
  std::future<Heights>         heights_loading = world.load<Elevation>(area);
  const miniverse::Ways        roads = roads_loading.get();
  const Heights                heights = heights_loading.get();
  ...
}

// Tables, in production:
World tables(conninfo, miniverse::Layer<Roads>("miniverse_example_local_roads"), miniverse::Layer<Elevation>("miniverse_example_local_heights"));

// Files, in a test:
World files(miniverse::osm::road_file<Roads>(streets), miniverse::gdal::raster_file<Elevation>(heights, grid));
```

## Examples

Each example is a program under [examples/](examples/), run with a connection string as its argument (`make_database`
takes the new database's name and its input files too). Each makes its own tables and drops them afterwards, except
`make_database`; the raster examples also make the schema `miniverse_functions`, which every raster table of a database
shares, and leave it.

| Example | Shows | Needs |
|---|---|---|
| `features` | a feature layer in one line, pushed as a dataset, loaded by polygon, pushed again | database |
| `road_network` | roads streamed into a graph of the worker's own, with routing | database |
| `elevation` | a raster table's grid, merged pushes, loads and streams, `value_at` | database |
| `own_kind` | a layer kind written by hand, with typed columns and its own query | database |
| `import_vector` | a vector file pushed in parts into a feature layer | database, gdal |
| `import_raster` | a raster file warped onto a table's grid, and a load as a GDAL dataset | database, gdal |
| `import_osm` | an OpenStreetMap extract pushed into a road layer | database, osm |
| `local_files` | one worker run on files and on tables | database, gdal, osm |
| `make_database` | a new database filled from an extract and a directory of GeoTIFFs, and kept | database, gdal, osm |

## Documentation

- [docs/guide.md](docs/guide.md): using miniverse, from layers and locations to files and readers of your own.
- [docs/developer.md](docs/developer.md): how it is put together, how to add a kind, how to build, test and lint.
- The API reference is generated from the headers with Doxygen: `cmake --build build --target miniverse_docs`, then open
  `build/docs/html/index.html`. The headers carry the same comments.
