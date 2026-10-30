#pragma once

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

namespace test {

/** @brief A directory of its own for a test's files, removed with everything in it afterwards. */
class Files
{
 public:
  Files()
      : _directory(std::filesystem::temp_directory_path() /
                   ("miniverse_file_test_" + std::to_string(std::random_device()())))
  {
    std::filesystem::create_directories(_directory);
  }

  /** @return The path of a file named `name` holding `text`. */
  [[nodiscard]] std::string write(const std::string& name, std::string_view text) const
  {
    const std::filesystem::path path = _directory / name;
    std::ofstream(path) << text;

    return path.string();
  }

  [[nodiscard]] std::string path_of(const std::string& name) const
  {
    return (_directory / name).string();
  }

 private:
  std::filesystem::path _directory;

 public:
  Files(const Files&)            = delete;
  Files(Files&&)                 = delete;
  Files& operator=(const Files&) = delete;
  Files& operator=(Files&&)      = delete;

  ~Files()
  {
    std::error_code ignored;
    std::filesystem::remove_all(_directory, ignored);
  }
};

}  // namespace test
