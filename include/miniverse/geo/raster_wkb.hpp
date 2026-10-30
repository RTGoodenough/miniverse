#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "miniverse/geo/concepts/raster.hpp"
#include "miniverse/geo/raster.hpp"

/**
 * @file
 * Raster well-known binary, the form `ST_AsBinary` gives a raster in and the `raster` type reads,
 * for a raster of one band in the database.
 *
 * - **Written** little-endian, version 0, with SRID 4326 and the band's nodata value set.
 * - **Read** in either byte order. Only one band of `pixel_t`, with a nodata value, stored in the
 * database (not out-db), north up without skew, and with SRID 4326, is read: anything else throws
 * `std::invalid_argument`, as malformed input does.
 */
namespace miniverse::geo::wkb {

/**
 * @return `raster` as raster WKB, as above.
 * @throws std::invalid_argument if its pixels are not `width * height`, or a side is more than
 * 65535 pixels.
 */
template <Pixel pixel_t>
[[nodiscard]] std::vector<std::byte> write(const Raster<pixel_t>& raster);

/** @return The raster `bytes` hold. @throws std::invalid_argument if `bytes` are not a raster as
 * above, or have bytes left over. */
template <Pixel pixel_t>
[[nodiscard]] Raster<pixel_t> read_raster(std::span<const std::byte> bytes);

// Each is defined for each pixel type a raster can hold: std::int8_t, std::uint8_t, std::int16_t,
// std::uint16_t, std::int32_t, std::uint32_t, float and double.

}  // namespace miniverse::geo::wkb
