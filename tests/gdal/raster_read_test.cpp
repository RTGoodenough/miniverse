// Raster files read with GDAL onto a table's grid: windows of whole tiles, the file's nodata as the
// grid's, a file in another coordinate system or turned warped, bands and pixel types, and the
// tiles a location reaches into. The files are GeoTIFFs made here with GDAL (gdal/tiffs.hpp).

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "gdal/tiffs.hpp"
#include "miniverse/gdal/gdal.hpp"
#include "miniverse/geo/grid.hpp"
#include "miniverse/geo/raster.hpp"
#include "support/files.hpp"
#include "support/geometry.hpp"

namespace gdal = miniverse::gdal;

using Catch::Matchers::ContainsSubstring;
using test::as_it_is;
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
using test::write_tiff;

TEST_CASE("gdal raster: a file on the grid reads as its own pixels", "[gdal]")
{
  // Two degrees each way from (1, 3): tiles 181 and 182 across, 87 and 88 down.
  const Files       files;
  const std::string path = files.path_of("on_grid.tif");
  write_tiff(path, {.epsg   = 4326,
                    .west   = 1,
                    .north  = 3,
                    .pixel  = 0.25,
                    .width  = 8,
                    .height = 8,
                    .pixels = numbered(8, 8)});

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

TEST_CASE("gdal raster: a file within a tile reads as the whole tile, with no data around it",
          "[gdal]")
{
  // Two pixels each way from (1.25, 2.75): inside the tile from (1, 3).
  const Files       files;
  const std::string path = files.path_of("inside.tif");
  write_tiff(path, {.epsg   = 4326,
                    .west   = 1.25,
                    .north  = 2.75,
                    .pixel  = 0.25,
                    .width  = 2,
                    .height = 2,
                    .pixels = {1, 2, 3, 4}});

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

TEST_CASE("gdal raster: a file is handed over in windows of whole tiles, from the north west",
          "[gdal]")
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

  std::vector<Raster>    windows;
  const gdal::RasterRead read = gdal::read_raster<std::int16_t>(
      as_it_is(path), GRID, 1, [&](Raster window) { windows.push_back(std::move(window)); });

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
  CHECK_THROWS_AS(gdal::read_raster<std::int16_t>(as_it_is(path), GRID, 0, [](const Raster&) {}),
                  std::invalid_argument);
}

TEST_CASE("gdal raster: the last window of a row is the tiles that are left", "[gdal]")
{
  // Three tiles side by side from (1, 3), read two across at a time.
  const Files       files;
  const std::string path = files.path_of("three.tif");
  write_tiff(path, {.epsg   = 4326,
                    .west   = 1,
                    .north  = 3,
                    .pixel  = 0.25,
                    .width  = 12,
                    .height = 4,
                    .pixels = numbered(12, 4)});

  std::vector<Raster>    windows;
  const gdal::RasterRead read = gdal::read_raster<std::int16_t>(
      as_it_is(path), GRID, 2, [&](Raster window) { windows.push_back(std::move(window)); });

  CHECK(read.windows == 2);
  REQUIRE(windows.size() == 2);
  CHECK(windows.at(0).width == 8);
  CHECK(windows.at(1).width == 4);
  CHECK(windows.at(1).west == 3);
  CHECK(windows.at(1).at(0, 0) == 800);  // the file's column 8
}

TEST_CASE("gdal raster: the file's nodata is the grid's, and a window with no data is left out",
          "[gdal]")
{
  // Two tiles side by side from (1, 3): the east one has no data at all, and the west one a pixel
  // without.
  const Files               files;
  const std::string         path = files.path_of("half.tif");
  std::vector<std::int16_t> pixels(32, 7);
  pixels.at(0) = FILE_NODATA;
  for ( std::size_t row = 0; row < 4; ++row ) {
    for ( std::size_t column = 4; column < 8; ++column ) {
      pixels.at((row * 8) + column) = FILE_NODATA;
    }
  }
  write_tiff(path, {.epsg   = 4326,
                    .west   = 1,
                    .north  = 3,
                    .pixel  = 0.25,
                    .width  = 8,
                    .height = 4,
                    .pixels = pixels});

  std::vector<Raster>    windows;
  const gdal::RasterRead read = gdal::read_raster<std::int16_t>(
      as_it_is(path), GRID, 1, [&](Raster window) { windows.push_back(std::move(window)); });

  CHECK(read.windows == 1);
  CHECK(read.without_data == 1);
  REQUIRE(windows.size() == 1);
  CHECK(windows.front().west == 1);
  CHECK(windows.front().at(0, 0) == GRID_NODATA);
  CHECK(windows.front().at(1, 0) == 7);
}

TEST_CASE("gdal raster: a file in another coordinate system is warped onto the grid", "[gdal]")
{
  // From longitude 1 to 2, and from latitude 2 nearly to 1, as ten pixels each way, every one 7:
  // ten pixels of a tenth of a degree of longitude end on longitude 2, and reach a little less far
  // south, as a degree of latitude is longer there.
  constexpr double  PIXEL = MERCATOR_DEGREE / 10;
  const Files       files;
  const std::string path = files.path_of("mercator.tif");
  write_tiff(path, {.epsg   = 3857,
                    .west   = MERCATOR_DEGREE,
                    .north  = mercator_north(2),
                    .pixel  = PIXEL,
                    .width  = 10,
                    .height = 10,
                    .pixels = std::vector<std::int16_t>(100, 7)});

  const Raster raster = gdal::read_raster<std::int16_t>(as_it_is(path), GRID);

  CHECK(raster.west == 1);
  CHECK(raster.north == 2);
  REQUIRE(raster.width == 4);
  REQUIRE(raster.height == 4);
  CHECK(std::ranges::all_of(raster.pixels, [](std::int16_t pixel) { return pixel == 7; }));  // the
                                                                                             // tile
                                                                                             // from
                                                                                             // (1,
                                                                                             // 2),
                                                                                             // all
                                                                                             // of
                                                                                             // it
}

TEST_CASE("gdal raster: a warped file reads the same pixels in windows as whole", "[gdal]")
{
  // Two degrees each way in Web Mercator, from longitude 1 and latitude 3, as numbered pixels: four
  // tiles of the grid.
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

  const Raster           whole = gdal::read_raster<std::int16_t>(as_it_is(path), GRID);
  std::vector<Raster>    windows;
  const gdal::RasterRead read = gdal::read_raster<std::int16_t>(
      as_it_is(path), GRID, 1, [&](Raster window) { windows.push_back(std::move(window)); });

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

TEST_CASE("gdal raster: a grid coarser than the file takes the mean of the file's pixels", "[gdal]")
{
  // Sixteen pixels to a degree, four times the grid's, over one tile: of every four columns the
  // last is 40 and the others 0. A grid pixel covers four by four of them, whose mean is 10: the
  // nearest would be 0 or 40, and a weighted mean not 10.
  const Files               files;
  const std::string         path = files.path_of("fine.tif");
  std::vector<std::int16_t> pixels;
  pixels.reserve(256);
  for ( int row = 0; row < 16; ++row ) {
    for ( int column = 0; column < 16; ++column ) {
      pixels.push_back(column % 4 == 3 ? std::int16_t{40} : std::int16_t{0});
    }
  }
  write_tiff(path, {.epsg   = 4326,
                    .west   = 1,
                    .north  = 3,
                    .pixel  = 0.0625,
                    .width  = 16,
                    .height = 16,
                    .pixels = pixels});

  const Raster raster = gdal::read_raster<std::int16_t>(
      {.path = path, .band = 1, .resampling = gdal::Resampling::Average}, GRID);

  REQUIRE(raster.width == 4);
  REQUIRE(raster.height == 4);
  CHECK(std::ranges::all_of(raster.pixels, [](std::int16_t pixel) { return pixel == 10; }));
}

TEST_CASE("gdal raster: the band asked for is the one read", "[gdal]")
{
  std::vector<std::int16_t> second = numbered(8, 8);
  std::ranges::for_each(
      second, [](std::int16_t& pixel) { pixel = static_cast<std::int16_t>(pixel + 1000); });
  const Files       files;
  const std::string path = files.path_of("two_bands.tif");
  write_tiff(path, {.epsg        = 4326,
                    .west        = 1,
                    .north       = 3,
                    .pixel       = 0.25,
                    .width       = 8,
                    .height      = 8,
                    .pixels      = numbered(8, 8),
                    .second_band = second});

  CHECK(gdal::read_raster<std::int16_t>(
            {.path = path, .band = 1, .resampling = gdal::Resampling::Nearest}, GRID)
            .pixels == numbered(8, 8));
  CHECK(gdal::read_raster<std::int16_t>(
            {.path = path, .band = 2, .resampling = gdal::Resampling::Nearest}, GRID)
            .pixels == second);
}

TEST_CASE("gdal raster: a file of signed bytes reads onto a grid of them", "[gdal]")
{
  // -32 to 31, but for the one pixel that is the file's nodata, -100.
  constexpr std::int8_t                       NODATA = -100;
  constexpr miniverse::geo::Grid<std::int8_t> BYTE_GRID{
      .pixels_per_degree = 4, .tile_pixels = 4, .nodata = -128};
  std::vector<std::int8_t> pixels;
  pixels.reserve(64);
  for ( int i = 0; i < 64; ++i ) {
    pixels.push_back(static_cast<std::int8_t>(i - 32));
  }
  pixels.at(9) = NODATA;
  const Files       files;
  const std::string path = files.path_of("bytes.tif");
  write_tiff<std::int8_t>(path, {.epsg   = 4326,
                                 .west   = 1,
                                 .north  = 3,
                                 .pixel  = 0.25,
                                 .width  = 8,
                                 .height = 8,
                                 .pixels = pixels,
                                 .nodata = NODATA});

  const miniverse::geo::Raster<std::int8_t> raster =
      gdal::read_raster<std::int8_t>(as_it_is(path), BYTE_GRID);

  pixels.at(9) = -128;
  CHECK(raster.pixels == pixels);
}

TEST_CASE("gdal raster: in a file of reals, \"not a number\" is no data, and only where it is",
          "[gdal]")
{
  // Two degrees each way from (1, 3), at the grid's four pixels to a degree, one pixel "not a
  // number". Read onto a grid twice as fine by weights, the pixel is four of the grid's, and its
  // neighbours are made from the other pixels around them.
  using Reals                                     = miniverse::geo::Raster<float>;
  constexpr float                       GRID_NONE = -5555;
  constexpr miniverse::geo::Grid<float> FINE{
      .pixels_per_degree = 8, .tile_pixels = 8, .nodata = GRID_NONE};
  constexpr miniverse::geo::Grid<std::int16_t> WHOLE{
      .pixels_per_degree = 4, .tile_pixels = 4, .nodata = -30000};
  std::vector<float> pixels(64, 7);
  pixels.at((3 * 8) + 3) = std::numeric_limits<float>::quiet_NaN();
  const Files       files;
  const std::string unnamed = files.path_of("reals.tif");
  const std::string named   = files.path_of("reals_with_nodata.tif");
  write_tiff<float>(unnamed, {.epsg   = 4326,
                              .west   = 1,
                              .north  = 3,
                              .pixel  = 0.25,
                              .width  = 8,
                              .height = 8,
                              .pixels = pixels,
                              .nodata = std::nullopt});
  std::vector<float> with_nodata = pixels;
  with_nodata.at((5 * 8) + 5)    = FILE_NODATA;
  write_tiff<float>(named, {.epsg   = 4326,
                            .west   = 1,
                            .north  = 3,
                            .pixel  = 0.25,
                            .width  = 8,
                            .height = 8,
                            .pixels = with_nodata,
                            .nodata = FILE_NODATA});

  const Reals fine = gdal::read_raster<float>(
      {.path = unnamed, .band = 1, .resampling = gdal::Resampling::Bilinear}, FINE);
  REQUIRE(fine.width == 16);
  CHECK(std::ranges::count(fine.pixels, GRID_NONE) == 4);
  CHECK(std::ranges::count(fine.pixels, 7.0F) == 252);
  CHECK(fine.at(6, 6) == GRID_NONE);
  CHECK(fine.at(7, 7) == GRID_NONE);

  // Onto a grid of whole numbers, it is still that grid's nodata.
  const miniverse::geo::Raster<std::int16_t> whole =
      gdal::read_raster<std::int16_t>(as_it_is(unnamed), WHOLE);
  CHECK(std::ranges::count(whole.pixels, std::int16_t{-30000}) == 1);
  CHECK(whole.at(3, 3) == -30000);

  // A file that names a number as its nodata, and holds "not a number" too, read as it is: both are
  // no data.
  const Reals as_named = gdal::read_raster<float>(
      as_it_is(named), {.pixels_per_degree = 4, .tile_pixels = 4, .nodata = GRID_NONE});
  CHECK(std::ranges::count(as_named.pixels, GRID_NONE) == 2);
  CHECK(as_named.at(3, 3) == GRID_NONE);
  CHECK(as_named.at(5, 5) == GRID_NONE);
}

TEST_CASE("gdal raster: a file turned a quarter reads onto the grid as it lies", "[gdal]")
{
  // Eight columns south and four rows west from (2, 3): from (1, 3), one degree across and two
  // down. The file's pixel `column * 100 + row` is the grid's `3 - row` across and `column` down.
  const Files       files;
  const std::string path = files.path_of("turned.tif");
  write_tiff(path, {.epsg   = 4326,
                    .west   = 1,
                    .north  = 3,
                    .pixel  = 0.25,
                    .width  = 8,
                    .height = 4,
                    .pixels = numbered(8, 4),
                    .turned = true});

  const Raster raster = gdal::read_raster<std::int16_t>(as_it_is(path), GRID);

  CHECK(raster.west == 1);
  CHECK(raster.north == 3);
  REQUIRE(raster.width == 4);
  REQUIRE(raster.height == 8);
  for ( std::size_t row = 0; row < raster.height; ++row ) {
    for ( std::size_t column = 0; column < raster.width; ++column ) {
      CHECK(raster.at(column, row) == static_cast<std::int16_t>((row * 100) + 3 - column));
    }
  }
}

TEST_CASE("gdal raster: a turned file reads as the same ground upright, by weights or by mean",
          "[gdal]")
{
  // Rough ground from (1, 3), upright and turned a quarter: the turned file's column `c` of row `r`
  // is the upright one's column `across - 1 - r` of row `c`. Read onto a grid coarser than the
  // files, and onto one finer than files of another shape, the two are the same pixels, but for how
  // a mean rounds.
  struct Ground {
    double                             pixel;
    int                                across;
    int                                down;
    miniverse::geo::Grid<std::int16_t> grid;
  };

  const std::vector<Ground> grounds{
      {.pixel = 0.0625, .across = 16, .down = 32, .grid = GRID},
      {.pixel  = 0.25,
       .across = 4,
       .down   = 16,
       .grid   = {.pixels_per_degree = 8, .tile_pixels = 8, .nodata = GRID_NODATA}},
  };

  for ( const Ground& ground : grounds ) {
    const std::vector<std::int16_t> upright = rough(ground.across, ground.down);
    std::vector<std::int16_t>       turned;
    turned.reserve(upright.size());
    for ( int row = 0; row < ground.across; ++row ) {
      for ( int column = 0; column < ground.down; ++column ) {
        const int upright_column = ground.across - 1 - row;
        turned.push_back(upright.at(
            (static_cast<std::size_t>(column) * static_cast<std::size_t>(ground.across)) +
            static_cast<std::size_t>(upright_column)));
      }
    }
    const Files       files;
    const std::string upright_path = files.path_of("upright.tif");
    const std::string turned_path  = files.path_of("turned.tif");
    write_tiff(upright_path, {.epsg   = 4326,
                              .west   = 1,
                              .north  = 3,
                              .pixel  = ground.pixel,
                              .width  = ground.across,
                              .height = ground.down,
                              .pixels = upright});
    write_tiff(turned_path, {.epsg   = 4326,
                             .west   = 1,
                             .north  = 3,
                             .pixel  = ground.pixel,
                             .width  = ground.down,
                             .height = ground.across,
                             .pixels = turned,
                             .turned = true});

    for ( const gdal::Resampling resampling :
          {gdal::Resampling::Bilinear, gdal::Resampling::Cubic, gdal::Resampling::Average} )
    {
      INFO("pixels of " << ground.pixel << " degrees, resampling " << static_cast<int>(resampling));
      const Raster as_upright = gdal::read_raster<std::int16_t>(
          {.path = upright_path, .band = 1, .resampling = resampling}, ground.grid);
      const Raster as_turned = gdal::read_raster<std::int16_t>(
          {.path = turned_path, .band = 1, .resampling = resampling}, ground.grid);

      CHECK(as_turned.west == as_upright.west);
      CHECK(as_turned.north == as_upright.north);
      REQUIRE(as_turned.width == as_upright.width);
      REQUIRE(as_turned.height == as_upright.height);
      for ( std::size_t i = 0; i < as_upright.pixels.size(); ++i ) {
        CHECK(std::abs(as_turned.pixels.at(i) - as_upright.pixels.at(i)) <= 1);  // a mean of the
                                                                                 // same pixels
                                                                                 // added in another
                                                                                 // order
      }
    }
  }
}

TEST_CASE(
    "gdal raster: a file that is not there, a band it does not have, or no coordinate system, is "
    "refused with the reason",
    "[gdal]")
{
  const Files       files;
  const std::string path    = files.path_of("on_grid.tif");
  const std::string nowhere = files.path_of("nowhere.tif");
  write_tiff(path, {.epsg   = 4326,
                    .west   = 1,
                    .north  = 3,
                    .pixel  = 0.25,
                    .width  = 4,
                    .height = 4,
                    .pixels = numbered(4, 4)});
  write_tiff(nowhere, {.epsg   = 0,
                       .west   = 1,
                       .north  = 3,
                       .pixel  = 0.25,
                       .width  = 4,
                       .height = 4,
                       .pixels = numbered(4, 4)});

  CHECK_THROWS_WITH(gdal::read_raster<std::int16_t>(as_it_is(files.path_of("nothing.tif")), GRID),
                    ContainsSubstring("can't be opened"));
  CHECK_THROWS_WITH(gdal::read_raster<std::int16_t>(
                        {.path = path, .band = 2, .resampling = gdal::Resampling::Nearest}, GRID),
                    ContainsSubstring("so no band 2"));
  CHECK_THROWS_WITH(gdal::read_raster<std::int16_t>(as_it_is(nowhere), GRID),
                    ContainsSubstring("names no coordinate system"));

  // Longitudes of 179 to 183: past the world, as a file of 0 to 360 is.
  const std::string past = files.path_of("past.tif");
  write_tiff(past, {.epsg   = 4326,
                    .west   = 179,
                    .north  = 3,
                    .pixel  = 0.25,
                    .width  = 16,
                    .height = 4,
                    .pixels = numbered(16, 4)});
  CHECK_THROWS_WITH(gdal::read_raster<std::int16_t>(as_it_is(past), GRID),
                    ContainsSubstring("reaches past the world"));
  CHECK_THROWS_AS(
      gdal::read_raster<std::int16_t>(
          as_it_is(path), {.pixels_per_degree = 0, .tile_pixels = 4, .nodata = GRID_NODATA}),
      std::invalid_argument);
}

TEST_CASE("gdal raster: what the callback throws is passed on", "[gdal]")
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

  std::size_t                       windows    = 0;
  const std::function<void(Raster)> taking_one = [&](const Raster& /*window*/) {
    if ( ++windows == 2 ) {
      throw std::runtime_error("one window is enough");
    }
  };

  CHECK_THROWS_WITH(gdal::read_raster<std::int16_t>(as_it_is(path), GRID, 1, taking_one),
                    ContainsSubstring("is enough"));
  CHECK(windows == 2);
}

TEST_CASE("gdal raster: pixels made by weights are the same whichever window they are read in",
          "[gdal]")
{
  // Rough ground, finer than the grid, in WGS 84 and in Web Mercator: two degrees each way from
  // longitude 1, latitude 3. A weighted mean of rough pixels shows any difference in how a window
  // is made; a smooth ramp would hide it.
  constexpr double  PIXEL = MERCATOR_DEGREE / 10;
  const Files       files;
  const std::string plain    = files.path_of("rough.tif");
  const std::string mercator = files.path_of("rough_mercator.tif");
  write_tiff(plain, {.epsg   = 4326,
                     .west   = 1,
                     .north  = 3,
                     .pixel  = 0.1,
                     .width  = 20,
                     .height = 20,
                     .pixels = rough(20, 20)});
  write_tiff(mercator, {.epsg   = 3857,
                        .west   = MERCATOR_DEGREE,
                        .north  = mercator_north(3),
                        .pixel  = PIXEL,
                        .width  = 20,
                        .height = 20,
                        .pixels = rough(20, 20)});

  for ( const std::string& path : {plain, mercator} ) {
    for ( const gdal::Resampling resampling :
          {gdal::Resampling::Bilinear, gdal::Resampling::Cubic, gdal::Resampling::Average} )
    {
      INFO(path << ", resampling " << static_cast<int>(resampling));
      const gdal::RasterSource source{.path = path, .band = 1, .resampling = resampling};

      const Raster        whole = gdal::read_raster<std::int16_t>(source, GRID);
      std::vector<Raster> windows;
      std::vector<Raster> tiles;
      std::ignore = gdal::read_raster<std::int16_t>(
          source, GRID, 1, [&windows](Raster window) { windows.push_back(std::move(window)); });
      gdal::read_tiles_in<std::int16_t>(source, GRID, polygon("POLYGON((0 0,6 0,6 6,0 6,0 0))"), 3,
                                        [&tiles](std::vector<Raster> chunk) {
                                          tiles.insert(tiles.end(), chunk.begin(), chunk.end());

                                          return true;
                                        });

      REQUIRE(whole.width == 8);
      REQUIRE(whole.height == 8);
      CHECK(in_order(tiles) == in_order(windows));  // a stream's runs of tiles, and a push's
                                                    // windows of one
      for ( const Raster& window : windows ) {
        const auto first_column =
            static_cast<std::size_t>(std::lround((window.west - whole.west) * 4));
        const auto first_row =
            static_cast<std::size_t>(std::lround((whole.north - window.north) * 4));
        for ( std::size_t row = 0; row < window.height; ++row ) {
          for ( std::size_t column = 0; column < window.width; ++column ) {
            CHECK(window.at(column, row) == whole.at(first_column + column, first_row + row));
          }
        }
      }
    }
  }
}

TEST_CASE("gdal raster: a read of tiles stops when its callback wants no more", "[gdal]")
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
  std::vector<std::size_t> sizes;

  gdal::read_tiles_in<std::int16_t>(as_it_is(path), GRID, polygon("POLYGON((0 0,6 0,6 6,0 6,0 0))"),
                                    1, [&sizes](const std::vector<Raster>& chunk) {
                                      sizes.push_back(chunk.size());

                                      return sizes.size() < 2;
                                    });

  CHECK(sizes == std::vector<std::size_t>{1, 1});  // of four tiles
  CHECK_THROWS_AS(
      gdal::read_tiles_in<std::int16_t>(
          as_it_is(path), {.pixels_per_degree = 100'000, .tile_pixels = 4, .nodata = GRID_NODATA},
          polygon("POLYGON((0 0,1 0,1 1,0 0))"), 1, {}),
      std::invalid_argument);  // a grid no table can be made with is none for a file either
}
