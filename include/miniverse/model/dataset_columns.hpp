#pragma once

#include <cstdint>
#include <string>

#include "schemacht/schema/field.hpp"

/**
 * @file
 * The columns a model with datasets (`writes::Datasets`) holds in its schema: which dataset a row belongs to, and which push
 * wrote it. The statements written against them are the policy's (writes.hpp).
 */
namespace miniverse::dataset {

/** @brief The name of the `Dataset` the row was pushed under: what a push replaces, and `Miniverse::remove` removes. */
using Name = schemacht::schema::Field<std::string, "dataset">;

/** @brief The stamp of the push that last wrote the row: a push's sweep deletes its dataset's rows that carry another. */
using Pushed = schemacht::schema::Field<std::int64_t, "pushed">;

}  // namespace miniverse::dataset
