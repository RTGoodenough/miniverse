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
 * The settings a layer of a kind with settings was made with (a raster's grid): read back from its table, or asked of its
 * reader. What `Miniverse::table_settings` gives, and what a push of such a kind makes its rows with.
 */
namespace miniverse::detail {

/** @return The statement that reads the settings the table named `table` was made with. */
template <HasSettings kind_t>
[[nodiscard]] auto settings_statement(const schemacht::schema::TableName& table) {
  return kind_t::settings_statement_type::bind(table.quoted());
}

/** @return A future for the settings `table` was made with, read from `database`. */
template <HasSettings kind_t>
[[nodiscard]] std::future<typename kind_t::settings_type> table_settings(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table
) {
  return execute_as<typename kind_t::settings_type>(
      database, [&] { return settings_statement<kind_t>(table.name()); }, [](auto rows) { return kind_t::settings_from_rows(std::move(rows)); }
  );
}

/** @return A future for `reader`'s settings, asked on the calling thread. */
template <HasSettings kind_t>
[[nodiscard]] std::future<typename kind_t::settings_type> reader_settings(const std::shared_ptr<const Reader<kind_t>>& reader) {
  return schemacht::util::future_from<typename kind_t::settings_type>([&](const schemacht::util::Settler<typename kind_t::settings_type>& done) {
    guarded(done, [&] { done(reader->settings(), nullptr); });
  });
}

}  // namespace miniverse::detail
