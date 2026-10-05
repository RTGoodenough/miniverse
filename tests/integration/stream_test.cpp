// Streamed loads against a real PostGIS database: a layer handed to a callback a chunk at a time.
//
// Skipped unless MINIVERSE_TEST_DB holds a libpq connection string to a database with PostGIS enabled. The tests make and drop
// their own table (miniverse_test_stream_shops), so point it at a scratch database.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <boost/geometry/io/wkt/read.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "schemacht/postgres/async_client.hpp"
#include "schemacht/postgres/database.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;

namespace {

struct Shops : miniverse::FeatureLayer<geo::Point> {};

using World = miniverse::Miniverse<Shops>;
using Chunk = miniverse::Features<geo::Point>;

[[nodiscard]] std::string test_db() {
  const char* conninfo = std::getenv("MINIVERSE_TEST_DB");
  if ( conninfo == nullptr ) {
    SKIP("MINIVERSE_TEST_DB is not set");
  }

  return conninfo;
}

const geo::Polygon NEAR_ORIGIN = bg::from_wkt<geo::Polygon>("POLYGON((-1 -1,5 -1,5 5,-1 5,-1 -1))");
const geo::Polygon NOWHERE = bg::from_wkt<geo::Polygon>("POLYGON((30 30,31 30,31 31,30 31,30 30))");

/** @brief A world of five shops near the origin (ids 1 to 5) and one far away (6); its table dropped afterwards. */
class Stocked {
 public:
  explicit Stocked(const std::string& conninfo, const schemacht::postgres::Database::Options& options = {})
      : _world(conninfo, options, miniverse::Layer<Shops>("miniverse_test_stream_shops")) {
    _world.drop_tables();
    _world.create_tables();

    Chunk shops;
    for ( std::int64_t id = 1; id <= 5; ++id ) {
      shops.push_back({.id = id, .geometry = geo::Point(static_cast<double>(id) / 2, 1)});
    }
    shops.push_back({.id = 6, .geometry = geo::Point(50, 50)});
    _world.push<Shops>(std::move(shops)).get();
  }

  [[nodiscard]] World& world() noexcept { return _world; }

 private:
  World _world;

 public:
  Stocked(const Stocked&) = delete;
  Stocked(Stocked&&) = delete;
  Stocked& operator=(const Stocked&) = delete;
  Stocked& operator=(Stocked&&) = delete;
  ~Stocked() {
    // A lost database must not end the run from a destructor, and the next test drops the table first anyway.
    try {
      _world.drop_tables();
    } catch ( ... ) {  // NOLINT(bugprone-empty-catch)
    }
  }
};

/** @brief What a stream's callback was handed: filled on a pool thread, one call at a time, and read once its future is done. */
struct Seen {
  std::vector<std::size_t>  chunk_sizes;
  std::vector<std::int64_t> ids;

  void take(const Chunk& chunk) {
    chunk_sizes.push_back(chunk.size());
    for ( const miniverse::Feature<geo::Point>& shop : chunk ) {
      ids.push_back(shop.id);
    }
  }
};

}  // namespace

TEST_CASE("integration: a stream hands a load over a chunk at a time, in the load's order", "[integration]") {
  Stocked stocked(test_db());
  Seen    seen;

  stocked.world().stream<Shops>(NEAR_ORIGIN, [&](const Chunk& chunk) { seen.take(chunk); }, {.chunk_rows = 2}).get();

  CHECK(seen.chunk_sizes == std::vector<std::size_t>{2, 2, 1});  // the last chunk is what is left
  CHECK(seen.ids == std::vector<std::int64_t>{1, 2, 3, 4, 5});   // not the shop far away
}

TEST_CASE("integration: a stream's chunks are the pool's own size unless it says otherwise", "[integration]") {
  Stocked by_default(test_db());
  Seen    whole;
  by_default.world().stream<Shops>(NEAR_ORIGIN, [&](const Chunk& chunk) { whole.take(chunk); }).get();

  // A pool made to read two rows at a time: its streams hand over chunks of two.
  Stocked in_twos(test_db(), {.pool = {}, .read = {.chunk_rows = 2}});
  Seen    pairs;
  in_twos.world().stream<Shops>(NEAR_ORIGIN, [&](const Chunk& chunk) { pairs.take(chunk); }).get();

  CHECK(whole.chunk_sizes == std::vector<std::size_t>{5});  // far fewer than a pool reads at a time by default
  CHECK(pairs.chunk_sizes == std::vector<std::size_t>{2, 2, 1});
}

TEST_CASE("integration: a stream's callback stops the load by returning false", "[integration]") {
  Stocked stocked(test_db());
  Seen    seen;

  stocked.world()
      .stream<Shops>(
          NEAR_ORIGIN,
          [&](const Chunk& chunk) {
            seen.take(chunk);

            return false;
          },
          {.chunk_rows = 2}
      )
      .get();  // stopped, which is not an error

  CHECK(seen.chunk_sizes == std::vector<std::size_t>{2});
  CHECK(seen.ids == std::vector<std::int64_t>{1, 2});
}

TEST_CASE("integration: what a stream's callback throws is the load's error", "[integration]") {
  Stocked stocked(test_db());
  Seen    seen;

  auto done = stocked.world().stream<Shops>(
      NEAR_ORIGIN,
      [&](const Chunk& chunk) {
        seen.take(chunk);
        throw std::runtime_error("the worker has had enough");
      },
      {.chunk_rows = 2}
  );

  CHECK_THROWS_WITH(done.get(), Catch::Matchers::ContainsSubstring("had enough"));
  CHECK(seen.chunk_sizes == std::vector<std::size_t>{2});  // handed before it threw, and nothing after
}

TEST_CASE("integration: what a stream's callback throws at the last chunk is the load's error too", "[integration]") {
  Stocked stocked(test_db());
  Seen    seen;

  // A callback with a count of its own: the third chunk is the one row left over, handed over as the load ends.
  auto done = stocked.world().stream<Shops>(
      NEAR_ORIGIN,
      [&seen, chunks = std::size_t{0}](const Chunk& chunk) mutable {
        seen.take(chunk);
        if ( ++chunks == 3 ) {
          throw std::runtime_error("one chunk too many");
        }
      },
      {.chunk_rows = 2}
  );

  CHECK_THROWS_WITH(done.get(), Catch::Matchers::ContainsSubstring("too many"));
  CHECK(seen.chunk_sizes == std::vector<std::size_t>{2, 2, 1});
}

TEST_CASE("integration: a stream where there is nothing never calls its callback", "[integration]") {
  Stocked stocked(test_db());
  Seen    seen;

  stocked.world().stream<Shops>(NOWHERE, [&](const Chunk& chunk) { seen.take(chunk); }).get();

  CHECK(seen.chunk_sizes.empty());
}

TEST_CASE("integration: a stream that fails reports why through its future", "[integration]") {
  Stocked stocked(test_db());
  Seen    seen;
  stocked.world().drop_tables();

  auto done = stocked.world().stream<Shops>(NEAR_ORIGIN, [&](const Chunk& chunk) { seen.take(chunk); });

  CHECK_THROWS_AS(done.get(), schemacht::postgres::QueryError);  // the table is gone
  CHECK(seen.chunk_sizes.empty());
}
