#pragma once

#include <cstdint>
#include <string>

#include "schemacht/query/command.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/prepared.hpp"
#include "schemacht/schema/concepts/schema.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/table_name.hpp"

/**
 * @file
 * What every kind with datasets keeps in its table, and does with it: the columns that say which dataset a row belongs to and
 * which push wrote it, the sweep and the remove of a dataset written against them, and the index the two use. A kind of your
 * own with datasets puts the two columns in its schema and takes the rest as they are (see `FeatureLayer`).
 */
namespace miniverse::dataset {

/** @brief The name of the `Dataset` the row was pushed under: what a push replaces, and `Miniverse::remove` removes. */
using Name = schemacht::schema::Field<std::string, "dataset">;

/** @brief The stamp of the push that last wrote the row: a push's sweep deletes its dataset's rows that carry another. */
using Pushed = schemacht::schema::Field<std::int64_t, "pushed">;

/** @brief The delete of the rows of `schema_t` of the dataset named by argument 0 that a push, whose stamp is argument 1, did not write. */
template <schemacht::schema::IsSchema schema_t>
inline constexpr auto SWEEP = schemacht::query::delete_where(
    schemacht::query::On<schema_t>::template col<"dataset">() == schemacht::query::arg<0>() &&
    schemacht::query::On<schema_t>::template col<"pushed">() != schemacht::query::arg<1>()
);

/** @brief The sweep of a dataset, prepared: what a kind names as its `sweep_statement_type`. */
template <schemacht::schema::IsSchema schema_t>
using Sweep = schemacht::query::Prepared<SWEEP<schema_t>>;

/** @brief The delete of every row of `schema_t` of the dataset named by argument 0. */
template <schemacht::schema::IsSchema schema_t>
inline constexpr auto REMOVE =
    schemacht::query::delete_where(schemacht::query::On<schema_t>::template col<"dataset">() == schemacht::query::arg<0>());

/** @brief The remove of a dataset, prepared: what a kind names as its `remove_statement_type`. */
template <schemacht::schema::IsSchema schema_t>
using Remove = schemacht::query::Prepared<REMOVE<schema_t>>;

/**
 * @return The index on `dataset` that a push's sweep and a remove use, for a kind's `setup_sql`. It is not named: PostgreSQL
 * names it after the table (`roads_dataset_idx` for `roads`), shortening and numbering the name as needed, so the name is unique.
 */
[[nodiscard]] inline std::string index_sql(const schemacht::schema::TableName& table) { return "CREATE INDEX ON " + table.quoted() + " (dataset)"; }

}  // namespace miniverse::dataset
