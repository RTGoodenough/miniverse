#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "miniverse/geo/concepts/raster.hpp"
#include "miniverse/geo/raster.hpp"

/**
 * @file
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
