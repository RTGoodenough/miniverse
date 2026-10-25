#pragma once

#include <string>

#include "miniverse/layer.hpp"

/**
 * @file
 * The layer a table's name on the command line names.
 */
namespace tool {

/**
 * @return The layer of `name`: `table`, in the connection's search path, or `schema.table`.
 * @throws std::invalid_argument for a name that is neither, as `miniverse::Layer` does.
 */
template <typename kind_t>
[[nodiscard]] miniverse::Layer<kind_t> layer_of(const std::string& name) {
  const std::string::size_type dot = name.find('.');
  if ( dot == std::string::npos ) {
    return miniverse::Layer<kind_t>(name);
  }

  return miniverse::Layer<kind_t>(name.substr(0, dot), name.substr(dot + 1));
}

}  // namespace tool
