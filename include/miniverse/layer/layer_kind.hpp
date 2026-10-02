#pragma once

#include <concepts>
#include <type_traits>
#include <utility>
#include <vector>

#include "miniverse/geo/types.hpp"
#include "schemacht/postgres/concepts/statements.hpp"
#include "schemacht/schema/concepts/schema.hpp"

/**
 * What a layer of a miniverse is: a kind of data (roads, elevation, ...) with the table it is stored in. A kind is a type,
 * used only as a name, never made; `Miniverse<Roads, Elevation>` holds the layers `Roads` and `Elevation`.
 *
 * A kind says:
 * - `result_type`: what a load gives, decoded (`Ways`, say), for the worker to build what it wants from.
 * - `schema_type`: its table, as a schemacht `Schema`. The table's name is part of the kind, so a second dataset of the same
 *   kind is a second type: `struct Roads : RoadLayer<"osm_roads"> {};` and `struct Tracks : RoadLayer<"gps_tracks"> {};`.
 * - `load_statement(location)`: the statement that reads what lies in `location`, a polygon in WGS 84.
 * - `from_rows(rows)`: that statement's rows made into a `result_type`, and `to_rows(result)` the other way, for writing.
 * - Optionally, `setup_statement_type`: a raw statement without arguments to run once the table is made, such as its spatial index.
 *
 * miniverse's own kinds (`RoadLayer`) are built this way, and a kind of your own needs only to meet `LayerKind` to be loaded
 * and pushed like them.
 */
namespace miniverse {

namespace detail {

/** @brief The rows `kind_t`'s load statement gives. */
template <typename kind_t>
using load_rows_t = std::vector<typename decltype(kind_t::load_statement(std::declval<const geo::Polygon&>()))::result_type>;

}  // namespace detail

/** @brief Whether `kind_t` is a layer kind: see the top of this file. */
template <typename kind_t>
concept LayerKind = requires {
  typename kind_t::result_type;
  typename kind_t::schema_type;
} && schemacht::schema::IsSchema<typename kind_t::schema_type> && requires(const geo::Polygon& location, const kind_t::result_type& data) {
  { kind_t::load_statement(location) } -> schemacht::postgres::RunnableStatement;
  { kind_t::to_rows(data) } -> std::same_as<std::vector<typename kind_t::schema_type::row_type>>;
} && requires(detail::load_rows_t<kind_t> rows) {
  { kind_t::from_rows(std::move(rows)) } -> std::same_as<typename kind_t::result_type>;
};

/** @brief Whether `kind_t` names statements to run once its table is made (`setup_statement_type`), such as an index. */
template <typename kind_t>
concept HasSetupStatement = requires {
  { kind_t::setup_statement_type::bind() } -> schemacht::postgres::CommandStatement;
};

}  // namespace miniverse
