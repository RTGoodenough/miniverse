#include "miniverse/world/datasets.hpp"

#include <cstdint>
#include <limits>
#include <random>

namespace miniverse::detail {

std::int64_t stamp()
{
  std::random_device                          random;
  std::uniform_int_distribution<std::int64_t> any(std::numeric_limits<std::int64_t>::min(),
                                                  std::numeric_limits<std::int64_t>::max());

  return any(random);
}

}  // namespace miniverse::detail
