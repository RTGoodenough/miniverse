#pragma once

#include <chrono>
#include <cstdint>
#include <string>

/**
 * @file
 * The unit a producer pushes and replaces: an export of a region, named by whoever pushes it, and what a table's list of
 * them says of one. A load never sees it.
 */
namespace miniverse {

/**
 * @brief An export of a region, named by the pusher (`Dataset{"massachusetts-260124"}`): what a push of a vector model writes
 * under, what a later push under the same name replaces, and what `Miniverse::remove` deletes. The name is free text that
 * means nothing to the store but sameness: refresh a region under the name it was first pushed under, and keep a name to
 * the same ground over time.
 */
struct Dataset {
  std::string name;  ///< Free text: see above.

  [[nodiscard]] bool operator==(const Dataset&) const = default;
};

/** @brief A dataset of a layer, as `Miniverse::datasets` lists them. */
struct DatasetInfo {
  std::string                           name;
  std::chrono::system_clock::time_point pushed;    ///< When the push that last wrote it committed, by the server's clock.
  std::uint64_t                         rows = 0;  ///< Counted when listed.

  [[nodiscard]] bool operator==(const DatasetInfo&) const = default;
};

}  // namespace miniverse
