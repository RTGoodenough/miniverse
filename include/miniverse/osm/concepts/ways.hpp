#pragma once

#include <concepts>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/model/way.hpp"

/**
 * @file
 * Which models an OpenStreetMap file can be read as, in place of a table (road_file.hpp): those whose load gives `Ways`.
 */
namespace miniverse::osm {

/**
 * @brief Whether `model_t` is a model of ways: a load gives `Ways`, and a streamed load some of them. Every `RoadModel` is one.
 */
template <typename model_t>
concept ReadsWays = LayerModel<model_t> && ! HasOwnChunks<model_t> && std::same_as<typename model_t::result_type, Ways>;

}  // namespace miniverse::osm
