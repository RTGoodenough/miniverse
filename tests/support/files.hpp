#pragma once

// What the tests of the file loaders share: a directory for a test's files, and the scratch database.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

namespace test {

/** @brief A directory of its own for a test's files, removed with everything in it afterwards. */
class Files {
 public:
  Files() : _directory(std::filesystem::temp_directory_path() / ("miniverse_file_test_" + std::to_string(std::random_device()()))) {
    std::filesystem::create_directories(_directory);
  }

  /** @return The path of a file named `name` holding `text`. */
  [[nodiscard]] std::string write(const std::string& name, std::string_view text) const {
    const std::filesystem::path path = _directory / name;
    std::ofstream(path) << text;

    return path.string();
  }

  [[nodiscard]] std::string path_of(const std::string& name) const { return (_directory / name).string(); }

 private:
  std::filesystem::path _directory;

 public:
  Files(const Files&) = delete;
  Files(Files&&) = delete;
  Files& operator=(const Files&) = delete;
  Files& operator=(Files&&) = delete;
  ~Files() {
    std::error_code ignored;
    std::filesystem::remove_all(_directory, ignored);
  }
};

/** @return The scratch database's connection string (MINIVERSE_TEST_DB); a test that asks for it is skipped without one. */
[[nodiscard]] inline std::string test_db() {
  const char* conninfo = std::getenv("MINIVERSE_TEST_DB");
  if ( conninfo == nullptr ) {
    SKIP("MINIVERSE_TEST_DB is not set");
  }

  return conninfo;
}

}  // namespace test
