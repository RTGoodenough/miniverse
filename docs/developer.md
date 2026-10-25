# Working on miniverse

How the library is put together, how to add to it, and how to build, test and lint it. The user guide is
[guide.md](guide.md); the headers document each type and call.

## Layout

```
include/miniverse/
  world.hpp              Miniverse: the front that holds the layers and the pool and tells a table from a reader
  world/                 one idea each: load.hpp (loads and streams), push.hpp, push_in_parts.hpp (PushInParts and the
                         push it runs), datasets.hpp (stamp, lock, sweep, remove), verify.hpp (TablesDiffer, the
                         checks), tables.hpp (make, drop), settings.hpp, async.hpp
  dataset.hpp            Dataset: the export a vector push writes under and replaces
  layer.hpp              Layer<Kind>: a table name or a reader
  reader.hpp             Reader<Kind>
  location.hpp           check_location: what a reader's load holds a polygon to, as a table's would refuse
  miniverse.hpp          umbrella: world.hpp plus the built-in kinds and geo types
  concepts/              LayerKind, HasSettings, HasNoSettings, HasDatasets, HasOwnChunks, ChunkCallback
  geo/                   one idea each: types.hpp (Boost.Geometry), grid.hpp, raster.hpp, wkb.hpp and raster_wkb.hpp
                         (over wkb_bytes.hpp), column_types.hpp, operations.hpp (PostGIS tests); concepts/ under
                         each area holds its concepts
  layer/                 feature.hpp and way.hpp (what a load gives), the kinds feature_layer.hpp, road_layer.hpp,
                         raster_layer.hpp; dataset_columns.hpp (what a kind with datasets keeps in its table, and its sweep
                         and remove); raster/reach.hpp (which tiles a location has), raster/tiles.hpp (cut, window)
  gdal/                  component miniverse::gdal: vector.hpp and raster.hpp (files read into layers), feature_file.hpp
                         and raster_file.hpp (files as layers), dataset.hpp; gdal.hpp is its umbrella
  osm/                   component miniverse::osm: ways.hpp (files read), ways_in_memory.hpp, road_file.hpp (a file as a
                         layer); osm.hpp is its umbrella
src/                     the compiled parts: geo WKB, the kinds' statements and conversions, layer/raster/, gdal/ (with its
                         private raster/ and vector/), osm/
tests/
  compile/               static_asserts on the concepts and types
  compile_fail/          cases that must not compile, each checked for its message
  unit/                  no database
  integration/           need MINIVERSE_TEST_DB
  gdal/, osm/            the components' tests, including the file-versus-table comparisons
  support/               a hand-written test kind (Places), the scratch database and a fresh world on it (Fresh),
                         temporary files, WKT polygons, small rasters, ids and rows for checks
examples/                nine programs; eight run as tests (make_database needs files outside the repository)
cmake/                   the installed package's config
```

Concepts live in `concepts/` directories beside the headers that use them. Each component is a static library of its own
(`miniverse_gdal`, `miniverse_osm`) so that the core never needs GDAL or libosmium.

## How it fits together

### A kind

A kind is a type that is never instantiated; `Miniverse<Roads, Elevation>` holds one layer per kind. The concept
`LayerKind` in `concepts/miniverse.hpp` says what a kind supplies:

| Member | Role |
|---|---|
| `result_type` | what a load returns (`Ways`, `Features<G>`, `geo::Raster<P>`) |
| `schema_type` | the table's columns, as a schemacht `Schema`; its own name is a placeholder, the `Layer` names the table |
| `load_statement_type` | a `schemacht::query::Prepared` select against `schema_type` whose argument 0 is the location polygon |
| `from_rows(rows, location)` | the load statement's rows made into a `result_type` |
| `to_rows(result[, settings \| dataset, stamp])` | the other way, in batches of one statement each; the kind sizes them |
| `write_statement(rows)` | the insert or upsert of one batch |
| `setup_sql(table[, settings])` | what runs after `CREATE TABLE`: an index, constraints, a function |
| `sweep_statement_type`, `remove_statement_type` | for a kind with datasets: the deletes of a dataset's unstamped rows, and of a dataset |

A kind is exactly one of `HasNoSettings`, `HasSettings` and `HasDatasets`. A kind with datasets (the two vector kinds)
has a generated id column, a `dataset` column and a `pushed` stamp; `to_rows(result, dataset, stamp)` produces
`insert_row_type` rows without the id, and `RowsOf`/`RowBatches` are those for every kind (the same as `row_type` when
nothing is generated). A kind with settings (`HasSettings`: a raster's grid) also has `settings_type`, `settings_statement_type` and
`settings_from_rows`, so a push can read the settings back from the table. A kind whose result cannot be made from part
of the rows (`HasOwnChunks`: a raster, stitched from all its tiles) says what a streamed chunk is instead: `chunk_type`
and `chunk_from_rows`.

The statements are built with schemacht's query builder and checked against the schema at compile time. PostGIS tests
are small operation types in `geo/operations.hpp` (`Intersects`, `OutlineIntersects`, `BoxesIntersect`), applied to a
column: `col<"geom">().apply<geo::Intersects>(arg<0>())`.

### A load, a stream, a push

`Miniverse` holds the layers and the pool. Each public call checks that the miniverse has the kind, tells a layer that
is a table from one with a reader, and calls a function template in `world/`, in `namespace miniverse::detail`: the
table side takes the `Database` and the layer's `Table`, the reader side the `Reader`. What follows is what those do.

- **load:** `Miniverse::load<Kind>` binds the kind's load statement to the location, runs it on the pool, and hands the
  rows to `from_rows` on a pool thread. The future carries the result or the error. For a layer with a reader, the
  reader's `load` runs on a thread started for the call (`on_a_thread`, in `world/async.hpp`). Both in `world/load.hpp`.
- **stream:** the statement runs in schemacht's chunked mode (`ReadOptions::chunk_rows`); each chunk of rows goes through
  `from_rows` or `chunk_from_rows` and to the callback, one call at a time, on a pool thread. A callback returning
  `false` cancels the statement.
- **push:** on a thread started for it (`on_a_thread`), `begun` reads the table's settings if the kind has any
  (`world/settings.hpp`) and begins a transaction; `Push::add` makes the batches with `to_rows` and sends one statement
  per batch, waiting for each; `Push::commit` runs what comes before the commit, then commits. A step that throws rolls
  the transaction back and keeps the error, which every later step throws. A whole push is `begun`, one `add` and a
  `commit` on one thread; a `PushInParts` holds the same `Push` and runs each `add` and the `commit` on a thread of its
  own (`world/push.hpp`, `world/push_in_parts.hpp`).
- **push of a dataset** (`world/datasets.hpp`): a random 64-bit stamp is drawn; the transaction begins with an advisory
  lock on the dataset's name (`pg_advisory_xact_lock(hashtext(name))`), so two pushes of one dataset run in turn; the
  rows carry the dataset and the stamp; before the commit the kind's sweep deletes the dataset's rows whose stamp is
  another push's. A failure anywhere rolls the whole back. `remove` runs the kind's remove statement outside a transaction.
- **verify:** schemacht's `table_differences_statement` for each table, all at once; the settings statement for kinds
  with settings; `Reader::problems` for readers. The results are gathered by `Checks` in `world/verify.hpp` and settled
  together.
- **create_table:** `CREATE TABLE` from the schema and the kind's `setup_sql`, in one transaction, waited for
  (`world/tables.hpp`).

Thread boundaries: a load's and a stream's conversions and the stream callbacks run on the pool's threads. A push, each
`PushInParts::add` and its `commit`, and a reader's load run on a thread started for the call, where the kind makes the
rows; their futures wait for that thread when dropped. What runs on the caller's thread and waits: the constructor
(it opens the pool), `create_table` and `drop_tables`, `osm::road_file` (it reads the file), `Reader::settings` and
`Reader::problems`, and the `.get()` calls the caller makes.

### The built-in kinds

- **FeatureLayer<G>** (`layer/feature_layer.hpp`): `feature_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, dataset
  text, geom geometry(<G>,4326), tags jsonb, pushed bigint`, a GiST index on `geom` and a btree on `dataset`. Writes are
  plain inserts; the load projects id, geometry and tags. Geometries are written and read as PostGIS WKB (`geo/wkb.hpp`).
  Batches are bounded by `FEATURES_PER_STATEMENT` and `POINTS_PER_STATEMENT`.
- **RoadLayer** (`layer/road_layer.hpp`): `way_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, dataset text, node_ids
  bigint[] UNIQUE, geom geometry(LineString,4326), tags jsonb, pushed bigint`, a GiST index and a btree on `dataset`.
  Writes are schemacht's `upsert_statement` with `ConflictOn<"node_ids">`: a way of nodes already there updates that
  row and keeps its id. The database numbers every row a statement tries, matched or not, so ids have gaps. Batches of
  `WAYS_PER_STATEMENT`.
- **RasterLayer<P>** (`layer/raster_layer.hpp`, `src/layer/raster_layer.cpp`; the pixel arithmetic in `layer/raster/tiles.hpp`,
  which tiles a location has in `layer/raster/reach.hpp`): `tile_id bigint PRIMARY KEY, rast raster`.
  The tile id is the tile's row and column in the world grid. `setup_sql` makes the schema `miniverse_functions` and its
  `merge_raster` function (shared by every raster table of the database), sets lz4 compression on `rast`, makes a GiST
  index on `ST_ConvexHull(rast)`, and adds the constraints that record the grid in the form `raster_columns` parses (so
  GDAL and QGIS read the table). `to_rows` cuts a raster into the grid's tiles, in batches sized by
  `PIXEL_BYTES_PER_STATEMENT`; `write_statement` is an upsert that merges through `merge_raster`. The load statement
  is `ST_Intersects(ST_ConvexHull("rast"), $1)`; `raster::Reach` then decides on the client which tiles the location
  reaches into, the same rule the file reader uses, and `raster::window` stitches them into the location's box. A push
  is `raster::cut`, then batches.

### The components

- **miniverse::gdal** (`src/gdal/`): `vector.cpp` is the read loop over a file's features; its stages are one private
  file each under `vector/`: `source` (open the file, find the layer and id field, the transformation to WGS 84),
  `geometry` (OGR's geometries as miniverse's, of the type asked for or one that reads as it), `fields` (a feature's
  id and its other fields as JSON) and `filter` (the layer narrowed to the location's box by its own index, then each
  feature tested against the location with a GEOS prepared geometry). `raster.cpp` is the read loop over a file's
  windows; under `raster/`: `source` (open the file, its extent in WGS 84, its scale to the grid), `tiles` (which tiles
  an extent or a location's box meets, a window of them, a tile cut from one) and `warp` (one window filled by
  `GDALWarp`, with one scale for the whole file so a pixel comes out the same whichever window it is read in).
  `dataset.cpp` copies a raster into a `MEM` dataset. `common.hpp` holds what all of them share: GDAL errors made into
  exceptions, driver registration, the pixel type mapping. The private headers are included as `gdal/...` from `src/`,
  and are not installed.
- **miniverse::osm** (`src/osm/`): `ways.cpp` reads ways with libosmium, holding node positions while it reads;
  `ways_in_memory.cpp` indexes ways in a Boost.Geometry R-tree for `road_file`. Boost.JSON is compiled into the library
  (`boost_json.cpp`) so it needs no Boost library to link.

The `gdal` component links GDAL publicly, because `dataset.hpp` hands out GDAL's own type; the `osm` component's
dependencies are private.

## How to

### Add a kind

`examples/own_kind.cpp` is the model: a `Towns` kind with typed columns, its own query and its own result. The steps:

1. Define the result type the program works with.
2. Define `schema_type` with schemacht `Field`s; a geometry column uses a `geo::` type, whose SQL type is
   `geometry(<Type>,4326)`.
3. Write `LOAD` as a `query::select` against the schema, applying `geo::Intersects` (or another operation) to the
   geometry column with `arg<0>()`; `using load_statement_type = query::Prepared<LOAD>`.
4. Write `setup_sql`, `to_rows`, `write_statement` and `from_rows`.
5. `static_assert(miniverse::LayerKind<Towns>)` tells you what is missing. `tests/compile/own_kind_test.cpp` shows that
   such a kind needs only `world.hpp` and the `geo` headers.

A kind with settings adds `settings_type`, `settings_statement_type` (a `RawStatement` taking the table's quoted name)
and `settings_from_rows`; `RasterLayer` is the example. A kind with datasets puts `dataset::Name` and `dataset::Pushed`
(`layer/dataset_columns.hpp`) in its schema, adds `to_rows(result, dataset, stamp)`, names `dataset::Sweep<schema_type>` and
`dataset::Remove<schema_type>` as its `sweep_statement_type` and `remove_statement_type`, and has `dataset::index_sql` in its
`setup_sql`; `FeatureLayer` is the plain example and `RoadLayer` the one with an identity of its own (`UNIQUE (node_ids)`).

### Add a reader

Derive from `Reader<Kind>` and implement `name`, `load`, `stream`, and `problems` if there is anything `verify` should
check; `settings` for a kind with settings. Keep every member `const` and thread-safe. `tests/unit/reader_test.cpp` has
two small readers and the tests a reader should pass; `gdal::FeatureFile`, `gdal::RasterFile` and `osm::RoadFile` are
the real ones.

### Add a file format

A reader of files follows the pattern of `gdal/vector.hpp` and `gdal/feature_file.hpp`: in the first, a `Source` struct
naming the file and options, a chunked `read_*` for importing into tables, a `read_*_in(location)` for layers and a
`problems_of` for `verify`; in a header of its own, the `*File<Kind>` reader and the `*_file<Kind>` function that makes
the `Layer`. Constrain `Kind` with a concept in `concepts/` that says
which kinds the format can feed (`gdal::FeatureKind`, `gdal::RasterKind`, `osm::WayKind`). Add a test that pushes a file
into a table and compares loads and streams of the two, as `tests/gdal/raster_test.cpp` does with
`check_loads_as_a_table`.

### Add a pixel type

The pixel types are listed in `geo/concepts/raster.hpp` (`Pixel`, and `PixelType<P>` with PostGIS's name for each),
`src/gdal/common.hpp` (`gdal_type`, which has a `static_assert` for a type it does not know), the `extern template`
list at the end of `layer/raster_layer.hpp`, and the explicit instantiation lists at the end of `src/geo/raster_wkb.cpp`,
`src/layer/raster/tiles.cpp`, `src/layer/raster_layer.cpp`, `src/gdal/raster.cpp` and `src/gdal/dataset.cpp`. A missed
instantiation is a link error. The function templates are only declared in their headers, so they need no `extern
template` lines: a comment at the end of each header names the types they are defined for, and takes the new one too.

## Building

```sh
cmake -S . -B build
cmake --build build -j6
```

Options:

| Option | Default | Effect |
|---|---|---|
| `MINIVERSE_BUILD_GDAL`, `MINIVERSE_BUILD_OSM` | `AUTO` | build the component if its dependencies are found; `ON` fails without them |
| `MINIVERSE_BUILD_TESTS`, `MINIVERSE_BUILD_EXAMPLES` | on when top level | |
| `MINIVERSE_BUILD_DOCS` | `AUTO` | the `miniverse_docs` target, if Doxygen is found |
| `MINIVERSE_INSTALL` | on when top level | the install rules and the CMake package |
| `MINIVERSE_SCHEMACHT_DIR` | `../schemacht` | schemacht's source tree, used when no installed schemacht is found |

The GDAL minimum, 3.7, is `MINIVERSE_MIN_GDAL` in `CMakeLists.txt`, checked after `find_package`, since GDAL's package
only accepts a request for its own minor version. A full rebuild with unlimited parallel jobs has run out of memory on
a 16 GB machine; it has been built with `-j6`.

`cmake --install build --prefix <dir>` installs `libminiverse.a` (and the components' libraries), the headers and
`lib/cmake/miniverse/`, from which `find_package(miniverse COMPONENTS gdal osm)` gives `miniverse::miniverse`,
`miniverse::gdal` and `miniverse::osm`. schemacht is installed beside it when built from the sibling tree.

## Testing

The suite is three test executables, `miniverse_tests` (compile, unit and integration tests), `miniverse_gdal_tests`
and `miniverse_osm_tests`; the example programs, each run as a test of its own; and the compile-fail cases, each a
build of `tests/compile_fail/cases.cpp` with one case defined that passes when the build fails with the expected
message.

Tests that need a database skip themselves unless `MINIVERSE_TEST_DB` is set:

```sh
MINIVERSE_TEST_DB="postgresql://user:password@localhost:5432/scratch" ctest --test-dir build
```

The database must have `postgis` and `postgis_raster` enabled and be one you can lose: the tests create and drop
tables named `miniverse_test_*` and `miniverse_example_*`, and one test drops the shared schema `miniverse_functions`.
Never point it at a database with data in it. The tests that share the database take a CTest `RESOURCE_LOCK`, so they
run one at a time under `ctest -j`.

Read the counts: `ctest` counts skipped tests in its "tests passed" percentage and lists them separately. With the
database up, no test should be skipped.

Parts of the suite:

- `tests/gdal/raster_test.cpp` and `vector_test.cpp`, `tests/osm/ways_test.cpp`: each test file is pushed into a table
  and the file layer's loads and streams are compared with the table's, for a list of locations including ones that
  touch tile edges. Use a grid of tenths of a degree and rough pixels for such tests: a dyadic grid and smooth data hide
  rounding differences.
- `tests/unit/reader_test.cpp`: the reader contract, including that loads run on their own threads.
- `tests/integration/raster_layer_test.cpp`: the table's constraints, index use (by `EXPLAIN`), merging, lz4.

A ThreadSanitizer build of `miniverse_tests` with the components off (`-DCMAKE_CXX_FLAGS=-fsanitize=thread
-DMINIVERSE_BUILD_GDAL=OFF -DMINIVERSE_BUILD_OSM=OFF -DMINIVERSE_BUILD_EXAMPLES=OFF`) is the check for the threaded
paths: run `./miniverse_tests "[reader],[integration]"` from it. The test discovery step fails under TSan; the binary
still links.

## Linting and style

clang-tidy 18 with the repository's `.clang-tidy` (all checks but a few families turned off at its top, warnings as
errors, include-cleaner included; `examples/.clang-tidy` and `tests/.clang-tidy` relax checks that fight sample and
test code):

```sh
clang-tidy -p build --header-filter='.*/miniverse/(include|src|tests|examples)/.*' <file>
```

Conventions the code follows:

- Classes are `CamelCase`, functions and variables `lower_case`, template parameters `lower_case` with a `_t` suffix,
  private members `_prefixed`, constants `UPPER_CASE`.
- Template parameters are constrained by named concepts, which live in `concepts/` directories.
- A class with a constructor declares all five special members (copy, move, destructor), defaulted or deleted, at the
  end of the class.
- A discarded result is `std::ignore = expr;`, never a cast to void.
- Steps of a function are separated by blank lines; comments say why, in prose; public declarations carry Doxygen
  comments (`@brief`, `@return`, `@throws`, with `@code` examples where a call is not obvious).
- No hand-rolled parsers or formats where a library does it (Boost.JSON for JSON, GDAL and libosmium for files).

The API reference is built from those comments: `cmake --build build --target miniverse_docs` writes `build/docs/html/` when
Doxygen is installed (`MINIVERSE_BUILD_DOCS`). README.md is its front page and the files in `docs/` are further pages.
