#include "miniverse/gdal/vector.hpp"

#include <cpl_error.h>
#include <gdal_priv.h>
#include <ogr_api.h>
#include <ogr_core.h>
#include <ogr_feature.h>
#include <ogr_geometry.h>
#include <ogrsf_frmts.h>

#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "gdal/common.hpp"
#include "gdal/vector/fields.hpp"
#include "gdal/vector/filter.hpp"
#include "gdal/vector/geometry.hpp"
#include "gdal/vector/source.hpp"
#include "miniverse/geo/concepts/wkb.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/geo/wkb.hpp"
#include "miniverse/model/feature.hpp"

namespace miniverse::gdal {

namespace {

using detail::fail;
using detail::id_field_of;
using detail::id_of;
using detail::in_wgs84;
using detail::intersects;
using detail::layer_of;
using detail::narrowed_to;
using detail::need_geos;
using detail::of_type;
using detail::ogr_type;
using detail::opened;
using detail::Prepared;
using detail::QuietErrors;
using detail::tags_of;
using detail::to_wgs84_from;

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
    // Drop errors earlier GDAL calls left: the loop's end is told from the last error.
    CPLErrorReset();
    const OGRFeatureUniquePtr feature{layer.GetNextFeature()};
    if ( ! feature ) {
      break;
    }

    std::unique_ptr<OGRGeometry> stolen(feature->StealGeometry());
    if ( ! stolen || stolen->IsEmpty() ) {
      if ( location == nullptr ) {
        ++read.without_geometry;  // with a location, a feature that is nowhere is merely not in it
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

    // A layer that declares its geometry type must declare the type asked for, or one that reads as it (a single type as the
    // multi one, a multi one as the single one, a curve as lines). A layer that declares none may hold any.
    const OGRwkbGeometryType found = wkbFlatten(OGR_GT_GetLinear(layer.GetGeomType()));
    const OGRwkbGeometryType wanted = ogr_type<geometry_t>();
    if ( found != wkbUnknown && found != wanted && OGR_GT_GetCollection(found) != wanted && OGR_GT_GetCollection(wanted) != found ) {
      return {
          "the geometries of '" + source.path + "' are " + OGRGeometryTypeToName(found) + ", not " +
          std::string(geo::wkb::type_name<geometry_t>().view())
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

  std::ignore =
      read_features<geometry_t>(source, std::numeric_limits<std::size_t>::max(), [&all](Features<geometry_t> chunk) { all = std::move(chunk); });

  return all;
}

template VectorRead read_features<geo::Point>(const VectorSource&, std::size_t, const std::function<void(Features<geo::Point>)>&);
template VectorRead read_features<geo::LineString>(const VectorSource&, std::size_t, const std::function<void(Features<geo::LineString>)>&);
template VectorRead read_features<geo::Polygon>(const VectorSource&, std::size_t, const std::function<void(Features<geo::Polygon>)>&);
template VectorRead read_features<geo::MultiLineString>(const VectorSource&, std::size_t, const std::function<void(Features<geo::MultiLineString>)>&);
template VectorRead read_features<geo::MultiPolygon>(const VectorSource&, std::size_t, const std::function<void(Features<geo::MultiPolygon>)>&);

template VectorRead read_features_in<geo::Point>(
    const VectorSource&, const geo::Polygon&, std::size_t, const std::function<bool(Features<geo::Point>)>&
);
template VectorRead read_features_in<geo::LineString>(
    const VectorSource&, const geo::Polygon&, std::size_t, const std::function<bool(Features<geo::LineString>)>&
);
template VectorRead read_features_in<geo::Polygon>(
    const VectorSource&, const geo::Polygon&, std::size_t, const std::function<bool(Features<geo::Polygon>)>&
);
template VectorRead read_features_in<geo::MultiLineString>(
    const VectorSource&, const geo::Polygon&, std::size_t, const std::function<bool(Features<geo::MultiLineString>)>&
);
template VectorRead read_features_in<geo::MultiPolygon>(
    const VectorSource&, const geo::Polygon&, std::size_t, const std::function<bool(Features<geo::MultiPolygon>)>&
);

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
