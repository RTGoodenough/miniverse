#pragma once

#include <string_view>

/**
 * @file
 * The schema the library keeps for itself in a database: the registry of its layers (world/registry.hpp) and the raster
 * models' merge function (model/raster_model.hpp). Not `miniverse`: that is the first schema on the search path of a role of
 * that name, which would then make its own tables in it.
 */
namespace miniverse {

inline constexpr std::string_view INTERNAL_SCHEMA = "miniverse_internal";

}  // namespace miniverse
