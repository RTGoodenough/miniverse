#pragma once

#include <cpl_error.h>
#include <gdal.h>

#include <mutex>
#include <stdexcept>
#include <string>

/** What the files of miniverse::gdal share: GDAL's drivers registered once, and its errors made into exceptions. Not installed. */
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

}  // namespace miniverse::gdal::detail
