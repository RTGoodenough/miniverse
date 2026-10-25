#pragma once

#include <string>

/**
 * @file
 * The unit a producer pushes and replaces: an export of a region, named by whoever pushes it. A load never sees it.
 */
namespace miniverse {

/**
 * @brief An export of a region, named by the pusher (`Dataset{"massachusetts-260124"}`): what a push of a vector kind writes
 * under, what a later push under the same name replaces, and what `Miniverse::remove` deletes. The name is free text that
 * means nothing to the store but sameness: refresh a region under the name it was first pushed under, and keep a name to
 * the same ground over time.
 */
struct Dataset {
  std::string name;  ///< Free text: see above.

  [[nodiscard]] bool operator==(const Dataset&) const = default;
};

}  // namespace miniverse
