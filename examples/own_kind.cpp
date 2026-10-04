// A kind of layer of your own: a table with the columns you choose, and the structure it is loaded into.
//
// `FeatureLayer` keeps what a feature is in its tags. A kind written by hand has typed columns instead, its own query, and
// its own result: here towns, with a name and a population, loaded the largest first. It is loaded and pushed like the
// built-in kinds, which are written the same way (include/miniverse/concepts/miniverse.hpp says what a kind needs).
//
// Run with a libpq connection string to a database with PostGIS: miniverse_example_own_kind "host=localhost dbname=gis user=gis"
// It creates the table miniverse_example_towns, and drops it when it is done.

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "miniverse/miniverse.hpp"
#include "schemacht/schemacht.hpp"
#include "support.hpp"

namespace geo = miniverse::geo;
namespace schema = schemacht::schema;
namespace query = schemacht::query;

/** @brief What the program works with. */
struct Town {
  std::int64_t id = 0;
  std::string  name;
  std::int32_t population = 0;
  geo::Point   centre;
};

/** @brief The layer kind of towns: one row each, loaded by where its centre is, the largest first. */
struct Towns {
  using result_type = std::vector<Town>;

  // The table's layout, each column NOT NULL: `town_id bigint PRIMARY KEY, name text, population integer, centre
  // geometry(Point,4326)`. The schema's own name is only a placeholder: the layer names its table.
  using schema_type = schema::Schema<
      "towns", schema::Field<std::int64_t, "town_id", schema::KeyRole::Primary>, schema::Field<std::string, "name">,
      schema::Field<std::int32_t, "population">, schema::Field<geo::Point, "centre">>;
  using row_type = schema_type::row_type;

  // What a load reads: the towns whose centre is in argument 0, the polygon. Checked against the schema while compiling.
  static constexpr auto LOAD = query::select(query::On<schema_type>::col<"centre">().apply<geo::Intersects>(query::arg<0>()))
                                   .order_by(query::On<schema_type>::col<"population">().desc());
  using load_statement_type = query::Prepared<LOAD>;

  /** @return What runs once the table is made: the spatial index the load is answered by. */
  [[nodiscard]] static std::vector<std::string> setup_sql(const schema::TableName& table) {
    return {"CREATE INDEX ON " + table.quoted() + " USING gist (centre)"};
  }

  /** @return `towns` as rows, in batches of one statement each: towns are small, so one batch. */
  [[nodiscard]] static std::vector<std::vector<row_type>> to_rows(result_type towns) {
    std::vector<std::vector<row_type>> batches(1);

    batches.front().reserve(towns.size());
    for ( Town& town : towns ) {
      batches.front().push_back(schema_type::make_row(town.id, std::move(town.name), town.population, town.centre));
    }

    return batches;
  }

  /** @return The write of one batch: a town already there takes the new name, population and centre. */
  [[nodiscard]] static auto write_statement(const std::vector<row_type>& rows) { return schemacht::postgres::upsert_statement<schema_type>(rows); }

  /** @return The towns of `rows`, which are already the ones in the location, in the load's order. */
  [[nodiscard]] static result_type from_rows(std::vector<row_type> rows, const geo::Polygon& /*location*/) {
    result_type towns;

    towns.reserve(rows.size());
    for ( row_type& row : rows ) {
      towns.push_back({
          .id = schema::get<"town_id">(row),
          .name = std::move(schema::get<"name">(row)),
          .population = schema::get<"population">(row),
          .centre = schema::get<"centre">(row),
      });
    }

    return towns;
  }
};

int main(int argc, char** argv) {
  try {
    miniverse::Miniverse world(example::conninfo(argc, argv), miniverse::Layer<Towns>("miniverse_example_towns"));

    world.drop_tables();
    world.create_tables();

    world
        .push<Towns>({
            {.id = 1, .name = "Ashford", .population = 12'400, .centre = geo::Point(10.10, 50.20)},
            {.id = 2, .name = "Brook", .population = 870, .centre = geo::Point(10.30, 50.25)},
            {.id = 3, .name = "Castleton", .population = 48'000, .centre = geo::Point(10.60, 50.10)},
            {.id = 4, .name = "Farwell", .population = 5'100, .centre = geo::Point(14.00, 53.00)},  // outside the area below
        })
        .get();

    // This kind's write is an upsert, so a second push of a town replaces it.
    world.push<Towns>({{.id = 2, .name = "Brook", .population = 910, .centre = geo::Point(10.30, 50.25)}}).get();

    std::cout << "the towns of the area, the largest first:\n";
    for ( const Town& town : world.load<Towns>(example::box(10, 50, 11, 51)).get() ) {
      std::cout << "  " << town.name << ": " << town.population << '\n';
    }

    world.drop_tables();

    return EXIT_SUCCESS;
  } catch ( const std::exception& error ) {
    std::cerr << "error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
