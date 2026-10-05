#pragma once

#include <concepts>
#include <cstdint>
#include <string_view>

namespace miniverse::geo {

/**
 * @brief What PostGIS calls the pixel type `pixel_t`: its number in raster WKB (`CODE`) and its name (`NAME`, as
 * `ST_BandPixelType` gives it). Specialized for each pixel type a raster can hold; undefined for any other type.
 */
template <typename pixel_t>
struct PixelType;

template <>
struct PixelType<std::int8_t> {
  static constexpr std::uint8_t     CODE = 3;
  static constexpr std::string_view NAME = "8BSI";
};

template <>
struct PixelType<std::uint8_t> {
  static constexpr std::uint8_t     CODE = 4;
  static constexpr std::string_view NAME = "8BUI";
};

template <>
struct PixelType<std::int16_t> {
  static constexpr std::uint8_t     CODE = 5;
  static constexpr std::string_view NAME = "16BSI";
};

template <>
struct PixelType<std::uint16_t> {
  static constexpr std::uint8_t     CODE = 6;
  static constexpr std::string_view NAME = "16BUI";
};

template <>
struct PixelType<std::int32_t> {
  static constexpr std::uint8_t     CODE = 7;
  static constexpr std::string_view NAME = "32BSI";
};

template <>
struct PixelType<std::uint32_t> {
  static constexpr std::uint8_t     CODE = 8;
  static constexpr std::string_view NAME = "32BUI";
};

template <>
struct PixelType<float> {
  static constexpr std::uint8_t     CODE = 10;
  static constexpr std::string_view NAME = "32BF";
};

template <>
struct PixelType<double> {
  static constexpr std::uint8_t     CODE = 11;
  static constexpr std::string_view NAME = "64BF";
};

/** @brief A type a raster's pixels can be: one PostGIS has a pixel type for (`PixelType`). */
template <typename pixel_t>
concept Pixel = requires {
  { PixelType<pixel_t>::CODE } -> std::convertible_to<std::uint8_t>;
  { PixelType<pixel_t>::NAME } -> std::convertible_to<std::string_view>;
};

}  // namespace miniverse::geo
