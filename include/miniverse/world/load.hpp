#pragma once

#include <concepts>
#include <cstddef>
#include <future>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "miniverse/check_location.hpp"
#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/reader.hpp"
#include "miniverse/world/async.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/schema/table.hpp"
#include "schemacht/util/futures.hpp"

/**
 * @file
 * Loads and streams of a layer: from its table, by the model's load statement, or from its reader; a load on a thread started
 * for it, a table's stream on the pool's workers. What `Miniverse::load` and `stream` do once they know which the layer is.
 */
namespace miniverse::detail {

/** @return What a streamed load of `model_t` hands over for `rows`, one chunk's: the model's own chunk, or its result of them. */
template <LayerModel model_t>
[[nodiscard]] ChunkOf<model_t> chunk_of(LoadedRows<model_t> rows, const geo::Polygon& location) {
  if constexpr ( HasOwnChunks<model_t> ) {
    return model_t::chunk_from_rows(std::move(rows), location);
  } else {
    return model_t::from_rows(std::move(rows), location);
  }
}

/**
 * @return A future for what `table` holds in `location`: the model's load statement, run on `database` and waited for on a
 * thread started for the load, where the model makes the result of the rows. See `Miniverse::load`.
 */
template <LayerModel model_t>
[[nodiscard]] std::future<typename model_t::result_type> load(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename model_t::schema_type>& table, const geo::Polygon& location
) {
  return on_a_thread([&database, &table, location] {
    return model_t::from_rows(database.execute(model_t::load_statement_type::bind(location).on(table)).get(), location);
  });
}

/** @return A future for what `reader` holds in `location`, read on a thread started for it. See `Miniverse::load`. */
template <LayerModel model_t>
[[nodiscard]] std::future<typename model_t::result_type> load(const std::shared_ptr<const Reader<model_t>>& reader, const geo::Polygon& location) {
  return on_a_thread([reader, location] {
    check_location(location);

    return reader->load(location);
  });
}

/**
 * @return A future that completes once what `table` holds in `location` has been handed to `on_chunk`, `read.chunk_rows` rows at
 * a time, on threads of the pool's, or `on_chunk` stopped it. See `Miniverse::stream`.
 */
template <LayerModel model_t, ChunkCallback<model_t> callback_t>
[[nodiscard]] std::future<void> stream(
    schemacht::postgres::Database& database, const schemacht::schema::Table<typename model_t::schema_type>& table, const geo::Polygon& location,
    callback_t on_chunk, const schemacht::postgres::ReadOptions& read
) {
  return schemacht::util::future_from<void>([&](const schemacht::util::Settler<void>& done) {
    guarded(done, [&] {
      database.stream_chunks(
          model_t::load_statement_type::bind(location).on(table),
          [on_chunk = std::move(on_chunk), location](LoadedRows<model_t>&& rows) mutable {
            return on_chunk(chunk_of<model_t>(std::move(rows), location));  // nothing, or whether it wants more
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
template <LayerModel model_t, ChunkCallback<model_t> callback_t>
[[nodiscard]] std::future<void> stream(
    const std::shared_ptr<const Reader<model_t>>& reader, const geo::Polygon& location, callback_t on_chunk, std::size_t chunk_rows
) {
  return on_a_thread([reader, location, on_chunk = std::move(on_chunk), chunk_rows]() mutable {
    if ( chunk_rows == 0 ) {
      throw std::invalid_argument("a stream needs chunks of at least one row");  // as a table's refuses it
    }

    check_location(location);

    reader->stream(location, chunk_rows, [&on_chunk](ChunkOf<model_t> chunk) {
      if constexpr ( std::same_as<std::invoke_result_t<callback_t&, ChunkOf<model_t>>, bool> ) {
        return on_chunk(std::move(chunk));
      } else {
        on_chunk(std::move(chunk));

        return true;
      }
    });
  });
}

}  // namespace miniverse::detail
