#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/geo/concepts/wkb_bytes.hpp"  // IWYU pragma: export

/**
 * The bytes of well-known binary, geometry's (geo/wkb.hpp) and raster's (geo/raster.hpp) alike: a `Reader` for either
 * byte order, as the data's own mark says, and a `Writer` that writes little-endian. For a WKB form of your own, such as
 * another PostGIS type read through `ST_AsBinary`.
 *
 * Malformed input throws `std::invalid_argument`, its message starting `wkb: `.
 */
namespace miniverse::geo::wkb {

/** @brief Throws `std::invalid_argument("wkb: " + what)`: for a reader of a WKB form to report what is wrong with it. */
[[noreturn]] inline void malformed(const std::string& what) { throw std::invalid_argument("wkb: " + what); }

/** @brief Reads WKB's numbers from a buffer, in the byte order its mark names (`byte_order`). Little-endian until then. */
class Reader {
 public:
  explicit Reader(std::span<const std::byte> bytes) : _bytes(bytes) {}

  /** @brief Reads a byte order mark, 0 (big-endian) or 1 (little-endian), and reads what follows in that order. */
  void byte_order() {
    const auto mark = std::to_integer<std::uint8_t>(byte());
    if ( mark != LITTLE_ENDIAN_MARK && mark != BIG_ENDIAN_MARK ) {
      malformed("byte order mark " + std::to_string(mark) + " is neither 0 (big-endian) nor 1 (little-endian)");
    }

    _little = mark == LITTLE_ENDIAN_MARK;
  }

  [[nodiscard]] std::byte byte() { return take(1).front(); }

  /** @return The next `number_t`, in the byte order. */
  template <Number number_t>
  [[nodiscard]] number_t number() {
    std::array<std::byte, sizeof(number_t)> raw{};
    std::ranges::copy(take(sizeof(number_t)), raw.begin());
    if ( _little != (std::endian::native == std::endian::little) ) {
      std::ranges::reverse(raw);
    }

    return std::bit_cast<number_t>(raw);
  }

  /** @return A count of items of `item_bytes` each, checked to fit in what is left, so a corrupt count can't allocate gigabytes. */
  [[nodiscard]] std::size_t count(std::size_t item_bytes) { return checked_count(number<std::uint32_t>(), item_bytes); }

  /** @return `items`, a count read some other way (a raster's width times its height), checked as `count` checks one. */
  [[nodiscard]] std::size_t checked_count(std::size_t items, std::size_t item_bytes) const {
    if ( items > remaining() / item_bytes ) {
      malformed("a count of " + std::to_string(items) + " is more than the " + std::to_string(remaining()) + " bytes left could hold");
    }

    return items;
  }

  [[nodiscard]] bool at_end() const noexcept { return _at == _bytes.size(); }

  [[nodiscard]] std::size_t remaining() const noexcept { return _bytes.size() - _at; }

  static constexpr std::uint8_t BIG_ENDIAN_MARK = 0;
  static constexpr std::uint8_t LITTLE_ENDIAN_MARK = 1;

 private:
  std::span<const std::byte> _bytes;
  std::size_t                _at = 0;
  bool                       _little = true;

  [[nodiscard]] std::span<const std::byte> take(std::size_t size) {
    if ( size > remaining() ) {
      malformed("the data ends early");
    }

    const std::span<const std::byte> taken = _bytes.subspan(_at, size);
    _at += size;

    return taken;
  }

 public:
  Reader(const Reader&) = delete;
  Reader(Reader&&) = delete;
  Reader& operator=(const Reader&) = delete;
  Reader& operator=(Reader&&) = delete;
  ~Reader() = default;
};

/** @brief Writes WKB's numbers, little-endian, starting with the byte order mark that says so. */
class Writer {
 public:
  Writer() { byte(std::byte{Reader::LITTLE_ENDIAN_MARK}); }

  void byte(std::byte value) { _bytes.push_back(value); }

  template <Number number_t>
  void number(number_t value) {
    auto raw = std::bit_cast<std::array<std::byte, sizeof(number_t)>>(value);
    if constexpr ( std::endian::native == std::endian::big ) {
      std::ranges::reverse(raw);
    }

    _bytes.insert(_bytes.end(), raw.begin(), raw.end());
  }

  /** @brief A count of parts, as WKB writes it: a 32-bit word. @throws std::invalid_argument if it doesn't fit one. */
  void count(std::size_t size) {
    if ( size > std::numeric_limits<std::uint32_t>::max() ) {
      throw std::invalid_argument("wkb: more parts than WKB can count");
    }

    number(static_cast<std::uint32_t>(size));
  }

  /** @return What was written. */
  [[nodiscard]] std::vector<std::byte> bytes() && { return std::move(_bytes); }

 private:
  std::vector<std::byte> _bytes;

 public:
  Writer(const Writer&) = delete;
  Writer(Writer&&) = delete;
  Writer& operator=(const Writer&) = delete;
  Writer& operator=(Writer&&) = delete;
  ~Writer() = default;
};

}  // namespace miniverse::geo::wkb
