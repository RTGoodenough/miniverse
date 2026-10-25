#include "gdal/raster/tiles.hpp"

#include <boost/geometry/algorithms/envelope.hpp>  // IWYU pragma: keep
#include <boost/geometry/geometries/box.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "miniverse/geo/grid.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/layer/raster/reach.hpp"

namespace miniverse::gdal::detail {

namespace {

/**
 * How far past a tile's edge an extent may reach, in tiles, and still count as ending on it: transforming the corner of a
 * file that ends on a tile's edge lands a rounding step to one side of it.
 */
constexpr double EDGE_TOLERANCE = 1e-9;

/** @brief Tiles as numbers, for the arithmetic: how many to a degree, and how many the world has each way. */
struct Counts {
  double per_degree;
  double across;
  double down;

  explicit Counts(const Tiling& tiling)
      : per_degree(static_cast<double>(tiling.pixels_per_degree) / static_cast<double>(tiling.tile_pixels)),
        across(static_cast<double>(tiling.tiles_across)),
        down(static_cast<double>(tiling.tiles_down)) {}

  // Kept within the world's tiles while still a double: only then is it sure to fit a whole number.
  [[nodiscard]] std::int64_t column(double tiles) const { return static_cast<std::int64_t>(std::clamp(tiles, 0.0, across)); }
  [[nodiscard]] std::int64_t row(double tiles) const { return static_cast<std::int64_t>(std::clamp(tiles, 0.0, down)); }

  Counts(const Counts&) = default;
  Counts(Counts&&) noexcept = default;
  Counts& operator=(const Counts&) = default;
  Counts& operator=(Counts&&) noexcept = default;
  ~Counts() = default;
};

}  // namespace

Tiles tiles_of(const geo::Box& extent, const Tiling& tiling) {
  const Counts counts(tiling);

  return {
      .first_column = counts.column(std::floor(((extent.min_corner().x() - geo::GRID_WEST) * counts.per_degree) + EDGE_TOLERANCE)),
      .end_column = counts.column(std::ceil(((extent.max_corner().x() - geo::GRID_WEST) * counts.per_degree) - EDGE_TOLERANCE)),
      .first_row = counts.row(std::floor(((geo::GRID_NORTH - extent.max_corner().y()) * counts.per_degree) + EDGE_TOLERANCE)),
      .end_row = counts.row(std::ceil(((geo::GRID_NORTH - extent.min_corner().y()) * counts.per_degree) - EDGE_TOLERANCE)),
  };
}

Tiles tiles_meeting(const geo::Polygon& location, const Tiling& tiling) {
  // Boost declares return_envelope in a detail header; algorithms/envelope.hpp is the public one.
  const auto box = boost::geometry::return_envelope<boost::geometry::model::box<geo::Point>>(location);  // NOLINT(misc-include-cleaner)
  if ( box.min_corner().x() > box.max_corner().x() ) {
    return {};
  }

  if ( ! std::isfinite(box.min_corner().x()) || ! std::isfinite(box.min_corner().y()) || ! std::isfinite(box.max_corner().x()) ||
       ! std::isfinite(box.max_corner().y()) ) {
    throw std::invalid_argument("the coordinates of a location must be finite numbers");
  }

  // A box that ends on a tile's edge meets the tile beyond it: the tolerance leans that way, where `tiles_of` leans the other.
  const Counts counts(tiling);

  return {
      .first_column = counts.column(std::floor(((box.min_corner().x() - geo::GRID_WEST) * counts.per_degree) - EDGE_TOLERANCE)),
      .end_column = counts.column(std::floor(((box.max_corner().x() - geo::GRID_WEST) * counts.per_degree) + EDGE_TOLERANCE) + 1),
      .first_row = counts.row(std::floor(((geo::GRID_NORTH - box.max_corner().y()) * counts.per_degree) - EDGE_TOLERANCE)),
      .end_row = counts.row(std::floor(((geo::GRID_NORTH - box.min_corner().y()) * counts.per_degree) + EDGE_TOLERANCE) + 1),
  };
}

Tiles in_both(const Tiles& one, const Tiles& other) {
  return {
      .first_column = std::max(one.first_column, other.first_column),
      .end_column = std::min(one.end_column, other.end_column),
      .first_row = std::max(one.first_row, other.first_row),
      .end_row = std::min(one.end_row, other.end_row),
  };
}

namespace {

/** @return The outline of the tile of `tiling` numbered `column` across and `row` down. */
[[nodiscard]] geo::Box outline_of(std::int64_t column, std::int64_t row, const Tiling& tiling) {
  const auto degrees = [&](std::int64_t tiles) {
    return static_cast<double>(tiles * tiling.tile_pixels) / static_cast<double>(tiling.pixels_per_degree);
  };

  return {
      {geo::GRID_WEST + degrees(column), geo::GRID_NORTH - degrees(row + 1)}, {geo::GRID_WEST + degrees(column + 1), geo::GRID_NORTH - degrees(row)}
  };
}

}  // namespace

std::vector<std::int64_t> reached_of(const raster::Reach& reach, const Tiles& run, const Tiling& tiling) {
  std::vector<std::int64_t> reached;
  for ( std::int64_t column = run.first_column; column < run.end_column; ++column ) {
    if ( reach.into(outline_of(column, run.first_row, tiling)) ) {
      reached.push_back(column);
    }
  }

  return reached;
}

}  // namespace miniverse::gdal::detail
