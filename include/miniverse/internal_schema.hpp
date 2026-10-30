#pragma once

#include "schemacht/util/compile_time.hpp"

#include <string_view>

/**
 * @file
 * The schema the library keeps for itself in a database: the registry of its layers
 * (world/registry.hpp) and the raster models' merge function (model/raster_model.hpp). Not
 * `miniverse`: that is the first schema on the search path of a role of that name, which would then
 * make its own tables in it.
 */
namespace miniverse {

/** @brief The schema's name, for statements written while compiling
 * (`schemacht::util::concat_ctstrings`). */
inline constexpr schemacht::util::CTString INTERNAL_SCHEMA_NAME{"miniverse_internal"};

/** @brief The schema's name. */
inline constexpr std::string_view INTERNAL_SCHEMA = INTERNAL_SCHEMA_NAME.view();

}  // namespace miniverse
