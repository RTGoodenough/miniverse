#pragma once

#include <ogr_core.h>
#include <ogr_feature.h>
#include <ogr_geometry.h>

#include <concepts>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "miniverse/gdal/vector.hpp"
#include "miniverse/geo/concepts/wkb.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/geo/wkb.hpp"

/**
 * @file
 * OGR's geometries as miniverse's: which OGR type each geometry type is, and a feature's geometry made into one, as that
 * type or one that reads as it. Not installed.
 */
namespace miniverse::gdal::detail {

/** @return The OGR type that is `geometry_t`. */
template <geo::wkb::Geometry geometry_t>
[[nodiscard]] constexpr OGRwkbGeometryType ogr_type() {
  if constexpr ( std::same_as<geometry_t, geo::Point> ) {
    return wkbPoint;

  } else if constexpr ( std::same_as<geometry_t, geo::LineString> ) {
    return wkbLineString;

  } else if constexpr ( std::same_as<geometry_t, geo::Polygon> ) {
    return wkbPolygon;

  } else if constexpr ( std::same_as<geometry_t, geo::MultiLineString> ) {
    return wkbMultiLineString;

  } else {
    static_assert(std::same_as<geometry_t, geo::MultiPolygon>, "a new geometry type needs its OGR type here");

    return wkbMultiPolygon;
  }
}

/** @return `from`'s points as a line. */
[[nodiscard]] geo::LineString line_of(const OGRLineString& from);

/** @return `from` as a polygon: its outer ring, then its holes. */
[[nodiscard]] geo::Polygon polygon_of(const OGRPolygon& from);

/**
 * @return `geometry` as the OGR type `wanted`, or null if it is not one: itself; a single geometry as a multi one of one part;
 * or the part of a multi one that has just one. Never OGR's own `forceTo` for the last: it makes a multipolygon of several
 * parts into one polygon of all their rings, without a word.
 */
[[nodiscard]] std::unique_ptr<OGRGeometry> as_type(std::unique_ptr<OGRGeometry> geometry, OGRwkbGeometryType wanted);

/** @return `geometry`, which is of `geometry_t`'s OGR type, as a `geometry_t`. */
template <geo::wkb::Geometry geometry_t>
[[nodiscard]] geometry_t converted(const OGRGeometry& geometry) {
  if constexpr ( std::same_as<geometry_t, geo::Point> ) {
    const OGRPoint* point = geometry.toPoint();

    return {point->getX(), point->getY()};

  } else if constexpr ( std::same_as<geometry_t, geo::LineString> ) {
    return line_of(*geometry.toLineString());

  } else if constexpr ( std::same_as<geometry_t, geo::Polygon> ) {
    return polygon_of(*geometry.toPolygon());

  } else if constexpr ( std::same_as<geometry_t, geo::MultiLineString> ) {
    geo::MultiLineString lines;
    for ( const OGRLineString* part : *geometry.toMultiLineString() ) {
      lines.push_back(line_of(*part));
    }

    return lines;

  } else {
    geo::MultiPolygon polygons;
    for ( const OGRPolygon* part : *geometry.toMultiPolygon() ) {
      polygons.push_back(polygon_of(*part));
    }

    return polygons;
  }
}

/** @return `geometry`, the one of `feature`, as a `geometry_t`. */
template <geo::wkb::Geometry geometry_t>
[[nodiscard]] geometry_t of_type(std::unique_ptr<OGRGeometry> geometry, const OGRFeature& feature, const VectorSource& source) {
  const std::string                  found = geometry->getGeometryName();
  const std::unique_ptr<OGRGeometry> typed = as_type(std::move(geometry), ogr_type<geometry_t>());
  if ( ! typed ) {
    throw std::runtime_error(
        "feature " + std::to_string(feature.GetFID()) + " of '" + source.path + "' is a " + found + " that is not a " +
        std::string(geo::wkb::type_name<geometry_t>().view())
    );
  }

  return converted<geometry_t>(*typed);
}

}  // namespace miniverse::gdal::detail
