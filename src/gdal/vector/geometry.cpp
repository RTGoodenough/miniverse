#include "gdal/vector/geometry.hpp"

#include <ogr_core.h>
#include <ogr_geometry.h>

#include <cstddef>
#include <memory>
#include <vector>

#include "miniverse/geo/types.hpp"

namespace miniverse::gdal::detail {

namespace {

/** @brief Appends the points of `curve`, a line or a ring, to `points`. */
void append_points(const OGRSimpleCurve& curve, std::vector<geo::Point>& points)
{
  const int count = curve.getNumPoints();

  points.reserve(points.size() + static_cast<std::size_t>(count));
  for ( int i = 0; i < count; ++i ) {
    points.emplace_back(curve.getX(i), curve.getY(i));
  }
}

}  // namespace

geo::LineString line_of(const OGRLineString& from)
{
  geo::LineString line;
  append_points(from, line);

  return line;
}

geo::Polygon polygon_of(const OGRPolygon& from)
{
  geo::Polygon polygon;

  if ( const OGRLinearRing* outer = from.getExteriorRing(); outer != nullptr ) {
    append_points(*outer, polygon.outer());
  }

  polygon.inners().resize(static_cast<std::size_t>(from.getNumInteriorRings()));
  for ( int i = 0; i < from.getNumInteriorRings(); ++i ) {
    append_points(*from.getInteriorRing(i), polygon.inners().at(static_cast<std::size_t>(i)));
  }

  return polygon;
}

std::unique_ptr<OGRGeometry> as_type(std::unique_ptr<OGRGeometry> geometry,
                                     OGRwkbGeometryType           wanted)
{
  const OGRwkbGeometryType type = wkbFlatten(geometry->getGeometryType());
  if ( type == wanted ) {
    return geometry;
  }

  if ( (type == wkbPolygon && wanted == wkbMultiPolygon) ||
       (type == wkbLineString && wanted == wkbMultiLineString) )
  {
    return std::unique_ptr<OGRGeometry>(
        OGRGeometryFactory::forceTo(geometry.release(), wanted));  // single to multi: forceTo wraps
                                                                   // it in a collection of one
  }

  if ( (type == wkbMultiPolygon && wanted == wkbPolygon) ||
       (type == wkbMultiLineString && wanted == wkbLineString) ||
       (type == wkbMultiPoint && wanted == wkbPoint) )
  {
    if ( const OGRGeometryCollection* parts = geometry->toGeometryCollection();
         parts->getNumGeometries() == 1 )
    {
      return std::unique_ptr<OGRGeometry>(parts->getGeometryRef(0)->clone());
    }
  }

  return nullptr;
}

}  // namespace miniverse::gdal::detail
