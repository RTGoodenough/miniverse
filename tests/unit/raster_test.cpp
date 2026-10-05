// Raster WKB, read and written, against what PostGIS 3.5 itself writes: each hex string below is `ST_AsHexWKB(...)` of the
// raster named beside it, made with ST_MakeEmptyRaster, ST_AddBand and ST_SetValues.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

#include "miniverse/geo/column_types.hpp"
#include "miniverse/geo/raster.hpp"
#include "schemacht/util/hex.hpp"

namespace geo = miniverse::geo;
namespace wkb = miniverse::geo::wkb;

using schemacht::util::format_hex;
using schemacht::util::parse_hex;

namespace {

// 3 by 2 pixels of 16BSI from (-1, 2), 0.5 degrees each, nodata -32768: {{1, -2, 300}, {nodata, 7, 32767}}
constexpr std::string_view INT16 =
    "0100000100000000000000e03f000000000000e0bf000000000000f0bf000000000000004000000000000000000000000000000000e6100000030002004500800100fe"
    "ff2c0100800700ff7f";
// The same, big-endian. PostGIS writes rasters only little-endian, so this was swapped by hand; PostGIS reads it back as the same raster.
constexpr std::string_view INT16_BIG_ENDIAN =
    "00000000013fe0000000000000bfe0000000000000bff0000000000000400000000000000000000000000000000000000000000000000010e6000300024580000001ff"
    "fe012c800000077fff";
// 2 by 1 pixels of 32BF from (10, 20), 0.25 degrees each, nodata -9999: {{1.5, nodata}}
constexpr std::string_view FLOAT32 =
    "0100000100000000000000d03f000000000000d0bf0000000000002440000000000000344000000000000000000000000000000000e6100000020001004a003c1cc600"
    "00c03f003c1cc6";
// 1 by 1 pixel of 16BSI from (0, 0), 1 degree: skewed, in SRID 3857, without a nodata value, with two bands, with no band
constexpr std::string_view SKEWED =
    "0100000100000000000000f03f000000000000f0bf000000000000000000000000000000009a9999999999b93f0000000000000000e6100000010001004500800000";
constexpr std::string_view SRID_3857 =
    "0100000100000000000000f03f000000000000f0bf0000000000000000000000000000000000000000000000000000000000000000110f0000010001004500800000";
constexpr std::string_view NO_NODATA =
    "0100000100000000000000f03f000000000000f0bf0000000000000000000000000000000000000000000000000000000000000000e6100000010001000500000500";
constexpr std::string_view TWO_BANDS =
    "0100000200000000000000f03f000000000000f0bf0000000000000000000000000000000000000000000000000000000000000000e61000000100010045008000004500"
    "800000";
constexpr std::string_view NO_BANDS =
    "0100000000000000000000f03f000000000000f0bf0000000000000000000000000000000000000000000000000000000000000000e610000001000100";

[[nodiscard]] geo::Raster<std::int16_t> int16_raster() {
  return {
      .west = -1,
      .north = 2,
      .pixel_width = 0.5,
      .pixel_height = 0.5,
      .width = 3,
      .height = 2,
      .nodata = -32768,
      .pixels = {1, -2, 300, -32768, 7, 32767}
  };
}

/** @return `hex` with its band's flags (the byte after the header, at hex digit 122) set to `flags`. */
[[nodiscard]] std::string with_band_flags(std::string_view hex, std::uint8_t flags) {
  constexpr std::size_t BAND_FLAGS_AT = 122;
  std::string           changed(hex);
  changed.replace(BAND_FLAGS_AT, 2, std::format("{:02x}", flags));

  return changed;
}

template <geo::Pixel pixel_t>
[[nodiscard]] bool refused(std::string_view hex) {
  try {
    std::ignore = wkb::read_raster<pixel_t>(parse_hex(hex));
  } catch ( const std::invalid_argument& ) {
    return true;
  }

  return false;
}

}  // namespace

TEST_CASE("raster wkb: a raster is written as PostGIS writes it", "[raster]") {
  CHECK(format_hex(wkb::write(int16_raster())) == INT16);
  CHECK(
      format_hex(
          wkb::write(
              geo::Raster<float>{
                  .west = 10,
                  .north = 20,
                  .pixel_width = 0.25,
                  .pixel_height = 0.25,
                  .width = 2,
                  .height = 1,
                  .nodata = -9999,
                  .pixels = {1.5F, -9999}
              }
          )
      ) == FLOAT32
  );
}

TEST_CASE("raster wkb: what PostGIS writes is read back", "[raster]") {
  CHECK(wkb::read_raster<std::int16_t>(parse_hex(INT16)) == int16_raster());
  CHECK(wkb::read_raster<std::int16_t>(parse_hex(INT16_BIG_ENDIAN)) == int16_raster());

  const geo::Raster<float> floats = wkb::read_raster<float>(parse_hex(FLOAT32));
  CHECK(floats.west == 10);
  CHECK(floats.north == 20);
  CHECK(floats.pixel_width == 0.25);
  CHECK(floats.pixel_height == 0.25);
  CHECK(floats.nodata == -9999);
  CHECK(floats.at(0, 0) == 1.5F);
  CHECK(floats.at(1, 0) == -9999);
}

TEST_CASE("raster wkb: what is not one in-db band of the pixel type, north up in WGS 84, is refused", "[raster]") {
  CHECK(refused<std::int16_t>(SKEWED));
  CHECK(refused<std::int16_t>(SRID_3857));
  CHECK(refused<std::int16_t>(NO_NODATA));
  CHECK(refused<std::int16_t>(TWO_BANDS));
  CHECK(refused<std::int16_t>(NO_BANDS));
  CHECK(refused<std::int16_t>(with_band_flags(INT16, 0xc5)));  // out-db
  CHECK(refused<float>(INT16));                                // another pixel type
  CHECK(refused<std::int16_t>("0101000100"));                  // version 1
  CHECK(refused<std::int16_t>(INT16.substr(0, INT16.size() - 2)));
  CHECK(refused<std::int16_t>(std::string(INT16) + "00"));
  CHECK(! refused<std::int16_t>(with_band_flags(INT16, 0x65)));  // flagged as all nodata: read as it is
}

TEST_CASE("raster wkb: a raster whose pixels are not width times height is not written", "[raster]") {
  geo::Raster<std::int16_t> raster = int16_raster();
  raster.pixels.pop_back();

  CHECK_THROWS_AS(wkb::write(raster), std::invalid_argument);
}

TEST_CASE("column type: a raster is read as ST_AsBinary's bytea, and written as hex raster WKB", "[raster]") {
  using RasterColumn = schemacht::ColumnType<geo::Raster<std::int16_t>>;

  CHECK(RasterColumn::parse("\\x" + std::string(INT16)) == int16_raster());
  CHECK_THROWS_AS(RasterColumn::parse(INT16), std::invalid_argument);  // the raster's own text form is not what a load selects
  CHECK(RasterColumn::format(int16_raster()) == INT16);
  CHECK(RasterColumn::parse_binary(parse_hex(INT16)) == int16_raster());
}

TEST_CASE("grid: a resolution in metres is the nearest whole pixels per degree", "[raster]") {
  using miniverse::geo::pixels_per_degree_of_metres;

  const miniverse::geo::Grid<std::int16_t> thirty{.pixels_per_degree = pixels_per_degree_of_metres(30), .tile_pixels = 256, .nodata = -32768};

  CHECK(thirty.pixels_per_degree == 3711);
  CHECK(thirty.metres_per_pixel() > 29.99);  // what 30 came to: 29.997
  CHECK(thirty.metres_per_pixel() < 30.0);
  CHECK(pixels_per_degree_of_metres(10) == 11132);
  CHECK(pixels_per_degree_of_metres(90) == 1237);
  CHECK(pixels_per_degree_of_metres(miniverse::geo::METRES_PER_DEGREE) == 1);
}

TEST_CASE("grid: a resolution finer than the finest grid, coarser than a degree, or no number above zero, is refused", "[raster]") {
  using miniverse::geo::pixels_per_degree_of_metres;

  CHECK(pixels_per_degree_of_metres(3.1) == 35910);
  CHECK_THROWS_AS(pixels_per_degree_of_metres(3.0), std::invalid_argument);  // 37106
  CHECK_THROWS_AS(pixels_per_degree_of_metres(1), std::invalid_argument);
  CHECK_THROWS_AS(pixels_per_degree_of_metres(250'000), std::invalid_argument);  // under half a pixel to a degree
  CHECK_THROWS_AS(pixels_per_degree_of_metres(0), std::invalid_argument);
  CHECK_THROWS_AS(pixels_per_degree_of_metres(-30), std::invalid_argument);
  CHECK_THROWS_AS(pixels_per_degree_of_metres(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
  CHECK_THROWS_AS(pixels_per_degree_of_metres(std::numeric_limits<double>::infinity()), std::invalid_argument);
}

namespace {

// 4 by 3 pixels from (7, 47), a tenth of a degree each unless told otherwise, each its column plus ten times its row, but
// the one at column 2, row 1, which has no data.
geo::Raster<std::int16_t> small(double pixel = 0.1) {
  geo::Raster<std::int16_t> raster{.west = 7, .north = 47, .pixel_width = pixel, .pixel_height = pixel, .width = 4, .height = 3, .nodata = -1, .pixels = {}};
  for ( std::size_t row = 0; row < raster.height; ++row ) {
    for ( std::size_t column = 0; column < raster.width; ++column ) {
      raster.pixels.push_back(column == 2 && row == 1 ? raster.nodata : static_cast<std::int16_t>(column + (10 * row)));
    }
  }

  return raster;
}

}  // namespace

TEST_CASE("raster: a position has the pixel it lies in, and that pixel's value", "[raster]") {
  const geo::Raster<std::int16_t> raster = small();

  CHECK(raster.pixel_at({7.05, 46.95}) == geo::PixelPosition{.column = 0, .row = 0});
  CHECK(raster.pixel_at({7.35, 46.75}) == geo::PixelPosition{.column = 3, .row = 2});
  CHECK(raster.value_at({7.35, 46.75}) == 23);
  CHECK(raster.value_at({7.15, 46.85}) == 11);

  CHECK(raster.value_at({7.25, 46.85}) == std::nullopt);  // a pixel with no data
  CHECK(raster.pixel_at({7.25, 46.85}) == geo::PixelPosition{.column = 2, .row = 1});
}

TEST_CASE("raster: a position on a line between pixels is in the eastern or southern one", "[raster]") {
  const geo::Raster<std::int16_t> raster = small(0.25);  // quarters of a degree, which doubles hold exactly

  CHECK(raster.pixel_at({7.5, 46.5}) == geo::PixelPosition{.column = 2, .row = 2});
  CHECK(raster.pixel_at({7.25, 46.9}) == geo::PixelPosition{.column = 1, .row = 0});
  CHECK(raster.pixel_at({7.1, 46.75}) == geo::PixelPosition{.column = 0, .row = 1});
}

TEST_CASE("raster: a raster has its four edges, also where a double puts a position a rounding beyond one", "[raster]") {
  const geo::Raster<std::int16_t> raster = small();

  CHECK(raster.pixel_at({7, 47}) == geo::PixelPosition{.column = 0, .row = 0});
  CHECK(raster.pixel_at({7.4, 46.7}) == geo::PixelPosition{.column = 3, .row = 2});

  CHECK((7.4 - 7) / 0.1 > 4);  // the east edge as a program writes it is past the last column, by a rounding
  CHECK(raster.pixel_at({7.4, 46.95}) == geo::PixelPosition{.column = 3, .row = 0});
  CHECK(raster.pixel_at({7.05, 46.7}) == geo::PixelPosition{.column = 0, .row = 2});
  CHECK(raster.pixel_at({6.999999999999999, 47.00000000000001}) == geo::PixelPosition{.column = 0, .row = 0});

  CHECK(! raster.pixel_at({7.4000001, 46.95}));  // a millionth of a pixel is not a rounding
  CHECK(! raster.pixel_at({6.9999999, 46.95}));
}

TEST_CASE("raster: a position outside a raster, or that is no number, has no pixel", "[raster]") {
  const geo::Raster<std::int16_t> raster = small();
  const double                    nan = std::numeric_limits<double>::quiet_NaN();

  CHECK(! raster.pixel_at({6.99, 46.95}));
  CHECK(! raster.pixel_at({7.41, 46.95}));
  CHECK(! raster.pixel_at({7.05, 47.01}));
  CHECK(! raster.pixel_at({7.05, 46.69}));
  CHECK(! raster.pixel_at({nan, 46.95}));
  CHECK(! raster.pixel_at({7.05, nan}));
  CHECK(! raster.value_at({-200, 46.95}));

  CHECK(! geo::Raster<std::int16_t>{}.pixel_at({0, 0}));  // a raster of no pixels has none anywhere
}

TEST_CASE("raster: the middle of every pixel of a fine raster is in that pixel", "[raster]") {
  // A grid of one arc second, whose pixel size no double holds exactly, far from the grid's corner.
  constexpr double                SECOND = 1.0 / 3600;
  const geo::Raster<std::int16_t> raster{
      .west = -71 - (1234 * SECOND), .north = 42 + (4321 * SECOND), .pixel_width = SECOND, .pixel_height = SECOND, .width = 700, .height = 500,
      .nodata = -1, .pixels = {}
  };

  std::size_t elsewhere = 0;
  for ( std::size_t row = 0; row < raster.height; ++row ) {
    for ( std::size_t column = 0; column < raster.width; ++column ) {
      elsewhere += raster.pixel_at(raster.centre_of(column, row)) == geo::PixelPosition{.column = column, .row = row} ? 0 : 1;
    }
  }

  CHECK(elsewhere == 0);
  CHECK(raster.centre_of(0, 0).x() == raster.west + (SECOND / 2));
  CHECK(raster.centre_of(0, 0).y() == raster.north - (SECOND / 2));
}

TEST_CASE("raster: a column or a row a raster does not have is refused, not read from the next row", "[raster]") {
  const geo::Raster<std::int16_t> raster = small();

  CHECK(raster.at(3, 2) == 23);
  CHECK_THROWS_AS(raster.at(4, 0), std::out_of_range);
  CHECK_THROWS_AS(raster.at(0, 3), std::out_of_range);
}
