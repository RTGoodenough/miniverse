#include "miniverse/layer/road_layer.hpp"

#include <utility>
#include <vector>

#include "schemacht/schema/field.hpp"

namespace miniverse::road {

std::vector<Row> to_rows(const Ways& ways) {
  std::vector<Row> rows;
  rows.reserve(ways.size());
  for ( const Way& way : ways ) {
    rows.emplace_back(WayId{way.id}, NodeIds{way.node_ids}, Geometry{way.coordinates}, Tags{way.tags});
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

}  // namespace miniverse::road
