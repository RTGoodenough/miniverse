#include "miniverse/layer/feature_layer.hpp"

#include <boost/geometry/algorithms/num_points.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/geo/concepts/wkb.hpp"
#include "miniverse/geo/types.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/table_name.hpp"

namespace miniverse {

template <geo::wkb::Geometry geometry_t>
std::vector<std::string> FeatureLayer<geometry_t>::setup_sql(const schemacht::schema::TableName& table) {
  return {"CREATE INDEX ON " + table.quoted() + " USING gist (geom)"};
}

template <geo::wkb::Geometry geometry_t>
std::vector<std::vector<typename FeatureLayer<geometry_t>::row_type>> FeatureLayer<geometry_t>::to_rows(result_type features) {
  std::vector<std::vector<row_type>> batches;
  std::size_t                        points = 0;  // in the last batch

  for ( Feature<geometry_t>& each : features ) {
    const std::size_t more = boost::geometry::num_points(each.geometry);
    if ( batches.empty() || batches.back().size() == FEATURES_PER_STATEMENT || points + more > POINTS_PER_STATEMENT ) {
      batches.emplace_back();  // never left empty: this feature goes in it, however many points it has
      points = 0;
    }

    points += more;
    batches.back().emplace_back(feature::Id{each.id}, feature::Geom<geometry_t>{std::move(each.geometry)}, feature::Tags{std::move(each.tags)});
  }

  return batches;
}

template <geo::wkb::Geometry geometry_t>
Features<geometry_t> FeatureLayer<geometry_t>::from_rows(std::vector<row_type> rows, const geo::Polygon& /*location*/) {
  Features<geometry_t> features;

  features.reserve(rows.size());
  for ( row_type& row : rows ) {
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

template struct FeatureLayer<geo::Point>;
template struct FeatureLayer<geo::LineString>;
template struct FeatureLayer<geo::Polygon>;
template struct FeatureLayer<geo::MultiLineString>;
template struct FeatureLayer<geo::MultiPolygon>;

}  // namespace miniverse
