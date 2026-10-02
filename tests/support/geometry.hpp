#pragma once

// Geometry for the tests: made from coordinate lists, and compared coordinate by coordinate (Boost.Geometry's models have
// no `==`, and `boost::geometry::equals` is spatial equality, which would hide a ring written in another order).

#include <boost/algorithm/hex.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "miniverse/geo/types.hpp"

namespace miniverse::test {

using Coordinates = std::vector<std::pair<double, double>>;

template <typename points_t>
[[nodiscard]] points_t points(std::initializer_list<std::pair<double, double>> coordinates) {
  points_t result;
  for ( const auto& [lon, lat] : coordinates ) {
    result.emplace_back(lon, lat);
  }
  return result;
}

[[nodiscard]] inline geo::LineString line(std::initializer_list<std::pair<double, double>> coordinates) {
  return points<geo::LineString>(coordinates);
}

/** @return A polygon with outer ring `outer` and no holes. */
[[nodiscard]] inline geo::Polygon polygon(std::initializer_list<std::pair<double, double>> outer) {
  geo::Polygon area;
  area.outer() = points<geo::Polygon::ring_type>(outer);
  return area;
}

/** @return The closed, counter-clockwise rectangle from (`west`, `south`) to (`east`, `north`). */
[[nodiscard]] inline geo::Polygon rectangle(double west, double south, double east, double north) {
  return polygon({{west, south}, {east, south}, {east, north}, {west, north}, {west, south}});
}

/** @return `positions`' coordinates, which Catch2 can compare and print. */
template <typename points_t>
[[nodiscard]] Coordinates coordinates(const points_t& positions) {
  Coordinates result;
  result.reserve(positions.size());
  for ( const geo::Point& position : positions ) {
    result.emplace_back(position.x(), position.y());
  }
  return result;
}

/** @return Each ring's coordinates, the outer ring first. */
[[nodiscard]] inline std::vector<Coordinates> rings(const geo::Polygon& area) {
  std::vector<Coordinates> result{coordinates(area.outer())};
  for ( const auto& inner : area.inners() ) {
    result.push_back(coordinates(inner));
  }
  return result;
}

/** @return The bytes that `text`, in hex, spells. */
[[nodiscard]] inline std::vector<std::byte> bytes(std::string_view text) {
  std::string octets;
  boost::algorithm::unhex(text, std::back_inserter(octets));

  std::vector<std::byte> result;
  result.reserve(octets.size());
  for ( const char octet : octets ) {
    result.push_back(static_cast<std::byte>(octet));
  }
  return result;
}

/** @return `data` in lowercase hex, as PostgreSQL's `encode(..., 'hex')` writes it. */
[[nodiscard]] inline std::string hex(const std::vector<std::byte>& data) {
  std::vector<std::uint8_t> octets;
  octets.reserve(data.size());
  for ( const std::byte part : data ) {
    octets.push_back(std::to_integer<std::uint8_t>(part));
  }

  std::string text;
  boost::algorithm::hex_lower(octets, std::back_inserter(text));
  return text;
}

}  // namespace miniverse::test
