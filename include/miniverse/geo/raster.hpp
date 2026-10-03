#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "miniverse/geo/concepts/raster.hpp"  // IWYU pragma: export

/**
 * Rasters in WGS 84: a grid of pixels in longitude and latitude, north up, as PostGIS stores them (`raster`) and a load of an
 * elevation layer gives them.
 */
namespace miniverse::geo {

/**
 * @brief The fixed grid a raster table's tiles lie on: `pixels_per_degree` pixels to a degree in both directions, counted from
 * the corner at longitude -180, latitude 90, in square tiles of `tile_pixels` pixels a side. Every source is warped onto it.
 *
 * Pixels per degree is a whole number, so where each tile and pixel lies is exact arithmetic on integers. 3600 is one arc
 * second, SRTM's finest.
 */
template <Pixel pixel_t>
struct Grid {
  std::int32_t pixels_per_degree = 0;  ///< 1 to 36000 (0.1 arc seconds): `raster_columns` can't tell finer grids apart.
  std::int32_t tile_pixels = 0;        ///< A tile's width and height, in pixels: 1 to 65535.
  pixel_t      nodata{};               ///< The value of a pixel that has no data. Finite.

  [[nodiscard]] bool operator==(const Grid&) const = default;
};

/**
 * @brief A raster: `width` by `height` pixels, each `pixel_width` by `pixel_height` degrees, from the north-west corner at
 * (`west`, `north`), with one band of `pixel_t`.
 *
 * Pixels are in rows from the north, each row from the west: the pixel `column` across and `row` down is `at(column, row)`.
 * A pixel equal to `nodata` has no data.
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

  [[nodiscard]] pixel_t at(std::size_t column, std::size_t row) const { return pixels.at((row * width) + column); }

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
