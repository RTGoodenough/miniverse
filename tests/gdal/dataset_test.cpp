// A raster as a GDAL dataset in memory: what GDAL then says of it, and what it reads back from it.

#include <catch2/catch_test_macros.hpp>

#include <cpl_error.h>
#include <gdal.h>
#include <gdal_priv.h>
#include <ogr_spatialref.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "miniverse/gdal/dataset.hpp"
#include "miniverse/gdal/raster.hpp"
#include "miniverse/geo/raster.hpp"
#include "support/files.hpp"

namespace geo = miniverse::geo;
namespace gdal = miniverse::gdal;

using test::Files;

namespace {

constexpr std::int16_t NODATA = -32768;

// Four pixels to a degree, in tiles of one degree.
constexpr geo::Grid<std::int16_t> GRID{.pixels_per_degree = 4, .tile_pixels = 4, .nodata = NODATA};

/** @return 8 by 4 pixels of the grid from (1, 3), each `column * 100 + row`, but the first, which has no data. */
[[nodiscard]] geo::Raster<std::int16_t> heights() {
  geo::Raster<std::int16_t> raster{.west = 1, .north = 3, .pixel_width = 0.25, .pixel_height = 0.25, .width = 8, .height = 4, .nodata = NODATA, .pixels = {}};
  for ( std::size_t row = 0; row < raster.height; ++row ) {
    for ( std::size_t column = 0; column < raster.width; ++column ) {
      raster.pixels.push_back(static_cast<std::int16_t>((column * 100) + row));
    }
  }
  raster.pixels.front() = NODATA;

  return raster;
}

}  // namespace

TEST_CASE("gdal dataset: a raster is a dataset in memory of its size, its place, WGS 84 and its nodata", "[gdal]") {
  const GDALDatasetUniquePtr dataset = gdal::dataset_of(heights());

  REQUIRE(dataset);
  CHECK(std::string(dataset->GetDriver()->GetDescription()) == "MEM");
  CHECK(dataset->GetRasterXSize() == 8);
  CHECK(dataset->GetRasterYSize() == 4);
  REQUIRE(dataset->GetRasterCount() == 1);

  std::array<double, 6> position{};
  REQUIRE(dataset->GetGeoTransform(position.data()) == CE_None);
  CHECK(position == std::array<double, 6>{1, 0.25, 0, 3, 0, -0.25});

  const OGRSpatialReference* system = dataset->GetSpatialRef();
  REQUIRE(system != nullptr);
  CHECK(std::string(system->GetAuthorityCode(nullptr)) == "4326");
  CHECK(system->GetDataAxisToSRSAxisMapping() == std::vector<int>{2, 1});  // longitude first

  GDALRasterBand* band = dataset->GetRasterBand(1);
  int             has_nodata = 0;
  CHECK(band->GetRasterDataType() == GDT_Int16);
  CHECK(band->GetNoDataValue(&has_nodata) == NODATA);
  CHECK(has_nodata != 0);
}

TEST_CASE("gdal dataset: the dataset has the raster's pixels, and keeps them when the raster is gone", "[gdal]") {
  GDALDatasetUniquePtr      dataset;
  std::vector<std::int16_t> expected;
  {
    const geo::Raster<std::int16_t> raster = heights();
    expected = raster.pixels;
    dataset = gdal::dataset_of(raster);
  }

  std::vector<std::int16_t> read(expected.size());
  REQUIRE(dataset->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 8, 4, read.data(), 8, 4, GDT_Int16, 0, 0) == CE_None);

  CHECK(read == expected);
}

TEST_CASE("gdal dataset: a raster of reals is a dataset of reals", "[gdal]") {
  const geo::Raster<float> raster{
      .west = -71.5, .north = 42.25, .pixel_width = 0.5, .pixel_height = 0.5, .width = 2, .height = 2, .nodata = -9999.0F, .pixels = {1.5F, -9999.0F, 0.25F, 4321.125F}
  };

  const GDALDatasetUniquePtr dataset = gdal::dataset_of(raster);
  std::vector<float>         read(4);
  REQUIRE(dataset->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 2, 2, read.data(), 2, 2, GDT_Float32, 0, 0) == CE_None);

  CHECK(dataset->GetRasterBand(1)->GetRasterDataType() == GDT_Float32);
  CHECK(dataset->GetRasterBand(1)->GetNoDataValue() == -9999.0);
  CHECK(read == raster.pixels);
}

TEST_CASE("gdal dataset: written as a GeoTIFF, it reads back as the raster it was made of", "[gdal]") {
  const Files                     files;
  const std::string               path = files.path_of("heights.tif");
  const geo::Raster<std::int16_t> raster = heights();

  {
    const GDALDatasetUniquePtr dataset = gdal::dataset_of(raster);
    const GDALDatasetUniquePtr written(GetGDALDriverManager()->GetDriverByName("GTiff")->CreateCopy(path.c_str(), dataset.get(), 0, nullptr, nullptr, nullptr));
    REQUIRE(written);
  }

  CHECK(gdal::read_raster<std::int16_t>({.path = path, .band = 1, .resampling = gdal::Resampling::Nearest}, GRID) == raster);
}

TEST_CASE("gdal dataset: a raster of no pixels, or whose pixels are not width times height, is no dataset", "[gdal]") {
  CHECK_THROWS_AS(gdal::dataset_of(geo::Raster<std::int16_t>{}), std::invalid_argument);

  geo::Raster<std::int16_t> short_of_pixels = heights();
  short_of_pixels.pixels.pop_back();
  CHECK_THROWS_AS(gdal::dataset_of(short_of_pixels), std::invalid_argument);
}
