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

#include "miniverse/concepts/miniverse.hpp"     // IWYU pragma: export
#include "miniverse/geo/column_types.hpp"       // IWYU pragma: export
#include "miniverse/geo/operations.hpp"         // IWYU pragma: export
#include "miniverse/geo/raster.hpp"             // IWYU pragma: export
#include "miniverse/geo/types.hpp"              // IWYU pragma: export
#include "miniverse/layer.hpp"                  // IWYU pragma: export
#include "miniverse/layer/elevation_layer.hpp"  // IWYU pragma: export
#include "miniverse/layer/road_layer.hpp"       // IWYU pragma: export
#include "schemacht/postgres/async_client.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/schema/table.hpp"
#include "schemacht/schema/table_name.hpp"
#include "schemacht/util/compile_time.hpp"
#include "schemacht/util/futures.hpp"

/**
 * A miniverse: the layers of the world a worker reads, each a kind of data in its own PostGIS table, loaded by polygon.
 *
 * @code
 * struct Roads : miniverse::RoadLayer {};
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
 * from a kind's own conversions, arrive through the future, never as a throw from the call itself.
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

    return table_of<kind_t>().name();
  }

  /**
   * @return A future for what the layer `kind_t` holds in `location`, a polygon in WGS 84 longitude and latitude (what that
   * is, the kind says: for a `RoadLayer`, the ways that intersect it). It holds the error instead if the load failed.
   */
  template <LayerKind kind_t>
  [[nodiscard]] std::future<typename kind_t::result_type> load(const geo::Polygon& location) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    return execute_as<typename kind_t::result_type>(
        [&] { return kind_t::load_statement_type::bind(location).on(table_of<kind_t>()); },
        [location](auto rows) { return kind_t::from_rows(std::move(rows), location); }
    );
  }

  /** @return A future for the settings the layer `kind_t`'s table was made with (an elevation's grid), read from the database. */
  template <HasSettings kind_t>
  [[nodiscard]] std::future<typename kind_t::settings_type> settings() {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    return execute_as<typename kind_t::settings_type>(
        [&] { return kind_t::settings_statement_type::bind(table_name<kind_t>().quoted()); },
        [](auto rows) { return kind_t::settings_from_rows(std::move(rows)); }
    );
  }

  /**
   * @return A future that completes once `data` is written to the layer `kind_t`'s table, or holds the error (nothing is then
   * written). `data` is taken by value and moved into the rows: pass it with `std::move` unless it is still needed.
   *
   * A kind with settings (`HasSettings`) is written with the settings read from its table: an elevation raster is cut into
   * the table's tiles. The insert is started from the settings' completion, so the miniverse must outlive the future.
   */
  template <LayerKind kind_t>
  [[nodiscard]] std::future<void> push(kind_t::result_type data) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    try {
      return schemacht::util::future_from<void>([&](schemacht::util::Settler<void> done) {
        if constexpr ( HasSettings<kind_t> ) {
          _database.execute(
              kind_t::settings_statement_type::bind(table_name<kind_t>().quoted()),
              [this, data = std::move(data), done](SettingsRows<kind_t> rows, const std::exception_ptr& error) mutable {
                if ( error ) {
                  done(error);
                  return;
                }

                try {
                  insert<kind_t>(kind_t::to_rows(std::move(data), kind_t::settings_from_rows(std::move(rows))), done);
                } catch ( ... ) {
                  done(std::current_exception());  // the settings or rows couldn't be made, or the insert couldn't start
                }
              }
          );
        } else {
          insert<kind_t>(kind_t::to_rows(std::move(data), NoSettings{}), done);
        }
      });
    } catch ( ... ) {
      return failed(std::current_exception());  // the kind's to_rows threw, or the first statement couldn't start
    }
  }

  /**
   * @brief Makes the layer `kind_t`'s table with `settings` (an elevation's grid), and runs its kind's setup (a spatial index,
   * say), and waits for them: for tests and a first start. PostGIS must already be enabled in the database (`CREATE EXTENSION
   * postgis`, and `postgis_raster` for a raster).
   * @throws schemacht::postgres::QueryError if the table exists already, or can't be made, or its setup fails (the table is
   * then dropped again).
   * @throws std::invalid_argument if the kind refuses `settings`.
   */
  template <LayerKind kind_t>
  void create_table(const kind_t::settings_type& settings) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    std::vector<std::string> setup = kind_t::setup_sql(table_name<kind_t>(), settings);  // checks the settings first
    _database.execute(schemacht::postgres::create_table_statement<typename kind_t::schema_type>().on(table_of<kind_t>())).get();

    try {
      for ( std::string& sql : setup ) {
        run_text(std::move(sql)).get();
      }
    } catch ( ... ) {
      // Without its setup the table can't be used, and it would stop the next create_table: drop it, and report why.
      const std::exception_ptr error = std::current_exception();
      try {
        drop_table<kind_t>();
      } catch ( ... ) {  // NOLINT(bugprone-empty-catch) -- the setup's error is the one to report
      }

      std::rethrow_exception(error);
    }
  }

  /** @brief As above, for a kind without settings. */
  template <LayerKind kind_t>
    requires(! HasSettings<kind_t>)
  void create_table() {
    create_table<kind_t>(NoSettings{});
  }

  /** @brief Makes every layer's table, as `create_table`, when no kind has settings. */
  void create_tables() {
    static_assert(
        (! HasSettings<kind_ts> && ...),
        "create_tables makes only tables without settings: make a kind with settings with create_table<Kind>(settings)"
    );

    if constexpr ( (! HasSettings<kind_ts> && ...) ) {  // else only the assertion's error, not a second one
      (create_table<kind_ts>(NoSettings{}), ...);
    }
  }

  /** @brief Drops every layer's table, those that exist, and waits for it. */
  void drop_tables() { (drop_table<kind_ts>(), ...); }

  /** @return The pool, for what the layers don't cover: a query of your own on the same connections. */
  [[nodiscard]] schemacht::postgres::Database& database() noexcept { return _database; }

 private:
  std::tuple<Layer<kind_ts>...> _layers;  // before the pool: checked before any connection is opened
  schemacht::postgres::Database _database;

  /** @brief The rows a statement gives. */
  template <schemacht::postgres::RunnableStatement statement_t>
  using RowsOf = std::vector<typename statement_t::result_type>;

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
  [[nodiscard]] const schemacht::schema::Table<typename kind_t::schema_type>& table_of() const noexcept {
    return std::get<Layer<kind_t>>(_layers).table();
  }

  template <LayerKind kind_t>
  void drop_table() {
    _database.execute(schemacht::postgres::drop_table_statement<typename kind_t::schema_type>().on(table_of<kind_t>())).get();
  }

  /** @brief Inserts `rows` into `kind_t`'s table, settling `done` with the outcome. */
  template <LayerKind kind_t>
  void insert(std::vector<typename kind_t::schema_type::row_type> rows, const schemacht::util::Settler<void>& done) {
    _database.execute(
        schemacht::postgres::insert_statement<typename kind_t::schema_type>(rows).on(table_of<kind_t>()),
        [done](std::uint64_t /*count*/, const std::exception_ptr& error) { done(error); }
    );
  }

  /**
   * @return A future for the rows of the statement `make()` gives, made into a `result_t` by `convert`. It holds the error
   * instead if the statement could not be made or run, or `convert` threw.
   */
  template <typename result_t, std::invocable make_t, std::invocable<RowsOf<std::invoke_result_t<make_t>>> convert_t>
  [[nodiscard]] std::future<result_t> execute_as(make_t make, convert_t convert) {
    const auto            promise = std::make_shared<std::promise<result_t>>();
    std::future<result_t> future = promise->get_future();

    try {
      _database.execute(make(), [promise, convert = std::move(convert)](RowsOf<std::invoke_result_t<make_t>> rows, const std::exception_ptr& error) {
        if ( error ) {
          promise->set_exception(error);
          return;
        }

        try {
          promise->set_value(convert(std::move(rows)));
        } catch ( ... ) {
          promise->set_exception(std::current_exception());
        }
      });
    } catch ( ... ) {
      promise->set_exception(std::current_exception());  // the statement could not be made: an argument could not be bound
    }

    return future;
  }

  /**
   * @return A future for a statement given as text that returns no rows (a kind's setup). It repeats schemacht's private
   * `run_without_rows` until schemacht has a public call for it (docs/open-decisions.md).
   */
  [[nodiscard]] std::future<void> run_text(std::string sql) {
    const auto        promise = std::make_shared<std::promise<void>>();
    std::future<void> future = promise->get_future();

    _database.pool().run(std::move(sql), {}, [promise](const schemacht::postgres::QueryResult& /*result*/, const std::exception_ptr& error) {
      schemacht::util::settle(*promise, error);
    });

    return future;
  }

  [[nodiscard]] static std::future<void> failed(const std::exception_ptr& error) {
    std::promise<void> promise;
    promise.set_exception(error);

    return promise.get_future();
  }

 public:
  Miniverse(const Miniverse&) = delete;  // it owns the pool
  Miniverse(Miniverse&&) = delete;
  Miniverse& operator=(const Miniverse&) = delete;
  Miniverse& operator=(Miniverse&&) = delete;
  ~Miniverse() = default;
};

}  // namespace miniverse
