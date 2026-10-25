#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "miniverse/gdal/concepts/vector.hpp"  // IWYU pragma: export
#include "miniverse/gdal/vector.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/layer.hpp"
#include "miniverse/reader.hpp"

/**
 * @file
 * A vector file as a layer: read in place of a table, by `read_features_in` (vector.hpp), for a test or a user with files
 * alone.
 */
namespace miniverse::gdal {

namespace detail {

/**
 * @brief The reader of a vector file as a layer of `kind_t`: what `feature_file` makes a layer with. A load gives the file's
 * features that intersect the location, in the file's order (a table's are by id), their tags as `read_features` writes them
 * (a table's are as `jsonb` prints them: the same document, another text); each load opens the file for itself, so loads
 * run side by side.
 */
template <FeatureKind kind_t>
class FeatureFile : public Reader<kind_t> {
 public:
  explicit FeatureFile(VectorSource source) : _source(std::move(source)) {}

  [[nodiscard]] std::string name() const override { return "'" + _source.path + "'"; }

  [[nodiscard]] typename kind_t::result_type load(const geo::Polygon& location) const override {
    typename kind_t::result_type all;

    // One chunk, of however many there are.
    std::ignore = read_features_in<GeometryOf<kind_t>>(_source, location, std::numeric_limits<std::size_t>::max(), [&all](auto chunk) {
      all = std::move(chunk);

      return true;
    });

    return all;
  }

  void stream(const geo::Polygon& location, std::size_t chunk_rows, const Reader<kind_t>::OnChunk& on_chunk) const override {
    std::ignore = read_features_in<GeometryOf<kind_t>>(_source, location, chunk_rows, on_chunk);
  }

  [[nodiscard]] std::vector<std::string> problems() const override { return problems_of<GeometryOf<kind_t>>(_source); }

 private:
  VectorSource _source;

 public:
  FeatureFile(const FeatureFile&) = delete;
  FeatureFile(FeatureFile&&) = delete;
  FeatureFile& operator=(const FeatureFile&) = delete;
  FeatureFile& operator=(FeatureFile&&) = delete;
  ~FeatureFile() override = default;
};

}  // namespace detail

/**
 * @return The layer of `kind_t` that is `source`, a vector file, read in place of a table:
 *
 * @code
 * miniverse::Miniverse world(miniverse::gdal::feature_file<Buildings>({.path = "buildings.gpkg"}));
 * world.load<Buildings>(area).get();  // the file's buildings that intersect the area
 * @endcode
 *
 * The file is not opened until a load, or `Miniverse::verify`, which says what is wrong with it.
 */
template <FeatureKind kind_t>
[[nodiscard]] Layer<kind_t> feature_file(VectorSource source) {
  return Layer<kind_t>(std::make_shared<detail::FeatureFile<kind_t>>(std::move(source)));
}

}  // namespace miniverse::gdal
