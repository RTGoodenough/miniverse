#pragma once

#include <concepts>
#include <utility>
#include <vector>

#include "miniverse/geo/types.hpp"
#include "schemacht/postgres/concepts/statements.hpp"

/**
 * What a layer of a miniverse is: a kind of data (roads, elevation, ...) with the table it is stored in. A kind is a type,
 * used only as a name, never made; `Miniverse<Roads, Elevation>` holds the layers `Roads` and `Elevation`.
 *
 * A kind says:
 * - `result_type`: what a load gives, decoded (`Ways`, say), for the worker to build what it wants from.
 * - `schema_type`: its table, as a schemacht `Schema`. The table's name is part of the kind, so a second dataset of the same
 *   kind is a second type: `struct Roads : RoadLayer<"osm_roads"> {};` and `struct Tracks : RoadLayer<"gps_tracks"> {};`.
 * - `load_statement_type`: the query that reads what lies in a location, a `schemacht::query::Prepared` whose one argument is
 *   a polygon in WGS 84 (with `geo::Intersects`, say).
 * - `setup_statement_type`: a statement without arguments that runs once the table is made, such as its spatial index.
 * - `from_rows(rows)`: the load statement's rows made into a `result_type`, and `to_rows(result)` the other way, for writing.
 *
 * miniverse's own kinds (`RoadLayer`) are built this way, and a kind of your own needs only to meet `LayerKind` to be loaded
 * and pushed like them.
 */
namespace miniverse {

/** @brief Whether `kind_t` is a layer kind: see the top of this file. */
template <typename kind_t>
concept LayerKind =
    requires {
      typename kind_t::result_type;
      typename kind_t::schema_type;
      typename kind_t::load_statement_type::result_type;
      typename kind_t::setup_statement_type;
    } && schemacht::postgres::TextSchema<typename kind_t::schema_type> &&
    requires(const geo::Polygon& location, kind_t::result_type data, std::vector<typename kind_t::load_statement_type::result_type> rows) {
      { kind_t::load_statement_type::bind(location) } -> schemacht::postgres::RunnableStatement;
      { kind_t::setup_statement_type::bind() } -> schemacht::postgres::CommandStatement;
      { kind_t::to_rows(std::move(data)) } -> std::same_as<std::vector<typename kind_t::schema_type::row_type>>;
      { kind_t::from_rows(std::move(rows)) } -> std::same_as<typename kind_t::result_type>;
    };

}  // namespace miniverse
