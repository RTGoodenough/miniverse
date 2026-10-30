// Positions on a raster: the pixel a longitude and latitude lies in, its value, the raster's edges
// and the middle of a pixel.

#include "miniverse/geo/raster.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace geo = miniverse::geo;

namespace {

// 4 by 3 pixels from (7, 47), a tenth of a degree each unless told otherwise, each its column plus
// ten times its row, but the one at column 2, row 1, which has no data.
geo::Raster<std::int16_t> small(double pixel = 0.1)
{
  geo::Raster<std::int16_t> raster{.west         = 7,
                                   .north        = 47,
                                   .pixel_width  = pixel,
                                   .pixel_height = pixel,
                                   .width        = 4,
                                   .height       = 3,
                                   .nodata       = -1,
                                   .pixels       = {}};
  for ( std::size_t row = 0; row < raster.height; ++row ) {
    for ( std::size_t column = 0; column < raster.width; ++column ) {
      raster.pixels.push_back(
          column == 2 && row == 1 ? raster.nodata : static_cast<std::int16_t>(column + (10 * row)));
    }
  }

  return raster;
}

}  // namespace

TEST_CASE("raster: a position has the pixel it lies in, and that pixel's value", "[raster]")
{
  const geo::Raster<std::int16_t> raster = small();

  CHECK(raster.pixel_at({7.05, 46.95}) == geo::PixelPosition{.column = 0, .row = 0});
  CHECK(raster.pixel_at({7.35, 46.75}) == geo::PixelPosition{.column = 3, .row = 2});
  CHECK(raster.value_at({7.35, 46.75}) == 23);
  CHECK(raster.value_at({7.15, 46.85}) == 11);

  CHECK(raster.value_at({7.25, 46.85}) == std::nullopt);  // a pixel with no data
  CHECK(raster.pixel_at({7.25, 46.85}) == geo::PixelPosition{.column = 2, .row = 1});
}

TEST_CASE("raster: a position on a line between pixels is in the eastern or southern one",
          "[raster]")
{
  const geo::Raster<std::int16_t> raster = small(0.25);  // quarters of a degree, which doubles hold
                                                         // exactly

  CHECK(raster.pixel_at({7.5, 46.5}) == geo::PixelPosition{.column = 2, .row = 2});
  CHECK(raster.pixel_at({7.25, 46.9}) == geo::PixelPosition{.column = 1, .row = 0});
  CHECK(raster.pixel_at({7.1, 46.75}) == geo::PixelPosition{.column = 0, .row = 1});
}

TEST_CASE(
    "raster: a raster has its four edges, also where a double puts a position a rounding beyond "
    "one",
    "[raster]")
{
  const geo::Raster<std::int16_t> raster = small();

  CHECK(raster.pixel_at({7, 47}) == geo::PixelPosition{.column = 0, .row = 0});
  CHECK(raster.pixel_at({7.4, 46.7}) == geo::PixelPosition{.column = 3, .row = 2});

  CHECK((7.4 - 7) / 0.1 > 4);  // the east edge as a program writes it is past the last column, by a
                               // rounding
  CHECK(raster.pixel_at({7.4, 46.95}) == geo::PixelPosition{.column = 3, .row = 0});
  CHECK(raster.pixel_at({7.05, 46.7}) == geo::PixelPosition{.column = 0, .row = 2});
  CHECK(raster.pixel_at({6.999999999999999, 47.00000000000001}) ==
        geo::PixelPosition{.column = 0, .row = 0});

  CHECK(! raster.pixel_at({7.40001, 46.95}));  // a ten-thousandth of a pixel is not a rounding
  CHECK(! raster.pixel_at({6.99999, 46.95}));
}

TEST_CASE("raster: a position outside a raster, or that is no number, has no pixel", "[raster]")
{
  const geo::Raster<std::int16_t> raster = small();
  const double                    nan    = std::numeric_limits<double>::quiet_NaN();

  CHECK(! raster.pixel_at({6.99, 46.95}));
  CHECK(! raster.pixel_at({7.41, 46.95}));
  CHECK(! raster.pixel_at({7.05, 47.01}));
  CHECK(! raster.pixel_at({7.05, 46.69}));
  CHECK(! raster.pixel_at({nan, 46.95}));
  CHECK(! raster.pixel_at({7.05, nan}));
  CHECK(! raster.value_at({-200, 46.95}));

  CHECK(! geo::Raster<std::int16_t>{}.pixel_at({0, 0}));  // a raster of no pixels has none anywhere
}

TEST_CASE("raster: the middle of every pixel of a fine raster is in that pixel", "[raster]")
{
  // A grid of one arc second, whose pixel size no double holds exactly, far from the grid's corner.
  constexpr double                SECOND = 1.0 / 3600;
  const geo::Raster<std::int16_t> raster{.west         = -71 - (1234 * SECOND),
                                         .north        = 42 + (4321 * SECOND),
                                         .pixel_width  = SECOND,
                                         .pixel_height = SECOND,
                                         .width        = 700,
                                         .height       = 500,
                                         .nodata       = -1,
                                         .pixels       = {}};

  std::size_t elsewhere = 0;
  for ( std::size_t row = 0; row < raster.height; ++row ) {
    for ( std::size_t column = 0; column < raster.width; ++column ) {
      elsewhere += raster.pixel_at(raster.centre_of(column, row)) ==
                           geo::PixelPosition{.column = column, .row = row}
                       ? 0U
                       : 1U;
    }
  }

  CHECK(elsewhere == 0);
  CHECK(raster.centre_of(0, 0).x() == raster.west + (SECOND / 2));
  CHECK(raster.centre_of(0, 0).y() == raster.north - (SECOND / 2));
}

TEST_CASE("raster: a column or a row a raster does not have is refused, not read from the next row",
          "[raster]")
{
  const geo::Raster<std::int16_t> raster = small();

  CHECK(raster.at(3, 2) == 23);
  CHECK_THROWS_AS(raster.at(4, 0), std::out_of_range);
  CHECK_THROWS_AS(raster.at(0, 3), std::out_of_range);
}
