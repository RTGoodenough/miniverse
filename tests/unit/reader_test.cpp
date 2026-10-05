// Layers read by a reader instead of a table: loaded and streamed by the same calls, in a miniverse that needs no database.
//
// The last test puts a table and a reader in one miniverse: it needs MINIVERSE_TEST_DB (a libpq connection string to a scratch
// database with PostGIS), and is skipped without it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <boost/geometry/algorithms/covered_by.hpp>  // IWYU pragma: keep

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "support/files.hpp"
#include "miniverse/miniverse.hpp"

namespace geo = miniverse::geo;

using Catch::Matchers::ContainsSubstring;
using miniverse::Layer;
using miniverse::Miniverse;

namespace {

struct Shops : miniverse::FeatureLayer<geo::Point> {};
struct Stalls : miniverse::FeatureLayer<geo::Point> {};
struct Elevation : miniverse::RasterLayer<std::int16_t> {};

using Points = miniverse::Features<geo::Point>;

constexpr geo::Grid<std::int16_t> GRID{.pixels_per_degree = 4, .tile_pixels = 4, .nodata = -32768};

[[nodiscard]] geo::Polygon box(double west, double south, double east, double north) {
  geo::Polygon area;
  area.outer() = {{west, south}, {east, south}, {east, north}, {west, north}, {west, south}};

  return area;
}

const geo::Polygon NEAR_ORIGIN = box(-1, -1, 5, 5);

/** @brief A reader of points held in memory: a load gives those in the location, in the order they were given. */
template <miniverse::LayerKind kind_t>
class FixedPoints : public miniverse::Reader<kind_t> {
 public:
  explicit FixedPoints(Points points, std::vector<std::string> problems = {}) : _points(std::move(points)), _problems(std::move(problems)) {}

  [[nodiscard]] std::string name() const override { return "fixed points"; }

  [[nodiscard]] Points load(const geo::Polygon& location) const override {
    _loaded_on = std::this_thread::get_id();

    Points inside;
    std::ranges::copy_if(_points, std::back_inserter(inside), [&location](const miniverse::Feature<geo::Point>& point) {
      return boost::geometry::covered_by(point.geometry, location);  // NOLINT(misc-include-cleaner)
    });

    return inside;
  }

  void stream(const geo::Polygon& location, std::size_t chunk_rows, const miniverse::Reader<kind_t>::OnChunk& on_chunk) const override {
    const Points inside = load(location);
    for ( std::size_t first = 0; first < inside.size(); first += chunk_rows ) {
      const std::size_t last = std::min(inside.size(), first + chunk_rows);
      if ( ! on_chunk(Points(inside.begin() + static_cast<std::ptrdiff_t>(first), inside.begin() + static_cast<std::ptrdiff_t>(last))) ) {
        return;
      }
    }
  }

  [[nodiscard]] std::vector<std::string> problems() const override { return _problems; }

  /** @return The thread that last ran a load. */
  [[nodiscard]] std::thread::id loaded_on() const { return _loaded_on; }

 private:
  Points                               _points;
  std::vector<std::string>             _problems;
  mutable std::atomic<std::thread::id> _loaded_on;

 public:
  FixedPoints(const FixedPoints&) = delete;
  FixedPoints(FixedPoints&&) = delete;
  FixedPoints& operator=(const FixedPoints&) = delete;
  FixedPoints& operator=(FixedPoints&&) = delete;
  ~FixedPoints() override = default;
};

/** @brief A reader that fails every load, and says only what its grid is. */
class NoHeights : public miniverse::Reader<Elevation> {
 public:
  [[nodiscard]] std::string             name() const override { return "no heights"; }
  [[nodiscard]] geo::Grid<std::int16_t> settings() const override { return GRID; }

  [[nodiscard]] geo::Raster<std::int16_t> load(const geo::Polygon& /*location*/) const override { throw std::runtime_error("the heights are lost"); }

  void stream(const geo::Polygon& /*location*/, std::size_t /*chunk_rows*/, const OnChunk& /*on_chunk*/) const override {
    throw std::runtime_error("the heights are lost");
  }
};

/** @return Five shops near the origin (ids 1 to 5) and one far away (6). */
[[nodiscard]] Points shops() {
  Points points;
  for ( std::int64_t id = 1; id <= 5; ++id ) {
    points.push_back({.id = id, .geometry = geo::Point(static_cast<double>(id) / 2, 1)});
  }
  points.push_back({.id = 6, .geometry = geo::Point(50, 50)});

  return points;
}

template <miniverse::LayerKind kind_t>
[[nodiscard]] Layer<kind_t> fixed(Points points, std::vector<std::string> problems = {}) {
  return Layer<kind_t>(std::make_shared<FixedPoints<kind_t>>(std::move(points), std::move(problems)));
}

[[nodiscard]] std::vector<std::int64_t> ids(const Points& points) {
  std::vector<std::int64_t> result;
  for ( const miniverse::Feature<geo::Point>& point : points ) {
    result.push_back(point.id);
  }

  return result;
}

}  // namespace

TEST_CASE("reader: a miniverse of readers has no database, and loads what its readers give", "[reader]") {
  Miniverse world(fixed<Shops>(shops()), fixed<Stalls>({{.id = 9, .geometry = geo::Point(2, 2)}}));

  // Started together, then waited for, as loads of tables are.
  std::future<Points> near_shops = world.load<Shops>(NEAR_ORIGIN);
  std::future<Points> near_stalls = world.load<Stalls>(NEAR_ORIGIN);

  CHECK(ids(near_shops.get()) == std::vector<std::int64_t>{1, 2, 3, 4, 5});
  CHECK(ids(near_stalls.get()) == std::vector<std::int64_t>{9});
  CHECK(world.load<Shops>(box(30, 30, 31, 31)).get().empty());
  CHECK_THROWS_AS(world.database(), std::logic_error);
  CHECK_THROWS_AS(world.table_name<Shops>(), std::logic_error);
}

TEST_CASE("reader: a layer is a table or has a reader, and a table needs a database", "[reader]") {
  CHECK_THROWS_AS(Layer<Shops>(std::shared_ptr<const miniverse::Reader<Shops>>()), std::invalid_argument);
  CHECK_THROWS_AS((Miniverse<Shops, Stalls>(fixed<Shops>({}), Layer<Stalls>("stalls"))), std::invalid_argument);

  const Layer<Shops> read = fixed<Shops>({});
  CHECK(! read.is_table());
  CHECK_THROWS_WITH(read.table(), ContainsSubstring("fixed points"));
  CHECK(Layer<Shops>("shops").is_table());
  CHECK(Layer<Shops>("shops").reader() == nullptr);
}

TEST_CASE("reader: a stream hands over chunks of the size asked for, in the reader's order", "[reader]") {
  Miniverse           world(fixed<Shops>(shops()));
  std::vector<Points> chunks;

  world.stream<Shops>(NEAR_ORIGIN, [&chunks](Points chunk) { chunks.push_back(std::move(chunk)); }, {.chunk_rows = 2}).get();

  REQUIRE(chunks.size() == 3);
  CHECK(ids(chunks.at(0)) == std::vector<std::int64_t>{1, 2});
  CHECK(ids(chunks.at(1)) == std::vector<std::int64_t>{3, 4});
  CHECK(ids(chunks.at(2)) == std::vector<std::int64_t>{5});
}

TEST_CASE("reader: a stream with no chunk size has the default's 1000 rows a chunk", "[reader]") {
  Miniverse   world(fixed<Shops>(shops()));
  std::size_t chunks = 0;

  world.stream<Shops>(NEAR_ORIGIN, [&chunks](const Points& chunk) { chunks += chunk.size() == 5 ? 1 : 100; }).get();

  CHECK(chunks == 1);
}

TEST_CASE("reader: a stream of chunks of no rows is refused, as a table's is", "[reader]") {
  Miniverse world(fixed<Shops>(shops()));

  CHECK_THROWS_AS(world.stream<Shops>(NEAR_ORIGIN, [](const Points& /*chunk*/) {}, {.chunk_rows = 0}).get(), std::invalid_argument);
}

TEST_CASE("reader: a load runs on a thread of its own, not the caller's", "[reader]") {
  const auto reader = std::make_shared<FixedPoints<Shops>>(shops());
  Miniverse  world{Layer<Shops>(reader)};

  std::ignore = world.load<Shops>(NEAR_ORIGIN).get();

  CHECK(reader->loaded_on() != std::thread::id());
  CHECK(reader->loaded_on() != std::this_thread::get_id());
}

TEST_CASE("reader: a location whose ring is not closed is refused, as PostGIS refuses it for most tables", "[reader]") {
  Miniverse    world(fixed<Shops>(shops()));
  geo::Polygon open_ring;
  open_ring.outer() = {{-1, -1}, {5, -1}, {5, 5}, {-1, 5}};
  geo::Polygon two_points;
  two_points.outer() = {{-1, -1}, {-1, -1}};
  geo::Polygon open_hole = NEAR_ORIGIN;
  open_hole.inners().push_back({{0, 0}, {0, 1}, {1, 1}, {1, 0}});

  CHECK_THROWS_AS(world.load<Shops>(open_ring).get(), std::invalid_argument);
  CHECK_THROWS_AS(world.load<Shops>(two_points).get(), std::invalid_argument);
  CHECK_THROWS_AS(world.load<Shops>(open_hole).get(), std::invalid_argument);
  CHECK_THROWS_AS(world.stream<Shops>(open_ring, [](const Points& /*chunk*/) {}).get(), std::invalid_argument);
  CHECK(world.load<Shops>(geo::Polygon{}).get().empty());  // a location of no points holds nothing
}

TEST_CASE("reader: a stream stops when its callback wants no more", "[reader]") {
  Miniverse   world(fixed<Shops>(shops()));
  std::size_t chunks = 0;

  world.stream<Shops>(NEAR_ORIGIN, [&chunks](const Points& /*chunk*/) { return ++chunks < 2; }, {.chunk_rows = 1}).get();

  CHECK(chunks == 2);
}

TEST_CASE("reader: what a callback or a reader throws comes through the future", "[reader]") {
  Miniverse world(fixed<Shops>(shops()), Layer<Elevation>(std::make_shared<NoHeights>()));

  std::future<void> thrown_at = world.stream<Shops>(NEAR_ORIGIN, [](const Points& /*chunk*/) { throw std::runtime_error("no more room"); });
  std::future<void> lost = world.stream<Elevation>(NEAR_ORIGIN, [](const std::vector<geo::Raster<std::int16_t>>& /*tiles*/) {});

  CHECK_THROWS_WITH(thrown_at.get(), ContainsSubstring("no more room"));
  CHECK_THROWS_WITH(lost.get(), ContainsSubstring("the heights are lost"));
  CHECK_THROWS_WITH(world.load<Elevation>(NEAR_ORIGIN).get(), ContainsSubstring("the heights are lost"));
}

TEST_CASE("reader: a layer with a reader is only read", "[reader]") {
  Miniverse world(fixed<Shops>(shops()), Layer<Elevation>(std::make_shared<NoHeights>()));

  CHECK_THROWS_WITH(world.push<Shops>({{.id = 7, .geometry = geo::Point(1, 1)}}).get(), ContainsSubstring("fixed points"));
  CHECK_THROWS_AS(world.begin_push<Shops>().get(), std::logic_error);
  CHECK_THROWS_AS(world.push<Elevation>({}).get(), std::logic_error);
  CHECK_THROWS_AS(world.begin_push<Elevation>().get(), std::logic_error);
  CHECK(ids(world.load<Shops>(NEAR_ORIGIN).get()).size() == 5);
}

TEST_CASE("reader: there is no table to make or drop, so a setup for tables does nothing", "[reader]") {
  Miniverse world(fixed<Shops>(shops()), Layer<Elevation>(std::make_shared<NoHeights>()));

  CHECK_NOTHROW(world.drop_tables());
  CHECK_NOTHROW(world.create_table<Shops>());
  CHECK_NOTHROW(world.create_table<Elevation>(GRID));
}

TEST_CASE("reader: a raster layer's settings are its reader's", "[reader]") {
  Miniverse world(Layer<Elevation>(std::make_shared<NoHeights>()));

  CHECK(world.table_settings<Elevation>().get() == GRID);
}

TEST_CASE("reader: verify asks each reader what is wrong, and says it of the reader's name", "[reader]") {
  Miniverse sound(fixed<Shops>(shops()), fixed<Stalls>({}));
  Miniverse broken(fixed<Shops>(shops(), {"it has no points", "it names no coordinate system"}), fixed<Stalls>({}));

  CHECK_NOTHROW(sound.verify().get());

  try {
    broken.verify().get();
    FAIL("verify passed a reader with problems");
  } catch ( const miniverse::TablesDiffer& differ ) {
    CHECK(differ.differences() == std::vector<std::string>{"fixed points: it has no points", "fixed points: it names no coordinate system"});
  }
}

TEST_CASE("integration: a miniverse holds tables and readers side by side", "[integration]") {
  Miniverse world(test::test_db(), Layer<Shops>("miniverse_test_reader_shops"), fixed<Stalls>({{.id = 9, .geometry = geo::Point(2, 2)}}));
  world.drop_tables();
  world.create_tables();  // the table's; the reader has none
  world.push<Shops>(shops()).get();

  std::future<Points> near_shops = world.load<Shops>(NEAR_ORIGIN);
  std::future<Points> near_stalls = world.load<Stalls>(NEAR_ORIGIN);

  CHECK(ids(near_shops.get()) == std::vector<std::int64_t>{1, 2, 3, 4, 5});
  CHECK(ids(near_stalls.get()) == std::vector<std::int64_t>{9});
  CHECK_NOTHROW(world.verify().get());
  CHECK_THROWS_AS(world.push<Stalls>({}).get(), std::logic_error);

  world.drop_tables();
}
