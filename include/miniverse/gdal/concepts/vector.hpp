#pragma once

#include <concepts>
#include <utility>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/geo/concepts/wkb.hpp"
#include "miniverse/model/feature.hpp"

/**
 * @file
 * Which models a vector file can be read as, in place of a table (feature_file.hpp): those whose load gives `Features`
 * of one geometry type.
 */
namespace miniverse::gdal {

/** @brief The geometry type of the features a load of `model_t` gives. */
template <typename model_t>
using GeometryOf = decltype(std::declval<typename model_t::result_type::value_type&>().geometry);

/**
 * @brief Whether `model_t` is a model of features: a load gives `Features` of one geometry type, and a streamed load some
 * of them. Every `FeatureModel` is one; so is a model of your own with that result, whatever its table's columns.
 */
template <typename model_t>
concept ReadsFeatures = LayerModel<model_t> && ! HasOwnChunks<model_t> && requires { typename GeometryOf<model_t>; } &&
                        geo::wkb::Geometry<GeometryOf<model_t>> && std::same_as<typename model_t::result_type, Features<GeometryOf<model_t>>>;

}  // namespace miniverse::gdal
