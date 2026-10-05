#pragma once

// A layer kind written by hand, as a user would: places, as points, loaded by polygon. Its conversions are only declared,
// since the concepts check their signatures, not their bodies. It needs only the geo headers, not the built-in kinds:
// tests/compile/own_kind_test.cpp makes a miniverse of it with world.hpp alone.

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "miniverse/geo/column_types.hpp"
#include "miniverse/geo/operations.hpp"
#include "miniverse/geo/types.hpp"
#include "schemacht/postgres/statements.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/prepared.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/schema.hpp"
#include "schemacht/schema/table_name.hpp"

namespace test {

using PlacesSchema = schemacht::schema::Schema<
    "places", schemacht::schema::Field<std::int64_t, "place_id", schemacht::schema::KeyRole::Primary>,
    schemacht::schema::Field<miniverse::geo::Point, "position">>;

constexpr auto PLACES_IN =
    schemacht::query::select(schemacht::query::On<PlacesSchema>::col<"position">().apply<miniverse::geo::Intersects>(schemacht::query::arg<0>()));

struct Places {
  using result_type = std::vector<miniverse::geo::Point>;
  using schema_type = PlacesSchema;
  using load_statement_type = schemacht::query::Prepared<PLACES_IN>;

  static std::vector<std::string>                                        setup_sql(const schemacht::schema::TableName& table);
  static std::vector<std::vector<schema_type::row_type>>                 to_rows(result_type places);
  static schemacht::postgres::SchemaStatement<schema_type, std::tuple<>> write_statement(const std::vector<schema_type::row_type>& rows);
  static result_type from_rows(std::vector<load_statement_type::result_type> rows, const miniverse::geo::Polygon& location);
};

}  // namespace test
