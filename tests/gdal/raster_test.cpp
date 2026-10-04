// Raster files read with GDAL onto a table's grid: windows of whole tiles, the file's nodata as the grid's, a file in another
// coordinate system warped, and a GeoTIFF pushed into a raster layer and loaded back.
//
// The files are GeoTIFFs made here with GDAL. The last test needs MINIVERSE_TEST_DB (a libpq connection string to a scratch
// database with PostGIS and postgis_raster), and is skipped without it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <boost/geometry/io/wkt/read.hpp>

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
