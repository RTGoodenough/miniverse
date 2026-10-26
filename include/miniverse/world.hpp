#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
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
 * A miniverse: the layers of the world a worker reads, each a kind of data in a PostGIS table or behind a reader, loaded by
 * polygon. This header holds `Miniverse` alone; miniverse.hpp adds the built-in kinds.
 *
 * @code
 * struct Roads : miniverse::RoadLayer {};  // layer/road_layer.hpp
 *
 * miniverse::Miniverse world("host=db dbname=gis user=worker", miniverse::Layer<Roads>("osm_roads"));
 *
 * const miniverse::geo::Polygon area = ...;                               // WGS 84 longitude and latitude
 * std::future<miniverse::Ways> roads = world.load<Roads>(area);           // the ways that intersect it
 * for ( const miniverse::Way& way : roads.get() ) { ... }
 * @endcode
 *
 * The kinds of layer are the template's arguments, so asking for a layer the miniverse does not hold does not compile. Each layer names its table
 * when the miniverse is made. One pool of connections (a schemacht `postgres::Database`) serves every layer. The calls that read and write data
 * return a `std::future`, and their errors arrive through it, not from the call; the constructor, `create_table` and `drop_tables` wait and throw.
 * The miniverse must outlive the futures it hands out. A layer made from a `Reader` (reader.hpp) is read from a file, or from data of the program's
 * own, by the same calls; a miniverse of readers alone is made without a connection string.
 *
 * `Miniverse` is a front: it holds the layers and the pool, and tells a table from a reader. What a call does is in world/,
 * one idea per header.
 */
namespace miniverse {

/** @brief One layer of each kind `kind_ts`, each in its own table of one PostGIS database, or read by a reader. */
template <LayerKind... kind_ts>
class Miniverse {
 public:
  /**
   * @brief Opens the pool of connections to the database at `conninfo` (a libpq connection string or URL), for `layers`.
   * @throws std::invalid_argument if two layers name the same table, before anything is opened. Names are compared as written:
   * `gis.roads` and `roads` count as different even when the search path makes them the same table.
   * @throws std::invalid_argument if `options` asks for no connections, no workers or an empty chunk (schemacht's `Database`).
   * @throws std::runtime_error if a connection can't be made, or the session settings can't be applied.
   */
  Miniverse(const std::string& conninfo, const schemacht::postgres::Database::Options& options, Layer<kind_ts>... layers)
      : _layers(distinct_tables(std::move(layers)...)), _database(std::in_place, conninfo, options) {}

  /** @brief As above, with the pool's default options. */
  explicit Miniverse(const std::string& conninfo, Layer<kind_ts>... layers) : Miniverse(conninfo, {}, std::move(layers)...) {}

  /**
   * @brief A miniverse with no database, for `layers` that each have a reader: nothing is connected to.
   * @throws std::invalid_argument if a layer is a table: that needs a connection string.
   */
  explicit Miniverse(Layer<kind_ts>... layers) : _layers(all_with_readers(std::move(layers)...)) {}

  /**
   * @return The name of the table the layer `kind_t` is stored in.
   * @throws std::logic_error if the layer is not a table: it has a reader.
   */
  template <LayerKind kind_t>
  [[nodiscard]] const schemacht::schema::TableName& table_name() const {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    return layer<kind_t>().table().name();
  }

  /**
   * @return A future for what the layer `kind_t` holds in `location`, a polygon in WGS 84 longitude and latitude (what that
   * is, the kind says: for a `RoadLayer`, the ways that intersect it). It holds the error instead if the load failed.
   *
   * A layer with a reader is read on a thread started for the load; the future waits for that thread if it is dropped.
   */
  template <LayerKind kind_t>
  [[nodiscard]] std::future<typename kind_t::result_type> load(const geo::Polygon& location) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    if ( const auto& reader = layer<kind_t>().reader(); reader ) {
      return detail::load<kind_t>(reader, location);
    }

    return detail::load<kind_t>(database(), layer<kind_t>().table(), location);
  }

  /**
   * @return A future that completes once what the layer `kind_t` holds in `location` has been handed to `on_chunk`, a chunk at a
   * time, or `on_chunk` stopped it; or holds the error. Nothing holds the whole result.
   *
   * A chunk is `read.chunk_rows` rows of what a load would give (the last chunk holds what is left), as `ChunkOf` says: for a
   * feature or road layer that many features, in the load's order; for a raster layer that many whole tiles, in no particular
   * order. A tile is megabytes, so ask for a few at a time.
   *
   * `on_chunk` runs on a thread of the pool's, one call at a time. It returns `void`, or a `bool` where `false` stops the load.
   * What it throws is the load's error; chunks handed over before an error stay handed over. It must not wait on another of
   * this miniverse's futures. For a layer with a reader, the chunks come in the reader's order (a file's own), on a thread
   * started for the load, which the future waits for if it is dropped.
   */
  template <LayerKind kind_t, ChunkCallback<kind_t> callback_t>
  [[nodiscard]] std::future<void> stream(const geo::Polygon& location, callback_t on_chunk, const schemacht::postgres::ReadOptions& read) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    if ( const auto& reader = layer<kind_t>().reader(); reader ) {
      return detail::stream<kind_t>(reader, location, std::move(on_chunk), read.chunk_rows);
    }

    return detail::stream<kind_t>(database(), layer<kind_t>().table(), location, std::move(on_chunk), read);
  }

  /**
   * @brief As above, with the pool's default chunk size (`Database::Options::read`, 1000 rows unless set otherwise): right for
   * features, far too many for a raster's tiles.
   */
  template <LayerKind kind_t, ChunkCallback<kind_t> callback_t>
  [[nodiscard]] std::future<void> stream(const geo::Polygon& location, callback_t on_chunk) {
    return stream<kind_t>(location, std::move(on_chunk), _database ? _database->read_options() : schemacht::postgres::ReadOptions{});
  }

  /**
   * @return A future for the settings the layer `kind_t`'s table was made with (a raster's grid), for a writer to warp its
   * source onto. It holds the error instead if the table does not exist or has no settings (it was not made by
   * `create_table`). For a layer with a reader, they are the reader's.
   */
  template <HasSettings kind_t>
  [[nodiscard]] std::future<typename kind_t::settings_type> table_settings() {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    if ( const auto& reader = layer<kind_t>().reader(); reader ) {
      return detail::reader_settings<kind_t>(reader);
    }

    return detail::table_settings<kind_t>(database(), layer<kind_t>().table());
  }

  /**
   * @return A future that completes once `data` is written to the layer `kind_t`'s table, or holds the error (nothing is then
   * written). `data` is moved into the rows: pass it with `std::move` unless it is still needed.
   *
   * The push runs on a thread started for it; the future waits for that thread if it is dropped. There the kind makes the rows
   * (with the table's settings, read first, for a kind that has them) and writes them in its batches, one statement each, in
   * one transaction: the push holds every row of `data`, and one statement's text at a time, and writes all of `data` or none. The transaction is
   * begun
   * before the rows are made, and holds one of the pool's connections until it ends. For data too large to hold at once,
   * `begin_push` takes it in parts.
   *
   * The kind's `write_statement` says what happens to a row already there: a `RasterLayer`'s tile is merged onto it (new pixels
   * win, nodata keeps the old pixel), and overlapping pushes merge in the order they commit, the later winning where both have
   * data. A kind with datasets is pushed with one: `push(dataset, data)`.
   *
   * A layer with a reader is only read: the future holds a `std::logic_error`.
   */
  template <LayerKind kind_t>
  [[nodiscard]] std::future<void> push(kind_t::result_type data) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");
    static_assert(! HasDatasets<kind_t>, "this kind's rows belong to a dataset: push<Kind>(Dataset{\"name\"}, data) replaces it");

    if ( ! layer<kind_t>().is_table() ) {
      return only_read<kind_t, void>();
    }

    return detail::push<kind_t>(database(), layer<kind_t>().table(), std::move(data));
  }

  /**
   * @return A future for a push of the layer `kind_t` that takes its data in parts, all written or none (`PushInParts`). On a
   * thread started for the call, the table's settings are read if the kind has any, the transaction is begun, and the push's
   * own thread, which its parts then run on, is started. It holds the error instead if any of that fails, or the layer has a
   * reader (`std::logic_error`). Commit or drop the push before the miniverse ends.
   */
  template <LayerKind kind_t>
  [[nodiscard]] std::future<PushInParts<kind_t>> begin_push() {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");
    static_assert(! HasDatasets<kind_t>, "this kind's rows belong to a dataset: begin_push<Kind>(Dataset{\"name\"}) replaces it");

    if ( ! layer<kind_t>().is_table() ) {
      return only_read<kind_t, PushInParts<kind_t>>();
    }

    return detail::begin_push<kind_t>(database(), layer<kind_t>().table());
  }

  /**
   * @return A future that completes once `data` has replaced `dataset` in the layer `kind_t`'s table, or holds the error
   * (nothing is then changed). On a thread started for the push, in one transaction: the dataset is locked, the rows are
   * written, and the dataset's rows this push did not write are deleted. The first push of a name writes the dataset; every
   * later push replaces it, and a row the kind can tell is the same (a `RoadLayer`'s way, by its id) keeps its row. Two
   * pushes of one dataset take turns; pushes of different datasets run side by side. What a dataset is, and what it asks of
   * the data, is in `Dataset` and the kind's own doc.
   *
   * A layer with a reader is only read: the future holds a `std::logic_error`.
   */
  template <LayerKind kind_t>
  [[nodiscard]] std::future<void> push(const Dataset& dataset, kind_t::result_type data) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");
    static_assert(HasDatasets<kind_t>, "this kind has no datasets: push<Kind>(data) writes it");

    if constexpr ( HasDatasets<kind_t> ) {
      if ( ! layer<kind_t>().is_table() ) {
        return only_read<kind_t, void>();
      }

      return detail::push<kind_t>(database(), layer<kind_t>().table(), dataset, std::move(data));
    }
  }

  /**
   * @return A future for a push of `dataset` into the layer `kind_t` that takes its data in parts (`PushInParts`). The dataset is
   * replaced when the push commits, as `push(dataset, data)` replaces it; a commit with no parts leaves it empty. On a thread
   * started for the call, the transaction is begun, the dataset locked, and the push's own thread, which its parts then run
   * on, is started. It holds the error instead if any of that fails, or the layer has a reader (`std::logic_error`). Commit
   * or drop the push before the miniverse ends.
   */
  template <LayerKind kind_t>
  [[nodiscard]] std::future<PushInParts<kind_t>> begin_push(const Dataset& dataset) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");
    static_assert(HasDatasets<kind_t>, "this kind has no datasets: begin_push<Kind>() writes it");

    if constexpr ( HasDatasets<kind_t> ) {
      if ( ! layer<kind_t>().is_table() ) {
        return only_read<kind_t, PushInParts<kind_t>>();
      }

      return detail::begin_push<kind_t>(database(), layer<kind_t>().table(), dataset);
    }
  }

  /**
   * @return A future for how many rows of `dataset` were deleted from the layer `kind_t`'s table, or the error: 0 for a
   * name no push wrote, which is not an error here, for the program to judge. A remove takes turns with a push of the
   * dataset. A layer with a reader is only read: the future holds a `std::logic_error`.
   */
  template <LayerKind kind_t>
  [[nodiscard]] std::future<std::uint64_t> remove(const Dataset& dataset) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");
    static_assert(HasDatasets<kind_t>, "this kind has no datasets to remove");

    if constexpr ( HasDatasets<kind_t> ) {
      if ( ! layer<kind_t>().is_table() ) {
        return only_read<kind_t, std::uint64_t>();
      }

      return detail::remove<kind_t>(database(), layer<kind_t>().table(), dataset);
    }
  }

  /**
   * @return A future for the datasets of the layer `kind_t`'s table, by name: each with when it was last pushed and how many
   * rows it holds now, or the error. A layer with a reader is only read: the future holds a `std::logic_error`.
   */
  template <LayerKind kind_t>
  [[nodiscard]] std::future<std::vector<DatasetInfo>> datasets() {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");
    static_assert(HasDatasets<kind_t>, "this kind has no datasets to list");

    if constexpr ( HasDatasets<kind_t> ) {
      if ( ! layer<kind_t>().is_table() ) {
        return only_read<kind_t, std::vector<DatasetInfo>>();
      }

      return detail::datasets<kind_t>(database(), layer<kind_t>().table());
    }
  }

  /**
   * @return A future that completes if every layer's table is in the registry as made with its kind's layout (`LAYOUT`) and
   * matches its kind's schema, as schemacht's `table_differences_statement` compares them (columns, types, nullability,
   * primary key, and no other column an insert would have to fill); for a kind with settings the table must also hold
   * settings to read. A table of another layout, or none recorded, is named with what to do: make it again. It holds a
   * `TablesDiffer` naming every difference of every layer instead, or the error of a check that could not run (a PostgreSQL
   * older than 17: a `QueryError`). Every layer is checked at once. For a program to call when it starts.
   *
   * Not checked: indexes, other constraints, the raster merge function (`raster::MergeRaster`), and the role's privileges. A
   * layer with a reader is checked by `Reader::problems`, on the calling thread; what it finds is in the `TablesDiffer` too.
   */
  [[nodiscard]] std::future<void> verify() {
    return schemacht::util::future_from<void>([&](const schemacht::util::Settler<void>& done) {
      detail::guarded(done, [&] {
        const auto  checks = std::make_shared<detail::Checks>(std::vector<std::string>{layer<kind_ts>().name()...}, done);
        std::size_t layer_number = 0;
        (check<kind_ts>(checks, layer_number++), ...);
      });
    });
  }

  /**
   * @brief Makes the layer `kind_t`'s table and runs its kind's setup, in one transaction, and waits. For a kind with settings
   * (a raster's grid) the table is made with `settings` and keeps them: pushes read them back, and `table_settings` gives
   * them to a writer. PostGIS must be enabled in the database (`CREATE EXTENSION postgis`, and `postgis_raster` for a
   * raster). For a layer with a reader nothing is done, here and in `drop_tables`, so one setup serves a miniverse of tables
   * and one of files.
   * @throws schemacht::postgres::QueryError if the table exists already, or a statement fails: nothing is then left behind.
   * @throws std::invalid_argument if the kind refuses `settings`, before anything is sent; for a layer with a reader they are
   * not looked at.
   */
  template <HasSettings kind_t>
  void create_table(const typename kind_t::settings_type& settings) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    if ( layer<kind_t>().is_table() ) {
      detail::make_table<kind_t>(database(), layer<kind_t>().table(), kind_t::setup_sql(table_name<kind_t>(), settings));
    }
  }

  /** @brief As above, for a kind without settings. */
  template <LayerKind kind_t>
  void create_table() {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");
    static_assert(! HasSettings<kind_t>, "this kind's table is made with settings (an elevation's grid): call create_table<Kind>(settings) for it");

    if constexpr ( ! HasSettings<kind_t> ) {
      if ( layer<kind_t>().is_table() ) {
        detail::make_table<kind_t>(database(), layer<kind_t>().table(), kind_t::setup_sql(table_name<kind_t>()));
      }
    }
  }

  /** @brief Makes every layer's table, as `create_table`: for a miniverse of kinds without settings. */
  void create_tables() { (create_table<kind_ts>(), ...); }

  /**
   * @brief Drops every layer's table, those that exist, and waits for it.
   * @throws schemacht::postgres::QueryError if a drop fails.
   */
  void drop_tables() { (drop_table<kind_ts>(), ...); }

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
  std::tuple<Layer<kind_ts>...>                _layers;    // initialised before the pool, so `distinct_tables` throws before a connection is opened
  std::optional<schemacht::postgres::Database> _database;  // none, in a miniverse of readers alone

  template <LayerKind kind_t>
  static constexpr std::size_t COUNT_OF = (std::size_t{0} + ... + std::size_t{std::same_as<kind_t, kind_ts>});

  static_assert(sizeof...(kind_ts) > 0, "a miniverse holds at least one layer: give Miniverse<...> its kinds");
  static_assert(((COUNT_OF<kind_ts> == 1) && ...), "a miniverse holds one layer of each kind: make another kind for a second layer of it");

  template <LayerKind kind_t>
  [[nodiscard]] static consteval bool holds() {
    return (std::same_as<kind_t, kind_ts> || ...);
  }

  [[nodiscard]] static std::tuple<Layer<kind_ts>...> distinct_tables(Layer<kind_ts>... layers) {
    std::vector<std::string_view> tables;
    tables.reserve(sizeof...(kind_ts));
    (..., (layers.is_table() ? tables.push_back(layers.table().name().quoted()) : void()));
    if ( ! schemacht::util::all_distinct(tables) ) {
      throw std::invalid_argument("two layers of the miniverse name the same table");
    }

    return {std::move(layers)...};
  }

  [[nodiscard]] static std::tuple<Layer<kind_ts>...> all_with_readers(Layer<kind_ts>... layers) {
    if ( (layers.is_table() || ...) ) {
      throw std::invalid_argument("a layer that is a table needs a database: make the miniverse with a connection string");
    }

    return {std::move(layers)...};
  }

  template <LayerKind kind_t>
  [[nodiscard]] const Layer<kind_t>& layer() const noexcept {
    return std::get<Layer<kind_t>>(_layers);
  }

  /** @return A future that holds why the layer `kind_t`, which has a reader, can't be written. */
  template <LayerKind kind_t, schemacht::util::FutureValue result_t>
  [[nodiscard]] std::future<result_t> only_read() const {
    std::promise<result_t> failed;
    try {
      throw std::logic_error("the layer read from " + layer<kind_t>().name() + " is only read: it has no table to write, or datasets to list");
    } catch ( ... ) {
      failed.set_exception(std::current_exception());  // also if the message itself could not be made
    }

    return failed.get_future();
  }

  /** @brief Checks the layer `kind_t` for `verify`: its table against its kind, or its reader asked for its problems. */
  template <LayerKind kind_t>
  void check(const std::shared_ptr<detail::Checks>& checks, std::size_t layer_number) {
    if ( const auto& reader = layer<kind_t>().reader(); reader ) {
      detail::check_reader<kind_t>(reader, checks, layer_number);
      return;
    }

    detail::check_table<kind_t>(database(), layer<kind_t>().table(), checks, layer_number);
  }

  template <LayerKind kind_t>
  void drop_table() {
    if ( layer<kind_t>().is_table() ) {
      detail::drop_table<kind_t>(database(), layer<kind_t>().table());
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
