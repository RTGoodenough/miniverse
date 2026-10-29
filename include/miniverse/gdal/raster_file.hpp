#pragma once

#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/gdal/concepts/raster.hpp"  // IWYU pragma: export
#include "miniverse/gdal/raster.hpp"
#include "miniverse/geo/grid.hpp"
#include "miniverse/geo/raster.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/layer.hpp"
#include "miniverse/model/raster/tiles.hpp"
#include "miniverse/reader.hpp"

/**
 * @file
 * A raster file as a layer, read in place of a table of the same grid by `read_tiles_in` (raster.hpp).
 */
namespace miniverse::gdal {

namespace detail {

/**
 * @brief The reader of a raster file as a layer of `model_t`, behind `raster_file`. A load gives the pixels in the location's box
 * on the layer's grid, as a table the file was pushed into gives them. Each load opens the file for itself, so loads run side
 * by side.
 */
template <ReadsRasters model_t>
class RasterFile : public Reader<model_t> {
 public:
  RasterFile(RasterSource source, const geo::Grid<PixelOf<model_t>>& grid) : _source(std::move(source)), _grid(grid) {}

  [[nodiscard]] std::string name() const override { return "'" + _source.path + "'"; }

  [[nodiscard]] geo::Grid<PixelOf<model_t>> settings() const override { return _grid; }

  [[nodiscard]] typename model_t::result_type load(const geo::Polygon& location) const override {
    // From the tiles a stream hands over, so a load and a stream agree.
    std::vector<geo::Raster<PixelOf<model_t>>> tiles;
    read_tiles_in<PixelOf<model_t>>(_source, _grid, location, TILES_AT_A_TIME, [&tiles](std::vector<geo::Raster<PixelOf<model_t>>> chunk) {
      tiles.insert(tiles.end(), std::make_move_iterator(chunk.begin()), std::make_move_iterator(chunk.end()));

      return true;
    });

    return raster::window(tiles, location);
  }

  void stream(const geo::Polygon& location, std::size_t chunk_rows, const Reader<model_t>::OnChunk& on_chunk) const override {
    read_tiles_in<PixelOf<model_t>>(_source, _grid, location, chunk_rows, on_chunk);
  }

  [[nodiscard]] std::vector<std::string> problems() const override { return problems_of<PixelOf<model_t>>(_source, _grid); }

 private:
  static constexpr std::size_t TILES_AT_A_TIME = 64;  // how many tiles of a load are read before they are taken over

  RasterSource                _source;
  geo::Grid<PixelOf<model_t>> _grid;

 public:
  RasterFile(const RasterFile&) = delete;
  RasterFile(RasterFile&&) = delete;
  RasterFile& operator=(const RasterFile&) = delete;
  RasterFile& operator=(RasterFile&&) = delete;
  ~RasterFile() override = default;
};

}  // namespace detail

/**
 * @return The layer `layer_t`, of a model of rasters, that is `source`, a raster file, warped onto `grid` and read in place of
 * a table:
 *
 * @code
 * using Elevation = miniverse::Layer<miniverse::RasterModel<std::int16_t>, "elevation">;
 *
 * miniverse::Miniverse world(miniverse::gdal::raster_file<Elevation>({.path = "town.tif"}, {.pixels_per_degree = 3600, .tile_pixels = 256, .nodata = -32768}));
 * world.load<"elevation">(area).get();  // the file's heights in the area's box, on the grid
 * @endcode
 *
 * The grid is given as it is given to `Miniverse::create_table`, and is what `Miniverse::table_settings` gives. The file is
 * first opened by a load, or by `Miniverse::verify`, which says what is wrong with it.
 */
template <IsLayer layer_t>
  requires ReadsRasters<typename layer_t::model_type>
[[nodiscard]] layer_t raster_file(RasterSource source, const geo::Grid<PixelOf<typename layer_t::model_type>>& grid) {
  return layer_t(std::make_shared<detail::RasterFile<typename layer_t::model_type>>(std::move(source), grid));
}

}  // namespace miniverse::gdal
