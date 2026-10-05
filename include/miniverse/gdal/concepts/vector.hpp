#pragma once

#include <concepts>
#include <utility>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/geo/concepts/wkb.hpp"
#include "miniverse/layer/feature_layer.hpp"

namespace miniverse::gdal {

/** @brief The geometry type of the features a load of `kind_t` gives. */
template <typename kind_t>
using GeometryOf = decltype(std::declval<typename kind_t::result_type::value_type&>().geometry);

/**
 * @brief Whether `kind_t` is a layer kind of features: a load gives `Features` of one geometry type, a streamed load some of
 * them, and its table is made without settings. Every `FeatureLayer` is one; so is a kind of your own with that result,
 * whatever its table's columns.
 */
template <typename kind_t>
concept FeatureKind = LayerKind<kind_t> && HasNoSettings<kind_t> && ! HasOwnChunks<kind_t> && requires { typename GeometryOf<kind_t>; } &&
                      geo::wkb::Geometry<GeometryOf<kind_t>> && std::same_as<typename kind_t::result_type, Features<GeometryOf<kind_t>>>;

}  // namespace miniverse::gdal
