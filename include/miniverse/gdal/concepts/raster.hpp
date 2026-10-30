#pragma once

#include <concepts>
#include <vector>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/geo/concepts/raster.hpp"
#include "miniverse/geo/grid.hpp"
#include "miniverse/geo/raster.hpp"

/**
 * @file
 * Which models a raster file can be read as, in place of a table (raster_file.hpp): those whose
 * load gives a `geo::Raster` on a `geo::Grid`.
 */
namespace miniverse::gdal {

/** @brief The pixel type of the rasters a load of `model_t` gives. */
template <typename model_t>
using PixelOf = decltype(SettingsOf<model_t>::nodata);

/**
 * @brief Whether `model_t` is a model of rasters on a grid: a load gives a `geo::Raster`, a
 * streamed load its tiles, and its settings are a `geo::Grid`. Every `RasterModel` is one.
 */
template <typename model_t>
concept ReadsRasters =
    LayerModel<model_t> &&
    WritesSettings<model_t> &&
    HasOwnChunks<model_t> &&
    requires { typename PixelOf<model_t>; } &&
    geo::Pixel<PixelOf<model_t>> &&
    std::same_as<SettingsOf<model_t>, geo::Grid<PixelOf<model_t>>> &&
    std::same_as<typename model_t::result_type, geo::Raster<PixelOf<model_t>>> &&
    std::same_as<typename model_t::chunk_type, std::vector<geo::Raster<PixelOf<model_t>>>>;

}  // namespace miniverse::gdal
