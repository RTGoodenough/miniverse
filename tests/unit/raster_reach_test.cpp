// Which tiles a location has: those it reaches into, told from those it only touches.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/geo/types.hpp"
#include "miniverse/layer/raster/reach.hpp"
#include "support/rasters.hpp"

namespace geo = miniverse::geo;

using miniverse::raster::Reach;
using test::polygon;

namespace {

/** @return The outline of the tile of one degree whose north-west corner is (`west`, `north`). */
[[nodiscard]] geo::Box tile(double west, double north) { return {{west, north - 1}, {west + 1, north}}; }

/** @return How many of two tiles side by side, from (0, 1) and from (1, 1), the location `wkt` reaches into. */
[[nodiscard]] std::size_t tiles_in(const std::string& wkt) {
  const Reach reach(polygon(wkt));

  return static_cast<std::size_t>(reach.into(tile(0, 1))) + static_cast<std::size_t>(reach.into(tile(1, 1)));
}

/** @return Where the tiles of nine, three by three from (0, 3), that `wkt` reaches into lie: the west and north of each, from the north west. */
[[nodiscard]] std::vector<std::pair<double, double>> tiles_of_nine_in(const std::string& wkt) {
  const Reach reach(polygon(wkt));

  std::vector<std::pair<double, double>> corners;
  for ( int row = 0; row < 3; ++row ) {
    for ( int column = 0; column < 3; ++column ) {
      if ( reach.into(tile(column, 3 - row)) ) {
        corners.emplace_back(column, 3 - row);
      }
    }
  }

  std::ranges::sort(corners, [](const auto& one, const auto& other) {
    return one.second != other.second ? one.second > other.second : one.first < other.first;
  });

  return corners;
}

// A strip a fiftieth of a degree wide from the south west to the north east, half a degree north of the diagonal: its box
// is nearly all of the nine tiles, and it crosses five of them, none at a corner.
const std::string STRIP = "POLYGON((0.1 0.6,0.12 0.58,2.42 2.88,2.4 2.9,0.1 0.6))";

}  // namespace

TEST_CASE("reach: a tile the location only touches, or reaches into by no more than a rounding, is not the location's", "[reach]") {
  CHECK(tiles_in("POLYGON((1 0.2,1.5 0.2,1.5 0.8,1 0.8,1 0.2))") == 1);                          // ending on the west tile's edge: the east one alone
  CHECK(tiles_in("POLYGON((0.9999999 0.2,1.5 0.2,1.5 0.8,0.9999999 0.8,0.9999999 0.2))") == 2);  // a hair into the west tile: both
  CHECK(tiles_in("POLYGON((0.999999999999 0.2,1.5 0.2,1.5 0.8,0.999999999999 0.8,0.999999999999 0.2))") == 1);  // a rounding into it: as on its edge
  CHECK(tiles_in("POLYGON((1.0000001 0.2,1.5 0.2,1.5 0.8,1.0000001 0.8,1.0000001 0.2))") == 1);
  CHECK(tiles_in("POLYGON((2.0000001 0.2,2.5 0.2,2.5 0.8,2.0000001 0.8,2.0000001 0.2))") == 0);
  CHECK(tiles_in("POLYGON((0.5 1,1.5 1,1.5 2,0.5 2,0.5 1))") == 0);  // touching both from the north: neither
  CHECK_FALSE(Reach({}).into(tile(0, 1)));                           // a location of no points
}

TEST_CASE("reach: an area that runs askew has the tiles it reaches into, not all the tiles of its box", "[reach]") {
  using Corners = std::vector<std::pair<double, double>>;

  CHECK(tiles_of_nine_in("POLYGON((0.1 0.1,2.9 0.1,2.9 2.9,0.1 2.9,0.1 0.1))").size() == 9);  // the strip's box, near enough
  CHECK(tiles_of_nine_in(STRIP) == Corners{{1, 3}, {2, 3}, {0, 2}, {1, 2}, {0, 1}});

  // Whichever way its ring runs round.
  CHECK(tiles_of_nine_in("POLYGON((0.1 0.6,2.4 2.9,2.42 2.88,0.12 0.58,0.1 0.6))") == tiles_of_nine_in(STRIP));

  // A tile that lies in a hole of the area is not reached, though the area is all around it.
  CHECK(
      tiles_of_nine_in("POLYGON((0.1 0.1,2.9 0.1,2.9 2.9,0.1 2.9,0.1 0.1),(0.9 0.9,0.9 2.1,2.1 2.1,2.1 0.9,0.9 0.9))") ==
      Corners{{0, 3}, {1, 3}, {2, 3}, {0, 2}, {2, 2}, {0, 1}, {1, 1}, {2, 1}}
  );
}

TEST_CASE("reach: an area of no size has the tile it lies in, and none if it lies along the line between tiles", "[reach]") {
  using Corners = std::vector<std::pair<double, double>>;

  CHECK(tiles_of_nine_in("POLYGON((1.5 1.5,1.5 1.5,1.5 1.5,1.5 1.5))") == Corners{{1, 2}});                  // a point
  CHECK(tiles_of_nine_in("POLYGON((0.5 1.5,2.5 1.5,2.5 1.5,0.5 1.5))") == Corners{{0, 2}, {1, 2}, {2, 2}});  // a line
  CHECK(tiles_of_nine_in("POLYGON((1 1,1 1,1 1,1 1))").empty());                                             // a point on a corner
  CHECK(tiles_of_nine_in("POLYGON((0.5 1,2.5 1,2.5 1,0.5 1))").empty());                                     // a line along one
}
