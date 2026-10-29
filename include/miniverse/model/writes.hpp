#pragma once

#include <string>
#include <vector>

#include "miniverse/dataset.hpp"
#include "miniverse/model/dataset_columns.hpp"  // IWYU pragma: export
#include "schemacht/query/command.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/prepared.hpp"
#include "schemacht/schema/concepts/schema.hpp"
#include "schemacht/schema/table_name.hpp"

/**
 * @file
 * The ways a push treats the rows already in a table, one of which a model names as its `writes`: the rows are written as
 * they are (`Plain`); they are made with settings the table keeps (`WithSettings`); or they belong to a `Dataset`, which a
 * push replaces whole (`Datasets`). What each asks of the model is in concepts/miniverse.hpp.
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
 * one dataset take turns. The model's schema holds `dataset::Name`, the dataset a row belongs to, and `dataset::Pushed`, the
 * stamp of the push that wrote it; the statements written against them are here.
 */
struct Datasets {
  /** @brief The delete of the rows of `schema_t` of the dataset named by argument 0 that the push whose stamp is argument 1 did not write. */
  template <schemacht::schema::IsSchema schema_t>
  static constexpr auto SWEEP = schemacht::query::delete_where(
      schemacht::query::On<schema_t>::template col<dataset::Name::COLUMN_NAME>() == schemacht::query::arg<0>() &&
      schemacht::query::On<schema_t>::template col<dataset::Pushed::COLUMN_NAME>() != schemacht::query::arg<1>()
  );

  /** @brief The sweep, prepared: run before a push of the dataset commits. */
  template <schemacht::schema::IsSchema schema_t>
  using sweep = schemacht::query::Prepared<SWEEP<schema_t>>;

  /** @brief The delete of every row of `schema_t` of the dataset named by argument 0. */
  template <schemacht::schema::IsSchema schema_t>
  static constexpr auto REMOVE =
      schemacht::query::delete_where(schemacht::query::On<schema_t>::template col<dataset::Name::COLUMN_NAME>() == schemacht::query::arg<0>());

  /** @brief The remove of a dataset, prepared. */
  template <schemacht::schema::IsSchema schema_t>
  using remove = schemacht::query::Prepared<REMOVE<schema_t>>;

  /**
   * @return What runs after the model's own setup: the index on `dataset` that the sweep and the remove use. It is not named:
   * PostgreSQL names it after the table (`roads_dataset_idx` for `roads`), shortening and numbering the name as needed, so the
   * name is unique.
   */
  [[nodiscard]] static std::vector<std::string> setup_sql(const schemacht::schema::TableName& table) {
    return {"CREATE INDEX ON " + table.quoted() + " (dataset)"};
  }

  /** @return The key of the advisory lock a push or a remove of `dataset` in `table` holds while it runs: a dataset name in two tables is two locks. */
  [[nodiscard]] static std::string lock_key(const schemacht::schema::TableName& table, const Dataset& dataset) {
    return table.quoted() + '/' + dataset.name;
  }
};

}  // namespace miniverse::writes
