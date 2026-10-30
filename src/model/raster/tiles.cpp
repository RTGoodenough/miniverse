#include "miniverse/model/raster/tiles.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "miniverse/geo/grid.hpp"
#include "miniverse/geo/raster.hpp"
#include "miniverse/geo/snap.hpp"
#include "miniverse/geo/types.hpp"

namespace miniverse::raster {

namespace {

/** @brief Where a raster's north-west pixel lies, in pixels, in a frame that another raster shares.
 */
struct PixelAt {
  std::int64_t column = 0;
  std::int64_t row    = 0;
};

/**
 * @brief Copies the pixels of `source` that have data into `target`, where they overlap: `source`
 * at `source_at` and `target` at `target_at` in one frame of pixels.
 * @return How many pixels were copied.
 */
template <geo::Pixel pixel_t>
std::size_t paste(const geo::Raster<pixel_t>& source,
                  PixelAt                     source_at,
                  geo::Raster<pixel_t>&       target,
                  PixelAt                     target_at)
{
  const std::int64_t west   = std::max(source_at.column, target_at.column);
  const std::int64_t east   = std::min(source_at.column + static_cast<std::int64_t>(source.width),
                                       target_at.column + static_cast<std::int64_t>(target.width));
  const std::int64_t north  = std::max(source_at.row, target_at.row);
  const std::int64_t south  = std::min(source_at.row + static_cast<std::int64_t>(source.height),
                                       target_at.row + static_cast<std::int64_t>(target.height));
  std::size_t        pasted = 0;

  for ( std::int64_t row = north; row < south; ++row ) {
    for ( std::int64_t column = west; column < east; ++column ) {
      const pixel_t value = source.at(static_cast<std::size_t>(column - source_at.column),
                                      static_cast<std::size_t>(row - source_at.row));
      if ( value == source.nodata ) {
        continue;
      }

      target.pixels.at((static_cast<std::size_t>(row - target_at.row) * target.width) +
                       static_cast<std::size_t>(column - target_at.column)) = value;
      ++pasted;
    }
  }

  return pasted;
}

}  // namespace

template <geo::Pixel pixel_t>
std::vector<Tile<pixel_t>> cut(const geo::Raster<pixel_t>& raster, const geo::Grid<pixel_t>& grid)
{
  geo::check_grid(grid);
  raster.check_pixel_count();

  if ( raster.nodata != grid.nodata &&
       std::ranges::find(raster.pixels, grid.nodata) != raster.pixels.end() )
  {
    throw std::invalid_argument("a pixel of the raster has the value " +
                                std::format("{}", static_cast<double>(grid.nodata)) +
                                ", which is the grid's nodata: it would be lost. Give the raster "
                                "the grid's nodata, or change the pixel");
  }

  const double size = grid.pixel_degrees();
  if ( ! geo::same_size(raster.pixel_width, size) || ! geo::same_size(raster.pixel_height, size) ) {
    throw std::invalid_argument("the raster's pixels are " +
                                std::format("{}", raster.pixel_width) +
                                " by " +
                                std::format("{}", raster.pixel_height) +
                                " degrees, not the grid's " +
                                std::format("{}", size));
  }

  const std::optional<geo::GridPixel> corner = grid.corner_pixel({raster.west, raster.north});
  if ( ! corner ) {
    throw std::invalid_argument("the raster's corner is not on a corner of the grid's pixels");
  }

  const PixelAt raster_at{.column = corner->column, .row = corner->row};
  const auto    width  = static_cast<std::int64_t>(raster.width);
  const auto    height = static_cast<std::int64_t>(raster.height);
  if ( raster_at.column < 0 ||
       raster_at.row < 0 ||
       raster_at.column + width > grid.pixels_across() ||
       raster_at.row + height > grid.pixels_down() )
  {
    throw std::invalid_argument(
        "the raster reaches past the world: longitude -180 to 180, latitude "
        "-90 to 90");
  }

  const std::int64_t         tile         = grid.tile_pixels;
  const std::int64_t         tiles_across = grid.tiles_across();
  std::vector<Tile<pixel_t>> tiles;

  // By ascending tile id: two overlapping pushes then lock their tiles in the same order, so
  // neither can deadlock the other.
  for ( std::int64_t tile_row = raster_at.row / tile; tile_row * tile < raster_at.row + height;
        ++tile_row )
  {
    for ( std::int64_t tile_column = raster_at.column / tile;
          tile_column * tile < raster_at.column + width; ++tile_column )
    {
      const geo::Point     tile_corner = grid.corner_of(tile_column * tile, tile_row * tile);
      geo::Raster<pixel_t> pixels{
          .west         = tile_corner.x(),
          .north        = tile_corner.y(),
          .pixel_width  = size,
          .pixel_height = size,
          .width        = static_cast<std::size_t>(tile),
          .height       = static_cast<std::size_t>(tile),
          .nodata       = grid.nodata,
          .pixels       = std::vector<pixel_t>(static_cast<std::size_t>(tile * tile), grid.nodata),
      };

      if ( paste(raster, raster_at, pixels,
                 PixelAt{.column = tile_column * tile, .row = tile_row * tile}) == 0 )
      {
        continue;
      }

      tiles.push_back(Tile<pixel_t>{.id     = (tile_row * tiles_across) + tile_column,
                                    .pixels = std::move(pixels)});
    }
  }

  return tiles;
}

template <geo::Pixel pixel_t>
geo::Raster<pixel_t> window(std::span<const geo::Raster<pixel_t>> tiles,
                            const geo::Polygon&                   location)
{
  const std::optional<geo::Box> box = geo::box_of(location);
  if ( tiles.empty() || ! box ) {
    return {};
  }

  // The window, in pixels of the first tile's frame: the box, widened to whole pixels, and at least
  // one pixel each way.
  const geo::Raster<pixel_t>& first = tiles.front();
  const std::int64_t          west =
      geo::floor_snapped((box->min_corner().x() - first.west) / first.pixel_width);
  const std::int64_t east = std::max(
      geo::ceil_snapped((box->max_corner().x() - first.west) / first.pixel_width), west + 1);
  const std::int64_t north =
      geo::floor_snapped((first.north - box->max_corner().y()) / first.pixel_height);
  const std::int64_t south = std::max(
      geo::ceil_snapped((first.north - box->min_corner().y()) / first.pixel_height), north + 1);

  geo::Raster<pixel_t> cut{
      .west         = first.west + (static_cast<double>(west) * first.pixel_width),
      .north        = first.north - (static_cast<double>(north) * first.pixel_height),
      .pixel_width  = first.pixel_width,
      .pixel_height = first.pixel_height,
      .width        = static_cast<std::size_t>(east - west),
      .height       = static_cast<std::size_t>(south - north),
      .nodata       = first.nodata,
      .pixels = std::vector<pixel_t>(static_cast<std::size_t>((east - west) * (south - north)),
                                     first.nodata),
  };

  for ( const geo::Raster<pixel_t>& tile : tiles ) {
    if ( ! geo::same_size(tile.pixel_width, first.pixel_width) ||
         ! geo::same_size(tile.pixel_height, first.pixel_height) ||
         tile.nodata != first.nodata )
    {
      throw std::invalid_argument("the tiles loaded don't share a pixel size and a nodata value");
    }

    const std::optional<std::int64_t> column =
        geo::whole((tile.west - first.west) / first.pixel_width);
    const std::optional<std::int64_t> tile_row =
        geo::whole((first.north - tile.north) / first.pixel_height);
    if ( ! column || ! tile_row ) {
      throw std::invalid_argument("the tiles loaded are not aligned to one grid of pixels");
    }

    paste(tile, PixelAt{.column = *column, .row = *tile_row}, cut,
          PixelAt{.column = west, .row = north});
  }

  return cut;
}

template std::vector<Tile<std::int8_t>>   cut(const geo::Raster<std::int8_t>&,
                                              const geo::Grid<std::int8_t>&);
template std::vector<Tile<std::uint8_t>>  cut(const geo::Raster<std::uint8_t>&,
                                              const geo::Grid<std::uint8_t>&);
template std::vector<Tile<std::int16_t>>  cut(const geo::Raster<std::int16_t>&,
                                              const geo::Grid<std::int16_t>&);
template std::vector<Tile<std::uint16_t>> cut(const geo::Raster<std::uint16_t>&,
                                              const geo::Grid<std::uint16_t>&);
template std::vector<Tile<std::int32_t>>  cut(const geo::Raster<std::int32_t>&,
                                              const geo::Grid<std::int32_t>&);
template std::vector<Tile<std::uint32_t>> cut(const geo::Raster<std::uint32_t>&,
                                              const geo::Grid<std::uint32_t>&);
template std::vector<Tile<float>>         cut(const geo::Raster<float>&, const geo::Grid<float>&);
template std::vector<Tile<double>>        cut(const geo::Raster<double>&, const geo::Grid<double>&);

template geo::Raster<std::int8_t>   window(std::span<const geo::Raster<std::int8_t>>,
                                           const geo::Polygon&);
template geo::Raster<std::uint8_t>  window(std::span<const geo::Raster<std::uint8_t>>,
                                           const geo::Polygon&);
template geo::Raster<std::int16_t>  window(std::span<const geo::Raster<std::int16_t>>,
                                           const geo::Polygon&);
template geo::Raster<std::uint16_t> window(std::span<const geo::Raster<std::uint16_t>>,
                                           const geo::Polygon&);
template geo::Raster<std::int32_t>  window(std::span<const geo::Raster<std::int32_t>>,
                                           const geo::Polygon&);
template geo::Raster<std::uint32_t> window(std::span<const geo::Raster<std::uint32_t>>,
                                           const geo::Polygon&);
template geo::Raster<float>  window(std::span<const geo::Raster<float>>, const geo::Polygon&);
template geo::Raster<double> window(std::span<const geo::Raster<double>>, const geo::Polygon&);

}  // namespace miniverse::raster
