#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/reader.hpp"  // IWYU pragma: export
#include "schemacht/schema/table.hpp"

namespace miniverse {

/**
 * @brief A layer of a miniverse: the kind `kind_t`, stored in a table named while the program runs (from configuration, say), or
 * read by a `Reader` (reader.hpp): a file, or data of the program's own.
 *
 * @code
 * miniverse::Miniverse world(conninfo, miniverse::Layer<Roads>("osm_roads"), miniverse::Layer<Elevation>("srtm"));
 * @endcode
 *
 * The table's name, or the reader, is all a layer holds. What else a kind's table is made with (an elevation's grid) is given
 * once, to `Miniverse::create_table`, and the table keeps it: a push reads it back, and `Miniverse::table_settings` gives it to a
 * writer. A layer with a reader is only read, and its reader says what its settings are.
 */
template <LayerKind kind_t>
class Layer {
 public:
  /**
   * @brief The layer stored in `table`, a table in the connection's search path.
   * @throws std::invalid_argument if `table` is not a valid table name (lowercase letters, digits and underscores, not starting
   * with a digit, at most 63 bytes).
   */
  explicit Layer(std::string_view table) : _table(std::in_place, table) {}

  /** @brief The layer stored in the table `table` of the PostgreSQL schema `schema_name`. */
  Layer(std::string_view schema_name, std::string_view table) : _table(std::in_place, schema_name, table) {}

  /**
   * @brief The layer read by `reader`, which the layer's copies and the loads that run share.
   * @throws std::invalid_argument if there is no reader.
   */
  explicit Layer(std::shared_ptr<const Reader<kind_t>> reader) : _reader(std::move(reader)) {
    if ( ! _reader ) {
      throw std::invalid_argument("a layer is a table or has a reader: this one was given no reader");
    }
  }

  /** @return Whether the layer is stored in a table; else it has a reader. */
  [[nodiscard]] bool is_table() const noexcept { return _table.has_value(); }

  /**
   * @return The layer's table, which the kind's statements are aimed at (`.on(table)`); its name is `table().name()`.
   * @throws std::logic_error if the layer is not a table.
   */
  [[nodiscard]] const schemacht::schema::Table<typename kind_t::schema_type>& table() const {
    if ( ! _table ) {
      throw std::logic_error("the layer read from " + name() + " is not a table");
    }

    return *_table;
  }

  /**
   * @return What the layer is called in a message: its table as a statement writes it (`"gis"."roads"`), or what its reader reads.
   * @throws std::logic_error if the layer was moved from, and so is neither.
   */
  [[nodiscard]] std::string name() const {
    if ( _table ) {
      return _table->name().quoted();
    }

    if ( ! _reader ) {
      throw std::logic_error("a layer that was moved from is no table and has no reader");
    }

    return _reader->name();
  }

  /** @return The layer's reader; none, if the layer is a table. */
  [[nodiscard]] const std::shared_ptr<const Reader<kind_t>>& reader() const noexcept { return _reader; }

 private:
  std::optional<schemacht::schema::Table<typename kind_t::schema_type>> _table;  // or
  std::shared_ptr<const Reader<kind_t>>                                 _reader;

 public:
  Layer(const Layer&) = default;
  Layer(Layer&&) noexcept = default;
  Layer& operator=(const Layer&) = default;
  Layer& operator=(Layer&&) noexcept = default;
  ~Layer() = default;
};

}  // namespace miniverse
