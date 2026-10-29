#pragma once

#include <concepts>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/reader.hpp"  // IWYU pragma: export
#include "schemacht/schema/table.hpp"
#include "schemacht/util/compile_time.hpp"

/**
 * @file
 * A layer of a miniverse: a model under a name of the program's, stored in a table named at run time or read by a reader.
 */
namespace miniverse {

/**
 * @brief A layer of a miniverse: the model `model_t` under the name `name`, which the program asks for it by (`load<"roads">`),
 * stored in a table named while the program runs (from configuration, say), or read by a `Reader` (reader.hpp): a file, or
 * data of the program's own.
 *
 * @code
 * using Roads = miniverse::Layer<miniverse::RoadModel, "roads">;
 * using Elevation = miniverse::Layer<miniverse::RasterModel<std::int16_t>, "elevation">;
 *
 * miniverse::Miniverse world(conninfo, Roads("osm_roads"), Elevation());  // the second in a table called elevation
 * @endcode
 *
 * The name is the layer's in the program alone; two layers of one model are two names, in two tables of the same shape. The
 * table's name, or the reader, is all a layer holds; a model's settings (a raster's grid) belong to the table, see
 * `Miniverse::create_table`. A layer with a reader is only read.
 */
template <LayerModel model_t, schemacht::util::CTString name>
class Layer {
 public:
  using model_type = model_t;

  /** @brief The layer's name: what the program asks a miniverse for it by. */
  static constexpr auto NAME = name;

  /**
   * @brief The layer stored in the table called `NAME`, in the connection's search path.
   * @throws std::invalid_argument if the name is not a valid table name, as below.
   */
  Layer() : Layer(NAME.view()) {}

  /**
   * @brief The layer stored in `table`, a table in the connection's search path.
   * @throws std::invalid_argument if `table` is not a valid table name (lowercase letters, digits and underscores, not starting
   * with a digit, at most 63 bytes).
   */
  explicit Layer(std::string_view table) : _table(std::in_place, table) {}

  /**
   * @brief The layer stored in the table `table` of the PostgreSQL schema `schema_name`.
   * @throws std::invalid_argument if either is not a valid name, as above.
   */
  Layer(std::string_view schema_name, std::string_view table) : _table(std::in_place, schema_name, table) {}

  /**
   * @brief The layer read by `reader`, which the layer's copies and the loads that run share.
   * @throws std::invalid_argument if there is no reader.
   */
  explicit Layer(std::shared_ptr<const Reader<model_t>> reader) : _reader(std::move(reader)) {
    if ( ! _reader ) {
      throw std::invalid_argument("a layer is a table or has a reader: " + std::string(NAME.view()) + " was given no reader");
    }
  }

  /** @return Whether the layer is stored in a table; else it has a reader. */
  [[nodiscard]] bool is_table() const noexcept { return _table.has_value(); }

  /**
   * @return The layer's table, which the model's statements are aimed at (`.on(table)`); its name is `table().name()`.
   * @throws std::logic_error if the layer is not a table.
   */
  [[nodiscard]] const schemacht::schema::Table<typename model_t::schema_type>& table() const {
    if ( ! _table ) {
      throw std::logic_error("the layer " + std::string(NAME.view()) + ", read from " + source() + ", is not a table");
    }

    return *_table;
  }

  /**
   * @return Where the layer is, for a message: its table as a statement writes it (`"gis"."roads"`), or what its reader reads.
   * @throws std::logic_error if the layer has neither, which only one that was moved from can lack.
   */
  [[nodiscard]] std::string source() const {
    if ( _table ) {
      return _table->name().quoted();
    }

    if ( ! _reader ) {
      throw std::logic_error("the layer " + std::string(NAME.view()) + " has no table and no reader");
    }

    return _reader->name();
  }

  /** @return The layer's reader; none, if the layer is a table. */
  [[nodiscard]] const std::shared_ptr<const Reader<model_t>>& reader() const noexcept { return _reader; }

 private:
  std::optional<schemacht::schema::Table<typename model_t::schema_type>> _table;
  std::shared_ptr<const Reader<model_t>>                                 _reader;

 public:
  Layer(const Layer&) = default;
  Layer(Layer&&) noexcept = default;
  Layer& operator=(const Layer&) = default;
  Layer& operator=(Layer&&) noexcept = default;
  ~Layer() = default;
};

/** @brief Whether `layer_t` is a `Layer`: what a `Miniverse` is made of. */
template <typename layer_t>
concept IsLayer = requires {
  typename layer_t::model_type;
  layer_t::NAME;
} && std::same_as<layer_t, Layer<typename layer_t::model_type, layer_t::NAME>>;

}  // namespace miniverse
