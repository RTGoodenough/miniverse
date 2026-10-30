#pragma once

/**
 * @file
 * All of the component `miniverse::gdal`: files read with GDAL into layers (vector.hpp,
 * raster.hpp), files as layers (feature_file.hpp, raster_file.hpp), and a loaded raster as a GDAL
 * dataset (dataset.hpp).
 */

#include "miniverse/gdal/dataset.hpp"       // IWYU pragma: export
#include "miniverse/gdal/feature_file.hpp"  // IWYU pragma: export
#include "miniverse/gdal/raster.hpp"        // IWYU pragma: export
#include "miniverse/gdal/raster_file.hpp"   // IWYU pragma: export
#include "miniverse/gdal/vector.hpp"        // IWYU pragma: export
