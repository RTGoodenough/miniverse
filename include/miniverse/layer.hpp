#pragma once

#include <concepts>
#include <string_view>
#include <utility>

#include "miniverse/concepts/miniverse.hpp"
#include "schemacht/schema/table.hpp"

namespace miniverse {

/** @brief The settings of a layer kind that has none (`RoadLayer`): its table is made the same way every time. */
struct NoSettings {};

/**
 * @brief A layer of a miniverse: the kind `kind_t` stored in a table named while the program runs (from configuration, say),
 * with the kind's settings, if it has any (an elevation's grid).
 *
 * @code
 * miniverse::Miniverse world(
 *     conninfo, miniverse::Layer<Roads>("osm_roads"),
 *     miniverse::Layer<Elevation>("srtm", {.pixels_per_degree = 3600, .tile_pixels = 256, .nodata = -32768})
 * );
 * @endcode
 *
 * The settings are configuration, as the table's name is: the table is made with them (`Miniverse::create_table`) and keeps
 * them, and what is pushed is written with them. A kind's table can refuse rows written with other settings (an elevation
 * table's raster constraints refuse tiles of another grid), and `Miniverse::table_settings` reads back the table's own.
 */
template <LayerKind kind_t>
class Layer {
 public:
  /**
   * @brief The layer stored in `table`, a table in the connection's search path, with `settings`.
   * @throws std::invalid_argument if `table` is not a valid table name (lowercase letters, digits and underscores, not starting
   * with a digit, at most 63 bytes).
   */
  Layer(std::string_view table, kind_t::settings_type settings) : _table(table), _settings(std::move(settings)) {}

  /** @brief The layer stored in the table `table` of the PostgreSQL schema `schema_name`, with `settings`. */
  Layer(std::string_view schema_name, std::string_view table, kind_t::settings_type settings)
      : _table(schema_name, table), _settings(std::move(settings)) {}

  /** @brief As above, for a kind without settings. */
  explicit Layer(std::string_view table)
    requires std::same_as<typename kind_t::settings_type, NoSettings>
      : Layer(table, NoSettings{}) {}

  /** @brief As above, for a kind without settings. */
  Layer(std::string_view schema_name, std::string_view table)
    requires std::same_as<typename kind_t::settings_type, NoSettings>
      : Layer(schema_name, table, NoSettings{}) {}

  /** @return The layer's table, which the kind's statements are aimed at (`.on(table)`); its name is `table().name()`. */
  [[nodiscard]] const schemacht::schema::Table<typename kind_t::schema_type>& table() const noexcept { return _table; }

  /** @return The settings the layer was made with. */
  [[nodiscard]] const kind_t::settings_type& settings() const noexcept { return _settings; }

 private:
  schemacht::schema::Table<typename kind_t::schema_type> _table;
  kind_t::settings_type                                  _settings;

 public:
  Layer(const Layer&) = default;
  Layer(Layer&&) noexcept = default;
  Layer& operator=(const Layer&) = default;
  Layer& operator=(Layer&&) noexcept = default;
  ~Layer() = default;
};

}  // namespace miniverse
