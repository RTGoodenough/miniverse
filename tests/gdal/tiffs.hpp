#pragma once

// GeoTIFFs made with GDAL for the raster tests, on the coarse grid they are read onto: four pixels to a degree, in tiles of
// one degree, with a kind of that grid and the helpers that number, roughen and order pixels.

#include <catch2/catch_test_macros.hpp>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <gdal.h>
#include <ogr_core.h>
#include <ogr_spatialref.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/gdal/raster.hpp"
#include "miniverse/geo/grid.hpp"
#include "miniverse/geo/raster.hpp"
#include "miniverse/layer/raster_layer.hpp"

namespace test {

using Raster = miniverse::geo::Raster<std::int16_t>;

constexpr std::int16_t GRID_NODATA = -32768;
constexpr std::int16_t FILE_NODATA = -9999;

constexpr miniverse::geo::Grid<std::int16_t> GRID{.pixels_per_degree = 4, .tile_pixels = 4, .nodata = GRID_NODATA};

struct Elevation : miniverse::RasterLayer<std::int16_t> {};

/** @brief Where a GeoTIFF lies and what it holds. */
struct Tiff {
  int                       epsg = 4326;
  double                    west = 0;
  double                    north = 0;
  double                    pixel = 0.25;  ///< A pixel's size each way, in the units of `epsg`.
  int                       width = 0;
  int                       height = 0;
  std::vector<std::int16_t> pixels;  ///< In rows from the north-west, `width * height` of them.
};

/** @return The pixel `column * 100 + row` for each of `width` by `height`. */
[[nodiscard]] inline std::vector<std::int16_t> numbered(int width, int height) {
  std::vector<std::int16_t> pixels;

  pixels.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
  for ( int row = 0; row < height; ++row ) {
    for ( int column = 0; column < width; ++column ) {
      pixels.push_back(static_cast<std::int16_t>((column * 100) + row));
    }
  }

  return pixels;
}

/** @return `width * height` pixels with no pattern to them, the same every time: heights of 0 to 2999. */
[[nodiscard]] inline std::vector<std::int16_t> rough(int width, int height) {
  std::vector<std::int16_t> pixels;
  std::uint32_t             state = 12345;

  pixels.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
  for ( int i = 0; i < width * height; ++i ) {
    state = (state * 1664525U) + 1013904223U;  // a linear congruential generator
    pixels.push_back(static_cast<std::int16_t>((state >> 16U) % 3000U));
  }

  return pixels;
}

/** @brief Writes `tiff` as a GeoTIFF of one 16-bit band at `path`, with FILE_NODATA as its nodata value. An `epsg` of 0 names no system. */
inline void write_tiff(const std::string& path, Tiff tiff) {
  GDALAllRegister();

  GDALDatasetH made = GDALCreate(GDALGetDriverByName("GTiff"), path.c_str(), tiff.width, tiff.height, 1, GDT_Int16, nullptr);
  REQUIRE(made != nullptr);

  std::array<double, 6> position{tiff.west, tiff.pixel, 0, tiff.north, 0, -tiff.pixel};
  REQUIRE(GDALSetGeoTransform(made, position.data()) == CE_None);

  if ( tiff.epsg != 0 ) {
    OGRSpatialReference system;
    REQUIRE(system.importFromEPSG(tiff.epsg) == OGRERR_NONE);
    char* text = nullptr;
    REQUIRE(system.exportToWkt(&text) == OGRERR_NONE);
    REQUIRE(GDALSetProjection(made, text) == CE_None);
    CPLFree(text);
  }

  GDALRasterBandH band = GDALGetRasterBand(made, 1);
  REQUIRE(GDALSetRasterNoDataValue(band, FILE_NODATA) == CE_None);
  REQUIRE(GDALRasterIO(band, GF_Write, 0, 0, tiff.width, tiff.height, tiff.pixels.data(), tiff.width, tiff.height, GDT_Int16, 0, 0) == CE_None);
  GDALClose(made);
}

// Web Mercator (EPSG:3857), in metres: a degree of longitude is this many, everywhere.
constexpr double MERCATOR_DEGREE = 111319.49079327357;

/** @return How far north of the equator the latitude `degrees` is in Web Mercator, in metres. */
[[nodiscard]] inline double mercator_north(double degrees) {
  constexpr double RADIUS = 6378137;
  constexpr double HALF_TURN = 180;

  return RADIUS * std::log(std::tan((std::numbers::pi / 4) + (degrees * std::numbers::pi / HALF_TURN / 2)));
}

/** @return A source that takes the file's pixels as they are: for files already on the grid. */
[[nodiscard]] inline miniverse::gdal::RasterSource as_it_is(std::string path) {
  return {.path = std::move(path), .band = 1, .resampling = miniverse::gdal::Resampling::Nearest};
}

/** @return `tiles` from the north west, row by row: a stream hands them over in no order that is promised. */
[[nodiscard]] inline std::vector<Raster> in_order(std::vector<Raster> tiles) {
  std::ranges::sort(tiles, [](const Raster& one, const Raster& other) {
    return one.north != other.north ? one.north > other.north : one.west < other.west;
  });

  return tiles;
}

}  // namespace test
