#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

/**
 * @file
 * What `Miniverse::verify` fails with: every difference of every layer, as one error.
 */
namespace miniverse {

/**
 * @brief What `Miniverse::verify` fails with when a layer's table is not as its model says, or its
 * reader finds something wrong with what it reads: every difference of every layer, as lines of
 * `what()` and one by one in `differences()`. A check that could not be run at all fails with its
 * own error.
 */
class TablesDiffer : public std::runtime_error
{
 public:
  /** @param differences Each the table's name, or what a reader reads, then a colon and what
   * differs; in the layers' order. */
  explicit TablesDiffer(std::vector<std::string> differences)
      : std::runtime_error(message_of(differences)), _differences(std::move(differences))
  {}

  /** @return The differences given, in the layers' order. */
  [[nodiscard]] const std::vector<std::string>& differences() const noexcept
  {
    return _differences;
  }

 private:
  std::vector<std::string> _differences;

  [[nodiscard]] static std::string message_of(const std::vector<std::string>& differences)
  {
    std::string message = "the miniverse's tables are not as its layers say:";
    for ( const std::string& difference : differences ) {
      message += "\n  " + difference;
    }

    return message;
  }

 public:
  TablesDiffer(const TablesDiffer&)                = default;
  TablesDiffer(TablesDiffer&&) noexcept            = default;
  TablesDiffer& operator=(const TablesDiffer&)     = default;
  TablesDiffer& operator=(TablesDiffer&&) noexcept = default;
  ~TablesDiffer() override                         = default;
};

}  // namespace miniverse
