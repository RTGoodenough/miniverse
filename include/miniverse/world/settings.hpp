#pragma once

#include <future>
#include <memory>
#include <utility>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/reader.hpp"
#include "miniverse/world/async.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/schema/table.hpp"
#include "schemacht/schema/table_name.hpp"
#include "schemacht/util/futures.hpp"

/**
 * @file
 * The settings a layer of a model with settings was made with (a raster's grid): read back from its table, or asked of its
 * reader. What `Miniverse::table_settings` gives, and what a push of such a model makes its rows with.
 */
namespace miniverse::detail {

/** @return The statement that reads the settings the table named `table` was made with. */
template <WritesSettings model_t>
[[nodiscard]] auto settings_statement(const schemacht::schema::TableName& table) {
  return model_t::settings_statement_type::bind(table.quoted());
}

/** @return A future for the settings `table` was made with, read from `database` on a thread started for it. */
template <WritesSettings model_t>
[[nodiscard]] std::future<SettingsOf<model_t>> table_settings(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename model_t::schema_type>& table
) {
  return on_a_thread([&database, &table] { return model_t::settings_from_rows(database.execute(settings_statement<model_t>(table.name())).get()); });
}

/** @return A future for `reader`'s settings, asked on the calling thread. */
template <WritesSettings model_t>
[[nodiscard]] std::future<SettingsOf<model_t>> reader_settings(const std::shared_ptr<const Reader<model_t>>& reader) {
  return schemacht::util::future_from<SettingsOf<model_t>>([&](const schemacht::util::Settler<SettingsOf<model_t>>& done) {
    guarded(done, [&] { done(reader->settings(), nullptr); });
  });
}

}  // namespace miniverse::detail
