#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include "miniverse/geo/types.hpp"
#include "miniverse/layer/feature_layer.hpp"
#include "miniverse/layer/raster_layer.hpp"
#include "miniverse/layer/road_layer.hpp"

/**
 * @file
 * The layer kinds the tool knows, by the names it is given them by: the built-in kinds, with each geometry type of a feature
 * layer and each pixel type of a raster layer.
 */
namespace tool {

namespace geo = miniverse::geo;

template <geo::wkb::Geometry geometry_t>
struct Features : miniverse::FeatureLayer<geometry_t> {};

struct Roads : miniverse::RoadLayer {};

template <geo::Pixel pixel_t>
struct Raster : miniverse::RasterLayer<pixel_t> {};

/** @brief A kind and the name the tool takes it by. */
template <typename kind_t>
struct Named {
  using kind_type = kind_t;

  std::string_view name;
};

inline constexpr std::tuple KINDS{
    Named<Features<geo::Point>>{"features:point"},
    Named<Features<geo::LineString>>{"features:linestring"},
    Named<Features<geo::Polygon>>{"features:polygon"},
    Named<Features<geo::MultiLineString>>{"features:multilinestring"},
    Named<Features<geo::MultiPolygon>>{"features:multipolygon"},
    Named<Roads>{"roads"},
    Named<Raster<std::int8_t>>{"raster:int8"},
    Named<Raster<std::uint8_t>>{"raster:uint8"},
    Named<Raster<std::int16_t>>{"raster:int16"},
    Named<Raster<std::uint16_t>>{"raster:uint16"},
    Named<Raster<std::int32_t>>{"raster:int32"},
    Named<Raster<std::uint32_t>>{"raster:uint32"},
    Named<Raster<float>>{"raster:float32"},
    Named<Raster<double>>{"raster:float64"},
};

/** @return The names of the kinds, in the order above. */
[[nodiscard]] inline std::vector<std::string> kind_names() {
  return std::apply([](const auto&... kinds) { return std::vector<std::string>{std::string(kinds.name)...}; }, KINDS);
}

/**
 * @brief Calls `visit` with `std::type_identity<kind>` for the kind called `name`.
 * @throws std::invalid_argument if there is no kind of that name.
 */
template <typename visitor_t>
void with_kind(std::string_view name, const visitor_t& visit) {
  bool found = false;
  std::apply(
      [&](const auto&... kinds) {
        const auto visit_if_named = [&](const auto& kind) {
          if ( ! found && kind.name == name ) {
            found = true;
            visit(std::type_identity<typename std::remove_cvref_t<decltype(kind)>::kind_type>{});
          }
        };
        (visit_if_named(kinds), ...);
      },
      KINDS
  );

  if ( ! found ) {
    throw std::invalid_argument("there is no kind '" + std::string(name) + "'");
  }
}

}  // namespace tool
