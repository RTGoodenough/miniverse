#pragma once

#include <boost/geometry/algorithms/envelope.hpp>  // IWYU pragma: keep

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "miniverse/geo/concepts/raster.hpp"  // IWYU pragma: export
#include "miniverse/geo/snap.hpp"
#include "miniverse/geo/types.hpp"

/**
 * @file
 * The grid a raster table's tiles lie on: pixels per degree of WGS 84 from the world's north-west
 * corner, square tiles, a nodata value; where each of its tiles and pixels lies, and which a box
 * covers or meets; and the way from a resolution in metres to such a grid.
 */
namespace miniverse::geo {

/** @brief The longitude every grid starts at: its north-west corner's. */
inline constexpr double GRID_WEST = -180;

/** @brief The latitude every grid starts at: its north-west corner's. */
inline constexpr double GRID_NORTH = 90;

/** @brief How many degrees of longitude the world spans from `GRID_WEST`. */
inline constexpr std::int64_t DEGREES_ACROSS = 360;

/** @brief How many degrees of latitude the world spans from `GRID_NORTH`. */
inline constexpr std::int64_t DEGREES_DOWN = 180;

/** @brief The finest grid there is, in pixels per degree (0.1 arc seconds, about 3.1 m):
 * `raster_columns` can't tell finer grids apart. */
inline constexpr std::int32_t MAX_PIXELS_PER_DEGREE = 36000;

/**
 * @brief How many metres a degree of latitude is, near enough, and a degree of longitude at the
 * equator: what a resolution in metres is made into pixels per degree by
 * (`pixels_per_degree_of_metres`). A 360th of WGS 84's equator.
 */
inline constexpr double METRES_PER_DEGREE = 111'319.490793;

/**
 * @return How many pixels to a degree a grid of about `metres_per_pixel` has: the nearest whole
 * number, so 30 m is 3711 of them (29.997 m; `Grid::metres_per_pixel` gives what a grid came to).
 * For the `pixels_per_degree` of a `Grid`:
 *
 * @code
 * world.create_table<"elevation">({.pixels_per_degree = geo::pixels_per_degree_of_metres(30),
 * .tile_pixels = 256, .nodata = -32768});
 * @endcode
 *
 * A pixel is that many metres from north to south everywhere, and from east to west at the equator;
 * towards the poles it is narrower on the ground by the cosine of the latitude. A source in arc
 * seconds keeps its own pixels only on its own grid: give 3600 pixels per degree for one arc
 * second, not 30 m.
 * @throws std::invalid_argument unless `metres_per_pixel` is above zero, at most
 * `METRES_PER_DEGREE`, and comes to at most `MAX_PIXELS_PER_DEGREE` pixels per degree: about 3.1 m
 * at the finest, a degree (about 111 km) at the coarsest. Near the coarse end the nearest grid is
 * far from what was asked: 80 km is a pixel to a degree too.
 */
[[nodiscard]] inline std::int32_t pixels_per_degree_of_metres(double metres_per_pixel)
{
  const double pixels = std::round(METRES_PER_DEGREE / metres_per_pixel);
  if ( ! (metres_per_pixel > 0) ||
       ! (metres_per_pixel <= METRES_PER_DEGREE) ||
       ! (pixels <= MAX_PIXELS_PER_DEGREE) )
  {
    throw std::invalid_argument("a grid of " +
                                std::to_string(metres_per_pixel) +
                                " m to a pixel can't be made: the finest is about 3.1 m (" +
                                std::to_string(MAX_PIXELS_PER_DEGREE) +
                                " pixels per degree), and the coarsest one pixel per degree");
  }

  return static_cast<std::int32_t>(pixels);
}

/**
 * @brief Tiles of a grid, numbered east from longitude -180 and south from latitude 90: the columns
 * `first_column` to before `end_column`, and the rows likewise.
 */
struct TileRange {
  std::int64_t first_column = 0;
  std::int64_t end_column   = 0;
  std::int64_t first_row    = 0;
  std::int64_t end_row      = 0;

  /** @return Whether there is no tile in it: `first` not before `end`, either way. */
  [[nodiscard]] bool empty() const noexcept
  {
    return first_column >= end_column || first_row >= end_row;
  }

  /** @return The tiles in both this and `other`: empty, if they share none. */
  [[nodiscard]] TileRange meet(const TileRange& other) const noexcept
  {
    return {
        .first_column = std::max(first_column, other.first_column),
        .end_column   = std::min(end_column, other.end_column),
        .first_row    = std::max(first_row, other.first_row),
        .end_row      = std::min(end_row, other.end_row),
    };
  }

  [[nodiscard]] bool operator==(const TileRange&) const = default;
};

/**
 * @brief Which pixel of a grid: its column, counted east from longitude -180, and its row, counted
 * south from latitude 90. Signed, so that a pixel off the world is a number too.
 */
struct GridPixel {
  std::int64_t column = 0;
  std::int64_t row    = 0;

  [[nodiscard]] bool operator==(const GridPixel&) const = default;
};

/**
 * @return The bounding box of `location`: what the tiles and pixels it has are found from. Nothing
 * for a location of no points.
 * @throws std::invalid_argument if a coordinate of the location is not a finite number.
 */
[[nodiscard]] inline std::optional<Box> box_of(const Polygon& location)
{
  // Boost declares return_envelope in a detail header; algorithms/envelope.hpp is the public one.
  const Box box = boost::geometry::return_envelope<Box>(location);  // NOLINT(misc-include-cleaner)
  if ( box.min_corner().x() > box.max_corner().x() ) {
    return std::nullopt;
  }

  if ( ! std::isfinite(box.min_corner().x()) ||
       ! std::isfinite(box.min_corner().y()) ||
       ! std::isfinite(box.max_corner().x()) ||
       ! std::isfinite(box.max_corner().y()) )
  {
    throw std::invalid_argument("the coordinates of a location must be finite numbers");
  }

  return box;
}

/**
 * @brief The fixed grid a raster table's tiles lie on: `pixels_per_degree` pixels to a degree in
 * both directions, counted from the corner at longitude -180, latitude 90, in square tiles of
 * `tile_pixels` pixels a side. Every source is warped onto it.
 *
 * Pixels per degree is a whole number, so where each tile and pixel lies is exact arithmetic on
 * integers, turned into degrees in one place (`corner_of`). 3600 is one arc second, SRTM's finest;
 * a resolution in metres is made into the nearest such grid by `pixels_per_degree_of_metres`. Tiles
 * of 512 pixels suit loads of some kilometres to a hundred; larger tiles slow small loads.
 */
template <Pixel pixel_t>
struct Grid {
  std::int32_t pixels_per_degree = 0;  ///< 1 to `MAX_PIXELS_PER_DEGREE`.
  std::int32_t tile_pixels       = 0;  ///< A tile's width and height, in pixels: 1 to 65535.
  pixel_t      nodata{};               ///< The value of a pixel that has no data. Finite.

  /**
   * @return How many metres a pixel spans from north to south, near enough (a degree of latitude is
   * 110.6 to 111.7 km), and from east to west at the equator: of a grid that has pixels.
   */
  [[nodiscard]] double metres_per_pixel() const { return METRES_PER_DEGREE / pixels_per_degree; }

  /** @return How many degrees a pixel spans, each way. */
  [[nodiscard]] double pixel_degrees() const
  {
    return 1.0 / static_cast<double>(pixels_per_degree);
  }

  /** @return How many pixels span the world's 360 degrees of longitude. */
  [[nodiscard]] std::int64_t pixels_across() const { return DEGREES_ACROSS * pixels_per_degree; }

  /** @return How many pixels span the world's 180 degrees of latitude. */
  [[nodiscard]] std::int64_t pixels_down() const { return DEGREES_DOWN * pixels_per_degree; }

  /** @return How many tiles span the world's 360 degrees of longitude: the last reaches past it
   * when tiles don't divide them. */
  [[nodiscard]] std::int64_t tiles_across() const { return tiles_spanning(DEGREES_ACROSS); }

  /** @return How many tiles span the world's 180 degrees of latitude, as `tiles_across`. */
  [[nodiscard]] std::int64_t tiles_down() const { return tiles_spanning(DEGREES_DOWN); }

  /**
   * @return The north-west corner of the pixel `column` across and `row` down, whether the grid has
   * such a pixel or not. Every corner of a pixel or a tile is worked out here, so that two of one
   * place are the same number to the last bit.
   */
  [[nodiscard]] Point corner_of(std::int64_t column, std::int64_t row) const
  {
    return {GRID_WEST + (static_cast<double>(column) / static_cast<double>(pixels_per_degree)),
            GRID_NORTH - (static_cast<double>(row) / static_cast<double>(pixels_per_degree))};
  }

  /** @return The outline of the tile `column` across and `row` down, whether the grid has it or
   * not: a whole tile, past the world's edge too. */
  [[nodiscard]] Box tile_outline(std::int64_t column, std::int64_t row) const
  {
    return {corner_of(column * tile_pixels, (row + 1) * tile_pixels),
            corner_of((column + 1) * tile_pixels, row * tile_pixels)};
  }

  /** @return The box all the grid's tiles fill: the world, and past its east and south edges by
   * what the last tiles reach over. */
  [[nodiscard]] Box outline() const
  {
    return {corner_of(0, tiles_down() * tile_pixels), corner_of(tiles_across() * tile_pixels, 0)};
  }

  /**
   * @return The tiles `box` covers, within the world: a tile it reaches no further into than
   * `EDGE_TOLERANCE`, in tiles, is not one of them, so a box ending on a tile's edge, or a rounding
   * step past it, ends there. For the extent of a file: the tiles its pixels fill. A box of no
   * width on a tile's edge has none.
   */
  [[nodiscard]] TileRange tiles_covering(const Box& box) const
  {
    return {
        .first_column = columns_within(floor_snapped(tile_columns(box.min_corner().x()))),
        .end_column   = columns_within(ceil_snapped(tile_columns(box.max_corner().x()))),
        .first_row    = rows_within(floor_snapped(tile_rows(box.max_corner().y()))),
        .end_row      = rows_within(ceil_snapped(tile_rows(box.min_corner().y()))),
    };
  }

  /**
   * @return The tiles whose outlines `box` meets, within the world: those it only touches too, at
   * an edge to within `EDGE_TOLERANCE`, so a box ending on a tile's edge meets the tile beyond it.
   * For a location's box: the tiles the location can reach into, of which `raster::Reach` says
   * which it does.
   */
  [[nodiscard]] TileRange tiles_meeting(const Box& box) const
  {
    return {
        .first_column = columns_within(ceil_snapped(tile_columns(box.min_corner().x())) - 1),
        .end_column   = columns_within(floor_snapped(tile_columns(box.max_corner().x())) + 1),
        .first_row    = rows_within(ceil_snapped(tile_rows(box.max_corner().y())) - 1),
        .end_row      = rows_within(floor_snapped(tile_rows(box.min_corner().y())) + 1),
    };
  }

  /**
   * @return The pixel whose north-west corner `corner` is, if it lies on one to within
   * `EDGE_TOLERANCE`, in pixels; off the world too. Nothing if it lies between corners, or is no
   * number.
   */
  [[nodiscard]] std::optional<GridPixel> corner_pixel(const Point& corner) const
  {
    const std::optional<std::int64_t> column = whole((corner.x() - GRID_WEST) * pixels_per_degree);
    const std::optional<std::int64_t> row    = whole((GRID_NORTH - corner.y()) * pixels_per_degree);
    if ( ! column || ! row ) {
      return std::nullopt;
    }

    return GridPixel{.column = *column, .row = *row};
  }

  [[nodiscard]] bool operator==(const Grid&) const = default;

 private:
  [[nodiscard]] std::int64_t tiles_spanning(std::int64_t degrees) const
  {
    const std::int64_t pixels = degrees * pixels_per_degree;

    return (pixels + tile_pixels - 1) / tile_pixels;
  }

  /** @return How many tiles east of the grid's west edge `longitude` is, as a number. */
  [[nodiscard]] double tile_columns(double longitude) const
  {
    return (longitude - GRID_WEST) * pixels_per_degree / tile_pixels;
  }

  /** @return How many tiles south of the grid's north edge `latitude` is, as a number. */
  [[nodiscard]] double tile_rows(double latitude) const
  {
    return (GRID_NORTH - latitude) * pixels_per_degree / tile_pixels;
  }

  [[nodiscard]] std::int64_t columns_within(std::int64_t column) const
  {
    return std::clamp<std::int64_t>(column, 0, tiles_across());
  }

  [[nodiscard]] std::int64_t rows_within(std::int64_t row) const
  {
    return std::clamp<std::int64_t>(row, 0, tiles_down());
  }
};

/** @brief The largest tile there is, in pixels a side: raster WKB holds a width and a height in 16
 * bits each. */
inline constexpr std::int32_t MAX_TILE_PIXELS = 65535;

/**
 * @brief Throws unless `grid` is a grid a raster table can be made with, and a file be read onto: 1
 * to `MAX_PIXELS_PER_DEGREE` pixels per degree, tiles of 1 to `MAX_TILE_PIXELS` pixels a side, and
 * a nodata value that is a finite number.
 * @throws std::invalid_argument for one that is not.
 */
template <Pixel pixel_t>
void check_grid(const Grid<pixel_t>& grid)
{
  if ( grid.pixels_per_degree < 1 || grid.pixels_per_degree > MAX_PIXELS_PER_DEGREE ) {
    throw std::invalid_argument("a grid has 1 to " +
                                std::to_string(MAX_PIXELS_PER_DEGREE) +
                                " pixels per degree, not " +
                                std::to_string(grid.pixels_per_degree));
  }

  if ( grid.tile_pixels < 1 || grid.tile_pixels > MAX_TILE_PIXELS ) {
    throw std::invalid_argument("a grid's tiles are 1 to " +
                                std::to_string(MAX_TILE_PIXELS) +
                                " pixels a side, not " +
                                std::to_string(grid.tile_pixels));
  }

  if constexpr ( std::floating_point<pixel_t> ) {
    if ( ! std::isfinite(grid.nodata) ) {
      throw std::invalid_argument("a grid's nodata value must be a finite number");
    }
  }
}

}  // namespace miniverse::geo
