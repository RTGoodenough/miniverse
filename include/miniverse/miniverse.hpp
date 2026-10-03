#pragma once

#include <array>
#include <concepts>
#include <exception>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "miniverse/concepts/miniverse.hpp"  // IWYU pragma: export
#include "miniverse/geo/column_types.hpp"    // IWYU pragma: export
#include "miniverse/geo/operations.hpp"      // IWYU pragma: export
#include "miniverse/geo/types.hpp"           // IWYU pragma: export
#include "miniverse/layer/road_layer.hpp"    // IWYU pragma: export
#include "schemacht/postgres/database.hpp"
#include "schemacht/util/compile_time.hpp"

/**
 * A miniverse: the layers of the world a worker reads, each a kind of data in its own PostGIS table, loaded by polygon.
 *
 * @code
 * struct Roads : miniverse::RoadLayer<"osm_roads"> {};
 *
 * miniverse::Miniverse<Roads> world("host=db dbname=gis user=worker");
 *
 * const miniverse::geo::Polygon area = ...;                               // WGS 84 longitude and latitude
 * std::future<miniverse::Ways> roads = world.load<Roads>(area);           // the ways that intersect it
 * for ( const miniverse::Way& way : roads.get() ) { ... }
 * @endcode
 *
 * The layers are the template's arguments: asking for a layer the miniverse does not hold does not compile. The miniverse
 * owns one pool of connections (a schemacht `postgres::Database`) that every layer shares, and every call returns a
 * `std::future`, so a worker can start loading several layers at once and wait for them together. Errors, including one
 * from a kind's own conversions, arrive through the future, never as a throw from the call itself.
 */
namespace miniverse {

/** @brief The layers `kind_ts`, each in its own table of one PostGIS database. */
template <LayerKind... kind_ts>
class Miniverse {
 public:
  static_assert(
      schemacht::util::all_distinct(std::array<std::string_view, sizeof...(kind_ts)>{kind_ts::schema_type::TABLE_NAME...}),
      "two layers of the miniverse are stored in the same table"
  );

  /**
   * @brief Opens the pool of connections to the database at `conninfo` (a libpq connection string or URL).
   * @throws std::runtime_error if a connection can't be made.
   */
  explicit Miniverse(const std::string& conninfo, const schemacht::postgres::Database::Options& options = {}) : _database(conninfo, options) {}

  /**
   * @return A future for what the layer `kind_t` holds in `location`, a polygon in WGS 84 longitude and latitude (what that
   * is, the kind says: for a `RoadLayer`, the ways that intersect it). It holds the error instead if the load failed.
   */
  template <LayerKind kind_t>
  [[nodiscard]] std::future<typename kind_t::result_type> load(const geo::Polygon& location) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    using result_t = kind_t::result_type;
    using rows_t = std::vector<typename kind_t::load_statement_type::result_type>;
    const auto            promise = std::make_shared<std::promise<result_t>>();
    std::future<result_t> future = promise->get_future();

    try {
      _database.execute_into(kind_t::load_statement_type::bind(location), [promise](rows_t rows, const std::exception_ptr& error) {
        if ( error ) {
          promise->set_exception(error);
          return;
        }

        try {
          promise->set_value(kind_t::from_rows(std::move(rows)));
        } catch ( ... ) {
          promise->set_exception(std::current_exception());
        }
      });
    } catch ( ... ) {
      promise->set_exception(std::current_exception());  // the location could not be bound
    }

    return future;
  }

  /**
   * @return A future that completes once `data` is written to the layer `kind_t`'s table, or holds the error (nothing is then
   * written). `data` is taken by value and moved into the rows: pass it with `std::move` unless it is still needed.
   */
  template <LayerKind kind_t>
  [[nodiscard]] std::future<void> push(kind_t::result_type data) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    try {
      return _database.insert<typename kind_t::schema_type>(kind_t::to_rows(std::move(data)));
    } catch ( ... ) {
      return failed(std::current_exception());  // the kind's to_rows threw
    }
  }

  /**
   * @brief Makes every layer's table and runs its kind's setup statement (a spatial index, say), and waits for them: for tests
   * and a first start. PostGIS must already be enabled in the database (`CREATE EXTENSION postgis`).
   * @throws schemacht::postgres::QueryError if a table exists already, or can't be made.
   */
  void create_tables() { (create_table<kind_ts>(), ...); }

  /** @brief Drops every layer's table, those that exist, and waits for it. */
  void drop_tables() { (_database.drop_table<typename kind_ts::schema_type>().get(), ...); }

  /** @return The pool, for what the layers don't cover: a query of your own on the same connections. */
  [[nodiscard]] schemacht::postgres::Database& database() noexcept { return _database; }

 private:
  schemacht::postgres::Database _database;

  template <LayerKind kind_t>
  [[nodiscard]] static consteval bool holds() {
    return (std::same_as<kind_t, kind_ts> || ...);
  }

  template <LayerKind kind_t>
  void create_table() {
    _database.create_table<typename kind_t::schema_type>().get();
    std::ignore = _database.execute(kind_t::setup_statement_type::bind()).get();
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
