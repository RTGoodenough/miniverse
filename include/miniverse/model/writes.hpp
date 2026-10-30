#pragma once

#include <cstdint>
#include <string>

#include "schemacht/schema/field.hpp"

/**
 * @file
 * The ways a push treats the rows already in a table, one of which a model names as its `writes`: the rows are written as
 * they are (`Plain`); they are made with settings the table keeps (`WithSettings`); or they belong to a `Dataset`, which a
 * push replaces whole (`Datasets`), and the schema then holds the two dataset columns. What each asks of the model is in
 * concepts/miniverse.hpp; what the library does for each is in world/.
 */
namespace miniverse::writes {

/** @brief A push writes its rows, and the model's `write_statement` says what becomes of a row already there. */
struct Plain {};

/** @brief The table is made with `settings_t` (a raster's grid) and keeps them; every push reads them back to make its rows. */
template <typename settings_t>
struct WithSettings {
  using settings_type = settings_t;
};

/**
 * @brief The rows belong to a `Dataset`: a push of a dataset replaces it, `Miniverse::remove` deletes it, and two pushes of
 * one dataset take turns. The model's schema holds `dataset::Name`, the dataset a row belongs to, and `dataset::Stamp`, the
 * stamp of the push that wrote it; the statements written against them are in world/datasets.hpp.
 */
struct Datasets {};

}  // namespace miniverse::writes

/** @brief The columns a model with datasets (`writes::Datasets`) holds in its schema. */
namespace miniverse::dataset {

/** @brief The name of the `Dataset` the row was pushed under: what a push replaces, and `Miniverse::remove` removes. */
using Name = schemacht::schema::Field<std::string, "dataset">;

/** @brief The stamp of the push that last wrote the row: a push's sweep deletes its dataset's rows that carry another. */
using Stamp = schemacht::schema::Field<std::int64_t, "stamp">;

}  // namespace miniverse::dataset
