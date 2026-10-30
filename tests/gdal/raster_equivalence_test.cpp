// A raster file loads and streams as the table it was pushed into: the file-versus-table
// comparisons. Each test needs MINIVERSE_TEST_DB (a libpq connection string to a scratch database
// with PostGIS and postgis_raster), and is skipped without it.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/geometry/io/wkt/read.hpp>
#include <boost/geometry/io/wkt/write.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "gdal/tiffs.hpp"
#include "miniverse/gdal/gdal.hpp"
#include "miniverse/miniverse.hpp"
#include "support/database.hpp"
#include "support/files.hpp"
#include "support/geometry.hpp"

namespace bg   = boost::geometry;
namespace geo  = miniverse::geo;
namespace gdal = miniverse::gdal;

using test::as_it_is;
using test::Elevation;
using test::FILE_NODATA;
using test::Files;
using test::GRID;
using test::GRID_NODATA;
using test::in_order;
using test::MERCATOR_DEGREE;
using test::mercator_north;
using test::numbered;
using test::polygon;
using test::Raster;
using test::rough;
using test::test_db;
using test::write_tiff;

namespace {

/**
 * @brief Checks that the file `source` on `grid` gives, for each of `locations`, what a table gives
 * that it was pushed into: the same raster for a load, and the same tiles for a stream.
 */
void check_loads_as_a_table(const gdal::RasterSource&        source,
                            const std::vector<geo::Polygon>& locations,
                            const geo::Grid<std::int16_t>&   grid = GRID)
{
  miniverse::Miniverse tables(test_db(), Elevation("miniverse_test_gdal_elevation"));
  tables.drop_tables();
  tables.create_table<"elevation">(grid);
  std::ignore = gdal::read_raster<std::int16_t>(source, grid, 1, [&tables](Raster window) {
    tables.push<"elevation">(std::move(window)).get();
  });

  miniverse::Miniverse files(gdal::raster_file<Elevation>(source, grid));

  for ( std::size_t i = 0; i < locations.size(); ++i ) {
    INFO("location " << i << ": " << bg::to_wkt(locations.at(i)));
    std::vector<Raster> table_tiles;
    std::vector<Raster> file_tiles;
    const auto          into = [](std::vector<Raster>& tiles) {
      return [&tiles](std::vector<Raster> chunk) {
        tiles.insert(tiles.end(), chunk.begin(), chunk.end());
      };
    };

    const Raster from_table = tables.load<"elevation">(locations.at(i)).get();
    const Raster from_file  = files.load<"elevation">(locations.at(i)).get();
    tables.stream<"elevation">(locations.at(i), into(table_tiles), {.chunk_rows = 3}).get();
    files.stream<"elevation">(locations.at(i), into(file_tiles), {.chunk_rows = 3}).get();

    CHECK(from_file == from_table);
    CHECK(in_order(file_tiles) == in_order(table_tiles));
  }

  tables.drop_tables();
}

}  // namespace

TEST_CASE(
    "integration: a GeoTIFF pushed a window at a time, onto the table's own grid, loads back as "
    "its pixels",
    "[gdal][integration]")
{
  const Files       files;
  const std::string path = files.path_of("on_grid.tif");
  write_tiff(path, {.epsg   = 4326,
                    .west   = 1,
                    .north  = 3,
                    .pixel  = 0.25,
                    .width  = 8,
                    .height = 8,
                    .pixels = numbered(8, 8)});

  miniverse::Miniverse world(test_db(), Elevation("miniverse_test_gdal_elevation"));
  world.drop_tables();
  world.create_table<"elevation">(GRID);

  const geo::Grid<std::int16_t> grid = world.table_settings<"elevation">().get();  // as a writer
                                                                                   // gets it: from
                                                                                   // the table
  const gdal::RasterRead        read = gdal::read_raster<std::int16_t>(
      as_it_is(path), grid, 1,
      [&](Raster window) { world.push<"elevation">(std::move(window)).get(); });
  const Raster loaded =
      world.load<"elevation">(bg::from_wkt<geo::Polygon>("POLYGON((1 1,3 1,3 3,1 3,1 1))")).get();

  CHECK(read.windows == 4);
  CHECK(loaded == gdal::read_raster<std::int16_t>(as_it_is(path), grid));  // what GDAL read from
                                                                           // the file, on the grid
  CHECK(loaded.pixels == numbered(8, 8));  // which is the file's own pixels

  world.drop_tables();
}

TEST_CASE(
    "integration: a raster file on the grid loads and streams as the table it was pushed into",
    "[gdal][integration]")
{
  // Three tiles by two from (1, 3), numbered; the east tile of the north row has no data at all,
  // and one pixel elsewhere none.
  const Files               files;
  const std::string         path   = files.path_of("holed.tif");
  std::vector<std::int16_t> pixels = numbered(12, 8);
  pixels.at(0)                     = FILE_NODATA;
  for ( std::size_t row = 0; row < 4; ++row ) {
    for ( std::size_t column = 8; column < 12; ++column ) {
      pixels.at((row * 12) + column) = FILE_NODATA;
    }
  }
  write_tiff(path, {.epsg   = 4326,
                    .west   = 1,
                    .north  = 3,
                    .pixel  = 0.25,
                    .width  = 12,
                    .height = 8,
                    .pixels = pixels});

  check_loads_as_a_table(
      as_it_is(path),
      {
          polygon("POLYGON((0 0,6 0,6 6,0 6,0 0))"),  // all of it, and around it
          polygon("POLYGON((1 1,4 1,4 3,1 3,1 1))"),  // its own extent
          polygon("POLYGON((1.3 2.3,1.6 2.3,1.6 2.6,1.3 2.6,1.3 2.3))"),  // inside one tile
          polygon("POLYGON((1.5 1.5,3.5 1.5,3.5 2.5,1.5 2.5,1.5 1.5))"),  // across tiles, and into
                                                                          // the one with no data
          polygon("POLYGON((3.1 2.1,3.9 2.1,3.9 2.9,3.1 2.9,3.1 2.1))"),  // in the tile with no
                                                                          // data alone: nothing
          polygon("POLYGON((0 2,1 2,1 3,0 3,0 2))"),  // west of it, touching its edge
          polygon("POLYGON((2 3,3 3,3 4,2 4,2 3))"),  // north of it, touching its edge
          polygon("POLYGON((0.5 2.5,1.5 2.5,1.5 3.5,0.5 3.5,0.5 2.5))"),  // over its corner
          polygon("POLYGON((1.5 2.5,1.5 2.5,1.5 2.5,1.5 2.5))"),          // a point
          polygon("POLYGON((1 2.6,3 1.2,2 1.2,1 2.6))"),  // a triangle: its box, from the tiles it
                                                          // reaches into
          polygon("POLYGON((1.1 1.6,1.12 1.58,3.4 2.88,3.38 2.9,1.1 1.6))"),  // a strip askew: few
                                                                              // of its box's tiles
          polygon("POLYGON((0.5 0.5,4.5 0.5,4.5 3.5,0.5 3.5,0.5 0.5),(1.9 0.9,1.9 2.1,3.1 2.1,3.1 "
                  "0.9,1.9 0.9))"),                       // a tile in its hole
          polygon("POLYGON((1.5 2,2.5 2,2.5 2,1.5 2))"),  // a line along the line between tiles:
                                                          // nothing
          polygon("POLYGON((40 40,41 40,41 41,40 41,40 40))"),  // far away
      });
}

TEST_CASE(
    "integration: a raster file in another coordinate system loads and streams as the table it was "
    "pushed into",
    "[gdal][integration]")
{
  // Two degrees each way in Web Mercator, from longitude 1 and latitude 3, as numbered pixels, made
  // into the grid's by bilinear weights: a pixel must come out the same whichever window it is
  // warped in.
  constexpr double  PIXEL = MERCATOR_DEGREE / 10;
  const Files       files;
  const std::string path = files.path_of("mercator.tif");
  write_tiff(path, {.epsg   = 3857,
                    .west   = MERCATOR_DEGREE,
                    .north  = mercator_north(3),
                    .pixel  = PIXEL,
                    .width  = 20,
                    .height = 20,
                    .pixels = numbered(20, 20)});

  check_loads_as_a_table(
      {.path = path, .band = 1, .resampling = gdal::Resampling::Bilinear},
      {
          polygon("POLYGON((0 0,6 0,6 6,0 6,0 0))"),
          polygon("POLYGON((1.3 2.3,1.6 2.3,1.6 2.6,1.3 2.6,1.3 2.3))"),
          polygon("POLYGON((1.5 1.5,2.5 1.5,2.5 2.5,1.5 2.5,1.5 1.5))"),
          polygon("POLYGON((2.6 0.5,3.4 0.5,3.4 1.4,2.6 1.4,2.6 0.5))"),      // over the file's
                                                                              // south-east corner
          polygon("POLYGON((1.1 1.2,1.12 1.18,2.9 2.88,2.88 2.9,1.1 1.2))"),  // a strip askew
          polygon("POLYGON((0 2,1 2,1 3,0 3,0 2))"),
          polygon("POLYGON((40 40,41 40,41 41,40 41,40 40))"),
      });
}

TEST_CASE(
    "integration: on a grid of tenths of a degree, a raster file loads and streams as the table it "
    "was pushed into",
    "[gdal][integration]")
{
  // Ten pixels to a degree in tiles of three: no tile's edge but the grid's own corner is a number
  // a double holds exactly, and tiles don't divide the world. Rough ground, three degrees by two
  // from longitude 1, latitude 3.
  const Files       files;
  const std::string path = files.path_of("tenths.tif");
  write_tiff(path, {.epsg   = 4326,
                    .west   = 1,
                    .north  = 3,
                    .pixel  = 0.1,
                    .width  = 30,
                    .height = 20,
                    .pixels = rough(30, 20)});

  check_loads_as_a_table(
      as_it_is(path),
      {
          polygon("POLYGON((0 0,6 0,6 6,0 6,0 0))"),
          polygon("POLYGON((1 1,4 1,4 3,1 3,1 1))"),
          polygon("POLYGON((1.33 2.33,1.66 2.33,1.66 2.66,1.33 2.66,1.33 2.33))"),
          polygon("POLYGON((1.5 1.5,3.5 1.5,3.5 2.5,1.5 2.5,1.5 1.5))"),
          polygon("POLYGON((0.7 2,1 2,1 3,0.7 3,0.7 2))"),  // west of it, touching its edge
          polygon("POLYGON((2.1 0.5,2.4 0.5,2.4 0.9,2.1 0.9,2.1 0.5))"),  // on a tile's edge
                                                                          // at 2.1, south of the
                                                                          // file
          polygon("POLYGON((1.05 1.25,1.07 1.23,3.95 2.93,3.93 2.95,1.05 1.25))"),  // a strip
                                                                                    // askew, across
                                                                                    // tiles of
                                                                                    // three tenths
          polygon("POLYGON((0.5 0.5,4.5 0.5,4.5 3.5,0.5 3.5,0.5 0.5),(1.7 1.4,1.7 2.5,2.8 2.5,2.8 "
                  "1.4,1.7 1.4))"),                                       // tiles in its hole
          polygon("POLYGON((1.2 1.5,1.2 2.4,2.1 2.4,2.1 1.5,1.2 1.5))"),  // its edges on tiles'
                                                                          // edges, which no double
                                                                          // holds
          polygon("POLYGON((40 40,41 40,41 41,40 41,40 40))"),
      },
      {.pixels_per_degree = 10, .tile_pixels = 3, .nodata = GRID_NODATA});
}
