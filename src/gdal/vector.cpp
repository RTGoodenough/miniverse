#include "miniverse/gdal/vector.hpp"

#include <cpl_error.h>
#include <cpl_json.h>
#include <gdal.h>
#include <gdal_priv.h>
#include <ogr_core.h>
#include <ogr_feature.h>
#include <ogr_geometry.h>
#include <ogr_spatialref.h>
#include <ogr_srs_api.h>
#include <ogrsf_frmts.h>

#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
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
      tags.AddNoSplitName(field->GetNameRef(), CPLJSONObject(feature.GetFieldAsInteger(i) != 0));

    } else if ( type == OFTInteger || type == OFTInteger64 ) {
      tags.AddNoSplitName(field->GetNameRef(), CPLJSONObject(static_cast<std::int64_t>(feature.GetFieldAsInteger64(i))));

    } else if ( type == OFTReal && std::isfinite(feature.GetFieldAsDouble(i)) ) {
      tags.AddNoSplitName(field->GetNameRef(), CPLJSONObject(feature.GetFieldAsDouble(i)));

    } else {  // text, dates, lists; and a real that is not a number, which JSON has no way to write
      tags.AddNoSplitName(field->GetNameRef(), CPLJSONObject(feature.GetFieldAsString(i)));
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

/** @return The geometry of `feature` as a `geometry_t` in WGS 84; `stolen` is the feature's own, taken from it. */
template <geo::wkb::Geometry geometry_t>
[[nodiscard]] geometry_t geometry_of(
    std::unique_ptr<OGRGeometry> stolen, const OGRFeature& feature, OGRCoordinateTransformation& to_wgs84, const VectorSource& source
) {
  stolen->flattenTo2D();

  const std::string                  found = stolen->getGeometryName();
  const std::unique_ptr<OGRGeometry> geometry = as_type(std::move(stolen), ogr_type<geometry_t>());
  if ( ! geometry ) {
    throw std::runtime_error(
        "feature " + std::to_string(feature.GetFID()) + " of '" + source.path + "' is a " + found + " that is not a " +
        std::string(geo::wkb::type_name<geometry_t>().view())
    );
  }

  if ( geometry->transform(&to_wgs84) != OGRERR_NONE ) {
    fail("feature " + std::to_string(feature.GetFID()) + " of '" + source.path + "' can't be transformed to WGS 84");
  }

  return converted<geometry_t>(*geometry);
}

}  // namespace

template <geo::wkb::Geometry geometry_t>
VectorRead read_features(const VectorSource& source, std::size_t chunk_features, const std::function<void(Features<geometry_t>)>& on_chunk) {
  if ( chunk_features == 0 ) {
    throw std::invalid_argument("a chunk of features holds at least one");
  }

  register_drivers();
  const QuietErrors quiet;

  const GDALDatasetUniquePtr dataset(GDALDataset::Open(source.path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY | GDAL_OF_VERBOSE_ERROR));
  if ( ! dataset ) {
    fail("'" + source.path + "' can't be opened as a vector file");
  }

  OGRLayer&  layer = layer_of(*dataset, source);
  const int  id_field = id_field_of(layer, source);
  const auto to_wgs84 = to_wgs84_from(layer, source);

  VectorRead           read;
  Features<geometry_t> chunk;

  // From here on, GDAL's last error is this read's own: opening and `on_chunk` may each have left one that is not.
  layer.ResetReading();
  CPLErrorReset();
  while ( const OGRFeatureUniquePtr feature{layer.GetNextFeature()} ) {
    std::unique_ptr<OGRGeometry> geometry(feature->StealGeometry());
    if ( ! geometry || geometry->IsEmpty() ) {
      ++read.without_geometry;
      continue;
    }

    chunk.push_back(
        Feature<geometry_t>{
            .id = id_of(*feature, id_field, source),
            .geometry = geometry_of<geometry_t>(std::move(geometry), *feature, *to_wgs84, source),
            .tags = tags_of(*feature, id_field),
        }
    );
    ++read.features;

    if ( chunk.size() == chunk_features ) {
      on_chunk(std::exchange(chunk, {}));
      CPLErrorReset();
    }
  }

  if ( CPLGetLastErrorType() == CE_Failure ) {
    fail("'" + source.path + "' could not be read to its end");  // the loop ends on an error as on the last feature
  }

  if ( ! chunk.empty() ) {
    on_chunk(std::move(chunk));
  }

  return read;
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

template Features<geo::Point>           read_features<geo::Point>(const VectorSource&);
template Features<geo::LineString>      read_features<geo::LineString>(const VectorSource&);
template Features<geo::Polygon>         read_features<geo::Polygon>(const VectorSource&);
template Features<geo::MultiLineString> read_features<geo::MultiLineString>(const VectorSource&);
template Features<geo::MultiPolygon>    read_features<geo::MultiPolygon>(const VectorSource&);

}  // namespace miniverse::gdal
