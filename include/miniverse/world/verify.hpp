#pragma once

#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/reader.hpp"
#include "miniverse/world/settings.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/postgres/table_differences.hpp"
#include "schemacht/schema/table.hpp"
#include "schemacht/util/futures.hpp"

/**
 * @file
 * The checks of a `Miniverse::verify`: each layer's table against its kind's schema and settings, or its reader asked for
 * its problems, all at once, settled together as one `TablesDiffer` naming every difference.
 */
namespace miniverse {

/**
 * @brief What `Miniverse::verify` fails with when a layer's table is not as its kind says, or its reader finds something wrong
 * with what it reads: every difference of every layer, as lines of `what()` and one by one in `differences()`. A check that
 * could not be run at all fails with its own error.
 */
class TablesDiffer : public std::runtime_error {
 public:
  /** @param differences Each as `<table, or what a reader reads>: <what differs>`, in the layers' order. */
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
  /** @param tables The layers' names (`Layer::name`), in the layers' order: what a difference is said of. */
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

/** @brief Reads the settings of `table`, whose columns are the kind's, and tells `checks` why if it can't. */
template <HasSettings kind_t>
void check_settings(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table,
    const std::shared_ptr<Checks>& checks, std::size_t layer_number
) {
  try {
    database.execute(settings_statement<kind_t>(table.name()), [checks, layer_number](SettingsRows<kind_t> rows, const std::exception_ptr& error) {
      if ( error ) {
        checks->finish(layer_number, {}, error);
        return;
      }

      // Exactly one `finish` per layer: a throw out of this callback would leave the checks waiting for it.
      std::vector<std::string> problems;
      std::exception_ptr       failed;
      try {
        std::ignore = kind_t::settings_from_rows(std::move(rows));
      } catch ( const std::exception& unreadable ) {
        problems.emplace_back(unreadable.what());
      } catch ( ... ) {
        failed = std::current_exception();
      }

      checks->finish(layer_number, std::move(problems), failed);
    });
  } catch ( ... ) {
    checks->finish(layer_number, {}, std::current_exception());
  }
}

/**
 * @brief Checks `table` against its kind's schema, then its settings if the kind has any, and tells `checks`. `database` and
 * `table` are used after it returns: both must outlive the check, as the miniverse's do.
 */
template <LayerKind kind_t>
void check_table(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table,
    const std::shared_ptr<Checks>& checks, std::size_t layer_number
) {
  try {
    database.execute(
        schemacht::postgres::table_differences_statement<typename kind_t::schema_type>().on(table),
        [&database, &table, checks,
         layer_number](const std::vector<schemacht::postgres::TableDifference>& differences, const std::exception_ptr& error) {
          if ( error ) {
            checks->finish(layer_number, {}, error);
            return;
          }

          // Only a table whose columns match is asked for its settings: for one that differs, the differences say enough, and
          // the question may not even be answerable (no raster type, so no `raster_columns`).
          if constexpr ( HasSettings<kind_t> ) {
            if ( differences.empty() ) {
              check_settings<kind_t>(database, table, checks, layer_number);
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

/** @brief Asks `reader` for its problems, on the calling thread, and tells `checks`. */
template <LayerKind kind_t>
void check_reader(const std::shared_ptr<const Reader<kind_t>>& reader, const std::shared_ptr<Checks>& checks, std::size_t layer_number) {
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
