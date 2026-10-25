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
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/gdal/raster.hpp"
#include "miniverse/geo/concepts/raster.hpp"
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
template <miniverse::geo::Pixel pixel_t = std::int16_t>
struct Tiff {
  int                   epsg = 4326;
  double                west = 0;
  double                north = 0;
  double                pixel = 0.25;  ///< A pixel's size each way, in the units of `epsg`.
  int                   width = 0;
  int                   height = 0;
  std::vector<pixel_t>  pixels;                ///< Band 1, in rows from the north-west, `width * height` of them.
  std::vector<pixel_t>  second_band;           ///< Band 2, as many pixels; none for a file of one band.
  std::optional<double> nodata = FILE_NODATA;  ///< Of each band; none for a file that names no nodata.
  bool                  turned = false;        ///< Turned a quarter clockwise: its columns run south and its rows west, from (west, north) on.
};

/** @return The GDAL type that is `pixel_t`. */
template <miniverse::geo::Pixel pixel_t>
[[nodiscard]] constexpr GDALDataType type_of() {
  if constexpr ( std::same_as<pixel_t, std::int8_t> ) {
    return GDT_Int8;

  } else if constexpr ( std::same_as<pixel_t, std::int16_t> ) {
    return GDT_Int16;

  } else {
    static_assert(std::same_as<pixel_t, float>, "a test of a new pixel type needs its GDAL type here");

    return GDT_Float32;
  }
}

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

/** @brief Writes `tiff` as a GeoTIFF at `path`, of one band or two. An `epsg` of 0 names no system. */
template <miniverse::geo::Pixel pixel_t = std::int16_t>
inline void write_tiff(const std::string& path, Tiff<pixel_t> tiff) {
  GDALAllRegister();

  const int    bands = tiff.second_band.empty() ? 1 : 2;
  GDALDatasetH made = GDALCreate(GDALGetDriverByName("GTiff"), path.c_str(), tiff.width, tiff.height, bands, type_of<pixel_t>(), nullptr);
  REQUIRE(made != nullptr);

  std::array<double, 6> position{tiff.west, tiff.pixel, 0, tiff.north, 0, -tiff.pixel};
  if ( tiff.turned ) {
    position = {tiff.west + (tiff.height * tiff.pixel), 0, -tiff.pixel, tiff.north, -tiff.pixel, 0};  // from its north-east corner
  }
  REQUIRE(GDALSetGeoTransform(made, position.data()) == CE_None);

  if ( tiff.epsg != 0 ) {
    OGRSpatialReference system;
    REQUIRE(system.importFromEPSG(tiff.epsg) == OGRERR_NONE);
    char* text = nullptr;
    REQUIRE(system.exportToWkt(&text) == OGRERR_NONE);
    REQUIRE(GDALSetProjection(made, text) == CE_None);
    CPLFree(text);
  }

  for ( int number = 1; number <= bands; ++number ) {
    std::vector<pixel_t>& pixels = number == 1 ? tiff.pixels : tiff.second_band;
    GDALRasterBandH       band = GDALGetRasterBand(made, number);
    if ( tiff.nodata ) {
      REQUIRE(GDALSetRasterNoDataValue(band, *tiff.nodata) == CE_None);
    }
    REQUIRE(GDALRasterIO(band, GF_Write, 0, 0, tiff.width, tiff.height, pixels.data(), tiff.width, tiff.height, type_of<pixel_t>(), 0, 0) == CE_None);
  }
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
