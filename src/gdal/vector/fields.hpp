#pragma once

#include <ogr_feature.h>
#include <ogrsf_frmts.h>

#include <cstdint>

#include "miniverse/gdal/vector.hpp"
#include "schemacht/json/json.hpp"

/**
 * @file
 * A feature's fields as a feature's id and tags: which field is the id (or OGR's own id is), the id, and every other field as
 * one JSON object. Not installed.
 */
namespace miniverse::gdal::detail {

/** @brief The id field of a source whose ids are OGR's own feature ids, not a field's. */
inline constexpr int NO_FIELD = -1;

/** @return The index of `source`'s id field in `layer`, or `NO_FIELD` if the id is OGR's own. */
[[nodiscard]] int id_field_of(OGRLayer& layer, const VectorSource& source);

/** @return The fields of `feature`, but for `id_field`, as one JSON object: see the top of vector.hpp. */
[[nodiscard]] schemacht::json::Json tags_of(const OGRFeature& feature, int id_field);

/** @return The id of `feature`: its `id_field`, or OGR's own. */
[[nodiscard]] std::int64_t id_of(const OGRFeature& feature, int id_field, const VectorSource& source);

}  // namespace miniverse::gdal::detail
