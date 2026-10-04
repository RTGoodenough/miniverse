#pragma once

/**
 * All of miniverse: the `Miniverse` (world.hpp), its built-in layer kinds (layer/), and the geometry and raster types they
 * store (geo/). A program with kinds of its own alone can leave the built-in kinds out: world.hpp, and the geo headers its
 * kinds use.
 */

#include "miniverse/concepts/miniverse.hpp"     // IWYU pragma: export
#include "miniverse/geo/column_types.hpp"       // IWYU pragma: export
#include "miniverse/geo/operations.hpp"         // IWYU pragma: export
#include "miniverse/geo/raster.hpp"             // IWYU pragma: export
#include "miniverse/geo/types.hpp"              // IWYU pragma: export
#include "miniverse/layer.hpp"                  // IWYU pragma: export
#include "miniverse/layer/elevation_layer.hpp"  // IWYU pragma: export
#include "miniverse/layer/road_layer.hpp"       // IWYU pragma: export
#include "miniverse/world.hpp"                  // IWYU pragma: export
