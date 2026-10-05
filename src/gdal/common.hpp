#pragma once

#include <cpl_error.h>
#include <gdal.h>

#include <concepts>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>

#include "miniverse/geo/concepts/raster.hpp"

/**
 * What the files of miniverse::gdal share: GDAL's drivers registered once, its errors made into exceptions, and its name for
 * each pixel type. Not installed.
 */
namespace miniverse::gdal::detail {

/** @brief Throws `std::runtime_error` with `what` and, if GDAL reported one, its own message. */
[[noreturn]] inline void fail(const std::string& what) {
  const std::string detail = CPLGetLastErrorMsg();

  throw std::runtime_error(detail.empty() ? what : what + ": " + detail);
}

/** @brief While one lives, GDAL keeps its errors as its last message (which `fail` reports), and does not print them. */
class QuietErrors {
 public:
  QuietErrors() {
    CPLPushErrorHandler(CPLQuietErrorHandler);
    CPLErrorReset();
  }

  QuietErrors(const QuietErrors&) = delete;
  QuietErrors(QuietErrors&&) = delete;
  QuietErrors& operator=(const QuietErrors&) = delete;
  QuietErrors& operator=(QuietErrors&&) = delete;
  ~QuietErrors() { CPLPopErrorHandler(); }
};

/** @brief Registers GDAL's drivers, once in the program. */
inline void register_drivers() {
  static std::once_flag registered;
  std::call_once(registered, [] { GDALAllRegister(); });
}

/** @return The GDAL type that is `pixel_t`. */
template <geo::Pixel pixel_t>
[[nodiscard]] constexpr GDALDataType gdal_type() {
  if constexpr ( std::same_as<pixel_t, std::int8_t> ) {
    return GDT_Int8;

  } else if constexpr ( std::same_as<pixel_t, std::uint8_t> ) {
    return GDT_Byte;

  } else if constexpr ( std::same_as<pixel_t, std::int16_t> ) {
    return GDT_Int16;

  } else if constexpr ( std::same_as<pixel_t, std::uint16_t> ) {
    return GDT_UInt16;

  } else if constexpr ( std::same_as<pixel_t, std::int32_t> ) {
    return GDT_Int32;

  } else if constexpr ( std::same_as<pixel_t, std::uint32_t> ) {
    return GDT_UInt32;

  } else if constexpr ( std::same_as<pixel_t, float> ) {
    return GDT_Float32;

  } else {
    static_assert(std::same_as<pixel_t, double>, "a new pixel type needs its GDAL type here");

    return GDT_Float64;
  }
}

}  // namespace miniverse::gdal::detail
