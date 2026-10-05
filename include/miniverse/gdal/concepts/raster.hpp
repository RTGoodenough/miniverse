#pragma once

#include <concepts>
#include <vector>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/geo/concepts/raster.hpp"
#include "miniverse/geo/raster.hpp"

namespace miniverse::gdal {

/** @brief The pixel type of the rasters a load of `kind_t` gives. */
template <typename kind_t>
using PixelOf = decltype(kind_t::settings_type::nodata);

/**
 * @brief Whether `kind_t` is a layer kind of rasters on a grid: a load gives a `geo::Raster`, a streamed load its tiles, and
 * its settings are a `geo::Grid`. Every `RasterLayer` is one.
 */
template <typename kind_t>
concept RasterKind = LayerKind<kind_t> && HasSettings<kind_t> && HasOwnChunks<kind_t> && requires { typename PixelOf<kind_t>; } &&
                     geo::Pixel<PixelOf<kind_t>> && std::same_as<typename kind_t::settings_type, geo::Grid<PixelOf<kind_t>>> &&
                     std::same_as<typename kind_t::result_type, geo::Raster<PixelOf<kind_t>>> &&
                     std::same_as<typename kind_t::chunk_type, std::vector<geo::Raster<PixelOf<kind_t>>>>;

}  // namespace miniverse::gdal
