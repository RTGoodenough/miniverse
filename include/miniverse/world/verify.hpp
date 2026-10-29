#pragma once

#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/reader.hpp"
#include "miniverse/world/registry.hpp"
#include "miniverse/world/settings.hpp"
#include "schemacht/postgres/async_client.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/postgres/table_differences.hpp"
#include "schemacht/schema/table.hpp"
#include "schemacht/util/futures.hpp"

/**
 * @file
 * The checks of a `Miniverse::verify`: each layer's table against the registry's record of it, then its model's schema and
 * settings, or its reader asked for its problems, all at once, settled together as one `TablesDiffer` naming every difference.
 */
namespace miniverse {

/**
 * @brief What `Miniverse::verify` fails with when a layer's table is not as its model says, or its reader finds something wrong
 * with what it reads: every difference of every layer, as lines of `what()` and one by one in `differences()`. A check that
 * could not be run at all fails with its own error.
 */
class TablesDiffer : public std::runtime_error {
 public:
  /** @param differences Each the table's name, or what a reader reads, then a colon and what differs; in the layers' order. */
  explicit TablesDiffer(std::vector<std::string> differences) : std::runtime_error(message_of(differences)), _differences(std::move(differences)) {}

  /** @return The differences given, in the layers' order. */
  [[nodiscard]] const std::vector<std::string>& differences() const noexcept { return _differences; }

 private:
  std::vector<std::string> _differences;

  [[nodiscard]] static std::string message_of(const std::vector<std::string>& differences) {
    std::string message = "the miniverse's tables are not as its layers say:";
    for ( const std::string& difference : differences ) {
      message += "\n  " + difference;
    }

    return message;
  }

 public:
  TablesDiffer(const TablesDiffer&) = default;
  TablesDiffer(TablesDiffer&&) noexcept = default;
  TablesDiffer& operator=(const TablesDiffer&) = default;
  TablesDiffer& operator=(TablesDiffer&&) noexcept = default;
  ~TablesDiffer() override = default;
};

namespace detail {

/** @brief The checks of a `verify` while they run, a layer each on any thread: what each found, and the future they settle together. */
class Checks {
 public:
  /** @param tables Where the layers are (`Layer::source`), in the layers' order: what a difference is said of. */
  Checks(std::vector<std::string> tables, schemacht::util::Settler<void> done)
      : _tables(std::move(tables)), _problems(_tables.size()), _pending(_tables.size()), _done(std::move(done)) {}

  /**
   * @brief Notes what the check of the layer numbered `layer` found (`problems`, each in words), or the `error` that kept it
   * from running, and settles the future once every layer's is in.
   */
  void finish(std::size_t layer, std::vector<std::string> problems, const std::exception_ptr& error) {
    {
      const std::scoped_lock lock(_mutex);
      _problems.at(layer) = std::move(problems);
      if ( error && ! _error ) {
        _error = error;
      }

      if ( --_pending > 0 ) {
        return;
      }
    }

    settle();  // outside the lock: this was the last check, and nothing else touches the checks now
  }

 private:
  std::mutex                            _mutex;
  std::vector<std::string>              _tables;
  std::vector<std::vector<std::string>> _problems;  // of each layer, in the layers' order
  std::exception_ptr                    _error;     // the first check that could not be run
  std::size_t                           _pending;
  schemacht::util::Settler<void>        _done;

  void settle() {
    if ( _error ) {
      _done.fail(_error);
      return;
    }

    try {
      std::vector<std::string> differences;
      for ( std::size_t layer = 0; layer < _tables.size(); ++layer ) {
        for ( const std::string& problem : _problems.at(layer) ) {
          differences.push_back(_tables.at(layer) + ": " + problem);
        }
      }

      if ( differences.empty() ) {
        _done(nullptr);
      } else {
        _done.fail(std::make_exception_ptr(TablesDiffer(std::move(differences))));
      }
    } catch ( ... ) {
      _done.fail(std::current_exception());  // when the message itself can't be made
    }
  }

 public:
  Checks(const Checks&) = delete;
  Checks(Checks&&) = delete;
  Checks& operator=(const Checks&) = delete;
  Checks& operator=(Checks&&) = delete;
  ~Checks() = default;
};

/**
 * @brief Reads the settings of `table`, whose columns are the model's, and tells `checks` why if it can't; if it can,
 * `unregistered` (what the registry had to say, if anything) is the one problem.
 */
template <WritesSettings model_t>
void check_settings(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename model_t::schema_type>& table,
    const std::shared_ptr<Checks>& checks, std::size_t layer_number, std::optional<std::string> unregistered
) {
  try {
    database.execute(
        settings_statement<model_t>(table.name()),
        [checks, layer_number, unregistered = std::move(unregistered)](SettingsRows<model_t> rows, const std::exception_ptr& error) {
          if ( error ) {
            checks->finish(layer_number, {}, error);
            return;
          }

          // Exactly one `finish` per layer: a throw out of this callback would leave the checks waiting for it.
          std::vector<std::string> problems;
          std::exception_ptr       failed;
          try {
            std::ignore = model_t::settings_from_rows(std::move(rows));
            if ( unregistered ) {
              problems.push_back(*unregistered);
            }
          } catch ( const std::exception& unreadable ) {
            problems.emplace_back(unreadable.what());
          } catch ( ... ) {
            failed = std::current_exception();
          }

          checks->finish(layer_number, std::move(problems), failed);
        }
    );
  } catch ( ... ) {
    checks->finish(layer_number, {}, std::current_exception());
  }
}

/**
 * @brief Checks `table`'s columns against its model's schema, then its settings if the model has any, and tells `checks`. A
 * table that is as its model says in both has `unregistered` (what the registry had to say, if anything) as its one problem:
 * a table that is not there, or not the model's, is told that instead.
 */
template <LayerModel model_t>
void check_columns(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename model_t::schema_type>& table,
    const std::shared_ptr<Checks>& checks, std::size_t layer_number, std::optional<std::string> unregistered
) {
  try {
    database.execute(
        schemacht::postgres::table_differences_statement<typename model_t::schema_type>().on(table),
        [&, checks, layer_number, unregistered = std::move(unregistered)](
            const std::vector<schemacht::postgres::TableDifference>& differences, const std::exception_ptr& error
        ) {
          if ( error ) {
            checks->finish(layer_number, {}, error);
            return;
          }

          // Only a table whose columns match is asked for its settings: for one that differs, the differences say enough, and
          // the question may not even be answerable (no raster type, so no `raster_columns`).
          if constexpr ( WritesSettings<model_t> ) {
            if ( differences.empty() ) {
              check_settings<model_t>(database, table, checks, layer_number, unregistered);
              return;
            }
          }

          std::vector<std::string> problems;
          std::exception_ptr       failed;
          try {
            problems.reserve(differences.size());
            for ( const schemacht::postgres::TableDifference& difference : differences ) {
              problems.push_back(schemacht::postgres::describe(difference));
            }
            if ( differences.empty() && unregistered ) {
              problems.push_back(*unregistered);
            }
          } catch ( ... ) {
            failed = std::current_exception();  // when the message itself can't be made
          }

          checks->finish(layer_number, std::move(problems), failed);
        }
    );
  } catch ( ... ) {
    checks->finish(layer_number, {}, std::current_exception());
  }
}

/** @return What the registry's answer `rows` (or the `error` it gave instead) says about a layer at `layout`; nothing if it is as recorded. */
[[nodiscard]] inline std::optional<std::string> registry_problem(
    const registry::LayerLayoutRows& rows, const std::exception_ptr& error, std::int32_t layout
) {
  std::optional<std::int32_t> found;
  if ( error ) {
    try {
      std::rethrow_exception(error);
    } catch ( const schemacht::postgres::QueryError& failed ) {
      if ( failed.sqlstate() != "42P01" ) {  // undefined_table: a database with no registry holds no layer of this layout
        throw;
      }
    }
  } else if ( ! rows.empty() ) {
    found = schemacht::schema::get<"layout">(rows.front());
  }

  return registry::layout_problem(found, layout);
}

/**
 * @brief Checks `table` against the registry's record of it, then against its model's schema and settings, and tells `checks`:
 * a table of another layout is told that alone; one the registry does not know is told what its columns say, or, if they say
 * nothing, that it is not in the registry. `database` and `table` are used after it returns: both must outlive the check, as
 * the miniverse's do.
 */
template <LayerModel model_t>
void check_table(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename model_t::schema_type>& table,
    const std::shared_ptr<Checks>& checks, std::size_t layer_number
) {
  try {
    database.execute(
        registry::LayerLayout::bind(registry::name_of(table.name())),
        [&, checks, layer_number](const registry::LayerLayoutRows& rows, const std::exception_ptr& error) {
          // Exactly one `finish` per layer: a throw out of this callback would leave the checks waiting for it.
          std::optional<std::string> problem;
          try {
            problem = registry_problem(rows, error, model_t::LAYOUT);
          } catch ( ... ) {
            checks->finish(layer_number, {}, std::current_exception());
            return;
          }

          if ( problem && ! rows.empty() ) {  // recorded, with another layout
            checks->finish(layer_number, {std::move(*problem)}, nullptr);
            return;
          }

          check_columns<model_t>(database, table, checks, layer_number, std::move(problem));
        }
    );
  } catch ( ... ) {
    checks->finish(layer_number, {}, std::current_exception());
  }
}

/** @brief Asks `reader` for its problems, on the calling thread, and tells `checks`. */
template <LayerModel model_t>
void check_reader(const std::shared_ptr<const Reader<model_t>>& reader, const std::shared_ptr<Checks>& checks, std::size_t layer_number) {
  std::vector<std::string> problems;
  std::exception_ptr       failed;
  try {
    problems = reader->problems();
  } catch ( ... ) {
    failed = std::current_exception();
  }

  checks->finish(layer_number, std::move(problems), failed);
}

}  // namespace detail

}  // namespace miniverse
