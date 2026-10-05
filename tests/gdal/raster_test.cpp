// Raster files read with GDAL onto a table's grid: windows of whole tiles, the file's nodata as the grid's, a file in another
// coordinate system warped, and a GeoTIFF pushed into a raster layer and loaded back.
//
// The files are GeoTIFFs made here with GDAL. The last test needs MINIVERSE_TEST_DB (a libpq connection string to a scratch
// database with PostGIS and postgis_raster), and is skipped without it.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <boost/geometry/io/wkt/read.hpp>
#include <boost/geometry/io/wkt/write.hpp>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <gdal.h>
#include <ogr_core.h>
#include <ogr_spatialref.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numbers>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "support/files.hpp"
#include "miniverse/gdal/raster.hpp"
#include "miniverse/miniverse.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;
namespace gdal = miniverse::gdal;

using Catch::Matchers::ContainsSubstring;
using test::Files;
using test::test_db;

namespace {

using Raster = geo::Raster<std::int16_t>;

constexpr std::int16_t GRID_NODATA = -32768;
constexpr std::int16_t FILE_NODATA = -9999;

// Four pixels to a degree, in tiles of one degree.
constexpr geo::Grid<std::int16_t> GRID{.pixels_per_degree = 4, .tile_pixels = 4, .nodata = GRID_NODATA};

/** @brief Where a GeoTIFF lies and what it holds. */
struct Tiff {
  int                       epsg = 4326;
  double                    west = 0;
  double                    north = 0;
  double                    pixel = 0.25;  ///< A pixel's size each way, in the units of `epsg`.
  int                       width = 0;
  int                       height = 0;
  std::vector<std::int16_t> pixels;  ///< In rows from the north-west, `width * height` of them.
};

/** @return The pixel `column * 100 + row` for each of `width` by `height`. */
[[nodiscard]] std::vector<std::int16_t> numbered(int width, int height) {
  std::vector<std::int16_t> pixels;

  pixels.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
  for ( int row = 0; row < height; ++row ) {
    for ( int column = 0; column < width; ++column ) {
      pixels.push_back(static_cast<std::int16_t>((column * 100) + row));
    }
  }

  return pixels;
}

/** @brief Writes `tiff` as a GeoTIFF of one 16-bit band at `path`, with FILE_NODATA as its nodata value. An `epsg` of 0 names no system. */
void write_tiff(const std::string& path, Tiff tiff) {
  GDALAllRegister();

  GDALDatasetH made = GDALCreate(GDALGetDriverByName("GTiff"), path.c_str(), tiff.width, tiff.height, 1, GDT_Int16, nullptr);
  REQUIRE(made != nullptr);

  std::array<double, 6> position{tiff.west, tiff.pixel, 0, tiff.north, 0, -tiff.pixel};
  REQUIRE(GDALSetGeoTransform(made, position.data()) == CE_None);

  if ( tiff.epsg != 0 ) {
    OGRSpatialReference system;
    REQUIRE(system.importFromEPSG(tiff.epsg) == OGRERR_NONE);
    char* text = nullptr;
    REQUIRE(system.exportToWkt(&text) == OGRERR_NONE);
    REQUIRE(GDALSetProjection(made, text) == CE_None);
    CPLFree(text);
  }

  GDALRasterBandH band = GDALGetRasterBand(made, 1);
  REQUIRE(GDALSetRasterNoDataValue(band, FILE_NODATA) == CE_None);
  REQUIRE(GDALRasterIO(band, GF_Write, 0, 0, tiff.width, tiff.height, tiff.pixels.data(), tiff.width, tiff.height, GDT_Int16, 0, 0) == CE_None);
  GDALClose(made);
}

// Web Mercator (EPSG:3857), in metres: a degree of longitude is this many, everywhere.
constexpr double MERCATOR_DEGREE = 111319.49079327357;

/** @return How far north of the equator the latitude `degrees` is in Web Mercator, in metres. */
[[nodiscard]] double mercator_north(double degrees) {
  constexpr double RADIUS = 6378137;
  constexpr double HALF_TURN = 180;

  return RADIUS * std::log(std::tan((std::numbers::pi / 4) + (degrees * std::numbers::pi / HALF_TURN / 2)));
}

/** @return A source that takes the file's pixels as they are: for files already on the grid. */
[[nodiscard]] gdal::RasterSource as_it_is(std::string path) { return {.path = std::move(path), .band = 1, .resampling = gdal::Resampling::Nearest}; }

struct Elevation : miniverse::RasterLayer<std::int16_t> {};

[[nodiscard]] geo::Polygon polygon(const std::string& wkt) { return bg::from_wkt<geo::Polygon>(wkt); }

/** @return `width * height` pixels with no pattern to them, the same every time: heights of 0 to 2999. */
[[nodiscard]] std::vector<std::int16_t> rough(int width, int height) {
  std::vector<std::int16_t> pixels;
  std::uint32_t             state = 12345;

  pixels.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
  for ( int i = 0; i < width * height; ++i ) {
    state = (state * 1664525U) + 1013904223U;  // a linear congruential generator
    pixels.push_back(static_cast<std::int16_t>((state >> 16U) % 3000U));
  }

  return pixels;
}

/** @return `tiles` from the north west, row by row: a stream hands them over in no order that is promised. */
[[nodiscard]] std::vector<Raster> in_order(std::vector<Raster> tiles) {
  std::ranges::sort(tiles, [](const Raster& one, const Raster& other) {
    return one.north != other.north ? one.north > other.north : one.west < other.west;
  });

  return tiles;
}

/**
 * @brief Checks that the file `source` on `grid` gives, for each of `locations`, what a table gives that it was pushed into:
 * the same raster for a load, and the same tiles for a stream.
 */
void check_loads_as_a_table(const gdal::RasterSource& source, const std::vector<geo::Polygon>& locations, const geo::Grid<std::int16_t>& grid = GRID) {
  miniverse::Miniverse tables(test_db(), miniverse::Layer<Elevation>("miniverse_test_gdal_elevation"));
  tables.drop_tables();
  tables.create_table<Elevation>(grid);
  std::ignore = gdal::read_raster<std::int16_t>(source, grid, 1, [&tables](Raster window) { tables.push<Elevation>(std::move(window)).get(); });

  miniverse::Miniverse files(gdal::raster_file<Elevation>(source, grid));

  for ( std::size_t i = 0; i < locations.size(); ++i ) {
    INFO("location " << i << ": " << bg::to_wkt(locations.at(i)));
    std::vector<Raster> table_tiles;
    std::vector<Raster> file_tiles;
    const auto          into = [](std::vector<Raster>& tiles) {
      return [&tiles](std::vector<Raster> chunk) { tiles.insert(tiles.end(), chunk.begin(), chunk.end()); };
    };

    const Raster from_table = tables.load<Elevation>(locations.at(i)).get();
    const Raster from_file = files.load<Elevation>(locations.at(i)).get();
    tables.stream<Elevation>(locations.at(i), into(table_tiles), {.chunk_rows = 3}).get();
    files.stream<Elevation>(locations.at(i), into(file_tiles), {.chunk_rows = 3}).get();

    CHECK(from_file == from_table);
    CHECK(in_order(file_tiles) == in_order(table_tiles));
  }

  tables.drop_tables();
}

}  // namespace

TEST_CASE("gdal raster: a file on the grid reads as its own pixels", "[gdal]") {
  // Two degrees each way from (1, 3): tiles 181 and 182 across, 87 and 88 down.
  const Files       files;
  const std::string path = files.path_of("on_grid.tif");
  write_tiff(path, {.epsg = 4326, .west = 1, .north = 3, .pixel = 0.25, .width = 8, .height = 8, .pixels = numbered(8, 8)});

  const Raster raster = gdal::read_raster<std::int16_t>(as_it_is(path), GRID);

  CHECK(raster.west == 1);
  CHECK(raster.north == 3);
  CHECK(raster.pixel_width == 0.25);
  CHECK(raster.pixel_height == 0.25);
  CHECK(raster.nodata == GRID_NODATA);
  REQUIRE(raster.width == 8);
  REQUIRE(raster.height == 8);
  CHECK(raster.pixels == numbered(8, 8));
}

TEST_CASE("gdal raster: a file within a tile reads as the whole tile, with no data around it", "[gdal]") {
  // Two pixels each way from (1.25, 2.75): inside the tile from (1, 3).
  const Files       files;
  const std::string path = files.path_of("inside.tif");
  write_tiff(path, {.epsg = 4326, .west = 1.25, .north = 2.75, .pixel = 0.25, .width = 2, .height = 2, .pixels = {1, 2, 3, 4}});

  const Raster raster = gdal::read_raster<std::int16_t>(as_it_is(path), GRID);

  CHECK(raster.west == 1);
  CHECK(raster.north == 3);
  REQUIRE(raster.width == 4);
  REQUIRE(raster.height == 4);
  CHECK(raster.at(0, 0) == GRID_NODATA);
  CHECK(raster.at(1, 1) == 1);
  CHECK(raster.at(2, 1) == 2);
  CHECK(raster.at(1, 2) == 3);
  CHECK(raster.at(2, 2) == 4);
  CHECK(raster.at(3, 3) == GRID_NODATA);
}

TEST_CASE("gdal raster: a file is handed over in windows of whole tiles, from the north west", "[gdal]") {
  const Files       files;
  const std::string path = files.path_of("on_grid.tif");
  write_tiff(path, {.epsg = 4326, .west = 1, .north = 3, .pixel = 0.25, .width = 8, .height = 8, .pixels = numbered(8, 8)});

  std::vector<Raster>    windows;
  const gdal::RasterRead read = gdal::read_raster<std::int16_t>(as_it_is(path), GRID, 1, [&](Raster window) { windows.push_back(std::move(window)); });

  CHECK(read.windows == 4);
  CHECK(read.without_data == 0);
  REQUIRE(windows.size() == 4);
  CHECK(windows.at(0).west == 1);
  CHECK(windows.at(0).north == 3);
  CHECK(windows.at(1).west == 2);  // then east
  CHECK(windows.at(1).north == 3);
  CHECK(windows.at(2).west == 1);  // then the row south
  CHECK(windows.at(2).north == 2);
  for ( const Raster& window : windows ) {
    CHECK(window.width == 4);
    CHECK(window.height == 4);
  }
  CHECK(windows.at(3).at(0, 0) == 404);  // the file's column 4, row 4
  CHECK_THROWS_AS(gdal::read_raster<std::int16_t>(as_it_is(path), GRID, 0, [](const Raster&) {}), std::invalid_argument);
}

TEST_CASE("gdal raster: the last window of a row is the tiles that are left", "[gdal]") {
  // Three tiles side by side from (1, 3), read two across at a time.
  const Files       files;
  const std::string path = files.path_of("three.tif");
  write_tiff(path, {.epsg = 4326, .west = 1, .north = 3, .pixel = 0.25, .width = 12, .height = 4, .pixels = numbered(12, 4)});

  std::vector<Raster>    windows;
  const gdal::RasterRead read = gdal::read_raster<std::int16_t>(as_it_is(path), GRID, 2, [&](Raster window) { windows.push_back(std::move(window)); });

  CHECK(read.windows == 2);
  REQUIRE(windows.size() == 2);
  CHECK(windows.at(0).width == 8);
  CHECK(windows.at(1).width == 4);
  CHECK(windows.at(1).west == 3);
  CHECK(windows.at(1).at(0, 0) == 800);  // the file's column 8
}

TEST_CASE("gdal raster: the file's nodata is the grid's, and a window with no data is left out", "[gdal]") {
  // Two tiles side by side from (1, 3): the east one has no data at all, and the west one a pixel without.
  const Files               files;
  const std::string         path = files.path_of("half.tif");
  std::vector<std::int16_t> pixels(32, 7);
  pixels.at(0) = FILE_NODATA;
  for ( std::size_t row = 0; row < 4; ++row ) {
    for ( std::size_t column = 4; column < 8; ++column ) {
      pixels.at((row * 8) + column) = FILE_NODATA;
    }
  }
  write_tiff(path, {.epsg = 4326, .west = 1, .north = 3, .pixel = 0.25, .width = 8, .height = 4, .pixels = pixels});

  std::vector<Raster>    windows;
  const gdal::RasterRead read = gdal::read_raster<std::int16_t>(as_it_is(path), GRID, 1, [&](Raster window) { windows.push_back(std::move(window)); });

  CHECK(read.windows == 1);
  CHECK(read.without_data == 1);
  REQUIRE(windows.size() == 1);
  CHECK(windows.front().west == 1);
  CHECK(windows.front().at(0, 0) == GRID_NODATA);
  CHECK(windows.front().at(1, 0) == 7);
}

TEST_CASE("gdal raster: a file in another coordinate system is warped onto the grid", "[gdal]") {
  // From longitude 1 to 2, and from latitude 2 nearly to 1, as ten pixels each way, every one 7: ten pixels of a tenth of a
  // degree of longitude end on longitude 2, and reach a little less far south, as a degree of latitude is longer there.
  constexpr double PIXEL = MERCATOR_DEGREE / 10;
  const Files       files;
  const std::string path = files.path_of("mercator.tif");
  write_tiff(
      path,
      {.epsg = 3857, .west = MERCATOR_DEGREE, .north = mercator_north(2), .pixel = PIXEL, .width = 10, .height = 10, .pixels = std::vector<std::int16_t>(100, 7)}
  );

  const Raster raster = gdal::read_raster<std::int16_t>(as_it_is(path), GRID);

  CHECK(raster.west == 1);
  CHECK(raster.north == 2);
  REQUIRE(raster.width == 4);
  REQUIRE(raster.height == 4);
  CHECK(std::ranges::all_of(raster.pixels, [](std::int16_t pixel) { return pixel == 7; }));  // the tile from (1, 2), all of it
}

TEST_CASE("gdal raster: a warped file reads the same pixels in windows as whole", "[gdal]") {
  // Two degrees each way in Web Mercator, from longitude 1 and latitude 3, as numbered pixels: four tiles of the grid.
  constexpr double PIXEL = MERCATOR_DEGREE / 10;
  const Files       files;
  const std::string path = files.path_of("mercator.tif");
  write_tiff(path, {.epsg = 3857, .west = MERCATOR_DEGREE, .north = mercator_north(3), .pixel = PIXEL, .width = 20, .height = 20, .pixels = numbered(20, 20)});

  const Raster           whole = gdal::read_raster<std::int16_t>(as_it_is(path), GRID);
  std::vector<Raster>    windows;
  const gdal::RasterRead read = gdal::read_raster<std::int16_t>(as_it_is(path), GRID, 1, [&](Raster window) { windows.push_back(std::move(window)); });

  REQUIRE(whole.width == 8);
  REQUIRE(whole.height == 8);
  REQUIRE(read.windows == 4);
  for ( const Raster& window : windows ) {
    // Where the window lies in the whole, in pixels.
    const auto first_column = static_cast<std::size_t>(std::lround((window.west - whole.west) * 4));
    const auto first_row = static_cast<std::size_t>(std::lround((whole.north - window.north) * 4));
    for ( std::size_t row = 0; row < window.height; ++row ) {
      for ( std::size_t column = 0; column < window.width; ++column ) {
        CHECK(window.at(column, row) == whole.at(first_column + column, first_row + row));
      }
    }
  }
}

TEST_CASE("gdal raster: a grid coarser than the file takes the mean of the file's pixels", "[gdal]") {
  // Sixteen pixels to a degree, four times the grid's, over one tile: of every four columns the last is 40 and the others 0.
  // A grid pixel covers four by four of them, whose mean is 10: the nearest would be 0 or 40, and a weighted mean not 10.
  const Files               files;
  const std::string         path = files.path_of("fine.tif");
  std::vector<std::int16_t> pixels;
  pixels.reserve(256);
  for ( int row = 0; row < 16; ++row ) {
    for ( int column = 0; column < 16; ++column ) {
      pixels.push_back(column % 4 == 3 ? std::int16_t{40} : std::int16_t{0});
    }
  }
  write_tiff(path, {.epsg = 4326, .west = 1, .north = 3, .pixel = 0.0625, .width = 16, .height = 16, .pixels = pixels});

  const Raster raster = gdal::read_raster<std::int16_t>({.path = path, .band = 1, .resampling = gdal::Resampling::Average}, GRID);

  REQUIRE(raster.width == 4);
  REQUIRE(raster.height == 4);
  CHECK(std::ranges::all_of(raster.pixels, [](std::int16_t pixel) { return pixel == 10; }));
}

TEST_CASE("gdal raster: a file that is not there, a band it does not have, or no coordinate system, is refused with the reason", "[gdal]") {
  const Files       files;
  const std::string path = files.path_of("on_grid.tif");
  const std::string nowhere = files.path_of("nowhere.tif");
  write_tiff(path, {.epsg = 4326, .west = 1, .north = 3, .pixel = 0.25, .width = 4, .height = 4, .pixels = numbered(4, 4)});
  write_tiff(nowhere, {.epsg = 0, .west = 1, .north = 3, .pixel = 0.25, .width = 4, .height = 4, .pixels = numbered(4, 4)});

  CHECK_THROWS_WITH(gdal::read_raster<std::int16_t>(as_it_is(files.path_of("nothing.tif")), GRID), ContainsSubstring("can't be opened"));
  CHECK_THROWS_WITH(
      gdal::read_raster<std::int16_t>({.path = path, .band = 2, .resampling = gdal::Resampling::Nearest}, GRID), ContainsSubstring("so no band 2")
  );
  CHECK_THROWS_WITH(gdal::read_raster<std::int16_t>(as_it_is(nowhere), GRID), ContainsSubstring("names no coordinate system"));

  // Longitudes of 179 to 183: past the world, as a file of 0 to 360 is.
  const std::string past = files.path_of("past.tif");
  write_tiff(past, {.epsg = 4326, .west = 179, .north = 3, .pixel = 0.25, .width = 16, .height = 4, .pixels = numbered(16, 4)});
  CHECK_THROWS_WITH(gdal::read_raster<std::int16_t>(as_it_is(past), GRID), ContainsSubstring("reaches past the world"));
  CHECK_THROWS_AS(
      gdal::read_raster<std::int16_t>(as_it_is(path), {.pixels_per_degree = 0, .tile_pixels = 4, .nodata = GRID_NODATA}), std::invalid_argument
  );
}

TEST_CASE("gdal raster: what the callback throws is passed on", "[gdal]") {
  const Files       files;
  const std::string path = files.path_of("on_grid.tif");
  write_tiff(path, {.epsg = 4326, .west = 1, .north = 3, .pixel = 0.25, .width = 8, .height = 8, .pixels = numbered(8, 8)});

  std::size_t windows = 0;
  const std::function<void(Raster)> taking_one = [&](const Raster& /*window*/) {
    if ( ++windows == 2 ) {
      throw std::runtime_error("one window is enough");
    }
  };

  CHECK_THROWS_WITH(gdal::read_raster<std::int16_t>(as_it_is(path), GRID, 1, taking_one), ContainsSubstring("is enough"));
  CHECK(windows == 2);
}

TEST_CASE("integration: a GeoTIFF pushed a window at a time, onto the table's own grid, loads back as its pixels", "[gdal][integration]") {
  const Files       files;
  const std::string path = files.path_of("on_grid.tif");
  write_tiff(path, {.epsg = 4326, .west = 1, .north = 3, .pixel = 0.25, .width = 8, .height = 8, .pixels = numbered(8, 8)});

  miniverse::Miniverse world(test_db(), miniverse::Layer<Elevation>("miniverse_test_gdal_elevation"));
  world.drop_tables();
  world.create_table<Elevation>(GRID);

  const geo::Grid<std::int16_t> grid = world.table_settings<Elevation>().get();  // as a writer gets it: from the table
  const gdal::RasterRead         read = gdal::read_raster<std::int16_t>(as_it_is(path), grid, 1, [&](Raster window) {
    world.push<Elevation>(std::move(window)).get();
  });
  const Raster loaded = world.load<Elevation>(bg::from_wkt<geo::Polygon>("POLYGON((1 1,3 1,3 3,1 3,1 1))")).get();

  CHECK(read.windows == 4);
  CHECK(loaded == gdal::read_raster<std::int16_t>(as_it_is(path), grid));  // what GDAL read from the file, on the grid
  CHECK(loaded.pixels == numbered(8, 8));                                  // which is the file's own pixels

  world.drop_tables();
}

TEST_CASE("gdal raster: a file as a layer loads the box's pixels on the grid, and streams its tiles, with no database", "[gdal]") {
  // Two degrees each way from (1, 3): four tiles of the grid, numbered pixels.
  const Files       files;
  const std::string path = files.path_of("on_grid.tif");
  write_tiff(path, {.epsg = 4326, .west = 1, .north = 3, .pixel = 0.25, .width = 8, .height = 8, .pixels = numbered(8, 8)});
  miniverse::Miniverse world(gdal::raster_file<Elevation>(as_it_is(path), GRID));
  std::vector<std::size_t> chunks;

  world.verify().get();
  const Raster whole = world.load<Elevation>(polygon("POLYGON((1 1,3 1,3 3,1 3,1 1))")).get();
  const Raster part = world.load<Elevation>(polygon("POLYGON((1.3 2.3,1.6 2.3,1.6 2.6,1.3 2.6,1.3 2.3))")).get();
  const Raster past = world.load<Elevation>(polygon("POLYGON((2.5 2.5,3.5 2.5,3.5 3.5,2.5 3.5,2.5 2.5))")).get();
  world.stream<Elevation>(polygon("POLYGON((0 0,5 0,5 5,0 5,0 0))"), [&chunks](const std::vector<Raster>& tiles) { chunks.push_back(tiles.size()); }, {.chunk_rows = 3})
      .get();

  CHECK(world.table_settings<Elevation>().get() == GRID);
  CHECK(whole.pixels == numbered(8, 8));

  // Widened to whole pixels: from 1.25 to 1.75 across, and from 2.75 down to 2.25.
  CHECK(part.west == 1.25);
  CHECK(part.north == 2.75);
  REQUIRE(part.width == 2);
  REQUIRE(part.height == 2);
  CHECK(part.at(0, 0) == 101);  // column 1, row 1 of the file

  // The box reaches past the file to the north and east: no data there, the file's pixels in its corner.
  REQUIRE(past.width == 4);
  REQUIRE(past.height == 4);
  CHECK(past.at(0, 0) == GRID_NODATA);
  CHECK(past.at(0, 2) == 600);  // column 6, row 0 of the file
  CHECK(past.at(2, 2) == GRID_NODATA);

  CHECK(chunks == std::vector<std::size_t>{3, 1});
  CHECK(world.load<Elevation>(polygon("POLYGON((40 40,41 40,41 41,40 41,40 40))")).get() == Raster{});
}

TEST_CASE("gdal raster: verify says what keeps a file from being a layer", "[gdal]") {
  const Files          files;
  miniverse::Miniverse world(gdal::raster_file<Elevation>(as_it_is(files.path_of("missing.tif")), GRID));

  try {
    world.verify().get();
    FAIL("it verified");
  } catch ( const miniverse::TablesDiffer& differ ) {
    REQUIRE(differ.differences().size() == 1);
    CHECK_THAT(differ.differences().front(), ContainsSubstring("missing.tif") && ContainsSubstring("can't be opened"));
  }

  CHECK_THROWS_WITH(world.load<Elevation>(polygon("POLYGON((0 0,1 0,1 1,0 0))")).get(), ContainsSubstring("can't be opened"));
  CHECK_THROWS_AS(
      world.stream<Elevation>(polygon("POLYGON((0 0,1 0,1 1,0 0))"), [](const std::vector<Raster>& /*tiles*/) {}, {.chunk_rows = 0}).get(), std::invalid_argument
  );
}

TEST_CASE("integration: a raster file on the grid loads and streams as the table it was pushed into", "[gdal][integration]") {
  // Three tiles by two from (1, 3), numbered; the east tile of the north row has no data at all, and one pixel elsewhere none.
  const Files               files;
  const std::string         path = files.path_of("holed.tif");
  std::vector<std::int16_t> pixels = numbered(12, 8);
  pixels.at(0) = FILE_NODATA;
  for ( std::size_t row = 0; row < 4; ++row ) {
    for ( std::size_t column = 8; column < 12; ++column ) {
      pixels.at((row * 12) + column) = FILE_NODATA;
    }
  }
  write_tiff(path, {.epsg = 4326, .west = 1, .north = 3, .pixel = 0.25, .width = 12, .height = 8, .pixels = pixels});

  check_loads_as_a_table(
      as_it_is(path),
      {
          polygon("POLYGON((0 0,6 0,6 6,0 6,0 0))"),                          // all of it, and around it
          polygon("POLYGON((1 1,4 1,4 3,1 3,1 1))"),                          // its own extent
          polygon("POLYGON((1.3 2.3,1.6 2.3,1.6 2.6,1.3 2.6,1.3 2.3))"),      // inside one tile
          polygon("POLYGON((1.5 1.5,3.5 1.5,3.5 2.5,1.5 2.5,1.5 1.5))"),      // across tiles, and into the one with no data
          polygon("POLYGON((3.1 2.1,3.9 2.1,3.9 2.9,3.1 2.9,3.1 2.1))"),      // in the tile with no data alone: nothing
          polygon("POLYGON((0 2,1 2,1 3,0 3,0 2))"),                          // west of it, touching its edge
          polygon("POLYGON((2 3,3 3,3 4,2 4,2 3))"),                          // north of it, touching its edge
          polygon("POLYGON((0.5 2.5,1.5 2.5,1.5 3.5,0.5 3.5,0.5 2.5))"),      // over its corner
          polygon("POLYGON((1.5 2.5,1.5 2.5,1.5 2.5,1.5 2.5))"),              // a point
          polygon("POLYGON((1 2.6,3 1.2,2 1.2,1 2.6))"),                      // a triangle: its box
          polygon("POLYGON((40 40,41 40,41 41,40 41,40 40))"),                // far away
      }
  );
}

TEST_CASE("integration: a raster file in another coordinate system loads and streams as the table it was pushed into", "[gdal][integration]") {
  // Two degrees each way in Web Mercator, from longitude 1 and latitude 3, as numbered pixels, made into the grid's by
  // bilinear weights: a pixel must come out the same whichever window it is warped in.
  constexpr double PIXEL = MERCATOR_DEGREE / 10;
  const Files       files;
  const std::string path = files.path_of("mercator.tif");
  write_tiff(path, {.epsg = 3857, .west = MERCATOR_DEGREE, .north = mercator_north(3), .pixel = PIXEL, .width = 20, .height = 20, .pixels = numbered(20, 20)});

  check_loads_as_a_table(
      {.path = path, .band = 1, .resampling = gdal::Resampling::Bilinear},
      {
          polygon("POLYGON((0 0,6 0,6 6,0 6,0 0))"),
          polygon("POLYGON((1.3 2.3,1.6 2.3,1.6 2.6,1.3 2.6,1.3 2.3))"),
          polygon("POLYGON((1.5 1.5,2.5 1.5,2.5 2.5,1.5 2.5,1.5 1.5))"),
          polygon("POLYGON((2.6 0.5,3.4 0.5,3.4 1.4,2.6 1.4,2.6 0.5))"),  // over the file's south-east corner
          polygon("POLYGON((0 2,1 2,1 3,0 3,0 2))"),
          polygon("POLYGON((40 40,41 40,41 41,40 41,40 40))"),
      }
  );
}

TEST_CASE("gdal raster: pixels made by weights are the same whichever window they are read in", "[gdal]") {
  // Rough ground, finer than the grid, in WGS 84 and in Web Mercator: two degrees each way from longitude 1, latitude 3.
  // A weighted mean of rough pixels shows any difference in how a window is made; a smooth ramp would hide it.
  constexpr double  PIXEL = MERCATOR_DEGREE / 10;
  const Files       files;
  const std::string plain = files.path_of("rough.tif");
  const std::string mercator = files.path_of("rough_mercator.tif");
  write_tiff(plain, {.epsg = 4326, .west = 1, .north = 3, .pixel = 0.1, .width = 20, .height = 20, .pixels = rough(20, 20)});
  write_tiff(mercator, {.epsg = 3857, .west = MERCATOR_DEGREE, .north = mercator_north(3), .pixel = PIXEL, .width = 20, .height = 20, .pixels = rough(20, 20)});

  for ( const std::string& path : {plain, mercator} ) {
    for ( const gdal::Resampling resampling : {gdal::Resampling::Bilinear, gdal::Resampling::Cubic, gdal::Resampling::Average} ) {
      INFO(path << ", resampling " << static_cast<int>(resampling));
      const gdal::RasterSource source{.path = path, .band = 1, .resampling = resampling};

      const Raster        whole = gdal::read_raster<std::int16_t>(source, GRID);
      std::vector<Raster> windows;
      std::vector<Raster> tiles;
      std::ignore = gdal::read_raster<std::int16_t>(source, GRID, 1, [&windows](Raster window) { windows.push_back(std::move(window)); });
      gdal::read_tiles_in<std::int16_t>(source, GRID, polygon("POLYGON((0 0,6 0,6 6,0 6,0 0))"), 3, [&tiles](std::vector<Raster> chunk) {
        tiles.insert(tiles.end(), chunk.begin(), chunk.end());

        return true;
      });

      REQUIRE(whole.width == 8);
      REQUIRE(whole.height == 8);
      CHECK(in_order(tiles) == in_order(windows));  // a stream's runs of tiles, and a push's windows of one
      for ( const Raster& window : windows ) {
        const auto first_column = static_cast<std::size_t>(std::lround((window.west - whole.west) * 4));
        const auto first_row = static_cast<std::size_t>(std::lround((whole.north - window.north) * 4));
        for ( std::size_t row = 0; row < window.height; ++row ) {
          for ( std::size_t column = 0; column < window.width; ++column ) {
            CHECK(window.at(column, row) == whole.at(first_column + column, first_row + row));
          }
        }
      }
    }
  }
}

TEST_CASE("gdal raster: a read of tiles stops when its callback wants no more", "[gdal]") {
  const Files       files;
  const std::string path = files.path_of("on_grid.tif");
  write_tiff(path, {.epsg = 4326, .west = 1, .north = 3, .pixel = 0.25, .width = 8, .height = 8, .pixels = numbered(8, 8)});
  std::vector<std::size_t> sizes;

  gdal::read_tiles_in<std::int16_t>(as_it_is(path), GRID, polygon("POLYGON((0 0,6 0,6 6,0 6,0 0))"), 1, [&sizes](const std::vector<Raster>& chunk) {
    sizes.push_back(chunk.size());

    return sizes.size() < 2;
  });

  CHECK(sizes == std::vector<std::size_t>{1, 1});  // of four tiles
  CHECK_THROWS_AS(
      gdal::read_tiles_in<std::int16_t>(as_it_is(path), {.pixels_per_degree = 100'000, .tile_pixels = 4, .nodata = GRID_NODATA}, polygon("POLYGON((0 0,1 0,1 1,0 0))"), 1, {}),
      std::invalid_argument
  );  // a grid no table can be made with is none for a file either
}

TEST_CASE("integration: on a grid of tenths of a degree, a raster file loads and streams as the table it was pushed into", "[gdal][integration]") {
  // Ten pixels to a degree in tiles of three: no tile's edge but the grid's own corner is a number a double holds exactly,
  // and tiles don't divide the world. Rough ground, three degrees by two from longitude 1, latitude 3.
  const Files       files;
  const std::string path = files.path_of("tenths.tif");
  write_tiff(path, {.epsg = 4326, .west = 1, .north = 3, .pixel = 0.1, .width = 30, .height = 20, .pixels = rough(30, 20)});

  check_loads_as_a_table(
      as_it_is(path),
      {
          polygon("POLYGON((0 0,6 0,6 6,0 6,0 0))"),
          polygon("POLYGON((1 1,4 1,4 3,1 3,1 1))"),
          polygon("POLYGON((1.33 2.33,1.66 2.33,1.66 2.66,1.33 2.66,1.33 2.33))"),
          polygon("POLYGON((1.5 1.5,3.5 1.5,3.5 2.5,1.5 2.5,1.5 1.5))"),
          polygon("POLYGON((0.7 2,1 2,1 3,0.7 3,0.7 2))"),    // west of it, touching its edge
          polygon("POLYGON((2.1 0.5,2.4 0.5,2.4 0.9,2.1 0.9,2.1 0.5))"),  // on a tile's edge at 2.1, south of the file
          polygon("POLYGON((40 40,41 40,41 41,40 41,40 40))"),
      },
      {.pixels_per_degree = 10, .tile_pixels = 3, .nodata = GRID_NODATA}
  );
}
