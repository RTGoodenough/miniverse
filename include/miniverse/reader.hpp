#pragma once

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/geo/types.hpp"

/**
 * A reader: where a layer that is not a table is read from. A `Layer<Kind>` is made from a table's name or from a reader, and
 * a miniverse loads and streams it the same way either way, so a worker's code does not know which it has: a table in
 * production, a file in a test (`miniverse::gdal` and `miniverse::osm` have readers of files), or a reader of your own that
 * hands out fixed data.
 *
 * @code
 * struct Roads : miniverse::RoadLayer {};
 *
 * class TwoRoads : public miniverse::Reader<Roads> { ... };  // `name`, `load` and `stream`; `settings` too, for a raster kind
 *
 * miniverse::Miniverse world(miniverse::Layer<Roads>(std::make_shared<TwoRoads>()));  // no database at all
 * miniverse::Ways roads = world.load<Roads>(area).get();
 * @endcode
 *
 * What a reader gives is meant to be what a table of the same data gives, and the readers of files are tested against one.
 * Two things are a reader's own all the same: the order (a file's, where a table's is by id), and the spelling of tags (a
 * file's JSON as its reader writes it, where a table's is as PostgreSQL's `jsonb` prints it: the same document, another text).
 *
 * A layer with a reader is only read: a push to it fails. Its reader is shared by the layer's copies and by the loads that
 * run, and loads run at the same time, each on a thread of its own: every member is `const`, and must be safe to call from
 * several threads at once. `load` and `stream` run on a thread started for the load; `name`, `settings` and `problems` on
 * the thread that asks the miniverse, before its call returns, so they should not take long.
 */
namespace miniverse {

/**
 * @brief Throws unless `location` is a polygon every table's load takes: each of its rings closed (its last point its first
 * again), and of four points or more. PostGIS can't test an unclosed ring against a line or an area, and fails the load when
 * it comes to one (against points it has a test of its own, which does not mind); a miniverse refuses such a location for
 * every layer with a reader, before the reader is asked, so that a worker tried on files does not fail on tables. A polygon
 * of no points at all is taken: nothing is in it.
 * @throws std::invalid_argument for such a ring.
 */
inline void check_location(const geo::Polygon& location) {
  const auto check = [](const auto& ring) {
    constexpr std::size_t FEWEST_POINTS = 4;
    if ( ring.empty() ) {
      return;
    }

    if ( ring.size() < FEWEST_POINTS || ring.front().x() != ring.back().x() || ring.front().y() != ring.back().y() ) {
      throw std::invalid_argument("a ring of a location must be closed, its last point its first again, and have four points or more");
    }
  };

  check(location.outer());
  for ( const auto& hole : location.inners() ) {
    check(hole);
  }
}

/** @brief The part of a `Reader` that only a kind with settings has; for a kind without, nothing. */
template <typename kind_t>
class ReaderSettings {
 protected:
  ReaderSettings() = default;

 public:
  ReaderSettings(const ReaderSettings&) = delete;
  ReaderSettings(ReaderSettings&&) = delete;
  ReaderSettings& operator=(const ReaderSettings&) = delete;
  ReaderSettings& operator=(ReaderSettings&&) = delete;
  virtual ~ReaderSettings() = default;
};

template <HasSettings kind_t>
class ReaderSettings<kind_t> {
 public:
  /** @return The settings the layer is read with (a raster's grid): what `Miniverse::table_settings` gives for the layer. */
  [[nodiscard]] virtual typename kind_t::settings_type settings() const = 0;

 protected:
  ReaderSettings() = default;

 public:
  ReaderSettings(const ReaderSettings&) = delete;
  ReaderSettings(ReaderSettings&&) = delete;
  ReaderSettings& operator=(const ReaderSettings&) = delete;
  ReaderSettings& operator=(ReaderSettings&&) = delete;
  virtual ~ReaderSettings() = default;
};

/** @brief Where a layer of `kind_t` that is not a table is read from: see the top of this file. */
template <LayerKind kind_t>
class Reader : public ReaderSettings<kind_t> {
 public:
  /** @brief What a streamed load hands each chunk to: it returns whether it wants more. */
  using OnChunk = std::function<bool(ChunkOf<kind_t>)>;

  /** @return What the reader reads, for a message: a file's path, say. It is what tells this layer from the others in one. */
  [[nodiscard]] virtual std::string name() const = 0;

  /**
   * @return What the layer holds in `location`, a polygon in WGS 84 longitude and latitude that `check_location` has passed:
   * what a load of a table of the kind would give for the same data. What it throws is the load's error.
   */
  [[nodiscard]] virtual typename kind_t::result_type load(const geo::Polygon& location) const = 0;

  /**
   * @brief Hands what the layer holds in `location` to `on_chunk`, `chunk_rows` at a time (the last of what is left), until it
   * returns `false`. For a kind with chunks of its own (a raster's tiles), a chunk is that many of them. What it throws, and
   * what `on_chunk` throws, is the load's error.
   *
   * `chunk_rows` is at least 1: the miniverse refuses a stream of none. `on_chunk` is called from the thread `stream` was
   * called on alone, one call at a time, not again once it has returned `false`, and never after `stream` returns.
   */
  virtual void stream(const geo::Polygon& location, std::size_t chunk_rows, const OnChunk& on_chunk) const = 0;

  /**
   * @return What is wrong with what the reader reads, each in words, for `Miniverse::verify`: a file that is missing, or does
   * not hold what the kind needs. Nothing, unless a reader says otherwise.
   */
  [[nodiscard]] virtual std::vector<std::string> problems() const { return {}; }

 protected:
  Reader() = default;

 public:
  Reader(const Reader&) = delete;
  Reader(Reader&&) = delete;
  Reader& operator=(const Reader&) = delete;
  Reader& operator=(Reader&&) = delete;
  ~Reader() override = default;
};

}  // namespace miniverse
