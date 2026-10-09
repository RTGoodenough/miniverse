#include "miniverse/gdal/vector.hpp"

#include <cpl_error.h>
#include <cpl_json.h>
#include <cpl_port.h>
#include <gdal.h>
#include <gdal_priv.h>
#include <ogr_api.h>
#include <ogr_core.h>
#include <ogr_feature.h>
#include <ogr_geometry.h>
#include <ogr_spatialref.h>
#include <ogr_srs_api.h>
#include <ogrsf_frmts.h>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "common.hpp"
#include "miniverse/geo/concepts/wkb.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/geo/wkb.hpp"
#include "miniverse/layer/feature_layer.hpp"
#include "schemacht/json/json.hpp"

namespace miniverse::gdal {

namespace {

constexpr int NO_FIELD = -1;  // OGR's own feature id is the id, not a field

using detail::fail;
using detail::QuietErrors;
using detail::register_drivers;

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

/** @brief Appends the points of `curve`, a line or a ring, to `points`. */
void append_points(const OGRSimpleCurve& curve, std::vector<geo::Point>& points) {
  const int count = curve.getNumPoints();

  points.reserve(points.size() + static_cast<std::size_t>(count));
  for ( int i = 0; i < count; ++i ) {
    points.emplace_back(curve.getX(i), curve.getY(i));
  }
}

[[nodiscard]] geo::LineString line_of(const OGRLineString& from) {
  geo::LineString line;
  append_points(from, line);

  return line;
}

[[nodiscard]] geo::Polygon polygon_of(const OGRPolygon& from) {
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

/** @return The layer of `dataset` that `source` asks for: the one it names, or the only one there is. */
[[nodiscard]] OGRLayer& layer_of(GDALDataset& dataset, const VectorSource& source) {
  if ( ! source.layer.empty() ) {
    OGRLayer* named = dataset.GetLayerByName(source.layer.c_str());
    if ( named == nullptr ) {
      throw std::runtime_error("'" + source.path + "' has no layer '" + source.layer + "'");
    }

    return *named;
  }

  if ( dataset.GetLayerCount() != 1 ) {
    throw std::runtime_error("'" + source.path + "' has " + std::to_string(dataset.GetLayerCount()) + " layers: name the one to read");
  }

  return *dataset.GetLayer(0);
}

/** @return The index of `source`'s id field in `layer`, or `NO_FIELD` if the id is OGR's own. */
[[nodiscard]] int id_field_of(OGRLayer& layer, const VectorSource& source) {
  if ( source.id_field.empty() ) {
    return NO_FIELD;
  }

  const OGRFeatureDefn* fields = layer.GetLayerDefn();
  const int             index = fields->GetFieldIndex(source.id_field.c_str());
  if ( index < 0 ) {
    throw std::runtime_error("'" + source.path + "' has no field '" + source.id_field + "' to take ids from");
  }

  if ( const OGRFieldType type = fields->GetFieldDefn(index)->GetType(); type != OFTInteger && type != OFTInteger64 ) {
    throw std::runtime_error("the field '" + source.id_field + "' of '" + source.path + "' is not a whole number, so it can't be the id");
  }

  return index;
}

/** @return The transformation from `layer`'s coordinate system to WGS 84 longitude and latitude. */
[[nodiscard]] std::unique_ptr<OGRCoordinateTransformation> to_wgs84_from(OGRLayer& layer, const VectorSource& source) {
  // The layer's own description says which axis its data has first; only the target's order is ours to choose.
  const OGRSpatialReference* from = layer.GetSpatialRef();
  if ( from == nullptr ) {
    throw std::runtime_error("'" + source.path + "' names no coordinate system, so its coordinates could be in any");
  }

  OGRSpatialReference wgs84;
  if ( wgs84.importFromEPSG(geo::WGS84_SRID) != OGRERR_NONE ) {
    fail("GDAL has no definition of WGS 84 (EPSG:4326): is PROJ's data installed?");
  }
  wgs84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);  // longitude first

  std::unique_ptr<OGRCoordinateTransformation> transformation(OGRCreateCoordinateTransformation(from, &wgs84));
  if ( ! transformation ) {
    fail("the coordinates of '" + source.path + "' can't be transformed to WGS 84");
  }

  return transformation;
}

/**
 * @return `value` as a JSON value of its own, to add to an object under any name. GDAL before 3.8 makes a value only as a
 * member of an object, under a name it splits at `/`; so it is made under a plain name and taken out again.
 */
template <typename value_t>
[[nodiscard]] CPLJSONObject json_value(value_t value) {
  CPLJSONObject holder;
  holder.Add("value", value);

  return holder.GetObj("value");
}

/** @return The fields of `feature`, but for `id_field`, as one JSON object: see the top of vector.hpp. */
[[nodiscard]] schemacht::json::Json tags_of(const OGRFeature& feature, int id_field) {
  const OGRFeatureDefn* fields = feature.GetDefnRef();
  CPLJSONObject         tags;

  for ( int i = 0; i < fields->GetFieldCount(); ++i ) {
    if ( i == id_field || ! feature.IsFieldSetAndNotNull(i) ) {
      continue;
    }

    // Added by its whole name: `Add` would take a `/` in it for a path, and nest or drop the value.
    const OGRFieldDefn* field = fields->GetFieldDefn(i);
    const OGRFieldType  type = field->GetType();
    if ( type == OFTInteger && field->GetSubType() == OFSTBoolean ) {
      tags.AddNoSplitName(field->GetNameRef(), json_value(feature.GetFieldAsInteger(i) != 0));

    } else if ( type == OFTInteger || type == OFTInteger64 ) {
      tags.AddNoSplitName(field->GetNameRef(), json_value(static_cast<GInt64>(feature.GetFieldAsInteger64(i))));

    } else if ( type == OFTReal && std::isfinite(feature.GetFieldAsDouble(i)) ) {
      tags.AddNoSplitName(field->GetNameRef(), json_value(feature.GetFieldAsDouble(i)));

    } else {  // text, dates, lists; and a real that is not a number, which JSON has no way to write
      tags.AddNoSplitName(field->GetNameRef(), json_value(feature.GetFieldAsString(i)));
    }
  }

  return schemacht::json::Json(tags.Format(CPLJSONObject::PrettyFormat::Plain));
}

/** @return The id of `feature`: its `id_field`, or OGR's own. */
[[nodiscard]] std::int64_t id_of(const OGRFeature& feature, int id_field, const VectorSource& source) {
  if ( id_field == NO_FIELD ) {
    if ( feature.GetFID() == OGRNullFID ) {
      throw std::runtime_error("a feature of '" + source.path + "' has no id of its own: name the field to take ids from (id_field)");
    }

    return feature.GetFID();
  }

  if ( ! feature.IsFieldSetAndNotNull(id_field) ) {
    throw std::runtime_error("feature " + std::to_string(feature.GetFID()) + " of '" + source.path + "' has no '" + source.id_field + "', its id");
  }

  return feature.GetFieldAsInteger64(id_field);
}

/**
 * @return `geometry` as the OGR type `wanted`, or null if it is not one: itself; a single geometry as a multi one of one part;
 * or the part of a multi one that has just one. Never OGR's own `forceTo` for the last: it makes a multipolygon of several
 * parts into one polygon of all their rings, without a word.
 */
[[nodiscard]] std::unique_ptr<OGRGeometry> as_type(std::unique_ptr<OGRGeometry> geometry, OGRwkbGeometryType wanted) {
  if ( geometry->hasCurveGeometry() ) {
    geometry.reset(geometry->getLinearGeometry());  // arcs as lines
  }

  const OGRwkbGeometryType type = wkbFlatten(geometry->getGeometryType());
  if ( type == wanted ) {
    return geometry;
  }

  if ( (type == wkbPolygon && wanted == wkbMultiPolygon) || (type == wkbLineString && wanted == wkbMultiLineString) ) {
    return std::unique_ptr<OGRGeometry>(OGRGeometryFactory::forceTo(geometry.release(), wanted));  // only ever wraps it, here
  }

  if ( (type == wkbMultiPolygon && wanted == wkbPolygon) || (type == wkbMultiLineString && wanted == wkbLineString) ||
       (type == wkbMultiPoint && wanted == wkbPoint) ) {
    if ( const OGRGeometryCollection* parts = geometry->toGeometryCollection(); parts->getNumGeometries() == 1 ) {
      return std::unique_ptr<OGRGeometry>(parts->getGeometryRef(0)->clone());
    }
  }

  return nullptr;
}

/**
 * @return The geometry of `feature` in WGS 84, flat and with its arcs as lines, of whatever type it is; `stolen` is the
 * feature's own, taken from it.
 */
[[nodiscard]] std::unique_ptr<OGRGeometry> in_wgs84(
    std::unique_ptr<OGRGeometry> stolen, const OGRFeature& feature, OGRCoordinateTransformation& to_wgs84, const VectorSource& source
) {
  stolen->flattenTo2D();
  if ( stolen->hasCurveGeometry() ) {
    stolen.reset(stolen->getLinearGeometry());
  }

  if ( stolen->transform(&to_wgs84) != OGRERR_NONE ) {
    fail("feature " + std::to_string(feature.GetFID()) + " of '" + source.path + "' can't be transformed to WGS 84");
  }

  return stolen;
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

/** @return `source`'s file, opened for reading its features. */
[[nodiscard]] GDALDatasetUniquePtr opened(const VectorSource& source) {
  register_drivers();

  GDALDatasetUniquePtr dataset(GDALDataset::Open(source.path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY | GDAL_OF_VERBOSE_ERROR));
  if ( ! dataset ) {
    fail("'" + source.path + "' can't be opened as a vector file");
  }

  return dataset;
}

/** @return `polygon` as OGR has it, its rings closed. */
[[nodiscard]] OGRPolygon ogr_polygon_of(const geo::Polygon& polygon) {
  const auto ring_of = [](const auto& points) {
    auto ring = std::make_unique<OGRLinearRing>();
    for ( const geo::Point& point : points ) {
      ring->addPoint(point.x(), point.y());
    }
    ring->closeRings();

    return ring;
  };

  OGRPolygon made;
  made.addRingDirectly(ring_of(polygon.outer()).release());
  for ( const auto& hole : polygon.inners() ) {
    made.addRingDirectly(ring_of(hole).release());
  }

  return made;
}

/**
 * @brief Has `layer` read only the features whose boxes meet the box of `location`, a polygon in WGS 84, as that box lies in
 * the layer's own coordinate system, and a little more: what its spatial index answers. The box's straight edges are curves
 * there, found by points along them, so the box is widened by a hundredth each way to hold what bulges between the points:
 * the test of each feature decides, and this only spares it features far off. If the box can't be placed there (the
 * location is partly outside what the system covers), every feature is read.
 */
void narrow_to(OGRLayer& layer, const OGRPolygon& location, OGRCoordinateTransformation& to_wgs84) {
  constexpr int    POINTS_ALONG_AN_EDGE = 21;
  constexpr double WIDER_BY = 0.01;        // of the box's own size, each way
  constexpr double AND_AT_LEAST = 1e-7;    // of how far from the origin it lies: a location of one point has no size

  OGREnvelope box;
  location.getEnvelope(&box);

  double                                             west = 0;
  double                                             south = 0;
  double                                             east = 0;
  double                                             north = 0;
  const std::unique_ptr<OGRCoordinateTransformation> from_wgs84(to_wgs84.GetInverse());
  if ( from_wgs84 && from_wgs84->TransformBounds(box.MinX, box.MinY, box.MaxX, box.MaxY, &west, &south, &east, &north, POINTS_ALONG_AN_EDGE) != 0 &&
       std::isfinite(west) && std::isfinite(south) && std::isfinite(east) && std::isfinite(north) && west <= east && south <= north ) {
    const double far = std::max({std::abs(west), std::abs(east), std::abs(south), std::abs(north), 1.0}) * AND_AT_LEAST;
    const double across = ((east - west) * WIDER_BY) + far;
    const double down = ((north - south) * WIDER_BY) + far;
    layer.SetSpatialFilterRect(west - across, south - down, east + across, north + down);
  }

  CPLErrorReset();  // a box that could not be placed is not the read's error
}

/** @brief Throws unless GDAL can test whether two geometries intersect: without GEOS it would compare their boxes, and say nothing. */
void need_geos() {
  if ( ! OGRGeometryFactory::haveGEOS() ) {
    throw std::runtime_error("this GDAL was built without GEOS, so it can't tell which features intersect a location");
  }
}

/** @brief A location as GEOS has it, made ready once for the test of every feature. */
using Prepared = std::unique_ptr<std::remove_pointer_t<OGRPreparedGeometryH>, void (*)(OGRPreparedGeometryH)>;

/** @return `location`, a polygon in WGS 84, ready to test features against; `layer` reads only the features near it from here on. */
[[nodiscard]] Prepared narrowed_to(OGRLayer& layer, const geo::Polygon& location, OGRCoordinateTransformation& to_wgs84, const VectorSource& source) {
  need_geos();

  OGRPolygon polygon = ogr_polygon_of(location);
  narrow_to(layer, polygon, to_wgs84);

  Prepared within(OGRCreatePreparedGeometry(OGRGeometry::ToHandle(&polygon)), OGRDestroyPreparedGeometry);
  if ( ! within ) {
    fail("the location can't be tested against the features of '" + source.path + "'");
  }

  return within;
}

/** @return Whether `geometry`, the one of `feature` in WGS 84, intersects the location `within`. */
[[nodiscard]] bool intersects(std::remove_pointer_t<OGRPreparedGeometryH>& within, OGRGeometry& geometry, const OGRFeature& feature, const VectorSource& source) {
  const bool found = OGRPreparedGeometryIntersects(&within, OGRGeometry::ToHandle(&geometry)) != 0;
  if ( CPLGetLastErrorType() == CE_Failure ) {
    fail("feature " + std::to_string(feature.GetFID()) + " of '" + source.path + "' can't be tested against the location");
  }

  return found;
}

/**
 * @brief Reads `source`'s features, those that intersect `location` if there is one, and hands them to `on_chunk`,
 * `chunk_features` at a time, until it returns `false`: what `read_features` and `read_features_in` do.
 */
template <geo::wkb::Geometry geometry_t>
VectorRead read_matching(
    const VectorSource& source, const geo::Polygon* location, std::size_t chunk_features, const std::function<bool(Features<geometry_t>)>& on_chunk
) {
  if ( chunk_features == 0 ) {
    throw std::invalid_argument("a chunk of features holds at least one");
  }

  const QuietErrors          quiet;
  const GDALDatasetUniquePtr dataset = opened(source);

  OGRLayer&  layer = layer_of(*dataset, source);
  const int  id_field = id_field_of(layer, source);
  const auto to_wgs84 = to_wgs84_from(layer, source);

  if ( location != nullptr && location->outer().empty() ) {
    return {};  // nothing is in a location of no points
  }

  const Prepared within = location != nullptr ? narrowed_to(layer, *location, *to_wgs84, source) : Prepared(nullptr, OGRDestroyPreparedGeometry);

  VectorRead           read;
  Features<geometry_t> chunk;

  layer.ResetReading();
  for ( ;; ) {
    // From here, GDAL's last error is this feature's own: not one that opening, an earlier feature or `on_chunk` left.
    CPLErrorReset();
    const OGRFeatureUniquePtr feature{layer.GetNextFeature()};
    if ( ! feature ) {
      break;
    }

    std::unique_ptr<OGRGeometry> stolen(feature->StealGeometry());
    if ( ! stolen || stolen->IsEmpty() ) {
      if ( location == nullptr ) {
        ++read.without_geometry;  // with a location, one that is nowhere is merely not in it
      }

      continue;
    }

    std::unique_ptr<OGRGeometry> geometry = in_wgs84(std::move(stolen), *feature, *to_wgs84, source);
    if ( within && ! intersects(*within, *geometry, *feature, source) ) {
      continue;  // whatever type it is: only a feature in the location must be a `geometry_t`
    }

    chunk.push_back(
        Feature<geometry_t>{
            .id = id_of(*feature, id_field, source),
            .geometry = of_type<geometry_t>(std::move(geometry), *feature, source),
            .tags = tags_of(*feature, id_field),
        }
    );
    ++read.features;

    if ( chunk.size() == chunk_features && ! on_chunk(std::exchange(chunk, {})) ) {
      return read;
    }
  }

  if ( CPLGetLastErrorType() == CE_Failure ) {
    fail("'" + source.path + "' could not be read to its end");  // the loop ends on an error as on the last feature
  }

  if ( ! chunk.empty() ) {
    std::ignore = on_chunk(std::move(chunk));
  }

  return read;
}

}  // namespace

template <geo::wkb::Geometry geometry_t>
VectorRead read_features(const VectorSource& source, std::size_t chunk_features, const std::function<void(Features<geometry_t>)>& on_chunk) {
  return read_matching<geometry_t>(source, nullptr, chunk_features, [&on_chunk](Features<geometry_t> chunk) {
    on_chunk(std::move(chunk));

    return true;
  });
}

template <geo::wkb::Geometry geometry_t>
VectorRead read_features_in(
    const VectorSource& source, const geo::Polygon& location, std::size_t chunk_features, const std::function<bool(Features<geometry_t>)>& on_chunk
) {
  return read_matching<geometry_t>(source, &location, chunk_features, on_chunk);
}

template <geo::wkb::Geometry geometry_t>
std::vector<std::string> problems_of(const VectorSource& source) {
  try {
    const QuietErrors          quiet;
    const GDALDatasetUniquePtr dataset = opened(source);

    OGRLayer& layer = layer_of(*dataset, source);
    std::ignore = id_field_of(layer, source);
    std::ignore = to_wgs84_from(layer, source);
    need_geos();

    // A layer that says what its geometries are: of the type asked for, or one that reads as it (a single one as a multi
    // one, a multi one of one part as its part, a curve as lines). One that does not say may hold any.
    const OGRwkbGeometryType found = wkbFlatten(OGR_GT_GetLinear(layer.GetGeomType()));
    const OGRwkbGeometryType wanted = ogr_type<geometry_t>();
    if ( found != wkbUnknown && found != wanted && OGR_GT_GetCollection(found) != wanted && OGR_GT_GetCollection(wanted) != found ) {
      return {
          "the geometries of '" + source.path + "' are " + OGRGeometryTypeToName(found) + ", not " + std::string(geo::wkb::type_name<geometry_t>().view())
      };
    }

    return {};
  } catch ( const std::exception& unreadable ) {
    return {unreadable.what()};
  }
}

template <geo::wkb::Geometry geometry_t>
Features<geometry_t> read_features(const VectorSource& source) {
  Features<geometry_t> all;

  std::ignore = read_features<geometry_t>(source, std::numeric_limits<std::size_t>::max(), [&all](Features<geometry_t> chunk) { all = std::move(chunk); });

  return all;
}

template VectorRead read_features<geo::Point>(const VectorSource&, std::size_t, const std::function<void(Features<geo::Point>)>&);
template VectorRead read_features<geo::LineString>(const VectorSource&, std::size_t, const std::function<void(Features<geo::LineString>)>&);
template VectorRead read_features<geo::Polygon>(const VectorSource&, std::size_t, const std::function<void(Features<geo::Polygon>)>&);
template VectorRead read_features<geo::MultiLineString>(const VectorSource&, std::size_t, const std::function<void(Features<geo::MultiLineString>)>&);
template VectorRead read_features<geo::MultiPolygon>(const VectorSource&, std::size_t, const std::function<void(Features<geo::MultiPolygon>)>&);

template VectorRead read_features_in<geo::Point>(const VectorSource&, const geo::Polygon&, std::size_t, const std::function<bool(Features<geo::Point>)>&);
template VectorRead
read_features_in<geo::LineString>(const VectorSource&, const geo::Polygon&, std::size_t, const std::function<bool(Features<geo::LineString>)>&);
template VectorRead read_features_in<geo::Polygon>(const VectorSource&, const geo::Polygon&, std::size_t, const std::function<bool(Features<geo::Polygon>)>&);
template VectorRead
read_features_in<geo::MultiLineString>(const VectorSource&, const geo::Polygon&, std::size_t, const std::function<bool(Features<geo::MultiLineString>)>&);
template VectorRead
read_features_in<geo::MultiPolygon>(const VectorSource&, const geo::Polygon&, std::size_t, const std::function<bool(Features<geo::MultiPolygon>)>&);

template std::vector<std::string> problems_of<geo::Point>(const VectorSource&);
template std::vector<std::string> problems_of<geo::LineString>(const VectorSource&);
template std::vector<std::string> problems_of<geo::Polygon>(const VectorSource&);
template std::vector<std::string> problems_of<geo::MultiLineString>(const VectorSource&);
template std::vector<std::string> problems_of<geo::MultiPolygon>(const VectorSource&);

template Features<geo::Point>           read_features<geo::Point>(const VectorSource&);
template Features<geo::LineString>      read_features<geo::LineString>(const VectorSource&);
template Features<geo::Polygon>         read_features<geo::Polygon>(const VectorSource&);
template Features<geo::MultiLineString> read_features<geo::MultiLineString>(const VectorSource&);
template Features<geo::MultiPolygon>    read_features<geo::MultiPolygon>(const VectorSource&);

}  // namespace miniverse::gdal
