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

#include "miniverse/geo/column_types.hpp"  // IWYU pragma: export
#include "miniverse/geo/types.hpp"         // IWYU pragma: export
#include "miniverse/layer/layer_kind.hpp"  // IWYU pragma: export
#include "miniverse/layer/road_layer.hpp"  // IWYU pragma: export
#include "schemacht/postgres/database.hpp"

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
 * `std::future`, so a worker can start loading several layers at once and wait for them together.
 */
namespace miniverse {

namespace detail {

template <std::size_t count>
[[nodiscard]] consteval bool distinct_names(const std::array<std::string_view, count>& names) {
  for ( std::size_t i = 0; i < count; ++i ) {
    for ( std::size_t j = i + 1; j < count; ++j ) {
      if ( names.at(i) == names.at(j) ) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace detail

/** @brief The layers `kind_ts`, each in its own table of one PostGIS database. */
template <LayerKind... kind_ts>
class Miniverse {
 public:
  static_assert(
      detail::distinct_names(std::array<std::string_view, sizeof...(kind_ts)>{kind_ts::schema_type::TABLE_NAME...}),
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
  template <typename kind_t>
  [[nodiscard]] std::future<typename kind_t::result_type> load(const geo::Polygon& location) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    using result_t = kind_t::result_type;
    const auto            promise = std::make_shared<std::promise<result_t>>();
    std::future<result_t> future = promise->get_future();

    _database.execute_into(kind_t::load_statement(location), [promise](detail::load_rows_t<kind_t> rows, const std::exception_ptr& error) {
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

    return future;
  }

  /** @return A future that completes once `data` is written to the layer `kind_t`'s table, or holds the error (nothing is then written). */
  template <typename kind_t>
  [[nodiscard]] std::future<void> push(const kind_t::result_type& data) {
    static_assert(holds<kind_t>(), "this miniverse has no such layer: add the kind to Miniverse<...>");

    try {
      return _database.insert<typename kind_t::schema_type>(kind_t::to_rows(data));
    } catch ( ... ) {
      return failed(std::current_exception());  // a value the table cannot hold, found while making the rows
    }
  }

  /**
   * @brief Makes every layer's table and what its kind sets up with it (a spatial index, say), and waits for them: for tests
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

  template <typename kind_t>
  [[nodiscard]] static consteval bool holds() {
    return (std::same_as<kind_t, kind_ts> || ...);
  }

  template <typename kind_t>
  void create_table() {
    _database.create_table<typename kind_t::schema_type>().get();
    if constexpr ( HasSetupStatement<kind_t> ) {
      std::ignore = _database.execute(kind_t::setup_statement_type::bind()).get();
    }
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
