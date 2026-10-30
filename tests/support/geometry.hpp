#pragma once

// Geometries written as WKT, for the locations and shapes a test uses.

#include <boost/geometry/io/wkt/read.hpp>

#include <string>

#include "miniverse/geo/types.hpp"

namespace test {

[[nodiscard]] inline miniverse::geo::Polygon polygon(const std::string& wkt)
{
  return boost::geometry::from_wkt<miniverse::geo::Polygon>(wkt);
}

}  // namespace test
