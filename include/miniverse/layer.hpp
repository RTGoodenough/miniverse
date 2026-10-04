#pragma once

#include <string_view>

#include "miniverse/concepts/miniverse.hpp"
#include "schemacht/schema/table.hpp"

namespace miniverse {

/**
 * @brief A layer of a miniverse: the kind `kind_t` stored in a table named while the program runs (from configuration, say).
 *
 * @code
 * miniverse::Miniverse world(conninfo, miniverse::Layer<Roads>("osm_roads"), miniverse::Layer<Elevation>("srtm"));
 * @endcode
 *
 * The name is all a layer holds. What else a kind's table is made with (an elevation's grid) is given once, to
 * `Miniverse::create_table`, and the table keeps it: a push reads it back, and `Miniverse::table_settings` gives it to a writer.
 */
template <LayerKind kind_t>
class Layer {
 public:
  /**
   * @brief The layer stored in `table`, a table in the connection's search path.
   * @throws std::invalid_argument if `table` is not a valid table name (lowercase letters, digits and underscores, not starting
   * with a digit, at most 63 bytes).
   */
  explicit Layer(std::string_view table) : _table(table) {}

  /** @brief The layer stored in the table `table` of the PostgreSQL schema `schema_name`. */
  Layer(std::string_view schema_name, std::string_view table) : _table(schema_name, table) {}

  /** @return The layer's table, which the kind's statements are aimed at (`.on(table)`); its name is `table().name()`. */
  [[nodiscard]] const schemacht::schema::Table<typename kind_t::schema_type>& table() const noexcept { return _table; }

 private:
  schemacht::schema::Table<typename kind_t::schema_type> _table;

 public:
  Layer(const Layer&) = default;
  Layer(Layer&&) noexcept = default;
  Layer& operator=(const Layer&) = default;
  Layer& operator=(Layer&&) noexcept = default;
  ~Layer() = default;
};

}  // namespace miniverse
