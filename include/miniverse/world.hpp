#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/dataset.hpp"  // IWYU pragma: export
#include "miniverse/geo/types.hpp"
#include "miniverse/layer.hpp"  // IWYU pragma: export
#include "miniverse/world/async.hpp"
#include "miniverse/world/datasets.hpp"
#include "miniverse/world/load.hpp"
#include "miniverse/world/push.hpp"
#include "miniverse/world/push_in_parts.hpp"  // IWYU pragma: export
#include "miniverse/world/settings.hpp"
#include "miniverse/world/tables.hpp"
#include "miniverse/world/verify.hpp"  // IWYU pragma: export
#include "schemacht/postgres/database.hpp"
#include "schemacht/schema/table_name.hpp"
#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/futures.hpp"

/**
 * @file
 * A miniverse: the layers of the world a worker reads, each one sort of data in a PostGIS table or behind a reader, loaded by
 * polygon. This header holds `Miniverse` alone; miniverse.hpp adds the built-in models.
 *
 * @code
 * using Roads = miniverse::Layer<miniverse::RoadModel, "roads">;  // model/road_model.hpp
 *
 * miniverse::Miniverse world("host=db dbname=gis user=worker", Roads("osm_roads"));
 *
 * const miniverse::geo::Polygon area = ...;                               // WGS 84 longitude and latitude
 * std::future<miniverse::Ways> roads = world.load<"roads">(area);         // the ways that intersect it
 * for ( const miniverse::Way& way : roads.get() ) { ... }
 * @endcode
 *
 * The layers are the template's arguments, each a model under a name, so asking for a name the miniverse does not hold does not
 * compile. Each layer names its table when the miniverse is made. One pool of connections (a schemacht `postgres::Database`)
 * serves every layer. The calls that read and write data return a `std::future`, and what they find wrong arrives through
 * it, not from the call; a call that cannot start, or makes no sense (a write to a layer that is only read), throws. The
 * constructor, `create_table` and `drop_tables` wait and throw. The miniverse must outlive the futures it hands
 * out. A layer made from a `Reader` (reader.hpp) is read from a file, or from data of the program's own, by the same calls; a
 * miniverse of readers alone is made without a connection string.
 *
 * Threads: `load`, `table_settings`, `push`, `begin_push`, `remove`, `datasets` and `verify` each run on a thread started
 * for the call, which the future waits for if it is dropped before it is done; a `stream` of a table runs its callback on the
 * pool's worker threads, a `stream` of a reader on a thread started for the call. The parts of a push in parts run on the
 * push's own thread (`PushInParts`). The constructor, `create_table` and `drop_tables` run on the calling thread and wait.
 * The pool itself is schemacht's: one reactor thread and two workers.
 *
 * `Miniverse` is a front: it holds the layers and the pool, and tells a table from a reader. What a call does is in world/,
 * one idea per header.
 */
namespace miniverse {

namespace detail {

/** @return Where the layer called `name` is among `layer_ts`; their count, if none is. */
template <schemacht::util::CTString name, IsLayer... layer_ts>
[[nodiscard]] consteval std::size_t index_of_layer() {
  constexpr std::array<std::string_view, sizeof...(layer_ts)> NAMES{layer_ts::NAME.view()...};
  for ( std::size_t i = 0; i < NAMES.size(); ++i ) {
    if ( NAMES.at(i) == name.view() ) {
      return i;
    }
  }

  return NAMES.size();
}

/** @brief The layer called `name` among `layer_ts`: `type`. A name of none fails the assertion, which is then the first error. */
template <schemacht::util::CTString name, IsLayer... layer_ts>
struct LayerNamed {
  static_assert(index_of_layer<name, layer_ts...>() < sizeof...(layer_ts), "this miniverse has no such layer: name it in Miniverse<...>");

  // Clamped, so that a name of none is still a type and the errors after the assertion are few.
  using type = std::tuple_element_t<std::min(index_of_layer<name, layer_ts...>(), sizeof...(layer_ts) - 1), std::tuple<layer_ts...>>;
};

}  // namespace detail

/** @brief The layers `layer_ts`, each a model under a name, in its own table of one PostGIS database or read by a reader. */
template <IsLayer... layer_ts>
class Miniverse {
 public:
  /** @brief The model of the layer called `name`: `ModelOf<"roads">::result_type` is what its loads give. */
  template <schemacht::util::CTString name>
  using ModelOf = typename detail::LayerNamed<name, layer_ts...>::type::model_type;

  /**
   * @brief Opens the pool of connections to the database at `conninfo` (a libpq connection string or URL), for `layers`.
   * @throws std::invalid_argument if two layers name the same table, before anything is opened. Names are compared as written:
   * `gis.roads` and `roads` count as different even when the search path makes them the same table.
   * @throws std::invalid_argument if `options` asks for no connections, no workers or an empty chunk (schemacht's `Database`).
   * @throws std::runtime_error if a connection can't be made, or the session settings can't be applied.
   */
  Miniverse(const std::string& conninfo, const schemacht::postgres::Database::Options& options, layer_ts... layers)
      : _layers(distinct_tables(std::move(layers)...)), _database(std::in_place, conninfo, options) {}

  /** @brief As above, with the pool's default options. */
  explicit Miniverse(const std::string& conninfo, layer_ts... layers) : Miniverse(conninfo, {}, std::move(layers)...) {}

  /**
   * @brief A miniverse with no database, for `layers` that each have a reader: nothing is connected to.
   * @throws std::invalid_argument if a layer is a table: that needs a connection string.
   */
  explicit Miniverse(layer_ts... layers) : _layers(all_with_readers(std::move(layers)...)) {}

  /** @return Whether the miniverse has a layer called `name`. */
  template <schemacht::util::CTString name>
  [[nodiscard]] static consteval bool holds() {
    return detail::index_of_layer<name, layer_ts...>() < sizeof...(layer_ts);
  }

  /**
   * @return The name of the table the layer `name` is stored in.
   * @throws std::logic_error if the layer is not a table: it has a reader.
   */
  template <schemacht::util::CTString name>
  [[nodiscard]] const schemacht::schema::TableName& table_name() const {
    return layer<name>().table().name();
  }

  /**
   * @return A future for what the layer `name` holds in `location`, a polygon in WGS 84 longitude and latitude (what that is,
   * the model says: for a `RoadModel`, the ways that intersect it). It holds the error instead if the load failed.
   *
   * The load runs on a thread started for it: the table's statement is waited for there and the model makes the result of
   * the rows there, or the reader reads there. The future waits for that thread if it is dropped.
   */
  template <schemacht::util::CTString name>
  [[nodiscard]] std::future<typename ModelOf<name>::result_type> load(const geo::Polygon& location) {
    using model_t = ModelOf<name>;

    if ( const auto& reader = layer<name>().reader(); reader ) {
      return detail::load<model_t>(reader, location);
    }

    return detail::load<model_t>(database(), layer<name>().table(), location);
  }

  /**
   * @return A future that completes once what the layer `name` holds in `location` has been handed to `on_chunk`, a chunk at a
   * time, or `on_chunk` stopped it; or holds the error. Nothing holds the whole result.
   *
   * A chunk is `read.chunk_rows` rows of what a load would give (the last chunk holds what is left), as `ChunkOf` says: for a
   * feature or road layer that many features, in the load's order; for a raster layer that many whole tiles, in no particular
   * order. A tile is megabytes, so ask for a few at a time.
   *
   * `on_chunk` runs on a thread of the pool's, one call at a time. It returns `void`, or a `bool` where `false` stops the load.
   * What it throws is the load's error, as is a connection lost under the load; chunks handed over before an error stay
   * handed over. It must not wait on another of this miniverse's futures. For a layer with a reader, the chunks come in the
   * reader's order (a file's own), on a thread started for the load, which the future waits for if it is dropped.
   */
  template <schemacht::util::CTString name, ChunkCallback<ModelOf<name>> callback_t>
  [[nodiscard]] std::future<void> stream(const geo::Polygon& location, callback_t on_chunk, const schemacht::postgres::ReadOptions& read) {
    using model_t = ModelOf<name>;

    if ( const auto& reader = layer<name>().reader(); reader ) {
      return detail::stream<model_t>(reader, location, std::move(on_chunk), read.chunk_rows);
    }

    return detail::stream<model_t>(database(), layer<name>().table(), location, std::move(on_chunk), read);
  }

  /**
   * @brief As above, with the pool's default chunk size (`Database::Options::read`, 1000 rows unless set otherwise): right for
   * features, far too many for a raster's tiles.
   */
  template <schemacht::util::CTString name, ChunkCallback<ModelOf<name>> callback_t>
  [[nodiscard]] std::future<void> stream(const geo::Polygon& location, callback_t on_chunk) {
    return stream<name>(location, std::move(on_chunk), _database ? _database->read_options() : schemacht::postgres::ReadOptions{});
  }

  /**
   * @return A future for the settings the layer `name`'s table was made with (a raster's grid), for a writer to warp its
   * source onto. It holds the error instead if the table does not exist or has no settings (it was not made by
   * `create_table`). For a layer with a reader, they are the reader's.
   */
  template <schemacht::util::CTString name>
    requires WritesSettings<ModelOf<name>>
  [[nodiscard]] std::future<SettingsOf<ModelOf<name>>> table_settings() {
    using model_t = ModelOf<name>;

    if ( const auto& reader = layer<name>().reader(); reader ) {
      return detail::reader_settings<model_t>(reader);
    }

    return detail::table_settings<model_t>(database(), layer<name>().table());
  }

  /**
   * @return A future that completes once `data` is written to the layer `name`'s table, or holds the error (nothing is then
   * written). `data` is moved into the rows: pass it with `std::move` unless it is still needed.
   *
   * The push runs on a thread started for it; the future waits for that thread if it is dropped. There the model makes the rows
   * (with the table's settings, read first, for a model that has them) and writes them in its batches, one statement each, in
   * one transaction: the push holds every row of `data`, and one statement's text at a time, and writes all of `data` or
   * none. The transaction is begun before the rows are made, and holds one of the pool's connections until it ends. For
   * data too large to hold at once, `begin_push` takes it in parts.
   *
   * The model's `write_statement` says what happens to a row already there: a `RasterModel`'s tile is merged onto it (new pixels
   * win, nodata keeps the old pixel), and overlapping pushes merge in the order they commit, the later winning where both have
   * data. A layer of a model with datasets is pushed with one: `push<"name">(dataset, data)`.
   *
   * @throws std::logic_error if the layer has a reader: it is only read.
   */
  template <schemacht::util::CTString name>
  [[nodiscard]] std::future<void> push(typename ModelOf<name>::result_type data) {
    using model_t = ModelOf<name>;
    static_assert(! WritesDatasets<model_t>, "this layer's rows belong to a dataset: push<\"name\">(Dataset{\"name\"}, data) replaces it");

    return detail::push<model_t>(database(), written<name>(), std::move(data));
  }

  /**
   * @return A future for a push of the layer `name` that takes its data in parts, all written or none (`PushInParts`). On a
   * thread started for the call, the table's settings are read if the model has any, the transaction is begun, and the push's
   * own thread, which its parts then run on, is started. It holds the error instead if any of that fails. Commit or drop the
   * push before the miniverse ends.
   * @throws std::logic_error if the layer has a reader: it is only read.
   */
  template <schemacht::util::CTString name>
  [[nodiscard]] std::future<PushInParts<ModelOf<name>>> begin_push() {
    using model_t = ModelOf<name>;
    static_assert(! WritesDatasets<model_t>, "this layer's rows belong to a dataset: begin_push<\"name\">(Dataset{\"name\"}) replaces it");

    return detail::begin_push<model_t>(database(), written<name>());
  }

  /**
   * @return A future that completes once `data` has replaced `dataset` in the layer `name`'s table, or holds the error
   * (nothing is then changed). On a thread started for the push, in one transaction: the dataset is locked, the rows are
   * written, and the dataset's rows this push did not write are deleted. The first push of a name writes the dataset; every
   * later push replaces it, and a row the model can tell is the same (a `RoadModel`'s way, by its id) keeps its row. Two
   * pushes of one dataset into one table take turns; pushes of different datasets, or into different tables, run side by
   * side. What a dataset is, and what it asks of the data, is in `Dataset` and the model's own doc.
   *
   * @throws std::logic_error if the layer has a reader: it is only read.
   */
  template <schemacht::util::CTString name>
  [[nodiscard]] std::future<void> push(const Dataset& dataset, typename ModelOf<name>::result_type data) {
    using model_t = ModelOf<name>;
    static_assert(WritesDatasets<model_t>, "this layer's model has no datasets: push<\"name\">(data) writes it");

    if constexpr ( WritesDatasets<model_t> ) {
      return detail::push<model_t>(database(), written<name>(), dataset, std::move(data));
    }
  }

  /**
   * @return A future for a push of `dataset` into the layer `name` that takes its data in parts (`PushInParts`). The dataset is
   * replaced when the push commits, as `push(dataset, data)` replaces it; a commit with no parts leaves it empty. On a thread
   * started for the call, the transaction is begun, the dataset locked, and the push's own thread, which its parts then run
   * on, is started. It holds the error instead if any of that fails. Commit or drop the push before the miniverse ends.
   * @throws std::logic_error if the layer has a reader: it is only read.
   */
  template <schemacht::util::CTString name>
  [[nodiscard]] std::future<PushInParts<ModelOf<name>>> begin_push(const Dataset& dataset) {
    using model_t = ModelOf<name>;
    static_assert(WritesDatasets<model_t>, "this layer's model has no datasets: begin_push<\"name\">() writes it");

    if constexpr ( WritesDatasets<model_t> ) {
      return detail::begin_push<model_t>(database(), written<name>(), dataset);
    }
  }

  /**
   * @return A future for how many rows of `dataset` were deleted from the layer `name`'s table, or the error: 0 for a
   * name no push wrote, which is not an error here, for the program to judge. A remove takes turns with a push of the
   * dataset.
   * @throws std::logic_error if the layer has a reader: it is only read.
   */
  template <schemacht::util::CTString name>
  [[nodiscard]] std::future<std::uint64_t> remove(const Dataset& dataset) {
    using model_t = ModelOf<name>;
    static_assert(WritesDatasets<model_t>, "this layer's model has no datasets to remove");

    if constexpr ( WritesDatasets<model_t> ) {
      return detail::remove<model_t>(database(), written<name>(), dataset);
    }
  }

  /**
   * @return A future for the datasets of the layer `name`'s table, by name: each with when it was last pushed and how many
   * rows it holds now, or the error.
   * @throws std::logic_error if the layer has a reader: it is only read.
   */
  template <schemacht::util::CTString name>
  [[nodiscard]] std::future<std::vector<DatasetInfo>> datasets() {
    using model_t = ModelOf<name>;
    static_assert(WritesDatasets<model_t>, "this layer's model has no datasets to list");

    if constexpr ( WritesDatasets<model_t> ) {
      return detail::datasets<model_t>(database(), written<name>());
    }
  }

  /**
   * @return A future that completes if every layer's table is in the registry as made with its model's layout (`LAYOUT`) and
   * matches its model's schema, as schemacht's `table_differences_statement` compares them (columns, types, nullability,
   * primary key, and no other column an insert would have to fill); for a model with settings the table must also hold
   * settings to read. A table of another layout, or none recorded, is named with what to do: make it again. It holds a
   * `TablesDiffer` naming every difference of every layer instead, or the error of a check that could not run (a PostgreSQL
   * older than 17: a `QueryError`). The layers are checked one after another, on a thread started for the call. For a
   * program to call when it starts.
   *
   * Not checked: indexes, other constraints, the raster merge function (`raster::MergeRaster`), and the role's privileges. A
   * layer with a reader is checked by `Reader::problems`; what it finds is in the `TablesDiffer` too.
   */
  [[nodiscard]] std::future<void> verify() {
    return detail::on_a_thread([this] {
      std::vector<std::string> differences;
      (check(std::get<layer_ts>(_layers), differences), ...);
      if ( ! differences.empty() ) {
        throw TablesDiffer(std::move(differences));
      }
    });
  }

  /**
   * @brief Makes the layer `name`'s table and runs its model's setup, in one transaction, and waits. For a model with settings
   * (a raster's grid) the table is made with `settings` and keeps them: pushes read them back, and `table_settings` gives
   * them to a writer. PostGIS must be enabled in the database (`CREATE EXTENSION postgis`, and `postgis_raster` for a
   * raster). For a layer with a reader nothing is done, here and in `drop_tables`, so one setup serves a miniverse of tables
   * and one of files.
   * @throws schemacht::postgres::QueryError if the table exists already, or a statement fails: nothing is then left behind.
   * @throws std::invalid_argument if the model refuses `settings`, before anything is sent; for a layer with a reader they are
   * not looked at.
   */
  template <schemacht::util::CTString name>
    requires WritesSettings<ModelOf<name>>
  void create_table(const SettingsOf<ModelOf<name>>& settings) {
    using model_t = ModelOf<name>;

    if ( layer<name>().is_table() ) {
      detail::make_table<model_t>(database(), layer<name>().table(), model_t::setup_sql(table_name<name>(), settings));
    }
  }

  /** @brief As above, for a layer of a model without settings. */
  template <schemacht::util::CTString name>
  void create_table() {
    using model_t = ModelOf<name>;
    static_assert(
        ! WritesSettings<model_t>, "this layer's table is made with settings (an elevation's grid): call create_table<\"name\">(settings) for it"
    );

    if constexpr ( ! WritesSettings<model_t> ) {
      if ( layer<name>().is_table() ) {
        detail::make_table<model_t>(database(), layer<name>().table(), model_t::setup_sql(table_name<name>()));
      }
    }
  }

  /** @brief Makes every layer's table, as `create_table`: for a miniverse of models without settings. */
  void create_tables() {
    static_assert(
        (! WritesSettings<typename layer_ts::model_type> && ...),
        "create_tables makes the tables of models without settings: call create_table<\"name\">(settings) for the layer with settings"
    );

    if constexpr ( (! WritesSettings<typename layer_ts::model_type> && ...) ) {
      (create_table<layer_ts::NAME>(), ...);
    }
  }

  /**
   * @brief Drops every layer's table, those that exist, and waits for it.
   * @throws schemacht::postgres::QueryError if a drop fails.
   */
  void drop_tables() { (drop_table<layer_ts::NAME>(), ...); }

  /**
   * @return The pool, for what the layers don't cover: a query of your own on the same connections.
   * @throws std::logic_error if the miniverse was made with no database.
   */
  [[nodiscard]] schemacht::postgres::Database& database() {
    if ( ! _database ) {
      throw std::logic_error("this miniverse was made with no database: its layers are read by readers");
    }

    return *_database;
  }

 private:
  std::tuple<layer_ts...>                      _layers;    // initialised before the pool, so `distinct_tables` throws before a connection is opened
  std::optional<schemacht::postgres::Database> _database;  // none, in a miniverse of readers alone

  static_assert(sizeof...(layer_ts) > 0, "a miniverse holds at least one layer: give Miniverse<...> its layers");
  static_assert(
      schemacht::util::all_distinct(std::array<std::string_view, sizeof...(layer_ts)>{layer_ts::NAME.view()...}),
      "two layers of the miniverse have one name: a second table of a model is a layer of another name"
  );

  [[nodiscard]] static std::tuple<layer_ts...> distinct_tables(layer_ts... layers) {
    std::vector<std::pair<std::string_view, std::string>> tables;  // the name and the table of each layer that is one
    const auto                                            note = [&tables](std::string_view name, const auto& layer) {
      if ( layer.is_table() ) {
        tables.emplace_back(name, layer.table().name().quoted());
      }
    };
    (note(layer_ts::NAME.view(), layers), ...);
    for ( auto first = tables.begin(); first != tables.end(); ++first ) {
      for ( auto second = std::next(first); second != tables.end(); ++second ) {
        if ( first->second == second->second ) {
          throw std::invalid_argument(
              "the layers " + std::string(first->first) + " and " + std::string(second->first) + " of the miniverse are both the table " +
              first->second
          );
        }
      }
    }

    return {std::move(layers)...};
  }

  [[nodiscard]] static std::tuple<layer_ts...> all_with_readers(layer_ts... layers) {
    std::string_view table_layer;
    const auto       note = [&table_layer](std::string_view name, const auto& layer) {
      if ( layer.is_table() && table_layer.empty() ) {
        table_layer = name;
      }
    };
    (note(layer_ts::NAME.view(), layers), ...);
    if ( ! table_layer.empty() ) {
      throw std::invalid_argument(
          "the layer " + std::string(table_layer) + " is a table, which needs a database: make the miniverse with a connection string"
      );
    }

    return {std::move(layers)...};
  }

  template <schemacht::util::CTString name>
  [[nodiscard]] const auto& layer() const noexcept {
    return std::get<typename detail::LayerNamed<name, layer_ts...>::type>(_layers);
  }

  /**
   * @return The table of the layer `name`, for a call that writes it or lists its datasets.
   * @throws std::logic_error if the layer has a reader: it is only read.
   */
  template <schemacht::util::CTString name>
  [[nodiscard]] const auto& written() const {
    if ( ! layer<name>().is_table() ) {
      throw std::logic_error(
          "the layer " + std::string(name.view()) + ", read from " + layer<name>().source() +
          ", is only read: it has no table to write, or datasets to list"
      );
    }

    return layer<name>().table();
  }

  /** @brief Checks `layer` for `verify`, its table against its model or its reader asked for its problems, and adds what it finds to `differences`. */
  template <IsLayer layer_t>
  void check(const layer_t& layer, std::vector<std::string>& differences) {
    using model_t = typename layer_t::model_type;

    const std::vector<std::string> problems =
        layer.reader() ? layer.reader()->problems() : detail::table_problems<model_t>(database(), layer.table());
    for ( const std::string& problem : problems ) {
      differences.push_back(layer.source() + ": " + problem);
    }
  }

  template <schemacht::util::CTString name>
  void drop_table() {
    if ( layer<name>().is_table() ) {
      detail::drop_table<ModelOf<name>>(database(), layer<name>().table());
    }
  }

 public:
  Miniverse(const Miniverse&) = delete;  // it owns the pool
  Miniverse(Miniverse&&) = delete;
  Miniverse& operator=(const Miniverse&) = delete;
  Miniverse& operator=(Miniverse&&) = delete;
  ~Miniverse() = default;
};

}  // namespace miniverse
