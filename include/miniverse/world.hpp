#pragma once

#include <concepts>
#include <cstddef>
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
 * A miniverse: the layers of the world a worker reads, each a kind of data in its own PostGIS table, loaded by polygon.
 * This header holds the `Miniverse` alone; miniverse.hpp adds the built-in kinds (`FeatureLayer`, `RoadLayer`, `RasterLayer`).
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
 * The kinds of layer are the template's arguments: asking for one the miniverse does not hold does not compile. Each layer
 * names its table when the miniverse is made, so one program serves whichever tables configuration names. The miniverse
 * owns one pool of connections (a schemacht `postgres::Database`) that every layer shares, and every call returns a
 * `std::future`, so a worker can start loading several layers at once and wait for them together. A `load` gives the whole
 * result; a `stream` hands it to a callback a chunk at a time, for a worker that builds a structure of its own from it. A
 * `verify` when the program starts checks that the tables are the layers' kinds'.
 * Errors, including one from a kind's own conversions, arrive through the future, never as a throw from the call itself. The
 * miniverse must outlive the futures it hands out: wait on them before it is destroyed.
 *
 * A layer need not be a table: one made from a `Reader` (reader.hpp) is read from a file, or from data of the program's own,
 * by the same `load` and `stream`. A miniverse whose layers all have readers needs no database, and is made without a
 * connection string: for a test, or a user with files alone.
 *
 * The `Miniverse` is a front: it holds the layers and the pool, and for each call tells a layer that is a table from one with
 * a reader. What a call then does is in world/, one idea each: load.hpp, push.hpp (with push_in_parts.hpp and
 * datasets.hpp), verify.hpp, tables.hpp, settings.hpp, and async.hpp for how a call becomes a future.
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
   * @throws std::runtime_error if a connection can't be made.
   */
  Miniverse(const std::string& conninfo, const schemacht::postgres::Database::Options& options, Layer<kind_ts>... layers)
      : _layers(distinct_tables(std::move(layers)...)), _database(std::in_place, conninfo, options) {}

  /** @brief As above, with the pool's default options. */
  explicit Miniverse(const std::string& conninfo, Layer<kind_ts>... layers) : Miniverse(conninfo, {}, std::move(layers)...) {}

  /**
   * @brief A miniverse with no database, for `layers` that each have a reader: nothing is connected to.
   * @throws std::invalid_argument if a layer is a table: that needs a connection string.
   */
  explicit Miniverse(Layer<kind_ts>... layers)
    requires(sizeof...(kind_ts) > 0)
      : _layers(all_with_readers(std::move(layers)...)) {}

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
   * A layer with a reader is read on a thread started for the load, so several loads run at once as those of tables do; the
   * future of such a load waits for it when it is dropped.
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
   * time, or `on_chunk` stopped it; or holds the error. Nothing holds the whole result, so a worker can build its own structure
   * from a large area as the rows arrive.
   *
   * A chunk is what a load would give, of `read.chunk_rows` rows (the last of what is left), as `ChunkOf` says:
   * - for a feature or road layer, that many features, in the load's order (by id);
   * - for a raster layer, that many tiles, in no particular order, each whole: the tiles `location` reaches into, not cut
   *   to it. A tile is megabytes, and about `chunk_rows` times `max_buffered_chunks` + 2 of them are held at once, so ask for a
   *   few at a time.
   *
   * `on_chunk` is called with one chunk at a time, never from two threads at once, on a thread of the pool's, not the
   * caller's. It returns `void`, or a `bool` where `false` stops the load, which is then cancelled. What it throws is the load's
   * error; chunks it was handed before an error stay handed. It must not wait on another of this miniverse's futures.
   *
   * A layer with a reader hands over chunks of `read.chunk_rows` too, on a thread started for the load, in the order its
   * reader says (a file's own, for the readers of files); the future of such a load waits for it when it is dropped.
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
   * @brief As above, with chunks of the size the miniverse's pool reads by default (`Database::Options::read`): 1000 rows,
   * also in a miniverse with no database, unless the pool was made with another. That suits features, and is far too many
   * for a raster's tiles.
   */
  template <LayerKind kind_t, ChunkCallback<kind_t> callback_t>
  [[nodiscard]] std::future<void> stream(const geo::Polygon& location, callback_t on_chunk) {
    return stream<kind_t>(location, std::move(on_chunk), _database ? _database->read_options() : schemacht::postgres::ReadOptions{});
  }

  /**
   * @return A future for the settings the layer `kind_t`'s table was made with (an elevation's grid), read from the database:
   * for a writer to warp its source onto before pushing. It holds the error instead if the table does not exist, or has no
   * settings (it was not made by `create_table`). For a layer with a reader, they are the reader's.
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
   * written). `data` is taken by value and moved into the rows: pass it with `std::move` unless it is still needed.
   *
   * The push runs on a thread started for it, so the call returns at once; the future waits for that thread if it is dropped
   * before it is done. There the rows are made by the kind (for a kind with settings, with the table's, read first: a raster
   * is cut into the tiles of the table's grid), and written in the kind's batches, one statement each, all in one
   * transaction: so a push holds all of `data`'s rows but one statement's text at a time, and writes all of `data` or none.
   * The transaction is begun before the rows are made, so data the kind refuses costs a transaction that is rolled back. It
   * holds one of the pool's connections until it ends (with one connection, nothing else runs during a push). For data too
   * large to hold at once, `begin_push` takes it in parts.
   *
   * The kind's `write_statement` says what happens to a row already there: a `RasterLayer`'s tile is merged with it (new
   * pixels win, and where the new tile has no data the old pixel stays). Pushes that overlap are merged in the order they
   * commit: where both have data, the last to commit wins. A kind with datasets is pushed with one: `push(dataset, data)`.
   *
   * A layer with a reader is only read: a push to it fails with a `std::logic_error`.
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
   * @return A future for a push of the layer `kind_t` that takes its data in parts, all of them written or none (see
   * `PushInParts`): for data too large to hold at once. On a thread started for the call, for a kind with settings the
   * table's are read first, once, and every part's rows are made with them; then the transaction is begun. It holds the error
   * instead if they can't be read, or no transaction begun, or the layer has a reader (a `std::logic_error`: it is only read).
   * Commit or drop the push before the miniverse ends: its transaction is the pool's.
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
   * (nothing is then changed). For a kind with datasets (`HasDatasets`: a `FeatureLayer`, a `RoadLayer`): on a thread started
   * for the push, the rows are made by the kind, written in its batches, and the dataset's rows that this push did not write
   * are deleted, all in one transaction. So the first push of a name writes the dataset, and every later push of it replaces it: what
   * the new data lacks is gone, what it has is there, and a row the kind can tell is the same as one already there (a
   * `RoadLayer`'s way, by its nodes) keeps its id. A dataset is an export of a region, named by the pusher: see `Dataset`.
   * Two pushes of one dataset at once take turns (a lock on the name, held for the transaction); pushes of different datasets
   * run side by side.
   *
   * The ids a load gives are the database's own, never the data's. A push of a `RoadLayer` dataset may also take over a
   * row of another dataset: a way of the same nodes, such as a way both of two neighbouring exports carry whole.
   *
   * A layer with a reader is only read: a push to it fails with a `std::logic_error`.
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
   * @return A future for a push of `dataset` into the layer `kind_t` that takes its data in parts (see `PushInParts`): for an
   * export too large to hold at once. The dataset is replaced when the push commits, as `push(dataset, data)` replaces it;
   * until then nothing of it has changed, and a commit with no parts leaves it empty, as a push of no data does. On a thread
   * started for the call, the transaction is begun and the dataset locked. It holds the error instead if either fails, or
   * the layer has a reader (a `std::logic_error`: it is only read). Commit or drop the push before the miniverse ends.
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
   * @return A future that completes once every row of `dataset` is deleted from the layer `kind_t`'s table, or holds the
   * error. A name that no push wrote is nothing to delete, not an error. A layer with a reader is only read: it fails with
   * a `std::logic_error`.
   */
  template <LayerKind kind_t>
  [[nodiscard]] std::future<void> remove(const Dataset& dataset) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");
    static_assert(HasDatasets<kind_t>, "this kind has no datasets to remove");

    if constexpr ( HasDatasets<kind_t> ) {
      if ( ! layer<kind_t>().is_table() ) {
        return only_read<kind_t, void>();
      }

      return detail::remove<kind_t>(database(), layer<kind_t>().table(), dataset);
    }
  }

  /**
   * @return A future that completes if every layer's table is in the database and is as its kind says, by what schemacht's
   * `table_differences_statement` compares: its columns, their types, which may be NULL, and its primary key, and that it has
   * no other column an insert would have to fill. For a kind with settings, a table that is so must also have settings to
   * read (a raster's grid). For a program to call when it starts: a table that is missing, or is another kind's, then fails
   * here, and not at the first load or push.
   *
   * It holds a `TablesDiffer` instead if any table is not so, which names every difference of every layer; or the error of a
   * check that could not be run (on a PostgreSQL older than 17, a `QueryError`). Every layer is checked, at the same time.
   *
   * Not checked: a table's indexes (the spatial index a load is fast by), its other constraints, the function a raster push
   * merges with (`raster::MergeRaster`), and what the connection's role may do.
   *
   * A layer with a reader is checked by its reader (`Reader::problems`: a file that is missing, or does not hold what the
   * kind needs), on the calling thread; what it finds is in the `TablesDiffer` too, said of the reader's name.
   */
  [[nodiscard]] std::future<void> verify() {
    return schemacht::util::future_from<void>([&](const schemacht::util::Settler<void>& done) {
      if constexpr ( sizeof...(kind_ts) == 0 ) {
        done(nullptr);
      } else {
        detail::guarded(done, [&] {
          const auto  checks = std::make_shared<detail::Checks>(std::vector<std::string>{layer<kind_ts>().name()...}, done);
          std::size_t layer_number = 0;
          (check<kind_ts>(checks, layer_number++), ...);
        });
      }
    });
  }

  /**
   * @brief Makes the layer `kind_t`'s table and runs its kind's setup (a spatial index, say), and waits for them: for tests and
   * a first start. For a kind with settings (an elevation's grid), the table is made with `settings`, and keeps them: every
   * push reads them back, and `table_settings` gives them to a writer. PostGIS must already be enabled in the database
   * (`CREATE EXTENSION postgis`, and `postgis_raster` for a raster).
   *
   * The table and its setup are one transaction: if any of it fails, nothing is left behind, and the error is that statement's own.
   * For a layer with a reader there is no table to make: nothing is done, here and in `drop_tables`, so that one setup serves
   * a miniverse of tables and one of files. Its `settings` are then neither checked nor kept: the reader has its own.
   * @throws schemacht::postgres::QueryError if the table exists already, or can't be made, or its setup fails.
   * @throws std::invalid_argument if the kind refuses `settings`, before anything is sent.
   */
  template <HasSettings kind_t>
  void create_table(const typename kind_t::settings_type& settings) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    if ( layer<kind_t>().is_table() ) {
      detail::make_table<kind_t>(
          database(), layer<kind_t>().table(), kind_t::setup_sql(table_name<kind_t>(), settings)
      );  // checks the settings first
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

  /** @brief Drops every layer's table, those that exist, and waits for it. */
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
  std::tuple<Layer<kind_ts>...>                _layers;    // before the pool: checked before any connection is opened
  std::optional<schemacht::postgres::Database> _database;  // none, in a miniverse of readers alone

  template <LayerKind kind_t>
  static constexpr std::size_t COUNT_OF = (std::size_t{0} + ... + std::size_t{std::same_as<kind_t, kind_ts>});

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
    return schemacht::util::future_from<result_t>([&](const schemacht::util::Settler<result_t>& done) {
      detail::guarded(done, [&] {
        done.fail(
            std::make_exception_ptr(
                std::logic_error("the layer read from " + layer<kind_t>().name() + " is only read: a push or a remove needs a layer that is a table")
            )
        );
      });
    });
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
