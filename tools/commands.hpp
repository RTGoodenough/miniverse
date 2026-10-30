#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @file
 * What each of the tool's commands takes and does, on one table of a database. Each writes what it did to standard output,
 * and throws what went wrong: `std::invalid_argument` for arguments it can't take, before it connects. An option for
 * a table of another model than the one named is one of those.
 */
namespace tool {

/** @brief The table a command acts on: where it is, and the model of the layer it holds (a name of `model_names`). */
struct Table {
  std::string conninfo;  ///< A libpq connection string; empty for libpq's defaults and PG* variables.
  std::string model;
  std::string name;  ///< `table`, in the connection's search path, or `schema.table`.
};

/** @brief The grid of a raster table: pixels per degree, or metres a pixel made into them. */
struct GridOptions {
  std::optional<std::int32_t> pixels_per_degree;
  std::optional<double>       metres;
  std::optional<std::int32_t> tile_pixels;
  std::optional<std::string>  nodata;  ///< As written, read as the model's pixel type.
};

/** @brief Makes `table`, with `grid` for a raster table, which needs all of it but one of `pixels_per_degree` and `metres`. */
void create(const Table& table, const GridOptions& grid);

/** @brief How files are read into a table. Unset, an option has the default named. */
/** @brief The resamplings a raster import takes, by name: the command line's choices, in `miniverse::gdal::Resampling`'s order. */
inline constexpr std::array<std::string_view, 4> RESAMPLINGS{"nearest", "bilinear", "cubic", "average"};

struct ImportOptions {
  std::vector<std::string>                files;         ///< One for features and roads; for a raster, any number, each pushed in turn.
  std::optional<std::string>              dataset;       ///< Of features and roads: the dataset the file replaces; the file's name.
  std::optional<std::string>              layer;         ///< Of a vector file with several layers.
  std::optional<std::vector<std::string>> keys;          ///< Of an OpenStreetMap file: the ways with a tag of one of them; highway.
  std::optional<std::size_t>              chunk;         ///< Of features and roads: how many a part; 10000.
  std::optional<int>                      band;          ///< Of a raster file: 1.
  std::optional<std::string>              resampling;    ///< Of a raster file: nearest, bilinear, cubic or average; bilinear.
  std::optional<std::size_t>              window_tiles;  ///< Of a raster file: a part's side, in tiles; 2.
};

/** @brief Checks `table` is as its model says, then pushes `options.files` into it, a part at a time. */
void import(const Table& table, const ImportOptions& options);

/** @brief Removes the dataset `dataset` from `table`, of features or roads. @throws std::runtime_error if it has no such dataset. */
void remove(const Table& table, const std::string& dataset);

/** @brief Checks `table` is as its model says. @throws miniverse::TablesDiffer if it is not. */
void verify(const Table& table);

/** @brief Writes the grid that `table`, of a raster, was made with. */
void settings(const Table& table);

/** @brief Checks `table`, of features or roads, then writes its datasets: name, rows, and when each was last pushed. */
void datasets(const Table& table);

}  // namespace tool
