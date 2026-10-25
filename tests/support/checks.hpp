#pragma once

// What a load or a kind's `to_rows` gives, reduced for a check: the ids of features or ways, and the rows of batches in order.

#include <concepts>
#include <cstdint>
#include <iterator>
#include <vector>

namespace test {

/** @return The ids of `items`, features or ways, in their order. */
template <typename item_t>
  requires requires(const item_t& item) {
    { item.id } -> std::convertible_to<std::int64_t>;
  }
[[nodiscard]] std::vector<std::int64_t> ids(const std::vector<item_t>& items) {
  std::vector<std::int64_t> result;

  result.reserve(items.size());
  for ( const item_t& item : items ) {
    result.push_back(item.id);
  }

  return result;
}

/** @return The rows of `batches`, in order: for checks that don't care about the batching. */
template <std::movable row_t>
[[nodiscard]] std::vector<row_t> rows(std::vector<std::vector<row_t>> batches) {
  std::vector<row_t> all;

  for ( std::vector<row_t>& batch : batches ) {
    all.insert(all.end(), std::move_iterator(batch.begin()), std::move_iterator(batch.end()));
  }

  return all;
}

}  // namespace test
