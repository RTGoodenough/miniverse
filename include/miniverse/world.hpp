#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/layer.hpp"  // IWYU pragma: export
#include "schemacht/postgres/database.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/postgres/table_differences.hpp"
#include "schemacht/postgres/transaction.hpp"
#include "schemacht/schema/table.hpp"
#include "schemacht/schema/table_name.hpp"
#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/futures.hpp"

/**
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
 */
namespace miniverse {

template <LayerKind... kind_ts>
class Miniverse;

namespace detail {

/** @brief Runs `step`; what it throws settles `done` with the error instead. For the steps of a call that returns a future. */
template <schemacht::util::FutureValue value_t, std::invocable step_t>
void guarded(const schemacht::util::Settler<value_t>& done, step_t step) {
  try {
    step();
  } catch ( ... ) {
    done.fail(std::current_exception());
  }
}

}  // namespace detail

/**
 * @brief What `Miniverse::verify` fails with when a layer's table is not as its kind says: every difference of every layer,
 * as lines of `what()` and one by one in `differences()`. A check that could not be run at all fails with its own error.
 */
class TablesDiffer : public std::runtime_error {
 public:
  /** @param differences Each as `<table>: <what differs>`, in the layers' order. */
  explicit TablesDiffer(std::vector<std::string> differences) : std::runtime_error(message_of(differences)), _differences(std::move(differences)) {}

  /** @return Each difference, as `<table>: <what differs>`, in the layers' order. */
  [[nodiscard]] const std::vector<std::string>& differences() const noexcept { return _differences; }

 private:
  std::vector<std::string> _differences;

  [[nodiscard]] static std::string message_of(const std::vector<std::string>& differences) {
    std::string message = "the miniverse's tables are not as its layers say:";
    for ( const std::string& difference : differences ) {
      message += "\n  " + difference;
    }

    return message;
  }

 public:
  TablesDiffer(const TablesDiffer&) = default;
  TablesDiffer(TablesDiffer&&) noexcept = default;
  TablesDiffer& operator=(const TablesDiffer&) = default;
  TablesDiffer& operator=(TablesDiffer&&) noexcept = default;
  ~TablesDiffer() override = default;
};

/**
 * @brief A push that takes its data in parts, all of them written or none: for a dataset too large to hold at once, such as a
 * file a loader reads a chunk at a time. Made by `Miniverse::begin_push`.
 *
 * @code
 * auto buildings = world.begin_push<Buildings>().get();
 * miniverse::gdal::read_features<miniverse::geo::MultiPolygon>(source, 10000, [&](auto chunk) {
 *   buildings.add(std::move(chunk)).get();
 * });
 * buildings.commit().get();  // now it is all there, for everyone at once
 * @endcode
 *
 * It is one transaction. The parts added are seen by no one else until `commit`, and one dropped without a commit writes
 * nothing. It holds one of the pool's connections from `begin_push` until it ends. A part that fails, for whatever reason,
 * ends the push: it is rolled back, so nothing of it is written, and every later `add`, and the `commit`, fails with that
 * part's error.
 *
 * One call at a time: wait for an `add`'s future before the next `add` or the `commit`. Wait for every `add` before the
 * miniverse ends. One that was moved from fails every call.
 */
template <LayerKind kind_t>
class PushInParts {
 public:
  /**
   * @return A future that completes once `data`, a part, is written into the push, or holds the error, which ends the push. The
   * rows are made by the kind on the calling thread (for a kind with settings, with the table's, read when the push began),
   * and written in the kind's batches, one statement each.
   */
  [[nodiscard]] std::future<void> add(kind_t::result_type data) {
    return schemacht::util::future_from<void>([&](const schemacht::util::Settler<void>& done) {
      if ( const std::exception_ptr ended = ended_by(); ended ) {
        done.fail(ended);
        return;
      }

      try {
        write(_state, *_table, _to_rows(std::move(data)), done, Then::GoOn);
      } catch ( ... ) {
        end_with(_state, done, std::current_exception());  // the part's rows could not be made: it can't be left out of the whole
      }
    });
  }

  /** @return A future that completes once every part added is there for everyone, or holds the error (nothing is then written). */
  [[nodiscard]] std::future<void> commit() {
    return schemacht::util::future_from<void>([&](const schemacht::util::Settler<void>& done) {
      if ( const std::exception_ptr ended = ended_by(); ended ) {
        done.fail(ended);
        return;
      }

      detail::guarded(done, [&] { _state->transaction.commit(done); });
    });
  }

 private:
  template <LayerKind... kind_ts>
  friend class Miniverse;

  using Table = schemacht::schema::Table<typename kind_t::schema_type>;
  using ToRows = std::function<RowBatches<kind_t>(typename kind_t::result_type)>;

  /** @brief What a write does once its last batch is written: commit its transaction, or leave it open for the next part. */
  enum class Then : std::uint8_t {
    Commit,
    GoOn,
  };

  /** @brief The push itself, shared with a write while it runs: its transaction, and why it ended if a part has failed. */
  struct State {
    schemacht::postgres::Transaction transaction;
    std::exception_ptr               failed;  ///< Set before the transaction is rolled back; what every later call gets.
  };

  /** @brief A write while it runs: its batches, sent one statement at a time inside the push's transaction. */
  struct Write {
    RowBatches<kind_t>             batches;
    std::shared_ptr<State>         state;
    schemacht::util::Settler<void> done;
    const Table*                   table = nullptr;
    Then                           then = Then::GoOn;
    std::size_t                    sent = 0;  ///< Batches written so far; the next to send.
  };

  std::shared_ptr<State> _state;  // null once moved from
  const Table*           _table;
  ToRows                 _to_rows;

  PushInParts(schemacht::postgres::Transaction transaction, const Table& table, ToRows to_rows)
      : _state(begun(std::move(transaction))), _table(&table), _to_rows(std::move(to_rows)) {}

  [[nodiscard]] static std::shared_ptr<State> begun(schemacht::postgres::Transaction transaction) {
    return std::make_shared<State>(State{.transaction = std::move(transaction), .failed = nullptr});
  }

  /** @return Why this push takes no more calls, if it doesn't: a part of it failed, or it was moved from. */
  [[nodiscard]] std::exception_ptr ended_by() const {
    if ( ! _state ) {
      return std::make_exception_ptr(std::logic_error("this push in parts was moved from: it is the other one now"));
    }

    return _state->failed;
  }

  /**
   * @brief Writes `batches` to `table` inside `state`'s transaction, one statement at a time, and settles `done` with the
   * outcome: once the last is written (and, for `Then::Commit`, the transaction committed), or with the error of the batch
   * that failed, which ends the push.
   */
  static void write(
      const std::shared_ptr<State>& state, const Table& table, RowBatches<kind_t> batches, const schemacht::util::Settler<void>& done, Then then
  ) {
    send_next(std::make_shared<Write>(Write{.batches = std::move(batches), .state = state, .done = done, .table = &table, .then = then}));
  }

  /**
   * @brief Sends `write`'s next batch, whose completion sends the one after, and so on; then commits, or goes on. One batch at a
   * time is made into a statement's text, on a thread of the pool's, and the batch is let go of once it is. A batch that
   * fails, on the server or before it gets there, ends the push.
   */
  static void send_next(std::shared_ptr<Write> write) {
    Write& running = *write;
    try {
      if ( running.sent == running.batches.size() ) {
        if ( running.then == Then::Commit ) {
          running.state->transaction.commit(running.done);
        } else {
          running.done(nullptr);
        }

        return;
      }

      const RowsOf<kind_t> batch = std::move(running.batches.at(running.sent));  // the statement copies it, so it ends with this step
      ++running.sent;
      running.state->transaction.execute(
          kind_t::write_statement(batch).on(*running.table), [write](std::uint64_t /*count*/, const std::exception_ptr& error) mutable {
            if ( error ) {
              end_with(write->state, write->done, error);
              return;
            }

            send_next(std::move(write));
          }
      );
    } catch ( ... ) {
      end_with(running.state, running.done, std::current_exception());
    }
  }

  /**
   * @brief Ends the push `state` is: notes `error` as why, rolls its transaction back, and settles `done` with `error`, the
   * part's own, whatever the rollback says.
   */
  static void end_with(const std::shared_ptr<State>& state, const schemacht::util::Settler<void>& done, const std::exception_ptr& error) {
    state->failed = error;  // before the rollback: whoever `done` wakes then finds the push ended, and leaves the transaction be

    try {
      state->transaction.rollback([state, done, error](const std::exception_ptr& /*rolled_back*/) { done.fail(error); });
    } catch ( ... ) {
      done.fail(error);  // the rollback couldn't be sent: dropping the transaction rolls it back
    }
  }

 public:
  PushInParts(const PushInParts&) = delete;  // it is one transaction
  PushInParts(PushInParts&&) noexcept = default;
  PushInParts& operator=(const PushInParts&) = delete;
  PushInParts& operator=(PushInParts&&) noexcept = default;
  ~PushInParts() = default;  // not committed: rolled back
};

/** @brief One layer of each kind `kind_ts`, each in its own table of one PostGIS database. */
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
      : _layers(distinct_tables(std::move(layers)...)), _database(conninfo, options) {}

  /** @brief As above, with the pool's default options. */
  explicit Miniverse(const std::string& conninfo, Layer<kind_ts>... layers) : Miniverse(conninfo, {}, std::move(layers)...) {}

  /** @return The name of the table the layer `kind_t` is stored in. */
  template <LayerKind kind_t>
  [[nodiscard]] const schemacht::schema::TableName& table_name() const noexcept {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    return layer<kind_t>().table().name();
  }

  /**
   * @return A future for what the layer `kind_t` holds in `location`, a polygon in WGS 84 longitude and latitude (what that
   * is, the kind says: for a `RoadLayer`, the ways that intersect it). It holds the error instead if the load failed.
   */
  template <LayerKind kind_t>
  [[nodiscard]] std::future<typename kind_t::result_type> load(const geo::Polygon& location) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    return execute_as<typename kind_t::result_type>(
        [&] { return kind_t::load_statement_type::bind(location).on(layer<kind_t>().table()); },
        [location](auto rows) { return kind_t::from_rows(std::move(rows), location); }
    );
  }

  /**
   * @return A future that completes once what the layer `kind_t` holds in `location` has been handed to `on_chunk`, a chunk at a
   * time, or `on_chunk` stopped it; or holds the error. Nothing holds the whole result, so a worker can build its own structure
   * from a large area as the rows arrive.
   *
   * A chunk is what a load would give, of `read.chunk_rows` rows (the last of what is left), as `ChunkOf` says:
   * - for a feature or road layer, that many features, in the load's order (by id);
   * - for a raster layer, that many tiles, in no particular order, each whole: the tiles that meet `location`'s box, not cut
   *   to it. A tile is megabytes, and about `chunk_rows` times `max_buffered_chunks` + 2 of them are held at once, so ask for a
   *   few at a time.
   *
   * `on_chunk` is called with one chunk at a time, never from two threads at once, on a thread of the pool's, not the
   * caller's. It returns `void`, or a `bool` where `false` stops the load, which is then cancelled. What it throws is the load's
   * error; chunks it was handed before an error stay handed. It must not wait on another of this miniverse's futures.
   */
  template <LayerKind kind_t, ChunkCallback<kind_t> callback_t>
  [[nodiscard]] std::future<void> stream(const geo::Polygon& location, callback_t on_chunk, const schemacht::postgres::ReadOptions& read) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    return schemacht::util::future_from<void>([&](const schemacht::util::Settler<void>& done) {
      detail::guarded(done, [&] {
        _database.stream_chunks(
            kind_t::load_statement_type::bind(location).on(layer<kind_t>().table()),
            [on_chunk = std::move(on_chunk), location](LoadedRows<kind_t>&& rows) mutable {
              return on_chunk(chunk_of<kind_t>(std::move(rows), location));  // nothing, or whether it wants more
            },
            read, done
        );
      });
    });
  }

  /**
   * @brief As above, with chunks of the size the miniverse's pool reads by default (`Database::Options::read`): 1000 rows
   * unless it was made with another, which suits features and is far too many for a raster's tiles.
   */
  template <LayerKind kind_t, ChunkCallback<kind_t> callback_t>
  [[nodiscard]] std::future<void> stream(const geo::Polygon& location, callback_t on_chunk) {
    return stream<kind_t>(location, std::move(on_chunk), _database.read_options());
  }

  /**
   * @return A future for the settings the layer `kind_t`'s table was made with (an elevation's grid), read from the database:
   * for a writer to warp its source onto before pushing. It holds the error instead if the table does not exist, or has no
   * settings (it was not made by `create_table`).
   */
  template <HasSettings kind_t>
  [[nodiscard]] std::future<typename kind_t::settings_type> table_settings() {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    return execute_as<typename kind_t::settings_type>(
        [&] { return settings_statement<kind_t>(); }, [](auto rows) { return kind_t::settings_from_rows(std::move(rows)); }
    );
  }

  /**
   * @return A future that completes once `data` is written to the layer `kind_t`'s table, or holds the error (nothing is then
   * written). `data` is taken by value and moved into the rows: pass it with `std::move` unless it is still needed.
   *
   * The rows are made by the kind. For a kind with settings, the table's are read first, and the rows are made with them on
   * a thread of the pool's (a raster is cut into the tiles of the table's grid there); for a kind without, they are made on the
   * calling thread. They are written in the kind's batches, one statement each, all in one transaction: so a push holds all
   * of `data`'s rows but one statement's text at a time, and writes all of `data` or none. The transaction holds one of the
   * pool's connections until it ends (with one connection, nothing else runs during a push). For data too large to hold at
   * once, `begin_push` takes it in parts.
   *
   * The kind's `write_statement` says what happens to a row already there: a `RoadLayer`'s way fails the push, and an
   * `RasterLayer`'s tile is merged with it (new pixels win, and where the new tile has no data the old pixel stays). Pushes
   * that overlap are merged in the order they commit: where both have data, the last to commit wins.
   */
  template <LayerKind kind_t>
  [[nodiscard]] std::future<void> push(kind_t::result_type data) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    return schemacht::util::future_from<void>([&](const schemacht::util::Settler<void>& done) {
      if constexpr ( HasSettings<kind_t> ) {
        detail::guarded(done, [&] {
          _database.execute(
              settings_statement<kind_t>(),
              [this, done, data = std::move(data)](SettingsRows<kind_t> rows, const std::exception_ptr& error) mutable {
                if ( error ) {
                  done(error);
                  return;
                }

                detail::guarded(done, [&] { write_rows<kind_t>(kind_t::to_rows(std::move(data), kind_t::settings_from_rows(std::move(rows))), done); });
              }
          );
        });

      } else {
        detail::guarded(done, [&] { write_rows<kind_t>(kind_t::to_rows(std::move(data)), done); });
      }
    });
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
   */
  [[nodiscard]] std::future<void> verify() {
    return schemacht::util::future_from<void>([&](const schemacht::util::Settler<void>& done) {
      if constexpr ( sizeof...(kind_ts) == 0 ) {
        done(nullptr);
      } else {
        detail::guarded(done, [&] {
          const auto  checks = std::make_shared<Checks>(std::vector<std::string>{table_name<kind_ts>().quoted()...}, done);
          std::size_t layer = 0;
          (check_table<kind_ts>(checks, layer++), ...);
        });
      }
    });
  }

  /**
   * @return A future for a push of the layer `kind_t` that takes its data in parts, all of them written or none (see
   * `PushInParts`): for a dataset too large to hold at once. For a kind with settings, the table's are read first, once, and
   * every part's rows are made with them. It holds the error instead if they can't be read, or no transaction begun.
   */
  template <LayerKind kind_t>
  [[nodiscard]] std::future<PushInParts<kind_t>> begin_push() {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    return schemacht::util::future_from<PushInParts<kind_t>>([&](const schemacht::util::Settler<PushInParts<kind_t>>& done) {
      if constexpr ( HasSettings<kind_t> ) {
        detail::guarded(done, [&] {
          _database.execute(settings_statement<kind_t>(), [this, done](SettingsRows<kind_t> rows, const std::exception_ptr& error) {
            if ( error ) {
              done.fail(error);
              return;
            }

            detail::guarded(done, [&] {
              begin_parts<kind_t>(
                  [settings = kind_t::settings_from_rows(std::move(rows))](typename kind_t::result_type data) {
                    return kind_t::to_rows(std::move(data), settings);
                  },
                  done
              );
            });
          });
        });

      } else {
        detail::guarded(done, [&] { begin_parts<kind_t>([](typename kind_t::result_type data) { return kind_t::to_rows(std::move(data)); }, done); });
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
   * @throws schemacht::postgres::QueryError if the table exists already, or can't be made, or its setup fails.
   * @throws std::invalid_argument if the kind refuses `settings`, before anything is sent.
   */
  template <HasSettings kind_t>
  void create_table(const typename kind_t::settings_type& settings) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    make_table<kind_t>(kind_t::setup_sql(table_name<kind_t>(), settings));  // checks the settings first
  }

  /** @brief As above, for a kind without settings. */
  template <LayerKind kind_t>
  void create_table() {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");
    static_assert(! HasSettings<kind_t>, "this kind's table is made with settings (an elevation's grid): call create_table<Kind>(settings) for it");

    if constexpr ( ! HasSettings<kind_t> ) {
      make_table<kind_t>(kind_t::setup_sql(table_name<kind_t>()));
    }
  }

  /** @brief Makes every layer's table, as `create_table`: for a miniverse of kinds without settings. */
  void create_tables() { (create_table<kind_ts>(), ...); }

  /** @brief Drops every layer's table, those that exist, and waits for it. */
  void drop_tables() { (drop_table<kind_ts>(), ...); }

  /** @return The pool, for what the layers don't cover: a query of your own on the same connections. */
  [[nodiscard]] schemacht::postgres::Database& database() noexcept { return _database; }

 private:
  std::tuple<Layer<kind_ts>...> _layers;  // before the pool: checked before any connection is opened
  schemacht::postgres::Database _database;

  /** @brief The rows a statement gives. */
  template <schemacht::postgres::RunnableStatement statement_t>
  using ResultRows = std::vector<typename statement_t::result_type>;

  template <LayerKind kind_t>
  static constexpr std::size_t COUNT_OF = (std::size_t{0} + ... + std::size_t{std::same_as<kind_t, kind_ts>});

  static_assert(((COUNT_OF<kind_ts> == 1) && ...), "a miniverse holds one layer of each kind: make another kind for a second dataset");

  template <LayerKind kind_t>
  [[nodiscard]] static consteval bool holds() {
    return (std::same_as<kind_t, kind_ts> || ...);
  }

  [[nodiscard]] static std::tuple<Layer<kind_ts>...> distinct_tables(Layer<kind_ts>... layers) {
    const std::array<std::string_view, sizeof...(kind_ts)> tables{std::string_view(layers.table().name().quoted())...};
    if ( ! schemacht::util::all_distinct(tables) ) {
      throw std::invalid_argument("two layers of the miniverse name the same table");
    }

    return {std::move(layers)...};
  }

  template <LayerKind kind_t>
  [[nodiscard]] const Layer<kind_t>& layer() const noexcept {
    return std::get<Layer<kind_t>>(_layers);
  }

  /** @return The statement that reads the settings the layer `kind_t`'s table was made with. */
  template <HasSettings kind_t>
  [[nodiscard]] auto settings_statement() const {
    return kind_t::settings_statement_type::bind(table_name<kind_t>().quoted());
  }

  /** @return What a streamed load of `kind_t` hands over for `rows`, one chunk's: the kind's own chunk, or its result of them. */
  template <LayerKind kind_t>
  [[nodiscard]] static ChunkOf<kind_t> chunk_of(LoadedRows<kind_t> rows, const geo::Polygon& location) {
    if constexpr ( HasOwnChunks<kind_t> ) {
      return kind_t::chunk_from_rows(std::move(rows), location);
    } else {
      return kind_t::from_rows(std::move(rows), location);
    }
  }

  /** @brief Writes `batches` to the layer `kind_t`'s table in one transaction of their own, and settles `done` with the outcome. */
  template <LayerKind kind_t>
  void write_rows(RowBatches<kind_t> batches, const schemacht::util::Settler<void>& done) {
    if ( batches.empty() ) {
      done(nullptr);  // nothing to write
      return;
    }

    _database.begin([batches = std::move(batches), done, table = &layer<kind_t>().table()](
                        schemacht::postgres::Transaction transaction, const std::exception_ptr& error
                    ) mutable {
      if ( error ) {
        done(error);
        return;
      }

      detail::guarded(done, [&] {
        PushInParts<kind_t>::write(PushInParts<kind_t>::begun(std::move(transaction)), *table, std::move(batches), done, PushInParts<kind_t>::Then::Commit);
      });
    });
  }

  /** @brief Begins a transaction, and settles `done` with the push in parts it is, whose rows `to_rows` makes. */
  template <LayerKind kind_t>
  void begin_parts(typename PushInParts<kind_t>::ToRows to_rows, const schemacht::util::Settler<PushInParts<kind_t>>& done) {
    _database.begin([to_rows = std::move(to_rows), done, table = &layer<kind_t>().table()](
                        schemacht::postgres::Transaction transaction, const std::exception_ptr& error
                    ) mutable {
      if ( error ) {
        done.fail(error);
        return;
      }

      detail::guarded(done, [&] { done(PushInParts<kind_t>(std::move(transaction), *table, std::move(to_rows)), nullptr); });
    });
  }

  /** @brief The checks of a `verify` while they run, a layer each on any thread: what each found, and the future they settle together. */
  class Checks {
   public:
    /** @param tables The layers' tables as a statement writes them, in the layers' order: what a difference is said of. */
    Checks(std::vector<std::string> tables, schemacht::util::Settler<void> done)
        : _tables(std::move(tables)), _problems(_tables.size()), _pending(_tables.size()), _done(std::move(done)) {}

    /**
     * @brief Notes what the check of the layer numbered `layer` found (`problems`, each in words), or the `error` that kept it
     * from running, and settles the future once every layer's is in.
     */
    void finish(std::size_t layer, std::vector<std::string> problems, const std::exception_ptr& error) {
      {
        const std::scoped_lock lock(_mutex);
        _problems.at(layer) = std::move(problems);
        if ( error && ! _error ) {
          _error = error;
        }

        if ( --_pending > 0 ) {
          return;
        }
      }

      settle();  // by the one thread that brought the count to nothing: no other touches the checks any more
    }

   private:
    std::mutex                            _mutex;
    std::vector<std::string>              _tables;
    std::vector<std::vector<std::string>> _problems;  // of each layer, in the layers' order
    std::exception_ptr                    _error;     // the first check that could not be run
    std::size_t                           _pending;
    schemacht::util::Settler<void>        _done;

    void settle() {
      if ( _error ) {
        _done.fail(_error);
        return;
      }

      try {
        std::vector<std::string> differences;
        for ( std::size_t layer = 0; layer < _tables.size(); ++layer ) {
          for ( const std::string& problem : _problems.at(layer) ) {
            differences.push_back(_tables.at(layer) + ": " + problem);
          }
        }

        if ( differences.empty() ) {
          _done(nullptr);
        } else {
          _done.fail(std::make_exception_ptr(TablesDiffer(std::move(differences))));
        }
      } catch ( ... ) {
        _done.fail(std::current_exception());  // no memory to say what differs: that, then
      }
    }

   public:
    Checks(const Checks&) = delete;
    Checks(Checks&&) = delete;
    Checks& operator=(const Checks&) = delete;
    Checks& operator=(Checks&&) = delete;
    ~Checks() = default;
  };

  /** @brief Checks the layer `kind_t`'s table against its kind's schema, then its settings if it has any, and tells `checks`. */
  template <LayerKind kind_t>
  void check_table(const std::shared_ptr<Checks>& checks, std::size_t layer_number) {
    try {
      _database.execute(
          schemacht::postgres::table_differences_statement<typename kind_t::schema_type>().on(layer<kind_t>().table()),
          [this, checks, layer_number](const std::vector<schemacht::postgres::TableDifference>& differences, const std::exception_ptr& error) {
            if ( error ) {
              checks->finish(layer_number, {}, error);
              return;
            }

            // A table whose columns are the kind's may still lack what the kind keeps in it beside them: a raster's grid.
            // Only such a table is asked for it: of one that differs, the differences say enough, and the question may not
            // even be one the database can answer (no raster type, so no `raster_columns` to ask).
            if constexpr ( HasSettings<kind_t> ) {
              if ( differences.empty() ) {
                check_settings<kind_t>(checks, layer_number);
                return;
              }
            }

            std::vector<std::string> problems;
            try {
              problems.reserve(differences.size());
              for ( const schemacht::postgres::TableDifference& difference : differences ) {
                problems.push_back(schemacht::postgres::describe(difference));
              }
            } catch ( ... ) {
              checks->finish(layer_number, {}, std::current_exception());
              return;
            }

            checks->finish(layer_number, std::move(problems), nullptr);
          }
      );
    } catch ( ... ) {
      checks->finish(layer_number, {}, std::current_exception());
    }
  }

  /** @brief Reads the settings of the layer `kind_t`'s table, whose columns are the kind's, and tells `checks` why if it can't. */
  template <HasSettings kind_t>
  void check_settings(const std::shared_ptr<Checks>& checks, std::size_t layer_number) {
    try {
      _database.execute(settings_statement<kind_t>(), [checks, layer_number](SettingsRows<kind_t> rows, const std::exception_ptr& error) {
        if ( error ) {
          checks->finish(layer_number, {}, error);
          return;
        }

        std::vector<std::string> problems;
        try {
          try {
            std::ignore = kind_t::settings_from_rows(std::move(rows));
          } catch ( const std::exception& unreadable ) {
            problems.emplace_back(unreadable.what());
          }
        } catch ( ... ) {
          checks->finish(layer_number, {}, std::current_exception());
          return;
        }

        checks->finish(layer_number, std::move(problems), nullptr);
      });
    } catch ( ... ) {
      checks->finish(layer_number, {}, std::current_exception());
    }
  }

  /** @brief Makes the layer `kind_t`'s table and runs `setup` on it, in one transaction, and waits for it: see `create_table`. */
  template <LayerKind kind_t>
  void make_table(std::vector<std::string> setup) {
    // Each waited for in turn, so the error is the failing statement's own. A throw drops the transaction, which rolls it back.
    schemacht::postgres::Transaction transaction = _database.begin().get();
    transaction.execute(schemacht::postgres::create_table_statement<typename kind_t::schema_type>().on(layer<kind_t>().table())).get();
    for ( std::string& sql : setup ) {
      transaction.execute(schemacht::postgres::unchecked_sql(std::move(sql))).get();
    }

    transaction.commit().get();
  }

  template <LayerKind kind_t>
  void drop_table() {
    _database.execute(schemacht::postgres::drop_table_statement<typename kind_t::schema_type>().on(layer<kind_t>().table())).get();
  }

  /**
   * @return A future for the rows of the statement `make()` gives, made into a `result_t` by `convert`. It holds the error
   * instead if the statement could not be made or run, or `convert` threw.
   */
  template <schemacht::util::FutureValue result_t, std::invocable make_t, std::invocable<ResultRows<std::invoke_result_t<make_t>>> convert_t>
  [[nodiscard]] std::future<result_t> execute_as(make_t make, convert_t convert) {
    return schemacht::util::future_from<result_t>([&](const schemacht::util::Settler<result_t>& done) {
      detail::guarded(done, [&] {  // the statement may not get made: an argument could not be bound
        _database.execute(make(), [done, convert = std::move(convert)](ResultRows<std::invoke_result_t<make_t>> rows, const std::exception_ptr& error) {
          if ( error ) {
            done.fail(error);
            return;
          }

          detail::guarded(done, [&] { done(convert(std::move(rows)), nullptr); });
        });
      });
    });
  }

 public:
  Miniverse(const Miniverse&) = delete;  // it owns the pool
  Miniverse(Miniverse&&) = delete;
  Miniverse& operator=(const Miniverse&) = delete;
  Miniverse& operator=(Miniverse&&) = delete;
  ~Miniverse() = default;
};

}  // namespace miniverse
