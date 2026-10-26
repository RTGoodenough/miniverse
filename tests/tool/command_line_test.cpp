// The tool itself, run as a user runs it: its options reach the commands, a table in a schema is named `schema.table`, and
// it exits with 0, 1 (failed) or 2 (arguments it can't take). Each test needs MINIVERSE_TEST_DB, and is skipped without it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <gdal.h>
#include <gdal_utils.h>

#include <stdio.h>   // NOLINT(modernize-deprecated-headers) -- popen and pclose are POSIX, of <stdio.h>
#include <stdlib.h>  // NOLINT(modernize-deprecated-headers) -- WIFEXITED and WEXITSTATUS

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "kinds.hpp"
#include "miniverse/miniverse.hpp"
#include "schemacht/postgres/statements.hpp"
#include "support/database.hpp"
#include "support/files.hpp"
#include "support/geometry.hpp"

namespace geo = miniverse::geo;

using Catch::Matchers::ContainsSubstring;
using test::polygon;
using test::test_db;

namespace {

const std::string BUILDINGS_FILE = MINIVERSE_EXAMPLE_DATA "/buildings.geojson";
const std::string STREETS_FILE = MINIVERSE_EXAMPLE_DATA "/streets.osm";
const std::string HEIGHTS_FILE = MINIVERSE_EXAMPLE_DATA "/heights.asc";

const geo::Polygon EVERYWHERE = polygon("POLYGON((-179 -89,179 -89,179 89,-179 89,-179 -89))");

/** @brief How a run of the tool went: its exit code, and what it wrote to standard output and error. */
struct Run {
  int         exit = -1;
  std::string output;
};

/** @return `text` in single quotes for the shell, each quote in it closed, escaped and reopened. */
[[nodiscard]] std::string quoted(const std::string& text) {
  std::string result = "'";
  for ( const char letter : text ) {
    result += letter == '\'' ? std::string("'\\''") : std::string(1, letter);
  }

  return result + "'";
}

/** @return The tool run with `arguments` (written for the shell) and `--db` the test database. */
[[nodiscard]] Run run(const std::string& arguments) {
  const std::string                     command = std::string(MINIVERSE_TOOL) + " " + arguments + " --db " + quoted(test_db()) + " 2>&1";
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(command.c_str(), "r"), pclose);  // NOLINT(cert-env33-c) -- run as a user runs it, from a shell
  REQUIRE(pipe != nullptr);

  Run                    result;
  std::array<char, 4096> buffer{};
  while ( fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr ) {
    result.output += buffer.data();
  }
  const int status = pclose(pipe.release());
  result.exit = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

  return result;
}

/** @brief Copies the vector file `from` to `to`, a directory GDAL makes of one shapefile. */
void to_shapefiles(const std::string& from, const std::string& to) {
  GDALAllRegister();

  std::vector<std::string> words{"-f", "ESRI Shapefile"};
  std::vector<char*>       arguments;
  arguments.reserve(words.size() + 1);
  for ( std::string& word : words ) {
    arguments.push_back(word.data());
  }
  arguments.push_back(nullptr);

  GDALDatasetH source = GDALOpenEx(from.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
  REQUIRE(source != nullptr);
  GDALVectorTranslateOptions* options = GDALVectorTranslateOptionsNew(arguments.data(), nullptr);
  GDALDatasetH                made = GDALVectorTranslate(to.c_str(), nullptr, 1, &source, options, nullptr);
  GDALVectorTranslateOptionsFree(options);
  REQUIRE(made != nullptr);
  GDALClose(made);
  GDALClose(source);
}

/** @return A world of the one layer `kind_t` in `table`, its table dropped before and after the test. */
template <typename kind_t>
[[nodiscard]] test::Fresh<miniverse::Miniverse<kind_t>> fresh(miniverse::Layer<kind_t> layer) {
  return test::Fresh<miniverse::Miniverse<kind_t>>([](miniverse::Miniverse<kind_t>& /*world*/) {}, test_db(), std::move(layer));
}

}  // namespace

TEST_CASE("tool command line: an import's options reach the command", "[tool][integration]") {
  using Buildings = tool::Features<geo::MultiPolygon>;
  auto buildings = fresh(miniverse::Layer<Buildings>("miniverse_test_tool_buildings"));
  auto roads = fresh(miniverse::Layer<tool::Roads>("miniverse_test_tool_roads"));
  auto heights = fresh(miniverse::Layer<tool::Raster<std::int16_t>>("miniverse_test_tool_heights"));

  CHECK(run("create --kind features:multipolygon miniverse_test_tool_buildings").exit == 0);
  const Run named = run("import --kind features:multipolygon miniverse_test_tool_buildings " + BUILDINGS_FILE + " --dataset village --chunk 1");
  CHECK(named.exit == 0);
  CHECK_THAT(named.output, ContainsSubstring("as the dataset 'village'"));
  CHECK_THAT(run("datasets --kind features:multipolygon miniverse_test_tool_buildings").output, ContainsSubstring("village\t4 rows\t20"));
  CHECK(run("remove --kind features:multipolygon miniverse_test_tool_buildings village").exit == 0);
  CHECK_THAT(run("datasets --kind features:multipolygon miniverse_test_tool_buildings").output, ContainsSubstring("no datasets"));
  CHECK(buildings.world().load<Buildings>(EVERYWHERE).get().empty());

  // Of the extract's ways, one is a building, and not a road.
  CHECK(run("create --kind roads miniverse_test_tool_roads").exit == 0);
  CHECK_THAT(run("import --kind roads miniverse_test_tool_roads " + STREETS_FILE + " --keys building").output, ContainsSubstring(": 1 ways"));
  CHECK(roads.world().load<tool::Roads>(EVERYWHERE).get().size() == 1);

  // A degree is 3711 pixels of 30 m; windows of one tile are more windows than of two.
  CHECK_THAT(
      run("create --kind raster:int16 miniverse_test_tool_heights --metres 30 --tile-pixels 256 --nodata -32768").output,
      ContainsSubstring("3711 pixels per degree")
  );
  const Run ones = run("import --kind raster:int16 miniverse_test_tool_heights " + HEIGHTS_FILE + " --window-tiles 1 --resampling nearest");
  const Run twos = run("import --kind raster:int16 miniverse_test_tool_heights " + HEIGHTS_FILE + " --window-tiles 2 --resampling nearest");
  CHECK(ones.exit == 0);
  CHECK(twos.exit == 0);
  CHECK(ones.output != twos.output);
  const Run band = run("import --kind raster:int16 miniverse_test_tool_heights " + HEIGHTS_FILE + " --band 2");
  CHECK(band.exit == 1);
  CHECK_THAT(band.output, ContainsSubstring("so no band 2"));
  CHECK_THAT(run("settings --kind raster:int16 miniverse_test_tool_heights").output, ContainsSubstring("nodata: -32768"));
}

TEST_CASE("tool command line: a table in a schema is named schema.table, and a directory's dataset is its name", "[tool][integration]") {
  using Places = tool::Features<geo::MultiPolygon>;
  miniverse::Miniverse<Places> world(test_db(), miniverse::Layer<Places>("miniverse_test_tool", "places"));
  std::ignore = world.database().execute(schemacht::postgres::unchecked_sql("DROP SCHEMA IF EXISTS miniverse_test_tool CASCADE")).get();
  std::ignore = world.database().execute(schemacht::postgres::unchecked_sql("CREATE SCHEMA miniverse_test_tool")).get();

  const test::Files files;
  const std::string shapes = files.path_of("shapes");
  to_shapefiles(BUILDINGS_FILE, shapes);

  CHECK(run("create --kind features:multipolygon miniverse_test_tool.places").exit == 0);
  CHECK(run("verify --kind features:multipolygon miniverse_test_tool.places").exit == 0);
  const Run directory = run("import --kind features:multipolygon miniverse_test_tool.places " + shapes + "/");  // as a shell completes it
  CHECK(directory.exit == 0);
  CHECK_THAT(directory.output, ContainsSubstring("as the dataset 'shapes'"));

  std::ignore = world.database().execute(schemacht::postgres::unchecked_sql("DROP SCHEMA miniverse_test_tool CASCADE")).get();
}

TEST_CASE("tool command line: it exits with 2 for arguments it can't take, and 1 for a command that failed", "[tool][integration]") {
  using Buildings = tool::Features<geo::MultiPolygon>;
  auto buildings = fresh(miniverse::Layer<Buildings>("miniverse_test_tool_buildings"));
  CHECK(run("create --kind features:multipolygon miniverse_test_tool_buildings").exit == 0);

  CHECK(run("verify --kind nothing miniverse_test_tool_buildings").exit == 2);
  CHECK(run("import --kind features:multipolygon miniverse_test_tool_buildings " + BUILDINGS_FILE + " --band 2").exit == 2);
  CHECK(run("create --kind raster:int8 miniverse_test_tool_bytes --pixels-per-degree 4 --tile-pixels 4 --nodata 300").exit == 2);
  CHECK(run("verify --kind features:point miniverse_test_tool_buildings").exit == 1);
  CHECK(run("create --kind features:multipolygon miniverse_test_tool_buildings").exit == 1);  // there already
  CHECK(run("remove --kind features:multipolygon miniverse_test_tool_buildings never_imported").exit == 1);
  CHECK(run("--help").exit == 0);
}
