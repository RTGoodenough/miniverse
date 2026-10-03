#include "miniverse/layer/road_layer.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/layer.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/table_name.hpp"

namespace miniverse {

std::vector<std::string> RoadLayer::setup_sql(const schemacht::schema::TableName& table, NoSettings /*settings*/) {
  return {"CREATE INDEX ON " + table.quoted() + " USING gist (geom)"};
}

namespace road {

std::vector<Row> to_rows(Ways ways) {
  std::vector<Row> rows;

  rows.reserve(ways.size());
  for ( Way& way : ways ) {
    if ( way.node_ids.size() != way.coordinates.size() ) {
      throw std::invalid_argument(
          "way " + std::to_string(way.id) + " has " + std::to_string(way.node_ids.size()) + " node ids but " +
          std::to_string(way.coordinates.size()) + " points: one id per point"
      );
    }

    rows.emplace_back(WayId{way.id}, NodeIds{std::move(way.node_ids)}, Geom{std::move(way.coordinates)}, Tags{std::move(way.tags)});
  }

  return rows;
}

Ways from_rows(std::vector<Row> rows) {
  Ways ways;

  ways.reserve(rows.size());
  for ( Row& row : rows ) {
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

}  // namespace road

}  // namespace miniverse
