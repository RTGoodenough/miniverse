// The multi-geometry column types against a real PostGIS database: a typed column takes what C++ writes, C++ reads what
// PostGIS wrote, and a polygon finds them by ST_Intersects.
//
// Skipped unless MINIVERSE_TEST_DB holds a libpq connection string to a database with PostGIS enabled. The tests make and drop
// their own tables (miniverse_test_*), so point it at a scratch database.

#include <catch2/catch_test_macros.hpp>

#include <boost/geometry/io/wkt/read.hpp>
#include <boost/geometry/io/wkt/write.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <tuple>
#include <vector>

#include "miniverse/geo/column_types.hpp"  // IWYU pragma: keep -- what makes the geometry types column types
#include "miniverse/geo/operations.hpp"
#include "miniverse/geo/types.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/prepared.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/query/raw_statement.hpp"
#include "schemacht/schema/field.hpp"
#include "schemacht/schema/schema.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;
namespace q = schemacht::query;
namespace sch = schemacht::schema;

namespace {

using Id = sch::Field<std::int64_t, "id", sch::KeyRole::Primary>;
using Areas = sch::Schema<"miniverse_test_areas", Id, sch::Field<geo::MultiPolygon, "geom">>;
using Rivers = sch::Schema<"miniverse_test_rivers", Id, sch::Field<geo::MultiLineString, "geom">>;

constexpr auto AREAS_IN = q::select(q::On<Areas>::col<"geom">().apply<geo::Intersects>(q::arg<0>())).order_by(q::On<Areas>::col<"id">().asc());
constexpr auto RIVERS_IN = q::select(q::On<Rivers>::col<"geom">().apply<geo::Intersects>(q::arg<0>())).order_by(q::On<Rivers>::col<"id">().asc());

// A row made by PostGIS itself, from text: argument 2 is EWKT.
using InsertArea = q::RawStatement<
    "INSERT INTO miniverse_test_areas (id, geom) VALUES ($1, ST_GeomFromEWKT($2)) RETURNING id", q::RawArguments<std::int64_t, std::string>,
    sch::Field<std::int64_t, "id">>;
using InsertRiver = q::RawStatement<
    "INSERT INTO miniverse_test_rivers (id, geom) VALUES ($1, ST_GeomFromEWKT($2)) RETURNING id", q::RawArguments<std::int64_t, std::string>,
    sch::Field<std::int64_t, "id">>;

[[nodiscard]] std::string test_db() {
  const char* conninfo = std::getenv("MINIVERSE_TEST_DB");
  if ( conninfo == nullptr ) {
    SKIP("MINIVERSE_TEST_DB is not set");
  }

  return conninfo;
}

// Near the origin, with a hole and a second part; and far from it.
const std::string NEAR_AREA = "MULTIPOLYGON(((0 0,4 0,4 4,0 4,0 0),(1 1,1 2,2 2,2 1,1 1)),((10 10,11 10,11 11,10 10)))";
const std::string FAR_AREA = "MULTIPOLYGON(((50 50,51 50,51 51,50 50)))";
const std::string NEAR_RIVER = "MULTILINESTRING((1 2,3.5 -4),(0 0,1 1,2 0))";
const std::string FAR_RIVER = "MULTILINESTRING((50 50,51 51))";

const geo::Polygon EVERYWHERE = bg::from_wkt<geo::Polygon>("POLYGON((-10 -10,60 -10,60 60,-10 60,-10 -10))");
const geo::Polygon NEAR_ORIGIN = bg::from_wkt<geo::Polygon>("POLYGON((-1 -5,5 -5,5 5,-1 5,-1 -5))");

}  // namespace

TEST_CASE("integration: a multipolygon column takes what C++ writes, and gives back what PostGIS wrote", "[integration]") {
  schemacht::postgres::Database database(test_db());
  std::ignore = database.drop_table<Areas>().get();
  std::ignore = database.create_table<Areas>().get();

  std::ignore = database.insert<Areas>({Areas::row_type{Id{1}, sch::Field<geo::MultiPolygon, "geom">{bg::from_wkt<geo::MultiPolygon>(NEAR_AREA)}}}).get();
  std::ignore = database.execute(InsertArea::bind(2, "SRID=4326;" + FAR_AREA)).get();
  const auto everywhere = database.execute(q::Prepared<AREAS_IN>::bind(EVERYWHERE)).get();
  const auto near_origin = database.execute(q::Prepared<AREAS_IN>::bind(NEAR_ORIGIN)).get();

  REQUIRE(everywhere.size() == 2);
  CHECK(bg::to_wkt(sch::get<"geom">(everywhere.at(0))) == NEAR_AREA);
  CHECK(bg::to_wkt(sch::get<"geom">(everywhere.at(1))) == FAR_AREA);
  REQUIRE(near_origin.size() == 1);
  CHECK(sch::get<"id">(near_origin.front()) == 1);

  std::ignore = database.drop_table<Areas>().get();
}

TEST_CASE("integration: a multilinestring column takes what C++ writes, and gives back what PostGIS wrote", "[integration]") {
  schemacht::postgres::Database database(test_db());
  std::ignore = database.drop_table<Rivers>().get();
  std::ignore = database.create_table<Rivers>().get();

  std::ignore =
      database.insert<Rivers>({Rivers::row_type{Id{1}, sch::Field<geo::MultiLineString, "geom">{bg::from_wkt<geo::MultiLineString>(NEAR_RIVER)}}}).get();
  std::ignore = database.execute(InsertRiver::bind(2, "SRID=4326;" + FAR_RIVER)).get();
  const auto everywhere = database.execute(q::Prepared<RIVERS_IN>::bind(EVERYWHERE)).get();
  const auto near_origin = database.execute(q::Prepared<RIVERS_IN>::bind(NEAR_ORIGIN)).get();

  REQUIRE(everywhere.size() == 2);
  CHECK(bg::to_wkt(sch::get<"geom">(everywhere.at(0))) == NEAR_RIVER);
  CHECK(bg::to_wkt(sch::get<"geom">(everywhere.at(1))) == FAR_RIVER);
  REQUIRE(near_origin.size() == 1);
  CHECK(sch::get<"id">(near_origin.front()) == 1);

  std::ignore = database.drop_table<Rivers>().get();
}
