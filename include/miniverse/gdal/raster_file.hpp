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
#include "miniverse/layer/raster/tiles.hpp"
#include "miniverse/reader.hpp"

/**
 * @file
 * A raster file as a layer, read in place of a table of the same grid by `read_tiles_in` (raster.hpp).
 */
namespace miniverse::gdal {

namespace detail {

/**
 * @brief The reader of a raster file as a layer of `kind_t`, behind `raster_file`. A load gives the pixels in the location's box
 * on the layer's grid, as a table the file was pushed into gives them. Each load opens the file for itself, so loads run side
 * by side.
 */
template <RasterKind kind_t>
class RasterFile : public Reader<kind_t> {
 public:
  RasterFile(RasterSource source, const geo::Grid<PixelOf<kind_t>>& grid) : _source(std::move(source)), _grid(grid) {}

  [[nodiscard]] std::string name() const override { return "'" + _source.path + "'"; }

  [[nodiscard]] geo::Grid<PixelOf<kind_t>> settings() const override { return _grid; }

  [[nodiscard]] typename kind_t::result_type load(const geo::Polygon& location) const override {
    // From the tiles a stream hands over, so a load and a stream agree.
    std::vector<geo::Raster<PixelOf<kind_t>>> tiles;
    read_tiles_in<PixelOf<kind_t>>(_source, _grid, location, TILES_AT_A_TIME, [&tiles](std::vector<geo::Raster<PixelOf<kind_t>>> chunk) {
      tiles.insert(tiles.end(), std::make_move_iterator(chunk.begin()), std::make_move_iterator(chunk.end()));

      return true;
    });

    return raster::window(tiles, location);
  }

  void stream(const geo::Polygon& location, std::size_t chunk_rows, const Reader<kind_t>::OnChunk& on_chunk) const override {
    read_tiles_in<PixelOf<kind_t>>(_source, _grid, location, chunk_rows, on_chunk);
  }

  [[nodiscard]] std::vector<std::string> problems() const override { return problems_of<PixelOf<kind_t>>(_source, _grid); }

 private:
  static constexpr std::size_t TILES_AT_A_TIME = 64;  // how many tiles of a load are read before they are taken over

  RasterSource               _source;
  geo::Grid<PixelOf<kind_t>> _grid;

 public:
  RasterFile(const RasterFile&) = delete;
  RasterFile(RasterFile&&) = delete;
  RasterFile& operator=(const RasterFile&) = delete;
  RasterFile& operator=(RasterFile&&) = delete;
  ~RasterFile() override = default;
};

}  // namespace detail

/**
 * @return The layer of `kind_t` that is `source`, a raster file, warped onto `grid` and read in place of a table:
 *
 * @code
 * miniverse::Miniverse world(miniverse::gdal::raster_file<Elevation>({.path = "town.tif"}, {.pixels_per_degree = 3600, .tile_pixels = 256, .nodata = -32768}));
 * world.load<Elevation>(area).get();  // the file's heights in the area's box, on the grid
 * @endcode
 *
 * The grid is given as it is given to `Miniverse::create_table`, and is what `Miniverse::table_settings` gives. The file is
 * first opened by a load, or by `Miniverse::verify`, which says what is wrong with it.
 */
template <RasterKind kind_t>
[[nodiscard]] Layer<kind_t> raster_file(RasterSource source, const geo::Grid<PixelOf<kind_t>>& grid) {
  return Layer<kind_t>(std::make_shared<detail::RasterFile<kind_t>>(std::move(source), grid));
}

}  // namespace miniverse::gdal
