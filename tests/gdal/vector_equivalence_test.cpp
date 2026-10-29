// A vector file pushed into a feature layer loads back as it was read, and a file loads as the table it was pushed into:
// the file-versus-table comparisons. Each test needs MINIVERSE_TEST_DB (a libpq connection string to a scratch database with
// PostGIS), and is skipped without it.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <boost/geometry/io/wkt/read.hpp>
#include <boost/geometry/io/wkt/write.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "gdal/geojsons.hpp"
#include "miniverse/gdal/gdal.hpp"
#include "miniverse/geo/concepts/wkb.hpp"
#include "miniverse/miniverse.hpp"
#include "support/database.hpp"
#include "support/files.hpp"
#include "support/geometry.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;
namespace gdal = miniverse::gdal;

using Catch::Matchers::ContainsSubstring;
using test::AREAS;
using test::Buildings;
using test::BUILDINGS;
using test::BY_THE_PARALLEL;
using test::Files;
using test::Paths;
using test::polygon;
using test::Shops;
using test::SHOPS;
using test::test_db;
using test::to_geopackage;
using test::TRACKS;

namespace {

const miniverse::Dataset DATASET{"buildings"};

/** @return The geometries of `features` as WKT, sorted: for comparing two loads as sets. */
template <geo::wkb::Geometry geometry_t>
[[nodiscard]] std::vector<std::string> wkts(const miniverse::Features<geometry_t>& features) {
  std::vector<std::string> result;

  result.reserve(features.size());
  for ( const miniverse::Feature<geometry_t>& feature : features ) {
    result.push_back(bg::to_wkt(feature.geometry));
  }
  std::ranges::sort(result);

  return result;
}

/**
 * @brief Checks that the file at each of `sources` gives, for each of `locations`, the features a table gives that was
 * pushed the first of them: the same ones, with the same geometries, in whatever order.
 */
template <miniverse::IsLayer layer_t>
void check_loads_as_a_table(const std::string& table, const std::vector<gdal::VectorSource>& sources, const std::vector<geo::Polygon>& locations) {
  using Geometry = gdal::GeometryOf<typename layer_t::model_type>;

  miniverse::Miniverse<layer_t> tables(test_db(), layer_t(table));
  tables.drop_tables();
  tables.create_tables();
  tables.template push<layer_t::NAME>(DATASET, gdal::read_features<Geometry>(sources.front())).get();

  for ( const gdal::VectorSource& source : sources ) {
    miniverse::Miniverse<layer_t> files(gdal::feature_file<layer_t>(source));

    for ( std::size_t i = 0; i < locations.size(); ++i ) {
      INFO(source.path << ", location " << i << ": " << bg::to_wkt(locations.at(i)));
      // The same features, whatever order each side gives them in: the table's ids are its own, the file's are not kept, and a
      // filtered read of a file may come in its index's order.
      CHECK(wkts(files.template load<layer_t::NAME>(locations.at(i)).get()) == wkts(tables.template load<layer_t::NAME>(locations.at(i)).get()));
    }
  }

  tables.drop_tables();
}

}  // namespace

TEST_CASE("integration: a GeoPackage pushed a chunk at a time, as parts of one dataset, loads back as it was read", "[gdal][integration]") {
  const Files       files;
  const std::string geopackage = files.path_of("buildings.gpkg");
  to_geopackage(files.write("buildings.geojson", BUILDINGS), geopackage);
  const gdal::VectorSource source{.path = geopackage, .layer = {}, .id_field = "osm_id"};

  miniverse::Miniverse world(test_db(), Buildings("miniverse_test_gdal_buildings"));
  world.drop_tables();
  world.create_tables();

  // Parts of one push: a push per chunk would replace the dataset with each chunk, and leave the last alone.
  miniverse::PushInParts<Buildings::model_type> parts = world.begin_push<"buildings">(DATASET).get();
  const gdal::VectorRead read = gdal::read_features<geo::MultiPolygon>(source, 1, [&](auto chunk) { parts.add(std::move(chunk)).get(); });
  parts.commit().get();
  const auto loaded = world.load<"buildings">(bg::from_wkt<geo::Polygon>("POLYGON((-1 -1,30 -1,30 30,-1 30,-1 -1))")).get();
  const auto in_file = gdal::read_features<geo::MultiPolygon>(source);

  CHECK(read.features == 2);
  REQUIRE(loaded.size() == in_file.size());
  for ( std::size_t i = 0; i < in_file.size(); ++i ) {
    CHECK(loaded.at(i).id == static_cast<std::int64_t>(i) + 1);  // the table's own ids, in the order written; the file's are not kept
    CHECK(bg::to_wkt(loaded.at(i).geometry) == bg::to_wkt(in_file.at(i).geometry));
  }
  CHECK(loaded.at(0).tags.text() == R"({"name": "Town hall", "height": 12.5, "levels": 3})");  // jsonb's own order: shorter keys first
  CHECK(loaded.at(1).tags.text() == R"({"name": "Depot", "height": 4.0})");

  world.drop_tables();
}

TEST_CASE("integration: a file pushed in parts is written whole, or not at all", "[gdal][integration]") {
  const Files              files;
  const std::string        path = files.write("buildings.geojson", BUILDINGS);
  const gdal::VectorSource source{.path = path, .layer = {}, .id_field = {}};
  const auto               everywhere = bg::from_wkt<geo::Polygon>("POLYGON((-1 -1,30 -1,30 30,-1 30,-1 -1))");

  miniverse::Miniverse world(test_db(), Buildings("miniverse_test_gdal_buildings"));
  world.drop_tables();
  world.create_tables();

  // A chunk to a part. The first time the reader stops after its first chunk, as a file damaged half way would: nothing is written.
  {
    auto        parts = world.begin_push<"buildings">(DATASET).get();
    std::size_t chunks = 0;
    CHECK_THROWS_WITH(
        gdal::read_features<geo::MultiPolygon>(
            source, 1,
            [&](auto chunk) {
              parts.add(std::move(chunk)).get();
              if ( ++chunks == 1 ) {
                throw std::runtime_error("the file ends here");
              }
            }
        ),
        ContainsSubstring("ends here")
    );
  }
  const auto after_half = world.load<"buildings">(everywhere).get();

  auto parts = world.begin_push<"buildings">(DATASET).get();
  std::ignore = gdal::read_features<geo::MultiPolygon>(source, 1, [&](auto chunk) { parts.add(std::move(chunk)).get(); });
  parts.commit().get();
  const auto after_all = world.load<"buildings">(everywhere).get();

  CHECK(after_half.empty());
  CHECK(after_all.size() == 2);

  world.drop_tables();
}

TEST_CASE("integration: a file of polygons loads as the table it was pushed into, as GeoJSON and as a GeoPackage", "[gdal][integration]") {
  const Files       files;
  const std::string geojson = files.write("areas.geojson", AREAS);
  const std::string geopackage = files.path_of("areas.gpkg");
  to_geopackage(geojson, geopackage);

  check_loads_as_a_table<Buildings>(
      "miniverse_test_gdal_areas", {{.path = geojson, .layer = {}, .id_field = {}}, {.path = geopackage, .layer = "buildings", .id_field = {}}},
      {
          polygon("POLYGON((-5 -5,30 -5,30 30,-5 30,-5 -5))"),
          polygon("POLYGON((-1 -1,5 -1,5 5,-1 5,-1 -1))"),
          polygon("POLYGON((1.4 1.4,1.6 1.4,1.6 1.6,1.4 1.6,1.4 1.4))"),
          polygon("POLYGON((10 10.5,10.3 10.5,10.3 10.9,10 10.9,10 10.5))"),
          polygon("POLYGON((10.6 10,10.9 10,10.9 10.2,10.6 10.2,10.6 10))"),
          polygon("POLYGON((6.5 0.5,7 0.5,7 1,6.5 1,6.5 0.5))"),
          polygon("POLYGON((9 9,12 9,9 12,9 9))"),
          polygon("POLYGON((3 3,9 3,9 9,3 9,3 3),(4.2 4.2,4.2 5.5,5.5 5.5,5.5 4.2,4.2 4.2))"),
          polygon("POLYGON((4 0,6 0,6 2,4 2,4 0))"),  // an edge of 1 on one side, an edge of 3 on the other
          polygon("POLYGON((50 50,51 50,51 51,50 51,50 50))"),
      }
  );
}

TEST_CASE("integration: files of points and of lines load as the tables they were pushed into", "[gdal][integration]") {
  const Files files;

  check_loads_as_a_table<Shops>(
      "miniverse_test_gdal_shops", {{.path = files.write("shops.geojson", SHOPS), .layer = {}, .id_field = {}}},
      {
          polygon("POLYGON((-10 -10,50 -10,50 50,-10 50,-10 -10))"),
          polygon("POLYGON((1 2,2 2,2 3,1 3,1 2))"),
          polygon("POLYGON((1.5 2.5,2 2.5,2 3,1.5 3,1.5 2.5))"),                                // the first shop is its corner
          polygon("POLYGON((6 7,8 7,8 9,6 9,6 7),(6.5 7.5,6.5 8.5,7.5 8.5,7.5 7.5,6.5 7.5))"),  // the third is in its hole
      }
  );
  check_loads_as_a_table<Paths>(
      "miniverse_test_gdal_tracks", {{.path = files.write("tracks.geojson", TRACKS), .layer = {}, .id_field = {}}},
      {
          polygon("POLYGON((-10 -10,50 -10,50 50,-10 50,-10 -10))"),
          polygon("POLYGON((0 0,1 0,1 1,0 1,0 0))"),  // track 1 crosses it; track 2's box overlaps it, and it passes by
          polygon("POLYGON((3 2,4 2,4 3,3 3,3 2))"),  // track 1 ends on its corner
          polygon("POLYGON((39 39,42 39,42 42,39 42,39 39))"),
      }
  );
}

TEST_CASE("integration: a file in another coordinate system loads as the table it was pushed into", "[gdal][integration]") {
  const Files files;

  check_loads_as_a_table<Shops>(
      "miniverse_test_gdal_shops", {{.path = files.write("by_the_parallel.geojson", BY_THE_PARALLEL), .layer = {}, .id_field = {}}},
      {
          polygon("POLYGON((8 50,10.1 50,10.1 51,8 51,8 50))"),  // its south edge between the two points
          polygon("POLYGON((8 49,10.1 49,10.1 50,8 50,8 49))"),  // its north edge there
          polygon("POLYGON((8 49,10.1 49,10.1 51,8 51,8 49))"),
          polygon("POLYGON((20 49,21 49,21 51,20 51,20 49))"),  // in the next zones: far from the file
      }
  );
}
