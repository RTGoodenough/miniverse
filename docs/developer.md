# Working on miniverse

How the library is put together, how to add to it, and how to build, test and lint it. The user guide is
[guide.md](guide.md); the headers document each type and call.

## Layout

```
include/miniverse/
  world.hpp              Miniverse: the front that holds the layers and the pool and tells a table from a reader
  internal_schema.hpp    the schema the library keeps for itself in a database
  world/                 one idea each: load.hpp (loads and streams), push.hpp, push_in_parts.hpp (PushInParts and the
                         push it runs), step_thread.hpp (the thread a push in parts runs on), datasets.hpp (stamp, lock,
                         sweep, remove, list), verify.hpp (the checks), tables.hpp (make, drop), registry.hpp
                         (what a database remembers about its layers: layouts and datasets), settings.hpp, async.hpp
  dataset.hpp            Dataset: the export a vector push writes under and replaces; DatasetInfo, one as listed
  tables_differ.hpp      TablesDiffer: what verify fails with
  layer.hpp              Layer<Model, "name">: a model under a name, in a table named at run time or read by a reader; IsLayer
  reader.hpp             Reader<Model>
  location.hpp           check_location: what a reader's load holds a polygon to, as a table's would refuse
  miniverse.hpp          umbrella: world.hpp plus the built-in models and geo types
  concepts/              LayerModel, WritesPlain, WritesSettings, WritesDatasets, HasOwnChunks, ChunkCallback
  geo/                   one idea each: types.hpp (Boost.Geometry), grid.hpp (the grid, where its tiles and pixels lie),
                         snap.hpp (positions against pixel and tile edges), raster.hpp, wkb.hpp and raster_wkb.hpp,
                         column_types.hpp, operations.hpp (PostGIS tests); concepts/ under each area holds its concepts
  model/                 feature.hpp and way.hpp (what a load gives), the models feature_model.hpp, road_model.hpp,
                         raster_model.hpp; writes.hpp (the three ways a push treats the rows already there, and the two
                         columns of a model with datasets); raster/reach.hpp (which tiles a location has), raster/tiles.hpp
                         (cut, window)
  gdal/                  component miniverse::gdal: vector.hpp and raster.hpp (files read into layers), feature_file.hpp
                         and raster_file.hpp (files as layers), dataset.hpp; gdal.hpp is its umbrella
  osm/                   component miniverse::osm: ways.hpp (files read), road_file.hpp (a file as a layer, over
                         ways_in_memory.hpp, a detail); osm.hpp is its umbrella
src/                     the compiled parts: geo WKB (over its private geo/wkb_bytes.hpp), the models' statements and
                         conversions, model/raster/, gdal/ (with its private raster/ and vector/), osm/
tests/
  compile/               static_asserts on the concepts and types
  compile_fail/          cases that must not compile, each checked for its message
  unit/                  no database
  integration/           need MINIVERSE_TEST_DB
  gdal/, osm/            the components' tests, including the file-versus-table comparisons
  tool/                  the command-line tool: its commands against tables loaded with the library, and the program run
  support/               a hand-written test model (Places), the scratch database and a fresh world on it (Fresh),
                         temporary files, WKT polygons, small rasters, ids and rows for checks
examples/                nine programs; eight run as tests (make_database needs files outside the repository)
tools/                   the command-line tool `miniverse`: models.hpp (the models it knows, by name), layer_of.hpp (a table's
                         name as a layer), commands.hpp and a file per command (a library of their own, which the tests
                         call), main.cpp (the command line, CLI11)
cmake/                   the installed package's config, and checks.cmake: the warnings and lint of the project's own targets
third_party/             code of others kept in the repository as its authors publish it, each with its license and a README
                         of where it is from: CLI11 (the tool's command-line parser)
```

Concepts live in `concepts/` directories beside the headers that use them. Each component is a static library of its own
(`miniverse_gdal`, `miniverse_osm`) so that the core never needs GDAL or libosmium.

## How it fits together

### A model

A model is a type that is never instantiated. A `Layer<Model, "name">` puts it under a name of the program's, and
`Miniverse<Roads, Elevation>` holds such layers, each found by its name at compile time (`detail::index_of_layer` in
`world.hpp`, over the layers' `NAME`s) and typed on its model from there on. The concept `LayerModel` in
`concepts/miniverse.hpp` says what a model supplies:

| Member | Role |
|---|---|
| `result_type` | what a load returns (`Ways`, `Features<G>`, `geo::Raster<P>`) |
| `schema_type` | the table's columns, as a schemacht `Schema`; its own name is a placeholder, the `Layer` names the table |
| `load_statement_type` | a `schemacht::query::Prepared` select against `schema_type` whose argument 0 is the location polygon |
| `writes` | how a push treats the rows already there: one of the three types in `model/writes.hpp` |
| `from_rows(rows, location)` | the load statement's rows made into a `result_type` |
| `to_rows(result[, settings \| dataset, stamp])` | the other way, in batches of one statement each; the model sizes them |
| `write_statement(rows)` | the insert or upsert of one batch |
| `setup_sql(table[, settings])` | what runs after `CREATE TABLE`: an index, constraints, a function |

`writes` says what else the model has, and the concepts `WritesPlain`, `WritesSettings` and `WritesDatasets` check it:

- `writes::Plain`: `setup_sql(table)` and `to_rows(result)`.
- `writes::WithSettings<T>` (a raster's grid, `SettingsOf<Model>`): `setup_sql(table, settings)`, `settings_statement_type`
  (a `RawStatement` taking the table's quoted name) and `settings_from_rows`, so a push can read the settings back from
  the table, and `to_rows(result, settings)`.
- `writes::Datasets` (the two vector models): `dataset::Name` and `dataset::Stamp` among the schema's columns,
  `setup_sql(table)` for the model's own indexes, and `to_rows(result, dataset, stamp)`. The rest is the library's, in
  `world/datasets.hpp`, so a model of its own writes none of it: the sweep of a dataset's rows another push wrote
  (`sweep_statement`), the remove of a dataset (`remove_statement`), the index on `dataset` (`dataset_index_sql`), and
  the key of the lock two pushes of one dataset take turns on (`lock_key`).

The three `writes` types are tags: they carry no code, only what the concepts ask of the model for each.

A model with datasets has an id column (generated by the database for features, the source's own for roads);
`to_rows(result, dataset, stamp)` produces `insert_row_type` rows, which leave out a generated id, and
`RowsOf`/`RowBatches` are those for every model (the same as `row_type` when nothing is generated). A model whose result
cannot be made from part of the rows (`HasOwnChunks`: a raster, stitched from all its tiles) says what a streamed chunk
is instead: `chunk_type` and `chunk_from_rows`.

The statements are built with schemacht's query builder and checked against the schema at compile time. PostGIS tests
are small operation types in `geo/operations.hpp` (`Intersects`, `OutlineIntersects`), applied to a
column: `col<"geom">().apply<geo::Intersects>(arg<0>())`.

### A load, a stream, a push

`Miniverse` holds the layers and the pool. Each public call takes a layer's name, finds the layer and its model
(`ModelOf<name>`; a name the miniverse lacks fails a `static_assert`), tells a layer that is a table from one with a
reader, and calls a function template in `world/`, in `namespace miniverse::detail`, typed on the model: the table side
takes the `Database` and the layer's `Table`, the reader side the `Reader`. What follows is what those do.

- **load:** `Miniverse::load<"name">` binds the model's load statement to the location, runs it on the pool and waits for
  the rows on a thread started for the call (`on_a_thread`, in `world/async.hpp`), where `from_rows` makes the result.
  The future carries the result or the error. For a layer with a reader, the reader's `load` runs on such a thread too.
  Both in `world/load.hpp`. Every call of several statements, or whose rows take work, is written this way: straight
  line, a `.get()` per statement, on a thread of its own; the pool's workers run only completions, which must not wait
  on a future of the same pool, so a stream alone runs on them.
- **stream:** the statement runs in schemacht's chunked mode (`ReadOptions::chunk_rows`); each chunk of rows goes through
  `from_rows` or `chunk_from_rows` and to the callback, one call at a time, on a pool thread. A callback returning
  `false` cancels the statement.
- **push:** on a thread started for it (`on_a_thread`), `begun` reads the table's settings if the model has any
  (`world/settings.hpp`) and begins a transaction; `Push::add` makes the batches with `to_rows` and sends one statement
  per batch, waiting for each; `Push::commit` runs what comes before the commit, then commits. A step that throws rolls
  the transaction back and keeps the error, which every later step throws. A whole push is `begun`, one `add` and a
  `commit` on one thread. `begin_push` runs `begun` on a thread started for the call too, and there starts a `StepThread`
  (`world/step_thread.hpp`), a thread with a queue of steps, which the `PushInParts` holds with the `Push`: each `add`
  and the `commit` post a step to it, so they run in order; dropping the handle runs the steps queued and joins the thread
  (`world/push.hpp`, `world/push_in_parts.hpp`).
- **push of a dataset** (`world/datasets.hpp`): a random 64-bit stamp is drawn; the transaction begins with an advisory
  lock on the table and the dataset's name (`pg_advisory_xact_lock(hashtext(lock_key(...)))`), so two
  pushes of one dataset into one table run in turn; the rows carry the dataset and the stamp; before the commit the
  sweep deletes the dataset's rows whose stamp is another push's, and the registry records the push. A failure
  anywhere rolls the whole back. `remove` runs the remove statement in a transaction under the same lock.
- **verify:** on a thread started for the call, each layer in turn: the registry's record of the table, then schemacht's
  `table_differences_statement`, then the settings statement for a model with settings (`table_problems` in
  `world/verify.hpp`); `Reader::problems` for a reader. What they find is thrown as one `TablesDiffer`.
- **create_table:** the registry's setup (if absent), `CREATE TABLE` from the schema and the model's `setup_sql`, and the
  registry's record of the table under the name it resolves to (`miniverse_internal.layer_name`), in one transaction,
  waited for (`world/tables.hpp`). `drop_tables` forgets the table and its datasets, if there is a registry, then drops it.

Thread boundaries: a stream's conversions and callbacks run on the pool's threads. A load, `table_settings`, a push, a
`remove`, `datasets` and `verify` run on a thread started for the call, where the model makes its result or its rows;
their futures wait for that thread when dropped. Errors the work finds arrive through the future; a call that cannot
start (no thread, no memory) or makes no sense (a write to a reader layer, a push in parts that was moved from) throws,
as schemacht's calls do, so there is no code for handing an error back in a future that was never started. The parts of a push in parts run on the push's own thread, started with the push and joined when the
handle is dropped. What runs on the caller's thread and waits: the constructor
(it opens the pool), `create_table` and `drop_tables`, `osm::road_file` (it reads the file), `Reader::settings` and
`Reader::problems`, and the `.get()` calls the caller makes.

### The built-in models

- **FeatureModel<G>** (`model/feature_model.hpp`): `feature_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, dataset
  text, geom geometry(<G>,4326), tags jsonb, stamp bigint`, a GiST index on `geom` and a btree on `dataset`. Writes are
  plain inserts; the load projects id, geometry and tags. Geometries are written and read as PostGIS WKB (`geo/wkb.hpp`).
  Batches are bounded by `FEATURES_PER_STATEMENT` and `POINTS_PER_STATEMENT`.
- **RoadModel** (`model/road_model.hpp`): `way_id bigint PRIMARY KEY, dataset text, node_ids bigint[], geom
  geometry(LineString,4326), tags jsonb, stamp bigint`, a GiST index and a btree on `dataset`. The way id is the
  source's own (OpenStreetMap's), and writes are schemacht's `upsert_statement` matched on it: a way whose id is there
  updates that row. The array is data, not a key: a btree entry holds at most 2704 bytes, which a way of some hundreds of
  nodes exceeds. Batches of `WAYS_PER_STATEMENT`.
- **RasterModel<P>** (`model/raster_model.hpp`, `src/model/raster_model.cpp`; the pixel arithmetic in `model/raster/tiles.hpp`,
  which tiles a location has in `model/raster/reach.hpp`): `tile_id bigint PRIMARY KEY, rast raster`.
  The tile id is the tile's row and column in the world grid. `setup_sql` makes, in the library's schema `miniverse_internal`, the
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

### Add a model

`examples/own_model.cpp` is the example: `TownModel`, with typed columns, its own query and its own result. The steps:

1. Define the result type the program works with, and `LAYOUT`, the table layout's number: 1 to begin with, raised
   whenever `schema_type` or the setup changes, since a table records the layout it was made with and `verify` names
   one of another.
2. Name `writes`: `writes::Plain` unless the rows belong to datasets or the table keeps settings. Define `schema_type`
   with schemacht `Field`s; a geometry column uses a `geo::` type, whose SQL type is `geometry(<Type>,4326)`.
3. Write `LOAD` as a `query::select` against the schema, applying `geo::Intersects` (or another operation) to the
   geometry column with `arg<0>()`; `using load_statement_type = query::Prepared<LOAD>`.
4. Write `setup_sql`, `to_rows`, `write_statement` and `from_rows`.
5. `static_assert(miniverse::LayerModel<TownModel>)` tells you what is missing. A `Layer<TownModel, "towns">` names it. `tests/compile/own_model_test.cpp` shows that
   such a model needs only `world.hpp` and the `geo` headers.

A model with `writes::WithSettings<T>` adds `settings_statement_type` (a `RawStatement` taking the table's quoted name),
`settings_from_rows`, and the settings argument to `setup_sql` and `to_rows`; `RasterModel` is the example. A model with
`writes::Datasets` puts `dataset::Name` and `dataset::Stamp` (`model/writes.hpp`) in its schema and takes the
dataset and stamp in `to_rows`; the rest is the library's. `FeatureModel` is the plain example and `RoadModel` the one
keyed by the source's own id, written by upsert.

### Add a reader

Derive from `Reader<Model>` and implement `name`, `load`, `stream`, and `problems` if there is anything `verify` should
check; `settings` for a model with settings. Keep every member `const` and thread-safe. A layer takes the reader as a
`std::shared_ptr<const Reader<Model>>`. `tests/unit/reader_test.cpp` has
two small readers and the tests a reader should pass; the readers behind `gdal::feature_file`, `gdal::raster_file` and `osm::road_file` are
the real ones.

### Add a file format

A reader of files follows the pattern of `gdal/vector.hpp` and `gdal/feature_file.hpp`: in the first, a `Source` struct
naming the file and options, a chunked `read_*` for importing into tables, a `read_*_in(location)` for layers and a
`problems_of` for `verify`; in a header of its own, the `*File<Model>` reader and the `*_file<Layer>` function that makes
the `Layer` of it. Constrain the layer's model with a concept in `concepts/` that says
which models the format can feed (`gdal::ReadsFeatures`, `gdal::ReadsRasters`, `osm::ReadsWays`). Add a test that pushes a file
into a table and compares loads and streams of the two, as `tests/gdal/raster_equivalence_test.cpp` does with
`check_loads_as_a_table`.

### Add a pixel type

The pixel types are listed in `geo/concepts/raster.hpp` (`Pixel`, and `PixelType<P>` with PostGIS's name for each),
`src/gdal/common.hpp` (`gdal_type`, which has a `static_assert` for a type it does not know), the `extern template`
list at the end of `model/raster_model.hpp`, and the explicit instantiation lists at the end of `src/geo/raster_wkb.cpp`,
`src/model/raster/tiles.cpp`, `src/model/raster_model.cpp`, `src/gdal/raster.cpp` and `src/gdal/dataset.cpp`. A missed
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
| `MINIVERSE_BUILD_DOCS` | `AUTO` | the `miniverse_docs` target, if Doxygen 1.9 or newer is found |
| `MINIVERSE_INSTALL` | on when top level | the install rules and the CMake package |
| `MINIVERSE_SCHEMACHT_DIR` | `../schemacht` | schemacht's source tree, used when no installed schemacht is found |
| `MINIVERSE_WARNINGS_AS_ERRORS` | on when top level | a warning in miniverse's own targets fails the build; configuring with `--compile-no-warning-as-error` turns it off for that build tree |
| `MINIVERSE_LINT` | off | run clang-tidy on every translation unit as it is compiled (`MINIVERSE_CLANG_TIDY` names the program, with arguments after `;`) |

The GDAL minimum, 3.7, is `MINIVERSE_MIN_GDAL` in `CMakeLists.txt`, checked after `find_package`, since GDAL's package
only accepts a request for its own minor version. A full rebuild with unlimited parallel jobs has run out of memory on
a 16 GB machine; it has been built with `-j6`.

`cmake --install build --prefix <dir>` installs `libminiverse.a` (and the components' libraries), the headers and
`lib/cmake/miniverse/`, from which `find_package(miniverse COMPONENTS gdal osm)` gives `miniverse::miniverse`,
`miniverse::gdal` and `miniverse::osm`. schemacht is installed beside it when built from the sibling tree.

## Testing

The suite is four test executables, `miniverse_tests` (compile, unit and integration tests), `miniverse_gdal_tests`,
`miniverse_osm_tests` and `miniverse_tool_tests` (built when the tool and both components are); the example programs,
each run as a test of its own; the compile-fail cases, each a build of
`tests/compile_fail/cases.cpp` with one case defined that passes when the build fails with the expected message; and
`format`, clang-format over the tree, when it is found.

Tests that need a database skip themselves unless `MINIVERSE_TEST_DB` is set:

```sh
MINIVERSE_TEST_DB="postgresql://user:password@localhost:5432/scratch" ctest --test-dir build
```

The database must have `postgis` and `postgis_raster` enabled and be one you can lose: the tests create and drop
tables named `miniverse_test_*` and `miniverse_example_*`, and some tests drop the library's schema `miniverse_internal`.
Never point it at a database with data in it. The tests that share the database take a CTest `RESOURCE_LOCK`, so they
run one at a time under `ctest -j`.

Read the counts: `ctest` counts skipped tests in its "tests passed" percentage and lists them separately. With the
database up, no test should be skipped.

Parts of the suite:

- `tests/gdal/raster_equivalence_test.cpp` and `vector_equivalence_test.cpp`, `tests/osm/ways_test.cpp`: each test file is pushed into a table
  and the file layer's loads and streams are compared with the table's, for a list of locations including ones that
  touch tile edges. Use a grid of tenths of a degree and rough pixels for such tests: a dyadic grid and smooth data hide
  rounding differences.
- `tests/unit/reader_test.cpp`: the reader contract, including that loads run on their own threads.
- `tests/unit/step_thread_test.cpp`: the thread a push in parts runs on.
- `tests/integration/failure_test.cpp`: the database failing under a call, and writers in each other's way. It ends
  backends of its own role with `pg_terminate_backend` (allowed without any grant), makes two miniverses over one
  table, and appends libpq's `options` to the connection string for the timeouts, so the string must take a keyword
  or a URL parameter at its end.
- `tests/integration/raster_model_test.cpp`: the table's constraints, index use (by `EXPLAIN`), merging, lz4.

A ThreadSanitizer build of `miniverse_tests` with the components off (`-DCMAKE_CXX_FLAGS=-fsanitize=thread
-DMINIVERSE_BUILD_GDAL=OFF -DMINIVERSE_BUILD_OSM=OFF -DMINIVERSE_BUILD_EXAMPLES=OFF`) is the check for the threaded
paths: run `setarch $(uname -m) -R ./miniverse_tests "[reader],[step_thread],[integration]"` from it. The test
discovery step fails under TSan, and the binary crashes at startup unless address randomization is off (`setarch -R`); it
still links.

## Static checks and style

Every target of the project (the libraries, the tests, the examples, the compile-fail cases) is compiled with the warnings
in `cmake/checks.cmake`: `-Wall -Wextra -Wpedantic` and a strict set beyond them, for GCC and clang, as errors when
miniverse is the top-level project. The warnings left off are listed there with the reason for each. schemacht's and
Catch2's headers are system headers to the compiler, so only miniverse's own code is checked. Consumers of the installed
package inherit none of the flags.

clang-tidy 18 runs with the repository's `.clang-tidy`: every check but the families for other codebases and the few
named at its top, warnings as errors, include-cleaner included; `examples/.clang-tidy` and `tests/.clang-tidy` relax checks
that fight sample and test code. A build with `-DMINIVERSE_LINT=ON` runs it on every translation unit as it is compiled,
which is the check to run before a commit (it adds a clang-tidy run per translation unit; keep `-j` low, a tidy process
takes as much memory as a compile). One file at a time:

```sh
clang-tidy -p build --header-filter='.*/miniverse/(include|src|tests|examples)/.*' <file>
```

The `miniverse_docs` target fails on a public item without a comment or on a malformed one, and the test `format` runs
clang-format over every source and header when `MINIVERSE_CLANG_FORMAT` finds one. Another version of clang-format can
disagree with the one the tree was formatted with on a line or two: the test is as good as the version match.

Conventions the code follows:

- Classes are `CamelCase`, functions and variables `lower_case`, template parameters `lower_case` with a `_t` suffix,
  private members `_prefixed`, constants `UPPER_CASE`.
- Template parameters are constrained by named concepts, which live in `concepts/` directories.
- A class with a constructor declares all five special members (copy, move, destructor), defaulted or deleted, at the
  end of the class.
- A discarded result is `std::ignore = expr;`, never a cast to void.
- Steps of a function are separated by blank lines. Public declarations carry Doxygen comments (`@brief`, `@return`,
  `@throws`, with `@code` examples where a call is not obvious); what goes in a comment is under Comments below.
- No hand-rolled parsers or formats where a library does it (Boost.JSON for JSON, GDAL and libosmium for files).

### Comments

A comment carries context the code cannot: why this and not the obvious thing, a fact from outside the code (what PostGIS,
GDAL, libosmium or PostgreSQL does), a consequence a reader would not see, a guarantee a caller relies on (threads,
lifetimes, ordering). The rules:

1. A comment says what the code cannot. One that restates the line below it goes.
2. Describe the code as it is. No history and no "so that we don't X": state the positive fact. A real trap that is not
   visible gets one clause naming it. A sentence that exists only because the code once did something else is deleted.
3. Measurements stay out of the API docs. Timings and benchmark results go to the developer notes; a header keeps the
   decision in one clause. Arithmetic that explains a constant's value stays.
4. One place per fact. The `@file` says what the file is, in a phrase and at most a short paragraph. A class doc gives the
   contract and the rules a user must follow. A function doc gives inputs, outputs, throws and what the caller must
   guarantee. A fact that is in the guide or here is pointed to, not repeated.
5. Inline comments are rare, one clause, and never a label. Keep a non-obvious reason for a line and every NOLINT's reason.
6. Plain sentences: one idea each, no chains of colons, no flourish. Bullets for lists of facts.
7. Length follows content: the test is rule 1 applied line by line, not a line count.

The API reference is built from the header comments: `cmake --build build --target miniverse_docs` writes
`build/docs/html/` when Doxygen is installed (`MINIVERSE_BUILD_DOCS`). README.md is its front page and the files in `docs/`
are further pages.
