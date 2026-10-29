// miniverse: makes, fills and checks the tables of a miniverse database from the command line, for the built-in models. A
// program with models of its own uses the library instead.
//
//   miniverse create   --model MODEL TABLE [--pixels-per-degree N | --metres M] [--tile-pixels N] [--nodata V]
//   miniverse import   --model MODEL TABLE FILE... [--dataset NAME] [--layer NAME] [--keys KEY,...] [--chunk N]
//                      [--band N] [--resampling NAME] [--window-tiles N]
//   miniverse remove   --model MODEL TABLE DATASET
//   miniverse verify   --model MODEL TABLE
//   miniverse settings --model MODEL TABLE
//   miniverse datasets --model MODEL TABLE
//
// `--db` gives the libpq connection string; without it, libpq's defaults and the PG* variables say where. TABLE is `table`
// or `schema.table`. It exits with 0 when the command is done, 1 when it failed, and 2 for arguments it can't take, before
// it connects; the reason is on standard error.

#include <CLI11.hpp>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include "commands.hpp"
#include "models.hpp"

namespace {

constexpr int FAILED = 1;
constexpr int CANT_TAKE = 2;

/** @brief Adds the options every command takes to `command`: the table's model, and its name. */
void add_table(CLI::App& command, tool::Table& table) {
  command.add_option("--model", table.model, "What the table holds")->required()->check(CLI::IsMember(tool::model_names()));
  command.add_option("table", table.name, "The table: table, or schema.table")->required();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    CLI::App app("Makes, fills and checks the tables of a miniverse database, for the built-in models.", "miniverse");
    app.require_subcommand(1);
    app.fallthrough();  // --db before or after the command

    tool::Table table;
    app.add_option("--db", table.conninfo, "The libpq connection string; libpq's defaults and PG* variables without it");

    CLI::App*         create = app.add_subcommand("create", "Make a table");
    tool::GridOptions grid;
    add_table(*create, table);
    CLI::Option* per_degree = create->add_option("--pixels-per-degree", grid.pixels_per_degree, "A raster table's pixels to a degree");
    create->add_option("--metres", grid.metres, "A raster table's metres to a pixel, made the nearest pixels per degree")->excludes(per_degree);
    create->add_option("--tile-pixels", grid.tile_pixels, "A raster table's tiles, in pixels a side");
    create->add_option("--nodata", grid.nodata, "A raster table's value of a pixel with no data");

    CLI::App*           import = app.add_subcommand("import", "Push files into a table, each all of it or none");
    tool::ImportOptions files;
    add_table(*import, table);
    import->add_option("files", files.files, "The files: one for features or roads, any number for a raster")->required();
    import->add_option("--dataset", files.dataset, "Features and roads: the dataset the file replaces (the file's name)");
    import->add_option("--layer", files.layer, "A vector file's layer, of one with several");
    import->add_option("--keys", files.keys, "An OpenStreetMap file: the ways with a tag of one of these keys (highway)")->delimiter(',');
    import->add_option("--chunk", files.chunk, "Features or ways a part (10000)");
    import->add_option("--band", files.band, "A raster file's band (1)");
    import->add_option("--resampling", files.resampling, "How a raster file's pixels are made the grid's (bilinear)")
        ->check(CLI::IsMember({"nearest", "bilinear", "cubic", "average"}));
    import->add_option("--window-tiles", files.window_tiles, "A raster file's parts, in tiles a side (2)");

    CLI::App*   remove = app.add_subcommand("remove", "Remove a dataset from a table of features or roads");
    std::string dataset;
    add_table(*remove, table);
    remove->add_option("dataset", dataset, "The dataset")->required();

    CLI::App* verify = app.add_subcommand("verify", "Check a table is as its model says");
    add_table(*verify, table);

    CLI::App* settings = app.add_subcommand("settings", "Show the grid a raster table was made with");
    add_table(*settings, table);

    CLI::App* datasets = app.add_subcommand("datasets", "List the datasets of a table of features or roads");
    add_table(*datasets, table);

    try {
      app.parse(argc, argv);
    } catch ( const CLI::ParseError& error ) {
      return app.exit(error) == 0 ? EXIT_SUCCESS : CANT_TAKE;  // --help is a parse error that exits with 0
    }

    if ( create->parsed() ) {
      tool::create(table, grid);
    } else if ( import->parsed() ) {
      tool::import(table, files);
    } else if ( remove->parsed() ) {
      tool::remove(table, dataset);
    } else if ( verify->parsed() ) {
      tool::verify(table);
    } else if ( settings->parsed() ) {
      tool::settings(table);
    } else {
      tool::datasets(table);
    }

    return EXIT_SUCCESS;
  } catch ( const std::invalid_argument& error ) {
    std::cerr << "error: " << error.what() << '\n';
    return CANT_TAKE;
  } catch ( const std::exception& error ) {  // the command's error, or one in making the command line
    std::cerr << "error: " << error.what() << '\n';
    return FAILED;
  }
}
