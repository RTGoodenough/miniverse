#pragma once

#include <bit>
#include <concepts>
#include <cstdint>

namespace miniverse::geo::wkb {

/** @brief A number WKB holds: an integer or a floating point number of 1, 2, 4 or 8 bytes. */
template <typename number_t>
concept Number = (std::integral<number_t> || std::floating_point<number_t>) && ! std::same_as<number_t, bool> &&
                 std::has_single_bit(sizeof(number_t)) && sizeof(number_t) <= sizeof(std::uint64_t);

}  // namespace miniverse::geo::wkb
