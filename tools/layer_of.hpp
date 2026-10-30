#pragma once

#include <string>

#include "miniverse/layer.hpp"

/**
 * @file
 * The layer a table's name on the command line names: `Layer<Model, "table">`, so a command asks
 * its world for `"table"`.
 */
namespace tool {

/** @brief The one layer of a command's world, of `model_t`, called `table` in the program whatever
 * its table is called. */
template <typename model_t>
using Layer = miniverse::Layer<model_t, "table">;

/**
 * @return The layer of `name`: `table`, in the connection's search path, or `schema.table`.
 * @throws std::invalid_argument for a name that is neither, as `miniverse::Layer` does.
 */
template <typename model_t>
[[nodiscard]] Layer<model_t> layer_of(const std::string& name)
{
  const std::string::size_type dot = name.find('.');
  if ( dot == std::string::npos ) {
    return Layer<model_t>(name);
  }

  return Layer<model_t>(name.substr(0, dot), name.substr(dot + 1));
}

}  // namespace tool
