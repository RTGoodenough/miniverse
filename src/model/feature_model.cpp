#include "miniverse/model/feature_model.hpp"

#include <boost/geometry/algorithms/num_points.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/dataset.hpp"
#include "miniverse/geo/concepts/wkb.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/model/writes.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/table_name.hpp"

namespace miniverse {

template <geo::wkb::Geometry geometry_t>
std::vector<std::string> FeatureModel<geometry_t>::setup_sql(const schemacht::schema::TableName& table) {
  return {"CREATE INDEX ON " + table.quoted() + " USING gist (geom)"};
}

template <geo::wkb::Geometry geometry_t>
std::vector<std::vector<typename FeatureModel<geometry_t>::insert_row_type>> FeatureModel<geometry_t>::to_rows(
    result_type features, const Dataset& dataset, std::int64_t stamp
) {
  std::vector<std::vector<insert_row_type>> batches;
  std::size_t                               points = 0;  // in the last batch

  for ( Feature<geometry_t>& each : features ) {
    const std::size_t more = boost::geometry::num_points(each.geometry);
    if ( batches.empty() || batches.back().size() == FEATURES_PER_STATEMENT || points + more > POINTS_PER_STATEMENT ) {
      batches.emplace_back();  // the new batch takes this feature whatever its size, so it is never empty
      points = 0;
    }

    points += more;
    batches.back().emplace_back(
        dataset::Name{dataset.name}, feature::Geom<geometry_t>{std::move(each.geometry)}, feature::Tags{std::move(each.tags)}, dataset::Pushed{stamp}
    );
  }

  return batches;
}

template <geo::wkb::Geometry geometry_t>
Features<geometry_t>
FeatureModel<geometry_t>::from_rows(std::vector<typename load_statement_type::result_type> rows, const geo::Polygon& /*location*/) {
  Features<geometry_t> features;

  features.reserve(rows.size());
  for ( auto& row : rows ) {
    features.push_back(
        Feature<geometry_t>{
            .id = schemacht::schema::get<"feature_id">(row),
            .geometry = std::move(schemacht::schema::get<"geom">(row)),
            .tags = std::move(schemacht::schema::get<"tags">(row)),
        }
    );
  }

  return features;
}

template struct FeatureModel<geo::Point>;
template struct FeatureModel<geo::LineString>;
template struct FeatureModel<geo::Polygon>;
template struct FeatureModel<geo::MultiLineString>;
template struct FeatureModel<geo::MultiPolygon>;

}  // namespace miniverse
