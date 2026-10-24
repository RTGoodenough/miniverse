#pragma once

#include <string>

/**
 * @file
 * The unit a producer pushes and replaces: an export of a region, named by whoever pushes it. A worker never sees it; a
 * loaded layer is one whole.
 */
namespace miniverse {

/**
 * @brief An export of a region as a producer made it, named by the pusher (`Dataset{"massachusetts-260124"}`): what a push of a
 * vector kind writes under, and what a later push under the same name replaces, all at once; `Miniverse::remove` takes it
 * out. The name is free text, and means nothing to the store but sameness: refresh a region under the name it was first
 * pushed under, and keep a name to the same ground over time.
 */
struct Dataset {
  std::string name;

  [[nodiscard]] bool operator==(const Dataset&) const = default;
};

}  // namespace miniverse
