#pragma once

#include <cmath>
#include <concepts>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "miniverse/geo/concepts/raster.hpp"  // IWYU pragma: export

/**
 * @file
 * The grid a raster table's tiles lie on: pixels per degree of WGS 84 from the world's north-west corner, square tiles, a
 * nodata value; and the way from a resolution in metres to such a grid.
 */
namespace miniverse::geo {

/** @brief The longitude every grid starts at: its north-west corner's. */
inline constexpr double GRID_WEST = -180;

/** @brief The latitude every grid starts at: its north-west corner's. */
inline constexpr double GRID_NORTH = 90;

/** @brief How many degrees of longitude the world spans from `GRID_WEST`. */
inline constexpr std::int64_t DEGREES_ACROSS = 360;

/** @brief How many degrees of latitude the world spans from `GRID_NORTH`. */
inline constexpr std::int64_t DEGREES_DOWN = 180;

/** @brief The finest grid there is, in pixels per degree (0.1 arc seconds, about 3.1 m): `raster_columns` can't tell finer grids apart. */
inline constexpr std::int32_t MAX_PIXELS_PER_DEGREE = 36000;

/**
 * @brief How many metres a degree of latitude is, near enough, and a degree of longitude at the equator: what a resolution in
 * metres is made into pixels per degree by (`pixels_per_degree_of_metres`). A 360th of WGS 84's equator.
 */
inline constexpr double METRES_PER_DEGREE = 111'319.490793;

/**
 * @return How many pixels to a degree a grid of about `metres_per_pixel` has: the nearest whole number, so 30 m is 3711 of
 * them (29.997 m; `Grid::metres_per_pixel` gives what a grid came to). For the `pixels_per_degree` of a `Grid`:
 *
 * @code
 * world.create_table<Elevation>({.pixels_per_degree = geo::pixels_per_degree_of_metres(30), .tile_pixels = 256, .nodata = -32768});
 * @endcode
 *
 * A pixel is that many metres from north to south everywhere, and from east to west at the equator; towards the poles it is
 * narrower on the ground by the cosine of the latitude. A source in arc seconds keeps its own pixels only on its own grid:
 * give 3600 pixels per degree for one arc second, not 30 m.
  * @throws std::invalid_argument unless `metres_per_pixel` is above zero, at most `METRES_PER_DEGREE`, and comes to at most
 * `MAX_PIXELS_PER_DEGREE` pixels per degree: about 3.1 m at the finest, a degree (about 111 km) at the coarsest. Near the
 * coarse end the nearest grid is far from what was asked: 80 km is a pixel to a degree too.
 */
[[nodiscard]] inline std::int32_t pixels_per_degree_of_metres(double metres_per_pixel) {
  const double pixels = std::round(METRES_PER_DEGREE / metres_per_pixel);
  if ( ! (metres_per_pixel > 0) || ! (metres_per_pixel <= METRES_PER_DEGREE) || ! (pixels <= MAX_PIXELS_PER_DEGREE) ) {
    throw std::invalid_argument(
        "a grid of " + std::to_string(metres_per_pixel) + " m to a pixel can't be made: the finest is about 3.1 m (" +
        std::to_string(MAX_PIXELS_PER_DEGREE) + " pixels per degree), and the coarsest one pixel per degree"
    );
  }

  return static_cast<std::int32_t>(pixels);
}

/**
 * @brief The fixed grid a raster table's tiles lie on: `pixels_per_degree` pixels to a degree in both directions, counted from
 * the corner at longitude -180, latitude 90, in square tiles of `tile_pixels` pixels a side. Every source is warped onto it.
 *
 * Pixels per degree is a whole number, so where each tile and pixel lies is exact arithmetic on integers. 3600 is one arc
 * second, SRTM's finest; a resolution in metres is made into the nearest such grid by `pixels_per_degree_of_metres`. Tiles
 * of 512 pixels suit loads of some kilometres to a hundred; larger tiles slow small loads.
 */
template <Pixel pixel_t>
struct Grid {
  std::int32_t pixels_per_degree = 0;  ///< 1 to `MAX_PIXELS_PER_DEGREE`.
  std::int32_t tile_pixels = 0;        ///< A tile's width and height, in pixels: 1 to 65535.
  pixel_t      nodata{};               ///< The value of a pixel that has no data. Finite.

  /**
   * @return How many metres a pixel spans from north to south, near enough (a degree of latitude is 110.6 to 111.7 km), and
   * from east to west at the equator: of a grid that has pixels.
   */
  [[nodiscard]] double metres_per_pixel() const { return METRES_PER_DEGREE / pixels_per_degree; }

  /** @return How many tiles span the world's 360 degrees of longitude: the last reaches past it when tiles don't divide them. */
  [[nodiscard]] std::int64_t tiles_across() const { return tiles_spanning(DEGREES_ACROSS); }

  /** @return How many tiles span the world's 180 degrees of latitude, as `tiles_across`. */
  [[nodiscard]] std::int64_t tiles_down() const { return tiles_spanning(DEGREES_DOWN); }

  [[nodiscard]] bool operator==(const Grid&) const = default;

 private:
  [[nodiscard]] std::int64_t tiles_spanning(std::int64_t degrees) const {
    const std::int64_t pixels = degrees * pixels_per_degree;

    return (pixels + tile_pixels - 1) / tile_pixels;
  }
};

/** @brief The largest tile there is, in pixels a side: raster WKB holds a width and a height in 16 bits each. */
inline constexpr std::int32_t MAX_TILE_PIXELS = 65535;

/**
 * @brief Throws unless `grid` is a grid a raster table can be made with, and a file be read onto: 1 to `MAX_PIXELS_PER_DEGREE`
 * pixels per degree, tiles of 1 to `MAX_TILE_PIXELS` pixels a side, and a nodata value that is a finite number.
 * @throws std::invalid_argument for one that is not.
 */
template <Pixel pixel_t>
void check_grid(const Grid<pixel_t>& grid) {
  if ( grid.pixels_per_degree < 1 || grid.pixels_per_degree > MAX_PIXELS_PER_DEGREE ) {
    throw std::invalid_argument(
        "a grid has 1 to " + std::to_string(MAX_PIXELS_PER_DEGREE) + " pixels per degree, not " + std::to_string(grid.pixels_per_degree)
    );
  }

  if ( grid.tile_pixels < 1 || grid.tile_pixels > MAX_TILE_PIXELS ) {
    throw std::invalid_argument(
        "a grid's tiles are 1 to " + std::to_string(MAX_TILE_PIXELS) + " pixels a side, not " + std::to_string(grid.tile_pixels)
    );
  }

  if constexpr ( std::floating_point<pixel_t> ) {
    if ( ! std::isfinite(grid.nodata) ) {
      throw std::invalid_argument("a grid's nodata value must be a finite number");
    }
  }
}

}  // namespace miniverse::geo
