# Using miniverse

This guide goes through what a program does with miniverse: open a world of layers, load and stream what lies in an
area, push data in, and read the same layers from files. The headers under `include/miniverse/` document every type and
call in full; the examples under `examples/` are complete programs, and the longer snippets below name the file they come from.

## The model

A **model** is a C++ type that says what a table's rows are, what a load returns, how the table is queried and how a push
treats the rows already there. The built-in models are `FeatureModel<Geometry>`, `RoadModel` and `RasterModel<Pixel>`;
a model of your own is a type of the same shape (see [Add a model](developer.md#add-a-model)). A model is never made: it
only names what a table of it is.

A **layer** is a model under a name of the program's, in one table of a PostGIS database, or read by a reader (a file, or
data of the program's own):

```cpp
using Roads = miniverse::Layer<miniverse::RoadModel, "roads">;
using Elevation = miniverse::Layer<miniverse::RasterModel<std::int16_t>, "elevation">;  // heights in whole metres
```

The name is what the program asks for the layer by (`load<"roads">`). Two tables of one model are two layers of two
names, `Layer<RoadModel, "roads">` and `Layer<RoadModel, "tracks">`.

A **miniverse** holds the layers it is made with, and one pool of connections they share:

```cpp
miniverse::Miniverse world(conninfo, Roads("osm_roads"), Elevation());
```

Each layer names its table when the miniverse is made, so one program serves whichever tables its configuration names;
made with no table name, a layer is in the table called as it is (`elevation`, here). A layer can also take a schema
name (`Roads("gis", "roads")`) or a reader (see [Files as layers](#files-as-layers)). Asking the miniverse for a name it
does not hold is a compile error, as is giving two layers one name.

The calls that read and write data (`load`, `stream`, `push`, `begin_push`, `remove`, `verify`, `table_settings`) return a
`std::future`, and their errors arrive through it, not as exceptions from the call. The constructor, `create_table`,
`drop_tables` and `Layer` itself wait and throw directly. The miniverse must outlive the futures it hands out.

## Locations

A location is a `geo::Polygon`: Boost.Geometry's polygon of `geo::Point`s in WGS 84, `x` the longitude and `y` the
latitude, in degrees. Its outer ring runs counter-clockwise and is closed (the last point repeats the first); holes are
further rings. The examples build boxes with a helper from `examples/support.hpp`:

```cpp
miniverse::geo::Polygon area;
area.outer() = {{west, south}, {east, south}, {east, north}, {west, north}, {west, south}};
```

A polygon with holes loads what is outside the holes. A polygon of no points loads nothing. A ring that is not closed,
or has fewer than four points, fails a load of a table of lines, areas or raster tiles (PostGIS cannot test it; a table
of points accepts it), and file layers refuse it before reading, so a worker tried on files does not fail later on
tables. Boost.Geometry's `correct` closes a ring and fixes its direction.

## Tables

### Making tables

```cpp
world.create_table<"roads">();
world.create_table<"elevation">({.pixels_per_degree = 3600, .tile_pixels = 512, .nodata = -32768});
```

`create_table` makes the table and runs the model's setup (its spatial index; for a raster, its constraints and merge
function) in one transaction, and waits. It also records the table in the registry, with the layout the model is at
(`Model::LAYOUT`, a number raised when a model's table layout changes), for `verify` to read back. The registry is the
schema `miniverse_internal`, which the library keeps for itself in the database: its tables `layers` and `datasets`,
and its functions. The role that first makes a table in a database makes the schema and owns it; another role that
makes tables needs CREATE on the database, and one that pushes needs USAGE on the schema with INSERT and UPDATE on
`datasets` (DELETE, to remove). A worker that only loads and verifies needs USAGE and SELECT on `layers`. A model with settings, such as a raster's grid, takes them here and the table
keeps them. `create_tables()` makes every table of a miniverse whose models have no settings. `drop_tables()` drops them.
PostGIS must already be enabled in the database (`CREATE EXTENSION postgis`, and `postgis_raster` for rasters). A raster
table's setup turns on lz4 compression for its tiles, which needs a PostgreSQL 14 or newer built with lz4; the usual
packages and images are.

### Pushing

```cpp
world.push<"roads">(miniverse::Dataset{"village"}, std::move(roads)).get();
world.push<"elevation">(std::move(heights)).get();
```

A push writes all of its data or none: the rows go in batches of one statement each, inside one transaction. The
transaction holds one of the pool's connections until it ends.

Feature and road layers are pushed as datasets. A `Dataset` is a name for an export: the file a producer made of a
region, under whatever name the pusher gives it. The first push of a name writes the dataset; every later push of the
same name replaces it, all at once: what the new export lacks is gone, what it has is there. `remove` deletes a dataset and gives how many rows it deleted: 0 for a name no push wrote.
`datasets<"roads">()` lists a table's datasets, each with when it was last pushed and how many rows it holds. A worker
never sees dataset names: a load gives the layer as one whole. A feature's id is one the database gave its row; a
way's is its own, OpenStreetMap's. So refresh a region by pushing its export again under the name it was first
pushed under, and keep a name to the same ground over time. Two pushes of one dataset of one layer at once take turns,
and a `remove` of it waits for an open push of it the same way; pushes of different datasets run side by side, and a
load or a stream never waits for a push, nor a push for one. A raster layer is not pushed as a dataset: each tile is
merged onto the one already there.

For data too large to hold at once, `begin_push` opens a push that takes parts, from `examples/import_osm.cpp`:

```cpp
miniverse::PushInParts<miniverse::RoadModel> roads = world.begin_push<"roads">(miniverse::Dataset{"town.osm.pbf"}).get();

miniverse::osm::read_ways({.path = path, .keys = {"highway"}}, 10'000, [&roads](miniverse::Ways chunk) {
  roads.add(std::move(chunk)).get();
});

roads.commit().get();  // now they are all there, for everyone at once
```

Every part is written in the same transaction, so a file that fails halfway leaves nothing, and workers see nothing
until the commit. Each `add` holds its part until the part is written: the `.get()` above holds one chunk at a time, and
a reader that only waits for the previous `add` before reading the next holds two. For a dataset, the parts together are
the dataset: it is replaced when the push commits, so a file read a chunk at a time goes through one `begin_push`, never
a `push` per chunk, which would replace the dataset with each chunk. A commit with no parts, like a push of no data,
leaves the dataset empty.

### Loading

```cpp
std::future<miniverse::Ways> roads = world.load<"roads">(area);
for ( const miniverse::Way& way : roads.get() ) { ... }
```

A load gives what the model says a load is: for features and roads, those that intersect the location, ordered by id;
for a raster, the pixels of the location's box (see [Raster layers](#raster-layers)). Loads of several layers can be
started together and waited for together, as `examples/local_files.cpp` does.

### Streaming

A stream hands the result to a callback a chunk at a time, so a worker can build a structure of its own from a large
area without holding the result. From `examples/road_network.cpp`:

```cpp
world
    .stream<"roads">(
        area,
        [&network, &roads, &chunks](const miniverse::Ways& chunk) {
          for ( const miniverse::Way& road : chunk ) {
            add_road(network, road);
          }

          roads += chunk.size();
          ++chunks;
        },
        {.chunk_rows = 2}
    )
    .get();  // once this returns, every chunk has been handed over
```

A chunk is `chunk_rows` rows made into the model's result (the last chunk has what is left); without options the pool's
default of 1000 rows applies. The callback runs on a thread of the pool (for a file layer, on the thread started for the
load), one call at a time, never two at once, so what it fills needs no lock as long as the caller only waits on the
future. It may return `bool`: `false` stops the stream.
What it throws becomes the stream's error. It must not wait on another future of the same miniverse.

For a raster layer a chunk is that many whole tiles, in no particular order. About `chunk_rows` times
(`max_buffered_chunks` + 2) tiles are held at once, with `max_buffered_chunks` 4 unless the options say otherwise; a
tile is megabytes, so ask for a few at a time.

### Checking the tables

```cpp
world.verify().get();
```

`verify` first asks the registry which layout each layer's table was made with. A table made with another layout than
its model is at now, or one the registry does not know (made before layouts were recorded, or by hand), is named with
what to do: make it again and import its data. A table of the right layout is then compared with its model: the
columns, their types, which may be null, the primary key, and that no other column would need filling. A model with
settings must also have them in the table. The layers are checked one after another, on a thread started for the call,
and the future holds a `TablesDiffer` naming every difference found. It is meant for a program's start, so a missing or wrong table fails there rather than
at the first load. Indexes and permissions are not checked. It needs PostgreSQL 17.

`table_settings<"elevation">()` gives the settings a table was made with, for a writer that has to warp its source onto
the table's grid.

## Feature layers

A `FeatureModel<Geometry>` holds features of one geometry type: `geo::Point`, `geo::LineString`, `geo::Polygon`,
`geo::MultiLineString` or `geo::MultiPolygon`. Each feature has an `id`, its `geometry` and its `tags`, one JSON object
held as text (`schemacht::json::Json`). From `examples/features.cpp`:

```cpp
using Cafes = miniverse::Layer<miniverse::FeatureModel<geo::Point>, "cafes">;
using Parks = miniverse::Layer<miniverse::FeatureModel<geo::Polygon>, "parks">;
```

The table's geometry column is typed (`geometry(MultiPolygon,4326)`), so the database refuses another type. A dataset
that mixes polygons and multipolygons, as most do, is stored as `MultiPolygon`, each single polygon as a multipolygon of
one part.

Files become features with `gdal::read_features`, which transforms coordinates to WGS 84, promotes single geometries
where a multi type is asked for, and makes the other fields into tags (numbers as numbers, booleans as `true`/`false`,
the rest as text). From `examples/import_vector.cpp`:

```cpp
miniverse::gdal::read_features<geo::MultiPolygon>({.path = path}, 10'000, [&buildings](miniverse::Features<geo::MultiPolygon> chunk) {
  buildings.add(std::move(chunk)).get();
});
```

A `VectorSource` names the file, optionally the layer in it, and optionally a whole-number field to take ids from
(otherwise the file's own feature ids are used). Those ids are the features' when a file is read or a file layer is
loaded; in a table they are not kept: a loaded feature's `id` is the database's own, and a dataset pushed again gives
its features new ids. Nothing tells a feature of one dataset from one of another, so two datasets that cover the same
ground are both loaded there: cut feature datasets to each other.

## Road layers

A `RoadModel` holds OpenStreetMap ways: each `Way` has its `id`, its `node_ids` in order, its `coordinates` as a
`geo::LineString` with one point per node, and its `tags`. Node ids are what let a worker join roads into a network:
two ways that meet share a node. `examples/road_network.cpp` builds a graph from them and finds the shortest route.

`osm::read_ways` reads a `.osm.pbf`, `.osm` or `.osm.bz2` file, the ways with a tag of one of the given keys (`highway`
for roads), in the file's order. It holds the position of every node of the file while it reads, 16 bytes each, so read
an extract of the area wanted rather than a continent. A way with a node the file lacks (an extract cuts ways at its
edge) is left out and counted.

In a table a way is its id, as in OpenStreetMap: the row's key is the way id the file gave it. A way that two exports
both carry whole (one that crosses the line between them) is one row, of the dataset that last pushed it, and a way
pushed again keeps its row and takes the later push's nodes, line and tags. A load gives the layer as one network,
joined by node ids, whatever exports it came from. What this asks of the data:

- One layer, one id space, OpenStreetMap's: way ids and node ids are stable across exports and over time, and never
  reused for another place. A producer that cannot promise that gets a layer of its own.
- An export holds its ways whole, as `osmium extract` and Geofabrik produce them; a way with a missing node is dropped on
  read.
- A dataset is refreshed under the name it was first pushed under, and a name covers the same ground over time.

A way that a neighbouring export took over and then dropped stays until that neighbour is pushed again.

Roads of your own join the same layer by being OpenStreetMap-shaped, which is how the OpenStreetMap tools combine
sources: `ogr2osm` turns a shapefile or GeoPackage into an `.osm` file (with placeholder negative ids, as editors give
new objects), `osmium renumber --start-id` moves those into a range of ids OpenStreetMap does not use (without
`--start-id` it numbers from 1, OpenStreetMap's own range), and a road that joins an OpenStreetMap street shares its node
at the junction. Pushed as a dataset of its own, or merged into an extract first
with `osmium merge`, it is one network with the rest. Where your source and OpenStreetMap describe the same street, that
is conflation, a step for people with tools such as JOSM's Conflation plugin, before the data comes here: miniverse
matches ways by id and nothing else. A source with ids of its own and no conflation is a second network, and belongs in
a layer of its own.

## Raster layers

A `RasterModel<Pixel>` holds one band of pixels, cut into the square tiles of a grid, one row per tile. The pixel type is
one of `std::int8_t`, `std::uint8_t`, `std::int16_t`, `std::uint16_t`, `std::int32_t`, `std::uint32_t`, `float` or
`double`.

### The grid

```cpp
world.create_table<"elevation">({.pixels_per_degree = 3600, .tile_pixels = 512, .nodata = -32768});
```

The grid is chosen once, when the table is made, and every source is warped onto it. It is in WGS 84 degrees: a whole
number of pixels per degree, counted from longitude -180 and latitude 90, in tiles of `tile_pixels` a side. 3600 pixels
per degree is one arc second, the resolution of SRTM and USGS 1-arc-second data. For a resolution given in metres,
`geo::pixels_per_degree_of_metres(30)` gives the nearest whole number of pixels per degree; `grid.metres_per_pixel()`
goes the other way.

Guidance from use so far: tiles of 512 pixels suit loads of a few kilometres to a hundred; for heights at 30 m, 16-bit
whole metres take less space than 32-bit reals, which the tiles' compression (lz4, turned on by the table's setup) does
little for.

A pixel is the same height everywhere but narrower towards the poles: at 3600 pixels per degree it is about 31 m north to
south at any latitude, and 31 m times the cosine of the latitude east to west (23 m at 42°). Anything computed across
pixels in metres has to allow for that.

### Pushing rasters

A pushed `geo::Raster` must lie on the table's grid: its pixel size the grid's, its corner on a pixel edge. It is cut
into tiles and each tile is merged onto the one already there: where the new tile has data it wins, where it has none
the old pixel stays. So several files can be pushed one after another; where neighbouring files overlap by a few pixels,
the table holds the later file's data there.

Files become rasters with `gdal::read_raster`, which warps the file from its own coordinate system and pixel size onto
the grid, a window of whole tiles at a time. From `examples/import_raster.cpp`:

```cpp
const geo::Grid<std::int16_t> grid = world.table_settings<"elevation">().get();

miniverse::gdal::read_raster<std::int16_t>({.path = path, .resampling = miniverse::gdal::Resampling::Bilinear}, grid, 2,
    [&heights](Heights window) { heights.add(std::move(window)).get(); });
```

`RasterSource` names the file, the band (1 unless given) and the resampling: nearest, bilinear (the default), cubic or
average. A file already on the grid reads as its own pixels with nearest resampling; a file in another system or of
another pixel size is resampled once, here, and never again.

### Loading rasters

A load gives one `geo::Raster`: the pixels of the location's bounding box, widened to whole pixels, stitched from the
tiles the location reaches into. Pixels in tiles the location does not reach are `nodata`, even where the table has
data there; so a long thin area that runs askew costs its own tiles, not all the tiles of its box. A tile the location
only touches at an edge is not read. Where there are no tiles at all, the raster is empty (0 by 0).

The raster knows where it lies (`west`, `north`, `pixel_width`, `pixel_height`), and answers positions directly
(`examples/elevation.cpp` uses `value_at`):

```cpp
heights.value_at({7.6, 46.6})   // the pixel's value, or nothing outside the raster or where the pixel is nodata
heights.pixel_at({7.6, 46.6})   // its column and row, or nothing outside
heights.centre_of(column, row)  // the pixel's middle, a longitude and latitude
heights.at(column, row)         // by index; std::out_of_range past the edge
```

`value_at` gives the pixel the position lies in, not a value interpolated between pixels.

A stream of a raster layer hands over whole tiles, as stored, a few at a time: for a worker with a large area that keeps
only what it needs from each tile (`examples/elevation.cpp` finds the highest value this way).

### A load as a GDAL dataset

```cpp
const GDALDatasetUniquePtr dataset = miniverse::gdal::dataset_of(heights);
```

`gdal::dataset_of` copies a raster into a GDAL dataset in memory, with its position, WGS 84 and nodata set, for GDAL's
own tools: a warp into a local projection, a slope, or a file of any format GDAL writes
(`GetGDALDriverManager()->GetDriverByName("GTiff")->CreateCopy(...)`). A raster of no pixels, which a load where there
are no tiles gives, is refused with `std::invalid_argument`. It is in `miniverse/gdal/dataset.hpp`, the one header of
miniverse that includes GDAL's.

Because the table's constraints record the grid the way PostGIS's `raster_columns` reads them, GDAL and QGIS also open
a raster table directly as one raster. Its extent is then the whole world.

## Files as layers

A layer need not be a table. `miniverse::gdal` and `miniverse::osm` give readers of files, and a miniverse whose layers
all have readers is made without a connection string, from `examples/local_files.cpp`:

```cpp
World files(miniverse::osm::road_file<Roads>(streets), miniverse::gdal::raster_file<Elevation>(heights, grid));
```

Each takes the layer type and gives a layer of it with the file as its reader:

- `gdal::feature_file<Layer>(VectorSource)`: a vector file; the features in the location, tested exactly against it,
  after the file's own spatial index has narrowed them down, if it has one.
- `gdal::raster_file<Layer>(RasterSource, grid)`: a raster file, warped onto the given grid on each load. The grid is
  what a table's would be, and `table_settings` returns it.
- `osm::road_file<Layer>(WaySource)`: an OpenStreetMap file, read whole into memory when the layer is made and indexed by
  position; a bad file fails there.

The readers are in `miniverse/gdal/feature_file.hpp`, `miniverse/gdal/raster_file.hpp` and `miniverse/osm/road_file.hpp`;
`miniverse/gdal/gdal.hpp` and `miniverse/osm/osm.hpp` include all of a component. No component header brings in the
built-in models or the `Miniverse` itself: include `miniverse/miniverse.hpp` (or `world.hpp`, with models of your own)
beside them.

`load`, `stream`, `verify` and `table_settings` work the same on these layers; `push`, `begin_push` and `remove` fail with
a `std::logic_error`, since a file is only read; `create_table` and `drop_tables` skip them, so one setup serves a world of
tables and a world of files. A load of a reader runs on a thread started for it, so loads of several file layers run at
once; the future of such a load waits for that thread if it is dropped before it is done.

What a file layer gives is what a table of the same data gives, with three differences: the ids are the file's own,
where a table's are the database's; the order is the file's own, where a table's is by id; and tags are spelt as the
reader writes the JSON, where a table's are as PostgreSQL's `jsonb` prints it (the same document, another text). The
tests push each test file into a table and compare the loads by geometry and nodes.

## A reader of your own

`Reader<Model>` is the class behind the file layers; a class of your own can hand a layer fixed data, for a test. It
implements `name`, `load` and `stream` (and `settings`, for a model with settings such as a raster), optionally
`problems` for `verify`. A layer is made from a shared pointer to one. From `tests/unit/reader_test.cpp`, a reader of
fixed points, abridged:

```cpp
using Points = miniverse::Features<geo::Point>;

template <miniverse::LayerModel model_t>
class FixedPoints : public miniverse::Reader<model_t> {
 public:
  [[nodiscard]] std::string name() const override { return "fixed points"; }

  [[nodiscard]] Points load(const geo::Polygon& location) const override {
    Points inside;
    std::ranges::copy_if(_points, std::back_inserter(inside), [&location](const miniverse::Feature<geo::Point>& point) {
      return boost::geometry::covered_by(point.geometry, location);
    });

    return inside;
  }

  void stream(const geo::Polygon& location, std::size_t chunk_rows, const miniverse::Reader<model_t>::OnChunk& on_chunk) const override {
    const Points inside = load(location);
    for ( std::size_t first = 0; first < inside.size(); first += chunk_rows ) {
      const std::size_t last = std::min(inside.size(), first + chunk_rows);
      if ( ! on_chunk(Points(inside.begin() + static_cast<std::ptrdiff_t>(first), inside.begin() + static_cast<std::ptrdiff_t>(last))) ) {
        return;
      }
    }
  }
  ...
};

using ShopModel = miniverse::FeatureModel<geo::Point>;
using Shops = miniverse::Layer<ShopModel, "shops">;

miniverse::Miniverse world(Shops(std::make_shared<FixedPoints<ShopModel>>(points)));
```

Every member is `const` and may be called from several threads at once. `load` and `stream` run on a thread started for
the load; `name`, `settings` and `problems` on the caller's thread.

## The command-line tool

`miniverse` does what an uploader does for the built-in models, without a program: it makes a table, imports files into
it, removes a dataset, checks a table, and shows a raster table's grid. Each command acts on one table, named with its
model:

```sh
miniverse <command> [--db <connection string>] --model <model> <table> ...
```

- `--db` is a libpq connection string, of keywords or a URI. Without it, libpq's defaults and the `PG*` variables say
  where.
- The table is `table`, in the connection's search path, or `schema.table`.
- `--model` is `features:<geometry>` (`point`, `linestring`, `polygon`, `multilinestring` or `multipolygon`), `roads`,
  or `raster:<pixel>` (`int8`, `uint8`, `int16`, `uint16`, `int32`, `uint32`, `float32` or `float64`).
- It exits with 0 when the command is done, 1 when it failed, and 2 for arguments it can't take, before it connects; the
  reason is on standard error. An option for a table of another model than the one named is one of those.

The database and its extensions are made beforehand, as for the library (`createdb`, then `CREATE EXTENSION postgis`
and `postgis_raster`).

| Command | Does |
|---|---|
| `create <table>` | Makes the table, as `create_table`. A raster table takes its grid: `--pixels-per-degree N` or `--metres M` (made the nearest pixels per degree), `--tile-pixels N` and `--nodata V`. |
| `import <table> <file>...` | Checks the table, as `verify`, then pushes the files into it a part at a time, each file all of it or none. |
| `remove <table> <dataset>` | Removes a dataset from a table of features or roads. A name no import wrote is an error. |
| `verify <table>` | Checks the table is as its model says; the differences are the error. |
| `settings <table>` | Shows the grid of a raster table. |
| `datasets <table>` | Lists the datasets of a table of features or roads: name, rows, and when each was last pushed. |

What `import` reads depends on the model:

- **Features:** one vector file of any format GDAL opens, as `gdal::read_features`. It replaces the dataset `--dataset`,
  which is the file's name unless given (a directory's, for a path ending in `/`): two files of one name in different
  directories are one dataset unless each is named. `--layer` names the layer of a file with several. The table numbers
  the features itself; a file's own ids are not kept.
- **Roads:** one OpenStreetMap file, as `osm::read_ways`, replacing its dataset in the same way. `--keys` names the tags
  a way must have one of, separated by commas or spaces: `highway` unless given.
- `--chunk` is how many features or ways make a part (10000).
- **Rasters:** any number of raster files, each warped onto the table's grid (as `gdal::read_raster`) and pushed in
  turn: where two overlap, the later one's data is what the table has. A file that fails is not written, nor the files
  after it; the files before it stay. `--band` (1) and `--resampling` (`nearest`, `bilinear`, `cubic` or `average`;
  `bilinear`) say how it is read, and `--window-tiles` the side of a part in tiles (2). A raster table has no datasets.

Vector and raster files need the tool built with `miniverse::gdal`, and OpenStreetMap files with `miniverse::osm`; a
tool built without one says so.

## Errors and threads

- Errors from the database and from a model's conversions arrive through the future of the call. `verify` fails with
  `TablesDiffer`; a push to a file layer with `std::logic_error`; a wrong argument (a grid that is none, a chunk of zero
  rows) with `std::invalid_argument`; GDAL and libosmium errors as `std::runtime_error` with the library's message.
- Stream callbacks run on the pool's threads (on the load's own thread for a file layer), one call at a time. Every other
  call that returns a future (`load`, `table_settings`, `push`, `begin_push`, `remove`, `datasets`, `verify`) runs on a
  thread started for the call: the call returns at once, and its future waits for that thread if it is dropped before it
  is done. A push in parts then has one thread of its own, on which every part is made
  into rows and written, in the order the parts were added; an `add` need not be waited for before the next, and
  dropping the push waits for the parts added to be written. A push begins its transaction before it makes its rows.
- The constructor opens the pool and waits; `create_table` and `drop_tables` wait; `osm::road_file` reads its file when
  it is called.
- A push holds one of the pool's connections for its transaction; with a pool of one connection nothing else runs
  during it. The pool's size is `Database::Options::pool.connections` (4 by default), given to the `Miniverse`
  constructor that takes options.
- A connection lost under a call (the server ended it, the network went) fails that call's future. For a push in parts,
  the part that found it gone fails with that error, and so does every later `add` and the `commit`: nothing of the push
  is written, and the server let go of the dataset's lock with the connection, so a new push of the dataset runs at
  once. A stream's future fails; the chunks handed over before it stay handed over. The pool re-establishes the
  connection, with its session settings, and the next call waits for it.
- The library sets no timeouts. libpq's `options` in the connection string sets them for every connection of the pool:
  `options='-c lock_timeout=5000 -c statement_timeout=60000'` (in a URL, `?options=-c%20lock_timeout%3D5000`). A push
  or remove that waits longer than `lock_timeout` for an open push of its dataset fails with SQLSTATE `55P03`, and a
  statement that runs longer than `statement_timeout` with `57014`, each through its future, with nothing written.

## Limits

- Coordinates are WGS 84 only; a file in another system is transformed when it is read.
- A raster file that crosses the antimeridian is refused; near the poles a pixel's width in metres shrinks towards zero.
- A raster layer holds one band. There are no coarser levels for zoomed-out viewing of a table: a viewer reading a
  country at 30 m reads every tile.
- A location that is a point or a line lying exactly along the line between two raster tiles has no tile; strictly inside
  a tile it has that tile.
- `verify` needs PostgreSQL 17 for the catalog query it uses.
- The pool sets its session settings with `SET` on each connection it opens. PgBouncer in transaction mode hands a
  connection's session to other clients between transactions, so those settings are lost and leak: connect directly,
  or through a pooler in session mode that passes libpq's `options` startup parameter through (PgBouncer rejects it
  unless `ignore_startup_parameters` names it, and then drops it, with the timeouts set in it).
- A dataset cannot be deleted by area, only whole, by name. A table made with another layout than its model is at now,
  or before the registry existed, does not verify and must be made again and refilled; `verify` says so. There is no
  migration.
