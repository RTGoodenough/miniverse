#include "miniverse/model/road_model.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/dataset.hpp"
#include "miniverse/geo/types.hpp"
#include "miniverse/model/writes.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/table_name.hpp"

namespace miniverse {

std::vector<std::string> RoadModel::setup_sql(const schemacht::schema::TableName& table) {
  return {"CREATE INDEX ON " + table.quoted() + " USING gist (geom)"};
}

std::vector<std::vector<RoadModel::insert_row_type>> RoadModel::to_rows(Ways ways, const Dataset& dataset, std::int64_t stamp) {
  std::vector<std::vector<insert_row_type>> batches;

  for ( std::size_t i = 0; i < ways.size(); ++i ) {
    Way& way = ways.at(i);
    if ( way.node_ids.size() != way.coordinates.size() ) {
      throw std::invalid_argument(
          "way " + std::to_string(way.id) + " has " + std::to_string(way.node_ids.size()) + " node ids but " +
          std::to_string(way.coordinates.size()) + " points: one id per point"
      );
    }

    if ( i % WAYS_PER_STATEMENT == 0 ) {
      batches.emplace_back().reserve(std::min(WAYS_PER_STATEMENT, ways.size() - i));
    }

    batches.back().emplace_back(
        road::WayId{way.id}, dataset::Name{dataset.name}, road::NodeIds{std::move(way.node_ids)}, road::Geom{std::move(way.coordinates)},
        road::Tags{std::move(way.tags)}, dataset::Pushed{stamp}
    );
  }

  return batches;
}

Ways RoadModel::from_rows(std::vector<load_statement_type::result_type> rows, const geo::Polygon& /*location*/) {
  Ways ways;

  ways.reserve(rows.size());
  for ( auto& row : rows ) {
    ways.push_back(
        Way{
            .id = schemacht::schema::get<"way_id">(row),
            .node_ids = std::move(schemacht::schema::get<"node_ids">(row)),
            .coordinates = std::move(schemacht::schema::get<"geom">(row)),
            .tags = std::move(schemacht::schema::get<"tags">(row)),
        }
    );
  }

  return ways;
}

}  // namespace miniverse
