#pragma once

#include <cstddef>
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
 * A vector file as a layer, read in place of a table by `read_features_in` (vector.hpp).
 */
namespace miniverse::gdal {

namespace detail {

/**
 * @brief The reader of a vector file as a layer of `model_t`, behind `feature_file`, which is the
 * only way to one: a program has no features of its own to give it, as it has ways for an
 * `osm::RoadFile`. A load gives the file's features that intersect the location, in the file's
 * order, with their tags as `read_features` writes them. Each load opens the file for itself, so
 * loads run side by side.
 */
template <ReadsFeatures model_t>
class FeatureFile : public Reader<model_t>
{
 public:
  explicit FeatureFile(VectorSource source) : _source(std::move(source)) {}

  [[nodiscard]] std::string name() const override { return "'" + _source.path + "'"; }

  [[nodiscard]] typename model_t::result_type load(const geo::Polygon& location) const override
  {
    typename model_t::result_type all;

    std::ignore =
        read_features_in<GeometryOf<model_t>>(_source, location, ONE_CHUNK, [&all](auto chunk) {
          all = std::move(chunk);

          return true;
        });

    return all;
  }

  void stream(const geo::Polygon&             location,
              std::size_t                     chunk_rows,
              const Reader<model_t>::OnChunk& on_chunk) const override
  {
    std::ignore = read_features_in<GeometryOf<model_t>>(_source, location, chunk_rows, on_chunk);
  }

  [[nodiscard]] std::vector<std::string> problems() const override
  {
    return problems_of<GeometryOf<model_t>>(_source);
  }

 private:
  VectorSource _source;

 public:
  FeatureFile(const FeatureFile&)            = delete;
  FeatureFile(FeatureFile&&)                 = delete;
  FeatureFile& operator=(const FeatureFile&) = delete;
  FeatureFile& operator=(FeatureFile&&)      = delete;
  ~FeatureFile() override                    = default;
};

}  // namespace detail

/**
 * @return The layer `layer_t`, of a model of features, that is `source`, a vector file, read in
 * place of a table:
 *
 * @code
 * using Buildings = miniverse::Layer<miniverse::FeatureModel<miniverse::geo::MultiPolygon>,
 * "buildings">;
 *
 * miniverse::Miniverse world(miniverse::gdal::feature_file<Buildings>({.path = "buildings.gpkg"}));
 * world.load<"buildings">(area).get();  // the file's buildings that intersect the area
 * @endcode
 *
 * The file is first opened by a load, or by `Miniverse::verify`, which says what is wrong with it.
 */
template <IsLayer layer_t>
  requires ReadsFeatures<typename layer_t::model_type>
[[nodiscard]] layer_t feature_file(VectorSource source)
{
  return layer_t(
      std::make_shared<detail::FeatureFile<typename layer_t::model_type>>(std::move(source)));
}

}  // namespace miniverse::gdal
