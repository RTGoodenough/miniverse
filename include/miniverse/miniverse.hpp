#pragma once

/**
 * @file
 * All of miniverse: the `Miniverse` (world.hpp), its built-in models (model/), and the geometry and
 * raster types they store (geo/). A program with models of its own alone can leave the built-in
 * models out: world.hpp, and the geo headers its models use.
 */

#include "miniverse/check_location.hpp"       // IWYU pragma: export
#include "miniverse/concepts/miniverse.hpp"   // IWYU pragma: export
#include "miniverse/geo/column_types.hpp"     // IWYU pragma: export
#include "miniverse/geo/grid.hpp"             // IWYU pragma: export
#include "miniverse/geo/operations.hpp"       // IWYU pragma: export
#include "miniverse/geo/raster.hpp"           // IWYU pragma: export
#include "miniverse/geo/raster_wkb.hpp"       // IWYU pragma: export
#include "miniverse/geo/types.hpp"            // IWYU pragma: export
#include "miniverse/layer.hpp"                // IWYU pragma: export
#include "miniverse/model/feature.hpp"        // IWYU pragma: export
#include "miniverse/model/feature_model.hpp"  // IWYU pragma: export
#include "miniverse/model/raster/reach.hpp"   // IWYU pragma: export
#include "miniverse/model/raster/tiles.hpp"   // IWYU pragma: export
#include "miniverse/model/raster_model.hpp"   // IWYU pragma: export
#include "miniverse/model/road_model.hpp"     // IWYU pragma: export
#include "miniverse/model/way.hpp"            // IWYU pragma: export
#include "miniverse/world.hpp"                // IWYU pragma: export
