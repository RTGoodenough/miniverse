// A miniverse when the database fails under it, or another writer holds what it needs: a connection lost under a push in
// parts and under a stream, a push and a remove that wait for an open push of their dataset, timeouts given in the
// connection string, what a stream sees of a table replaced while it runs, and a database that is not there.
//
// Skipped unless MINIVERSE_TEST_DB holds a libpq connection string to a database with PostGIS enabled. The tests make and
// drop their own table (miniverse_test_failure_roads), and end backends of their own role with pg_terminate_backend.

#include <catch2/catch_test_macros.hpp>

#include <boost/geometry/io/wkt/read.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "schemacht/json/json.hpp"
#include "schemacht/postgres/async_client.hpp"
#include "schemacht/postgres/database.hpp"
#include "schemacht/postgres/statement_runner.hpp"
#include "schemacht/query/predicate.hpp"
#include "schemacht/query/prepared.hpp"
#include "schemacht/query/query.hpp"
#include "schemacht/query/raw_statement.hpp"
#include "schemacht/schema/field.hpp"
#include "support/checks.hpp"
#include "support/database.hpp"
#include "support/geometry.hpp"

namespace bg = boost::geometry;
namespace geo = miniverse::geo;
namespace pg = schemacht::postgres;

using test::ids;
using test::test_db;

namespace {

struct TestRoads : miniverse::RoadLayer {};

/**
 * @brief Roads whose load sleeps 5 ms per row of the table, and gives the rows in no order, so the server is still producing
 * them when a test pulls the connection away or replaces the table under them. The sleep is a subquery around the column,
 * `(SELECT geom FROM pg_sleep(...))`, whose sleep takes a value of the row: with a constant, PostgreSQL runs the function
 * once and rewinds its rows for every other row. No `order_by`, which would hold every row back for the sort.
 */
struct SlowRoads : miniverse::RoadLayer {
  struct SleepyIntersects {
    static constexpr std::string_view FUNCTION = "ST_Intersects";
    static constexpr std::string_view COLUMN_PREFIX = "(SELECT ";
    static constexpr std::string_view COLUMN_SUFFIX = " FROM pg_sleep(0.005 * sign(ST_NPoints(geom))))";
    using argument_type = geo::Polygon;
  };

  static constexpr auto LOAD =
      schemacht::query::select(schemacht::query::On<schema_type>::col<"geom">().apply<SleepyIntersects>(schemacht::query::arg<0>()))
          .project(
              schemacht::query::On<schema_type>::col<"way_id">(), schemacht::query::On<schema_type>::col<"node_ids">(),
              schemacht::query::On<schema_type>::col<"geom">(), schemacht::query::On<schema_type>::col<"tags">()
          );
  using load_statement_type = schemacht::query::Prepared<LOAD>;
};

constexpr const char* TABLE = "miniverse_test_failure_roads";

/** @brief A pool of one connection: the backend a test asks the pid of is the one its next call runs on. */
const pg::Database::Options ONE_CONNECTION{.pool = {.connections = 1, .workers = 2, .session = {}}, .read = {}};

const miniverse::Dataset SURVEY{"survey"};

const geo::Polygon FAR_EAST = test::polygon("POLYGON((50 50,51 50,51 51,50 51,50 50))");

[[nodiscard]] miniverse::Way way(std::int64_t id) {
  return miniverse::Way{
      .id = id,
      .node_ids = {2 * id, (2 * id) + 1},
      .coordinates = bg::from_wkt<geo::LineString>("LINESTRING(50.1 50.1,50.2 50.2)"),
      .tags = schemacht::json::Json("{}")
  };
}

/** @return Ways with ids `first` to `last`, all in the far east. */
[[nodiscard]] miniverse::Ways ways(std::int64_t first, std::int64_t last) {
  miniverse::Ways made;
  for ( std::int64_t id = first; id <= last; ++id ) {
    made.push_back(way(id));
  }

  return made;
}

/** @return A world of `kind_t` over the test table, made fresh with `survey` as the dataset `SURVEY`, dropped afterwards. */
template <typename kind_t>
[[nodiscard]] test::Fresh<miniverse::Miniverse<kind_t>> with_survey(
    const std::string& conninfo, miniverse::Ways survey, const pg::Database::Options& options = {}
) {
  return test::Fresh<miniverse::Miniverse<kind_t>>(
      [survey = std::move(survey)](miniverse::Miniverse<kind_t>& world) mutable {
        world.create_tables();
        world.template push<kind_t>(SURVEY, std::move(survey)).get();
      },
      conninfo, options, miniverse::Layer<kind_t>(TABLE)
  );
}

using BackendPid =
    schemacht::query::RawStatement<"SELECT pg_backend_pid() AS pid", schemacht::query::RawArguments<>, schemacht::schema::Field<std::int32_t, "pid">>;
using Terminate = schemacht::query::RawStatement<
    "SELECT pg_terminate_backend($1) AS ended", schemacht::query::RawArguments<std::int32_t>, schemacht::schema::Field<bool, "ended">>;
using LockWaiters = schemacht::query::RawStatement<
    "SELECT count(*)::integer AS waiting FROM pg_locks WHERE locktype = 'advisory' AND NOT granted"
    " AND database = (SELECT oid FROM pg_database WHERE datname = current_database())",
    schemacht::query::RawArguments<>, schemacht::schema::Field<std::int32_t, "waiting">>;
using Jit = schemacht::query::RawStatement<"SHOW jit", schemacht::query::RawArguments<>, schemacht::schema::Field<std::string, "jit">>;

/** @return The pid of the backend `database`'s next statement runs on: the pool's one connection. */
[[nodiscard]] std::int32_t pid_of(pg::Database& database) { return schemacht::schema::get<"pid">(database.execute(BackendPid::bind()).get().at(0)); }

/** @brief Ends the backend `pid` through `database`, another pool's. */
void terminate(pg::Database& database, std::int32_t pid) {
  REQUIRE(schemacht::schema::get<"ended">(database.execute(Terminate::bind(pid)).get().at(0)));
}

/** @brief Waits, through `probe`, until a transaction waits on an advisory lock (a dataset's): up to 10 s, then fails the test. */
void wait_for_a_lock_waiter(pg::Database& probe) {
  for ( const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10); std::chrono::steady_clock::now() < until;
        std::this_thread::sleep_for(std::chrono::milliseconds(10)) ) {
    if ( schemacht::schema::get<"waiting">(probe.execute(LockWaiters::bind()).get().at(0)) > 0 ) {
      return;
    }
  }

  FAIL("no transaction came to wait on the dataset's lock");
}

/** @return What `result` failed with: the SQLSTATE of a server's error, the message of any other, or "ok". */
template <typename result_t>
[[nodiscard]] std::string failure_of(std::future<result_t> result) {
  try {
    if constexpr ( std::is_void_v<result_t> ) {
      result.get();
    } else {
      std::ignore = result.get();
    }
  } catch ( const pg::QueryError& error ) {
    return error.sqlstate();
  } catch ( const std::exception& error ) {
    return error.what();
  }

  return "ok";
}

/** @return `conninfo` with `setting` (`-c name=value`) among libpq's `options`: as a keyword, or as a parameter of a URL. */
[[nodiscard]] std::string with_server_setting(const std::string& conninfo, std::string_view setting) {
  if ( conninfo.starts_with("postgresql://") || conninfo.starts_with("postgres://") ) {
    std::string encoded;
    for ( const char c : setting ) {
      if ( c == ' ' ) {
        encoded += "%20";
      } else if ( c == '=' ) {
        encoded += "%3D";
      } else {
        encoded += c;
      }
    }

    return conninfo + (conninfo.find('?') == std::string::npos ? "?" : "&") + "options=" + encoded;
  }

  return conninfo + " options='" + std::string(setting) + "'";
}

/** @return The ids of `loaded`, sorted: for a load that gives no order. */
[[nodiscard]] std::vector<std::int64_t> sorted_ids(const miniverse::Ways& loaded) {
  std::vector<std::int64_t> sorted = ids(loaded);
  std::ranges::sort(sorted);

  return sorted;
}

}  // namespace

TEST_CASE(
    "failure: a push in parts whose connection is lost ends with that error, writes nothing, and the miniverse goes on", "[integration][failure]"
) {
  using World = miniverse::Miniverse<TestRoads>;
  test::Fresh<World> made = with_survey<TestRoads>(test_db(), ways(1, 2), ONE_CONNECTION);
  World&             world = made.world();
  pg::Database       other(test_db());

  const std::int32_t                pid = pid_of(world.database());
  miniverse::PushInParts<TestRoads> parts = world.begin_push<TestRoads>(SURVEY).get();
  parts.add(ways(10, 10)).get();
  terminate(other, pid);

  // The step that finds the link gone fails, and every later call fails with that same error.
  SECTION("a part finds it gone") {
    const std::string lost = failure_of(parts.add(ways(11, 11)));
    CHECK(lost != "ok");
    CHECK(failure_of(parts.add(ways(12, 12))) == lost);
    CHECK(failure_of(parts.commit()) == lost);
  }
  SECTION("the commit finds it gone") {
    const std::string lost = failure_of(parts.commit());
    CHECK(lost != "ok");
    CHECK(failure_of(parts.add(ways(11, 11))) == lost);
  }

  // The failing step ended the push's transaction, which gave the pool's one connection back; it is re-established, with
  // its settings, for the next statement. Nothing of the push is there, and the dataset is as it was.
  CHECK(pid_of(world.database()) != pid);
  CHECK(schemacht::schema::get<"jit">(world.database().execute(Jit::bind()).get().at(0)) == "off");
  CHECK(ids(world.load<TestRoads>(FAR_EAST).get()) == std::vector<std::int64_t>{1, 2});
  REQUIRE(world.datasets<TestRoads>().get().size() == 1);
  CHECK(world.datasets<TestRoads>().get().front().rows == 2);

  // The server let go of the dataset's lock with the backend: a new push of it runs at once.
  world.push<TestRoads>(SURVEY, ways(20, 20)).get();
  CHECK(ids(world.load<TestRoads>(FAR_EAST).get()) == std::vector<std::int64_t>{20});
}

TEST_CASE("failure: a stream whose connection is lost fails through its future, and the miniverse goes on", "[integration][failure]") {
  using World = miniverse::Miniverse<SlowRoads>;
  test::Fresh<World> made = with_survey<SlowRoads>(test_db(), ways(1, 200), ONE_CONNECTION);  // a second of server time
  World&             world = made.world();
  pg::Database       other(test_db());

  const std::int32_t       pid = pid_of(world.database());
  std::atomic<std::size_t> chunks{0};
  std::atomic<bool>        ended{false};
  std::future<void>        streamed = world.stream<SlowRoads>(
      FAR_EAST,
      [&](const miniverse::Ways& /*chunk*/) {
        if ( chunks++ == 0 ) {  // the first chunk is in hand while the server still has the rest to find
          ended = schemacht::schema::get<"ended">(other.execute(Terminate::bind(pid)).get().at(0));
        }
      },
      pg::ReadOptions{.chunk_rows = 1}
  );

  // Together: the stream failed after it had begun handing rows over, and before it had them all.
  CHECK_THROWS_AS(streamed.get(), std::runtime_error);
  CHECK(ended);
  CHECK(chunks >= 1);
  CHECK(chunks < 200);

  CHECK(pid_of(world.database()) != pid);
  CHECK(world.load<SlowRoads>(FAR_EAST).get().size() == 200);
}

TEST_CASE("failure: a push of a dataset waits for an open push of it, and is what remains", "[integration][failure]") {
  using World = miniverse::Miniverse<TestRoads>;
  test::Fresh<World> made = with_survey<TestRoads>(test_db(), ways(1, 2));
  World&             world = made.world();
  World              other(test_db(), miniverse::Layer<TestRoads>(TABLE));
  pg::Database       probe(test_db());

  miniverse::PushInParts<TestRoads> open = world.begin_push<TestRoads>(SURVEY).get();
  open.add(ways(10, 10)).get();

  std::future<void> pushing = other.push<TestRoads>(SURVEY, ways(20, 20));
  wait_for_a_lock_waiter(probe);
  CHECK(pushing.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
  CHECK(ids(other.load<TestRoads>(FAR_EAST).get()) == std::vector<std::int64_t>{1, 2});  // neither push is there yet

  open.commit().get();
  pushing.get();

  CHECK(ids(world.load<TestRoads>(FAR_EAST).get()) == std::vector<std::int64_t>{20});
  REQUIRE(world.datasets<TestRoads>().get().size() == 1);
  CHECK(world.datasets<TestRoads>().get().front().rows == 1);
}

TEST_CASE("failure: a remove waits for an open push of its dataset, and removes what that push wrote", "[integration][failure]") {
  using World = miniverse::Miniverse<TestRoads>;
  test::Fresh<World> made = with_survey<TestRoads>(test_db(), ways(1, 2));
  World&             world = made.world();
  World              other(test_db(), miniverse::Layer<TestRoads>(TABLE));
  pg::Database       probe(test_db());

  miniverse::PushInParts<TestRoads> open = world.begin_push<TestRoads>(SURVEY).get();
  open.add(ways(10, 12)).get();

  std::future<std::uint64_t> removed = other.remove<TestRoads>(SURVEY);
  wait_for_a_lock_waiter(probe);
  CHECK(removed.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);

  open.commit().get();

  CHECK(removed.get() == 3);
  CHECK(world.load<TestRoads>(FAR_EAST).get().empty());
  CHECK(world.datasets<TestRoads>().get().empty());
}

TEST_CASE(
    "failure: a lock timeout in the connection string ends the wait for a dataset with its error, and nothing is written", "[integration][failure]"
) {
  using World = miniverse::Miniverse<TestRoads>;
  test::Fresh<World> made = with_survey<TestRoads>(test_db(), ways(1, 2));
  World&             world = made.world();
  World              impatient(with_server_setting(test_db(), "-c lock_timeout=200"), miniverse::Layer<TestRoads>(TABLE));

  miniverse::PushInParts<TestRoads> open = world.begin_push<TestRoads>(SURVEY).get();
  open.add(ways(10, 10)).get();

  CHECK(failure_of(impatient.push<TestRoads>(SURVEY, ways(20, 20))) == "55P03");
  CHECK(failure_of(impatient.remove<TestRoads>(SURVEY)) == "55P03");
  CHECK(failure_of(impatient.push<TestRoads>(miniverse::Dataset{"another"}, ways(30, 30))) == "ok");  // another dataset: no wait

  open.commit().get();
  CHECK(ids(world.load<TestRoads>(FAR_EAST).get()) == std::vector<std::int64_t>{10, 30});
}

TEST_CASE("failure: a statement timeout in the connection string ends a load that outruns it, through its future", "[integration][failure]") {
  using World = miniverse::Miniverse<SlowRoads>;
  test::Fresh<World> made = with_survey<SlowRoads>(test_db(), ways(1, 50));  // a quarter of a second of server time
  World&             world = made.world();
  World              impatient(with_server_setting(test_db(), "-c statement_timeout=100"), miniverse::Layer<SlowRoads>(TABLE));

  CHECK(failure_of(impatient.load<SlowRoads>(FAR_EAST)) == "57014");
  CHECK(world.load<SlowRoads>(FAR_EAST).get().size() == 50);
}

TEST_CASE("failure: a push does not wait for a stream, and the stream sees the table as it was when it started", "[integration][failure]") {
  using World = miniverse::Miniverse<SlowRoads>;
  test::Fresh<World> made = with_survey<SlowRoads>(test_db(), ways(1, 50));
  World&             world = made.world();
  World              other(test_db(), miniverse::Layer<SlowRoads>(TABLE));

  std::promise<void> started;
  std::promise<void> replaced;
  std::future<void>  replaced_done = replaced.get_future();
  std::mutex         seen_mutex;
  miniverse::Ways    seen;
  std::future<void>  streamed = world.stream<SlowRoads>(
      FAR_EAST,
      [&](miniverse::Ways chunk) {
        const std::lock_guard lock(seen_mutex);
        if ( seen.empty() ) {  // the first chunk: hold it until the dataset has been replaced
          started.set_value();
          std::ignore = replaced_done.wait_for(std::chrono::seconds(30));
        }
        seen.insert(seen.end(), std::make_move_iterator(chunk.begin()), std::make_move_iterator(chunk.end()));
      },
      pg::ReadOptions{.chunk_rows = 1}
  );

  REQUIRE(started.get_future().wait_for(std::chrono::seconds(10)) == std::future_status::ready);
  other.push<SlowRoads>(SURVEY, ways(101, 110)).get();  // committed while the stream's statement runs, and holds a chunk
  replaced.set_value();
  streamed.get();

  CHECK(sorted_ids(seen) == sorted_ids(ways(1, 50)));
  CHECK(sorted_ids(world.load<SlowRoads>(FAR_EAST).get()) == sorted_ids(ways(101, 110)));
}

TEST_CASE("failure: a miniverse to a database that is not there fails in its constructor", "[failure]") {
  using World = miniverse::Miniverse<TestRoads>;

  CHECK_THROWS_AS(World("host=localhost port=1 connect_timeout=2", miniverse::Layer<TestRoads>(TABLE)), std::runtime_error);
}
