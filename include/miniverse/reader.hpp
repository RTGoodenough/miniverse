#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "miniverse/concepts/miniverse.hpp"
#include "miniverse/geo/types.hpp"

/**
 * @file
 * A reader: where a layer that is not a table is read from. A `Layer` is made from a table's name
 * or from a reader of its model, and a miniverse loads and streams it the same way either way: a
 * table in production, a file in a test
 * (`miniverse::gdal` and `miniverse::osm` have readers of files), or a reader of your own that
 * hands out fixed data.
 *
 * @code
 * class TwoRoads : public miniverse::Reader<miniverse::RoadModel> { ... };  // `name`, `load` and
 * `stream`; `settings` too, for a raster model
 *
 * using Roads = miniverse::Layer<miniverse::RoadModel, "roads">;
 *
 * miniverse::Miniverse world(Roads(std::make_shared<TwoRoads>()));  // no database at all
 * miniverse::Ways roads = world.load<"roads">(area).get();
 * @endcode
 *
 * A reader gives what a table of the same data gives, and the readers of files are tested against
 * one, but for three things: the ids (a file's own, where a table's are the database's), the order
 * (a file's, where a table's is by id), and the spelling of tags (as the reader writes the JSON,
 * where a table's is as `jsonb` prints it).
 *
 * A layer with a reader is only read. The reader is shared by the layer's copies and by the loads
 * that run, each on a thread of its own: every member is `const` and must be safe to call from
 * several threads at once. `load` and `stream` run on a thread started for the load; `name`,
 * `settings` and `problems` on the caller's thread, so they should not take long.
 */
namespace miniverse {

/** @brief The part of a `Reader` that only a model with settings has; for a model without, nothing.
 */
template <typename model_t>
class ReaderSettings
{
 protected:
  ReaderSettings() = default;

 public:
  ReaderSettings(const ReaderSettings&)            = delete;
  ReaderSettings(ReaderSettings&&)                 = delete;
  ReaderSettings& operator=(const ReaderSettings&) = delete;
  ReaderSettings& operator=(ReaderSettings&&)      = delete;
  virtual ~ReaderSettings()                        = default;
};

/** @brief The part of a `Reader` of a model with settings: the settings the layer is read with. */
template <WritesSettings model_t>
class ReaderSettings<model_t>
{
 public:
  /** @return The settings the layer is read with (a raster's grid): what
   * `Miniverse::table_settings` gives for the layer. */
  [[nodiscard]] virtual SettingsOf<model_t> settings() const = 0;

 protected:
  ReaderSettings() = default;

 public:
  ReaderSettings(const ReaderSettings&)            = delete;
  ReaderSettings(ReaderSettings&&)                 = delete;
  ReaderSettings& operator=(const ReaderSettings&) = delete;
  ReaderSettings& operator=(ReaderSettings&&)      = delete;
  virtual ~ReaderSettings()                        = default;
};

/** @brief Where a layer of `model_t` that is not a table is read from: see the top of this file. */
template <LayerModel model_t>
class Reader : public ReaderSettings<model_t>
{
 public:
  /** @brief What a streamed load hands each chunk to: it returns whether it wants more. */
  using OnChunk = std::function<bool(ChunkOf<model_t>)>;

  /** @return What the reader reads, for a message: a file's path, say. It is what tells this layer
   * from the others in a message. */
  [[nodiscard]] virtual std::string name() const = 0;

  /**
   * @return What the layer holds in `location`, a polygon in WGS 84 longitude and latitude that
   * `check_location` (check_location.hpp) has passed: what a load of a table of the model would
   * give for the same data. What it throws is the load's error.
   */
  [[nodiscard]] virtual typename model_t::result_type load(const geo::Polygon& location) const = 0;

  /**
   * @brief Hands what the layer holds in `location` to `on_chunk`, `chunk_rows` at a time (the last
   * of what is left), until it returns `false`. For a model with chunks of its own (a raster's
   * tiles), a chunk is that many of them. What it throws, and what `on_chunk` throws, is the load's
   * error.
   *
   * `chunk_rows` is at least 1: the miniverse refuses a stream of none. `on_chunk` is called from
   * the thread `stream` was called on alone, one call at a time, not again once it has returned
   * `false`, and never after `stream` returns.
   */
  virtual void stream(
      const geo::Polygon& location, std::size_t chunk_rows, const OnChunk& on_chunk) const = 0;

  /**
   * @return What is wrong with what the reader reads, each in words, for `Miniverse::verify`: a
   * file that is missing, or does not hold what the model needs. Nothing, unless a reader says
   * otherwise.
   */
  [[nodiscard]] virtual std::vector<std::string> problems() const { return {}; }

 protected:
  Reader() = default;

 public:
  Reader(const Reader&)            = delete;
  Reader(Reader&&)                 = delete;
  Reader& operator=(const Reader&) = delete;
  Reader& operator=(Reader&&)      = delete;
  ~Reader() override               = default;
};

}  // namespace miniverse
