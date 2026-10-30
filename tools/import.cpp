#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "commands.hpp"
#include "layer_of.hpp"
#include "miniverse/dataset.hpp"
#include "miniverse/geo/concepts/raster.hpp"
#include "miniverse/geo/concepts/wkb.hpp"
#include "miniverse/model/feature_model.hpp"
#include "miniverse/model/raster_model.hpp"
#include "miniverse/model/road_model.hpp"
#include "miniverse/world.hpp"
#include "models.hpp"

#if MINIVERSE_TOOL_GDAL
#include "miniverse/gdal/raster.hpp"
#include "miniverse/gdal/vector.hpp"
#include "miniverse/geo/grid.hpp"
#include "miniverse/geo/raster.hpp"
#include "miniverse/model/feature.hpp"
#endif

#if MINIVERSE_TOOL_OSM
#include "miniverse/model/way.hpp"
#include "miniverse/osm/ways.hpp"
#endif

namespace tool {

namespace {

constexpr std::size_t PART_FEATURES = 10'000;
constexpr std::size_t WINDOW_TILES = 2;

/** @brief Throws if `option`, called `name`, is given: it is not for a table of `model`. */
template <typename value_t>
void refuse(const std::optional<value_t>& option, const std::string& name, const std::string& model) {
  if ( option ) {
    throw std::invalid_argument("--" + name + " is not for a table of " + model);
  }
}

/** @return `size`, or `otherwise` if it is unset. @throws std::invalid_argument if it is 0. */
[[nodiscard]] std::size_t size_of(const std::optional<std::size_t>& size, std::size_t otherwise, const std::string& name) {
  if ( size == 0 ) {
    throw std::invalid_argument("--" + name + " is at least 1");
  }

  return size.value_or(otherwise);
}

/** @brief The file of an import of features or roads: where it is, the dataset it replaces, the size of its parts. */
struct OneFile {
  std::string        path;
  miniverse::Dataset dataset;
  std::size_t        chunk = 0;
};

/**
 * @return The one file of `options`, for a table of `model`. Its dataset is `options.dataset`, or else the file's name (a
 * directory's, for a path that ends in a separator).
 * @throws std::invalid_argument for another number of files, or a dataset of no name.
 */
[[nodiscard]] OneFile one_file_of(const ImportOptions& options, const std::string& model) {
  if ( options.files.size() != 1 ) {
    throw std::invalid_argument("a table of " + model + " takes one file at a time, not " + std::to_string(options.files.size()));
  }

  const std::string&    path = options.files.front();
  std::filesystem::path named = std::filesystem::path(path).lexically_normal();
  if ( ! named.has_filename() ) {
    named = named.parent_path();
  }
  const std::string dataset = options.dataset.value_or(named.filename().string());
  if ( dataset.empty() || dataset == "." || dataset == ".." ) {
    throw std::invalid_argument("the dataset '" + dataset + "' has no name: name it with --dataset");
  }

  return {.path = path, .dataset = {dataset}, .chunk = size_of(options.chunk, PART_FEATURES, "chunk")};
}

template <geo::wkb::Geometry geometry_t>
void import_into(std::type_identity<miniverse::FeatureModel<geometry_t>> /*model*/, const Table& table, const ImportOptions& options) {
  using Model = miniverse::FeatureModel<geometry_t>;

  refuse(options.keys, "keys", table.model);
  refuse(options.band, "band", table.model);
  refuse(options.resampling, "resampling", table.model);
  refuse(options.window_tiles, "window-tiles", table.model);

#if MINIVERSE_TOOL_GDAL
  const OneFile file = one_file_of(options, table.model);

  miniverse::Miniverse world(table.conninfo, layer_of<Model>(table.name));
  world.verify().get();  // a table of another model is refused before anything is read

  miniverse::PushInParts<Model>     parts = world.template begin_push<"table">(file.dataset).get();
  const miniverse::gdal::VectorRead read = miniverse::gdal::read_features<geometry_t>(
      {.path = file.path, .layer = options.layer.value_or(""), .id_field = {}}, file.chunk,
      [&parts](miniverse::Features<geometry_t> chunk) { parts.add(std::move(chunk)).get(); }
  );
  parts.commit().get();

  std::cout << table.name << ": " << read.features << " features of " << file.path << " as the dataset '" << file.dataset.name << "', "
            << read.without_geometry << " without a geometry left out\n";
#else
  throw std::invalid_argument("this tool was built without miniverse::gdal, so it reads no vector files");
#endif
}

void import_into(std::type_identity<miniverse::RoadModel> /*model*/, const Table& table, const ImportOptions& options) {
  refuse(options.layer, "layer", table.model);
  refuse(options.band, "band", table.model);
  refuse(options.resampling, "resampling", table.model);
  refuse(options.window_tiles, "window-tiles", table.model);

#if MINIVERSE_TOOL_OSM
  const OneFile file = one_file_of(options, table.model);

  miniverse::Miniverse world(table.conninfo, layer_of<miniverse::RoadModel>(table.name));
  world.verify().get();

  miniverse::PushInParts<miniverse::RoadModel> parts = world.begin_push<"table">(file.dataset).get();
  const miniverse::osm::WayRead                read = miniverse::osm::read_ways(
      {.path = file.path, .keys = options.keys.value_or(std::vector<std::string>{"highway"})}, file.chunk,
      [&parts](miniverse::Ways chunk) { parts.add(std::move(chunk)).get(); }
  );
  parts.commit().get();

  std::cout << table.name << ": " << read.ways << " ways of " << file.path << " as the dataset '" << file.dataset.name << "', " << read.without_nodes
            << " left out (a node the file lacks, or fewer than two)\n";
#else
  throw std::invalid_argument("this tool was built without miniverse::osm, so it reads no OpenStreetMap files");
#endif
}

#if MINIVERSE_TOOL_GDAL
/** @return The resampling called `name`, one of `RESAMPLINGS`. @throws std::invalid_argument for a name of none. */
[[nodiscard]] miniverse::gdal::Resampling resampling_of(const std::string& name) {
  using miniverse::gdal::Resampling;

  // What each name of RESAMPLINGS means, in its order.
  constexpr std::array<Resampling, RESAMPLINGS.size()> MEANINGS{Resampling::Nearest, Resampling::Bilinear, Resampling::Cubic, Resampling::Average};

  const auto found = std::ranges::find(RESAMPLINGS, name);
  if ( found == RESAMPLINGS.end() ) {
    throw std::invalid_argument("there is no resampling '" + name + "': nearest, bilinear, cubic or average");
  }

  return MEANINGS.at(static_cast<std::size_t>(found - RESAMPLINGS.begin()));
}
#endif

template <geo::Pixel pixel_t>
void import_into(std::type_identity<miniverse::RasterModel<pixel_t>> /*model*/, const Table& table, const ImportOptions& options) {
  using Model = miniverse::RasterModel<pixel_t>;

  refuse(options.dataset, "dataset", table.model + " (a raster table has no datasets: a push merges into it)");
  refuse(options.layer, "layer", table.model);
  refuse(options.keys, "keys", table.model);
  refuse(options.chunk, "chunk", table.model + " (its parts are windows: --window-tiles)");

#if MINIVERSE_TOOL_GDAL
  if ( options.files.empty() ) {
    throw std::invalid_argument("no files to read");
  }
  const std::size_t                          window_tiles = size_of(options.window_tiles, WINDOW_TILES, "window-tiles");
  std::optional<miniverse::gdal::Resampling> resampling;
  if ( options.resampling ) {
    resampling = resampling_of(*options.resampling);
  }

  miniverse::Miniverse world(table.conninfo, layer_of<Model>(table.name));
  world.verify().get();
  const geo::Grid<pixel_t> grid = world.template table_settings<"table">().get();

  // Each file a push of its own, all of it or none; where files overlap, the later one's data is what the table has.
  for ( const std::string& path : options.files ) {
    miniverse::gdal::RasterSource source{.path = path};
    source.band = options.band.value_or(source.band);
    source.resampling = resampling.value_or(source.resampling);

    try {
      miniverse::PushInParts<Model>     parts = world.template begin_push<"table">().get();
      const miniverse::gdal::RasterRead read = miniverse::gdal::read_raster<pixel_t>(
          source, grid, window_tiles, [&parts](geo::Raster<pixel_t> window) { parts.add(std::move(window)).get(); }
      );
      parts.commit().get();

      std::cout << table.name << ": " << read.windows << " windows of " << path << ", " << read.without_data << " with no data left out\n";
    } catch ( const std::exception& error ) {
      throw std::runtime_error(path + ", so neither it nor the files after it are written: " + error.what());
    }
  }
#else
  throw std::invalid_argument("this tool was built without miniverse::gdal, so it reads no raster files");
#endif
}

}  // namespace

void import(const Table& table, const ImportOptions& options) {
  with_model(table.model, [&]<typename model_t>(std::type_identity<model_t> model) { import_into(model, table, options); });
}

}  // namespace tool
