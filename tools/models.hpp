#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include "miniverse/geo/types.hpp"
#include "miniverse/model/feature_model.hpp"
#include "miniverse/model/raster_model.hpp"
#include "miniverse/model/road_model.hpp"

/**
 * @file
 * The models the tool knows, by the names it is given them by: the built-in models, with each
 * geometry type of a feature layer and each pixel type of a raster layer.
 */
namespace tool {

namespace geo = miniverse::geo;

/** @brief A model and the name the tool takes it by. */
template <typename model_t>
struct Named {
  using model = model_t;

  std::string_view name;
};

inline constexpr std::tuple MODELS{
    Named<miniverse::FeatureModel<geo::Point>>{"features:point"},
    Named<miniverse::FeatureModel<geo::LineString>>{"features:linestring"},
    Named<miniverse::FeatureModel<geo::Polygon>>{"features:polygon"},
    Named<miniverse::FeatureModel<geo::MultiLineString>>{"features:multilinestring"},
    Named<miniverse::FeatureModel<geo::MultiPolygon>>{"features:multipolygon"},
    Named<miniverse::RoadModel>{"roads"},
    Named<miniverse::RasterModel<std::int8_t>>{"raster:int8"},
    Named<miniverse::RasterModel<std::uint8_t>>{"raster:uint8"},
    Named<miniverse::RasterModel<std::int16_t>>{"raster:int16"},
    Named<miniverse::RasterModel<std::uint16_t>>{"raster:uint16"},
    Named<miniverse::RasterModel<std::int32_t>>{"raster:int32"},
    Named<miniverse::RasterModel<std::uint32_t>>{"raster:uint32"},
    Named<miniverse::RasterModel<float>>{"raster:float32"},
    Named<miniverse::RasterModel<double>>{"raster:float64"},
};

/** @return The names of the models, in the order above. */
[[nodiscard]] inline std::vector<std::string> model_names()
{
  return std::apply(
      [](const auto&... models) { return std::vector<std::string>{std::string(models.name)...}; },
      MODELS);
}

/**
 * @brief Calls `visit` with `std::type_identity<model>` for the model called `name`.
 * @throws std::invalid_argument if there is no model of that name.
 */
template <typename visitor_t>
void with_model(std::string_view name, const visitor_t& visit)
{
  bool found = false;
  std::apply(
      [&](const auto&... models) {
        const auto visit_if_named = [&](const auto& model) {
          if ( ! found && model.name == name ) {
            found = true;
            visit(std::type_identity<typename std::remove_cvref_t<decltype(model)>::model>{});
          }
        };
        (visit_if_named(models), ...);
      },
      MODELS);

  if ( ! found ) {
    throw std::invalid_argument("there is no model '" + std::string(name) + "'");
  }
}

}  // namespace tool
