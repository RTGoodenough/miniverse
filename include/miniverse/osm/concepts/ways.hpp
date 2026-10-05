#pragma once

#include <concepts>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/layer/road_layer.hpp"

namespace miniverse::osm {

/**
 * @brief Whether `kind_t` is a layer kind of ways: a load gives `Ways`, a streamed load some of them, and its table is made
 * without settings. Every `RoadLayer` is one.
 */
template <typename kind_t>
concept WayKind = LayerKind<kind_t> && HasNoSettings<kind_t> && ! HasOwnChunks<kind_t> && std::same_as<typename kind_t::result_type, Ways>;

}  // namespace miniverse::osm
