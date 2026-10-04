include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
  GIT_SHALLOW TRUE
  FIND_PACKAGE_ARGS 3
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# ==============================================================================
# Compile-time (static) checks, unit tests and integration tests: one executable
# ==============================================================================
# The integration tests skip themselves unless MINIVERSE_TEST_DB holds a libpq connection string to a database with PostGIS
# and postgis_raster.
add_executable(
  miniverse_tests
  compile/layer_test.cpp
  unit/wkb_test.cpp
  unit/column_type_test.cpp
  unit/layer_test.cpp
  unit/raster_test.cpp
  unit/elevation_layer_test.cpp
  integration/miniverse_test.cpp
  integration/elevation_test.cpp
)
target_include_directories(miniverse_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(miniverse_tests PRIVATE miniverse::miniverse Catch2::Catch2WithMain)
# The integration tests share the test database's tables, so the tests run one at a time even under `ctest -j` (all of them:
# the others take milliseconds, so they aren't worth discovering apart).
catch_discover_tests(miniverse_tests PROPERTIES RESOURCE_LOCK miniverse_test_db)

# ==============================================================================
# Compile-fail tests
# ==============================================================================
# Each builds compile_fail/cases.cpp with one case defined, as a target left out of the default build, and passes if the
# compiler's output has the expected message (which it can only print by failing).
function(miniverse_compile_fail case regex)
  add_library(compile_fail_${case} OBJECT EXCLUDE_FROM_ALL compile_fail/cases.cpp)
  target_link_libraries(compile_fail_${case} PRIVATE miniverse::miniverse)
  target_compile_definitions(compile_fail_${case} PRIVATE CASE_${case})

  add_test(NAME compile_fail.${case} COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG> --target compile_fail_${case})
  set_tests_properties(compile_fail.${case} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}")
endfunction()

miniverse_compile_fail(LOAD_LAYER_NOT_HELD "this miniverse has no such layer")
miniverse_compile_fail(PUSH_LAYER_NOT_HELD "this miniverse has no such layer")
miniverse_compile_fail(DUPLICATE_KIND "a miniverse holds one layer of each kind")
