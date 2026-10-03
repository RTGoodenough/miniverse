// Raster WKB, read and written, against what PostGIS 3.5 itself writes: each hex string below is `ST_AsHexWKB(...)` of the
// raster named beside it, made with ST_MakeEmptyRaster, ST_AddBand and ST_SetValues.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
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
