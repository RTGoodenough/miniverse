#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
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
#include "schemacht/postgres/transaction.hpp"
#include "schemacht/schema/table.hpp"
#include "schemacht/schema/table_name.hpp"
#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/futures.hpp"

/**
 * A miniverse: the layers of the world a worker reads, each a kind of data in its own PostGIS table, loaded by polygon.
 * This header holds the `Miniverse` alone; miniverse.hpp adds the built-in kinds (`RoadLayer`, `ElevationLayer`).
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
 * `std::future`, so a worker can start loading several layers at once and wait for them together. Errors, including one
 * from a kind's own conversions, arrive through the future, never as a throw from the call itself. The miniverse must outlive
 * the futures it hands out: wait on them before it is destroyed.
 */
namespace miniverse {

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
   * pool's connections until it ends (with one connection, nothing else runs during a push).
   *
   * The kind's `write_statement` says what happens to a row already there: a `RoadLayer`'s way fails the push, and an
   * `ElevationLayer`'s tile is merged with it (new pixels win, and where the new tile has no data the old pixel stays). Pushes
   * that overlap are merged in the order they commit: where both have data, the last to commit wins.
   */
  template <LayerKind kind_t>
  [[nodiscard]] std::future<void> push(kind_t::result_type data) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    return schemacht::util::future_from<void>([&](const schemacht::util::Settler<void>& done) {
      if constexpr ( HasSettings<kind_t> ) {
        guarded(done, [&] {
          _database.execute(
              settings_statement<kind_t>(),
              [this, done, data = std::move(data)](SettingsRows<kind_t> rows, const std::exception_ptr& error) mutable {
                if ( error ) {
                  done(error);
                  return;
                }

                guarded(done, [&] { write_rows<kind_t>(kind_t::to_rows(std::move(data), kind_t::settings_from_rows(std::move(rows))), done); });
              }
          );
        });

      } else {
        guarded(done, [&] { write_rows<kind_t>(kind_t::to_rows(std::move(data)), done); });
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

  /** @brief Runs `step`; what it throws settles `done` with the error instead. For the steps of a push, on any thread. */
  template <std::invocable step_t>
  static void guarded(const schemacht::util::Settler<void>& done, step_t step) {
    try {
      step();
    } catch ( ... ) {
      done(std::current_exception());
    }
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

  /** @brief A push's write while it runs: its batches, sent one statement at a time inside one transaction. */
  template <LayerKind kind_t>
  struct Write {
    RowBatches<kind_t>                                             batches;
    schemacht::postgres::Transaction                               transaction;
    schemacht::util::Settler<void>                                 done;
    const schemacht::schema::Table<typename kind_t::schema_type>* table = nullptr;
    std::size_t                                                    sent = 0;  ///< Batches written so far; the next to send.
  };

  /** @brief Writes `batches` to the layer `kind_t`'s table in one transaction, and settles `done` with the outcome. */
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

      guarded(done, [&] {
        send_next<kind_t>(std::make_shared<Write<kind_t>>(
            Write<kind_t>{.batches = std::move(batches), .transaction = std::move(transaction), .done = done, .table = table}
        ));
      });
    });
  }

  /**
   * @brief Sends `write`'s next batch, whose completion sends the one after, and so on; once every batch is written, commits.
   * One batch at a time is made into a statement's text, on a thread of the pool's, and the batch is let go of once it is.
   * A batch that fails rolls the transaction back.
   */
  template <LayerKind kind_t>
  static void send_next(std::shared_ptr<Write<kind_t>> write) {
    Write<kind_t>& state = *write;
    guarded(state.done, [&] {
      if ( state.sent == state.batches.size() ) {
        state.transaction.commit([write](const std::exception_ptr& error) { write->done(error); });
        return;
      }

      const RowsOf<kind_t> batch = std::move(state.batches.at(state.sent));  // the statement copies it, so it ends with this step
      ++state.sent;
      state.transaction.execute(kind_t::write_statement(batch).on(*state.table), [write](std::uint64_t /*count*/, const std::exception_ptr& error) mutable {
        if ( error ) {
          roll_back<kind_t>(std::move(write), error);
          return;
        }

        send_next<kind_t>(std::move(write));
      });
    });
  }

  /** @brief Rolls `write`'s transaction back, and settles it with `error`, the failing batch's own, whatever the rollback says. */
  template <LayerKind kind_t>
  static void roll_back(std::shared_ptr<Write<kind_t>> write, const std::exception_ptr& error) {
    try {
      write->transaction.rollback([write, error](const std::exception_ptr& /*rolled_back*/) { write->done(error); });
    } catch ( ... ) {
      write->done(error);  // the rollback couldn't be sent: dropping the transaction rolls it back
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
      try {
        _database.execute(make(), [done, convert = std::move(convert)](ResultRows<std::invoke_result_t<make_t>> rows, const std::exception_ptr& error) {
          if ( error ) {
            done.fail(error);
            return;
          }

          try {
            done(convert(std::move(rows)), nullptr);
          } catch ( ... ) {
            done.fail(std::current_exception());
          }
        });
      } catch ( ... ) {
        done.fail(std::current_exception());  // the statement could not be made: an argument could not be bound
      }
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
