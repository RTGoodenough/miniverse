#pragma once

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "miniverse/geo/concepts/raster.hpp"  // IWYU pragma: export
#include "miniverse/geo/types.hpp"

/**
 * Rasters in WGS 84: a grid of pixels in longitude and latitude, north up, as PostGIS stores them (`raster`) and a load of an
 * raster layer gives them.
 */
namespace miniverse::geo {

/** @brief Where every grid starts, its north-west corner, and how many degrees the world spans from there: see `Grid`. */
inline constexpr double       GRID_WEST = -180;
inline constexpr double       GRID_NORTH = 90;
inline constexpr std::int64_t DEGREES_ACROSS = 360;
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
 * A pixel is that many metres from north to south everywhere, and from east to west at the equator: towards the poles it is
 * narrower on the ground, by the cosine of the latitude (21 m at 45 degrees, for 30), as on every grid of longitude and
 * latitude. A source that is in arc seconds keeps its own pixels only on its own grid: give 3600 pixels per degree for one
 * arc second, not 30 m.
 * @throws std::invalid_argument unless `metres_per_pixel` is a number above zero that comes to 1 to `MAX_PIXELS_PER_DEGREE`
 * pixels per degree: about 3.1 m at the finest, and a pixel to a degree, about 111 km, at the coarsest. Near that coarse
 * end the nearest grid is far from what was asked: 80 km is a pixel to a degree too.
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
 * second, SRTM's finest. A resolution in metres is made into the nearest such grid by `pixels_per_degree_of_metres`.
 *
 * Tiles of 512 pixels suit loads of some kilometres to a hundred, at 30 m to a pixel: measured, a 100 km load is some 15%
 * faster than with tiles of 256, and a 10 km load as fast; with tiles of 1024 the small load is two to three times slower.
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

  [[nodiscard]] bool operator==(const Grid&) const = default;
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
    throw std::invalid_argument("a grid's tiles are 1 to " + std::to_string(MAX_TILE_PIXELS) + " pixels a side, not " + std::to_string(grid.tile_pixels));
  }

  if constexpr ( std::floating_point<pixel_t> ) {
    if ( ! std::isfinite(grid.nodata) ) {
      throw std::invalid_argument("a grid's nodata value must be a finite number");
    }
  }
}

/** @brief Which pixel of a raster: its column, counted from the west, and its row, counted from the north, each from 0. */
struct PixelPosition {
  std::size_t column = 0;
  std::size_t row = 0;

  [[nodiscard]] bool operator==(const PixelPosition&) const = default;
};

/**
 * @brief A raster: `width` by `height` pixels, each `pixel_width` by `pixel_height` degrees, from the north-west corner at
 * (`west`, `north`), with one band of `pixel_t`.
 *
 * Pixels are in rows from the north, each row from the west: the pixel `column` across and `row` down is `at(column, row)`.
 * A pixel equal to `nodata` has no data.
 *
 * A position, a longitude and latitude, has its pixel by `pixel_at`, and its value by `value_at`: the value of the pixel it
 * lies in, not one worked out between pixels. Since a raster is north up and in degrees, as every raster of a table is, a
 * position's pixel is a matter of two divisions, and exact: there is no projection in it. What is not the same everywhere
 * is a pixel's size on the ground: as tall in metres at every latitude, and narrower by the latitude's cosine.
 */
template <Pixel pixel_t>
struct Raster {
  double               west = 0;          ///< The longitude of the west edge, in degrees.
  double               north = 0;         ///< The latitude of the north edge, in degrees.
  double               pixel_width = 0;   ///< Degrees of longitude a pixel spans.
  double               pixel_height = 0;  ///< Degrees of latitude a pixel spans (rows run south, so this is positive).
  std::size_t          width = 0;         ///< Pixels across.
  std::size_t          height = 0;        ///< Pixels down.
  pixel_t              nodata{};          ///< The value of a pixel with no data.
  std::vector<pixel_t> pixels;            ///< `width * height` of them, in rows from the north-west.

  /** @throws std::out_of_range unless `column` is less than `width`, and `row` less than `height`. */
  [[nodiscard]] pixel_t at(std::size_t column, std::size_t row) const {
    if ( column >= width || row >= height ) {
      throw std::out_of_range(
          "a raster of " + std::to_string(width) + " by " + std::to_string(height) + " pixels has no column " + std::to_string(column) + ", row " +
          std::to_string(row)
      );
    }

    return pixels.at((row * width) + column);
  }

  /**
   * @return The pixel `position` lies in; nothing if it lies outside the raster, or is no number. A position on the line
   * between two pixels is in the eastern or southern one, as nearly as a double tells: a longitude written in decimals is
   * rarely on a line to its last digit. The raster's own four edges are its outer pixels', and a position a rounding beyond
   * one (a billionth of a pixel) is on it: the corner of the area a raster was loaded for has a pixel.
   */
  [[nodiscard]] std::optional<PixelPosition> pixel_at(const Point& position) const {
    constexpr double ROUNDING = 1e-9;

    const double across = (position.x() - west) / pixel_width;
    const double down = (north - position.y()) / pixel_height;

    const bool inside = across >= -ROUNDING && across <= static_cast<double>(width) + ROUNDING && down >= -ROUNDING &&
                        down <= static_cast<double>(height) + ROUNDING;
    if ( width == 0 || height == 0 || ! inside ) {
      return std::nullopt;
    }

    const auto within = [](double pixels, std::size_t count) { return std::min(static_cast<std::size_t>(std::max(pixels, 0.0)), count - 1); };

    return PixelPosition{.column = within(across, width), .row = within(down, height)};
  }

  /** @return The value of the pixel `position` lies in; nothing if it lies outside the raster, or the pixel has no data. */
  [[nodiscard]] std::optional<pixel_t> value_at(const Point& position) const {
    const std::optional<PixelPosition> pixel = pixel_at(position);
    if ( ! pixel ) {
      return std::nullopt;
    }

    const pixel_t value = at(pixel->column, pixel->row);
    if ( value == nodata ) {
      return std::nullopt;
    }

    return value;
  }

  /** @return The box the raster's pixels fill, from its south-west corner to its north-east. */
  [[nodiscard]] Box outline() const {
    return {{west, north - (static_cast<double>(height) * pixel_height)}, {west + (static_cast<double>(width) * pixel_width), north}};
  }

  /** @return The middle of the pixel `column` across and `row` down, whether the raster has such a pixel or not. */
  [[nodiscard]] Point centre_of(std::size_t column, std::size_t row) const {
    constexpr double HALF = 0.5;

    return {west + ((static_cast<double>(column) + HALF) * pixel_width), north - ((static_cast<double>(row) + HALF) * pixel_height)};
  }

  /** @throws std::invalid_argument unless there are `width * height` pixels. */
  void check_pixel_count() const {
    if ( pixels.size() != width * height ) {
      throw std::invalid_argument(
          "a raster of " + std::to_string(width) + " by " + std::to_string(height) + " pixels has " + std::to_string(pixels.size()) + " of them"
      );
    }
  }

  [[nodiscard]] bool operator==(const Raster&) const = default;
};

}  // namespace miniverse::geo

/**
 * Raster well-known binary, the form `ST_AsBinary` gives a raster in and the `raster` type reads, for a raster of one band in
 * the database.
 *
 * - **Written** little-endian, version 0, with SRID 4326 and the band's nodata value set.
 * - **Read** in either byte order. Only one band of `pixel_t`, with a nodata value, stored in the database (not out-db), north
 *   up without skew, and with SRID 4326, is read: anything else throws `std::invalid_argument`, as malformed input does.
 */
namespace miniverse::geo::wkb {

template <Pixel pixel_t>
[[nodiscard]] std::vector<std::byte> write(const Raster<pixel_t>& raster);

/** @throws std::invalid_argument if `bytes` are not a raster as above, or have bytes left over. */
template <Pixel pixel_t>
[[nodiscard]] Raster<pixel_t> read_raster(std::span<const std::byte> bytes);

extern template std::vector<std::byte> write<std::int8_t>(const Raster<std::int8_t>& raster);
extern template std::vector<std::byte> write<std::uint8_t>(const Raster<std::uint8_t>& raster);
extern template std::vector<std::byte> write<std::int16_t>(const Raster<std::int16_t>& raster);
extern template std::vector<std::byte> write<std::uint16_t>(const Raster<std::uint16_t>& raster);
extern template std::vector<std::byte> write<std::int32_t>(const Raster<std::int32_t>& raster);
extern template std::vector<std::byte> write<std::uint32_t>(const Raster<std::uint32_t>& raster);
extern template std::vector<std::byte> write<float>(const Raster<float>& raster);
extern template std::vector<std::byte> write<double>(const Raster<double>& raster);

extern template Raster<std::int8_t>   read_raster<std::int8_t>(std::span<const std::byte> bytes);
extern template Raster<std::uint8_t>  read_raster<std::uint8_t>(std::span<const std::byte> bytes);
extern template Raster<std::int16_t>  read_raster<std::int16_t>(std::span<const std::byte> bytes);
extern template Raster<std::uint16_t> read_raster<std::uint16_t>(std::span<const std::byte> bytes);
extern template Raster<std::int32_t>  read_raster<std::int32_t>(std::span<const std::byte> bytes);
extern template Raster<std::uint32_t> read_raster<std::uint32_t>(std::span<const std::byte> bytes);
extern template Raster<float>         read_raster<float>(std::span<const std::byte> bytes);
extern template Raster<double>        read_raster<double>(std::span<const std::byte> bytes);

}  // namespace miniverse::geo::wkb
