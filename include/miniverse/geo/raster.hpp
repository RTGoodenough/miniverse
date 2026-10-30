#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "miniverse/geo/concepts/raster.hpp"  // IWYU pragma: export
#include "miniverse/geo/snap.hpp"
#include "miniverse/geo/types.hpp"

/**
 * @file
 * A raster in WGS 84: a grid of pixels in longitude and latitude, north up, as PostGIS stores one (`raster`) and a load of a
 * raster layer gives one, with the pixel a position lies in.
 */
namespace miniverse::geo {

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
 * A position has its pixel by `pixel_at` and its value by `value_at`: the value of the pixel it lies in, not one worked out
 * between pixels. A pixel's size on the ground is not the same everywhere: as tall at every latitude, and narrower by the
 * latitude's cosine.
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
   * @return The pixel `position` lies in; nothing if it lies outside the raster, or is no number. A position on the line between
   * two pixels is in the eastern or southern one. The raster's edges belong to its outer pixels, to `EDGE_TOLERANCE` of a
   * pixel, so the corner of the area a raster was loaded for has a pixel.
   */
  [[nodiscard]] std::optional<PixelPosition> pixel_at(const Point& position) const {
    const double across = (position.x() - west) / pixel_width;
    const double down = (north - position.y()) / pixel_height;

    const bool inside = across >= -EDGE_TOLERANCE && across <= static_cast<double>(width) + EDGE_TOLERANCE && down >= -EDGE_TOLERANCE &&
                        down <= static_cast<double>(height) + EDGE_TOLERANCE;
    if ( width == 0 || height == 0 || ! inside ) {
      return std::nullopt;
    }

    const auto within = [](double along, std::size_t count) { return std::min(static_cast<std::size_t>(std::max(along, 0.0)), count - 1); };

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
