#pragma once

#include <concepts>
#include <cstddef>
#include <future>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/location.hpp"
#include "miniverse/reader.hpp"
#include "miniverse/world/async.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/schema/table.hpp"
#include "schemacht/util/futures.hpp"

/**
 * @file
 * Loads and streams of a layer: from its table, by the kind's load statement on the pool, or from its reader, on a thread
 * started for the load. What `Miniverse::load` and `stream` do once they know which the layer is.
 */
namespace miniverse::detail {

/** @return What a streamed load of `kind_t` hands over for `rows`, one chunk's: the kind's own chunk, or its result of them. */
template <LayerKind kind_t>
[[nodiscard]] ChunkOf<kind_t> chunk_of(LoadedRows<kind_t> rows, const geo::Polygon& location) {
  if constexpr ( HasOwnChunks<kind_t> ) {
    return kind_t::chunk_from_rows(std::move(rows), location);
  } else {
    return kind_t::from_rows(std::move(rows), location);
  }
}

/** @return A future for what `table` holds in `location`, by the kind's load statement on `database`. See `Miniverse::load`. */
template <LayerKind kind_t>
[[nodiscard]] std::future<typename kind_t::result_type> load(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table, const geo::Polygon& location
) {
  return execute_as<typename kind_t::result_type>(
      database, [&] { return kind_t::load_statement_type::bind(location).on(table); },
      [location](auto rows) { return kind_t::from_rows(std::move(rows), location); }
  );
}

/** @return A future for what `reader` holds in `location`, read on a thread started for it. See `Miniverse::load`. */
template <LayerKind kind_t>
[[nodiscard]] std::future<typename kind_t::result_type> load(const std::shared_ptr<const Reader<kind_t>>& reader, const geo::Polygon& location) {
  return on_a_thread([reader, location] {
    check_location(location);

    return reader->load(location);
  });
}

/**
 * @return A future that completes once what `table` holds in `location` has been handed to `on_chunk`, `read.chunk_rows` rows at
 * a time, on threads of the pool's, or `on_chunk` stopped it. See `Miniverse::stream`.
 */
template <LayerKind kind_t, ChunkCallback<kind_t> callback_t>
[[nodiscard]] std::future<void> stream(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename kind_t::schema_type>& table, const geo::Polygon& location,
    callback_t on_chunk, const schemacht::postgres::ReadOptions& read
) {
  return schemacht::util::future_from<void>([&](const schemacht::util::Settler<void>& done) {
    guarded(done, [&] {
      database.stream_chunks(
          kind_t::load_statement_type::bind(location).on(table),
          [on_chunk = std::move(on_chunk), location](LoadedRows<kind_t>&& rows) mutable {
            return on_chunk(chunk_of<kind_t>(std::move(rows), location));  // nothing, or whether it wants more
          },
          read, done
      );
    });
  });
}

/**
 * @return A future that completes once what `reader` holds in `location` has been handed to `on_chunk`, `chunk_rows` at a time,
 * on a thread started for it, or `on_chunk` stopped it. See `Miniverse::stream`.
 */
template <LayerKind kind_t, ChunkCallback<kind_t> callback_t>
[[nodiscard]] std::future<void> stream(
    const std::shared_ptr<const Reader<kind_t>>& reader, const geo::Polygon& location, callback_t on_chunk, std::size_t chunk_rows
) {
  return on_a_thread([reader, location, on_chunk = std::move(on_chunk), chunk_rows]() mutable {
    if ( chunk_rows == 0 ) {
      throw std::invalid_argument("a stream needs chunks of at least one row");  // as a table's refuses it
    }

    check_location(location);

    reader->stream(location, chunk_rows, [&on_chunk](ChunkOf<kind_t> chunk) {
      if constexpr ( std::same_as<std::invoke_result_t<callback_t&, ChunkOf<kind_t>>, bool> ) {
        return on_chunk(std::move(chunk));
      } else {
        on_chunk(std::move(chunk));

        return true;
      }
    });
  });
}

}  // namespace miniverse::detail
