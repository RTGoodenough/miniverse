#pragma once

// What the examples share: the connection string each is run with, and a box as the polygon a load takes.

#include <cstddef>
#include <span>
#include <string>

#include "miniverse/geo/types.hpp"

namespace example {

/**
 * @return The libpq connection string the example was run with, its one argument ("host=localhost dbname=gis user=gis");
 * empty, if it was run without, which uses libpq's defaults and PG* variables.
 */
[[nodiscard]] inline std::string conninfo(int argc, char** argv) {
  const std::span<char*> args(argv, static_cast<std::size_t>(argc));

  return args.size() > 1 ? args[1] : "";
}

/** @return The area between two longitudes and two latitudes, as the polygon a load takes: closed, and counter-clockwise. */
[[nodiscard]] inline miniverse::geo::Polygon box(double west, double south, double east, double north) {
  miniverse::geo::Polygon area;
  area.outer() = {{west, south}, {east, south}, {east, north}, {west, north}, {west, south}};

  return area;
}

}  // namespace example
