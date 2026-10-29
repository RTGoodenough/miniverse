include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
  GIT_SHALLOW TRUE
  SYSTEM  # its headers are system headers here: the tests' warnings are about the tests
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
  compile/own_model_test.cpp
  unit/wkb_test.cpp
  unit/column_type_test.cpp
  unit/layer_test.cpp
  unit/road_model_test.cpp
  unit/feature_model_test.cpp
  unit/grid_test.cpp
  unit/raster_test.cpp
  unit/raster_wkb_test.cpp
  unit/raster_reach_test.cpp
  unit/raster_tiles_test.cpp
  unit/raster_model_test.cpp
  unit/reader_test.cpp
  unit/step_thread_test.cpp
  integration/failure_test.cpp
  integration/feature_model_test.cpp
  integration/geometry_test.cpp
  integration/miniverse_test.cpp
  integration/raster_model_test.cpp
  integration/stream_test.cpp
  integration/verify_test.cpp
)
target_include_directories(miniverse_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(miniverse_tests PRIVATE miniverse::miniverse Catch2::Catch2WithMain)
miniverse_checks(miniverse_tests)
# The integration tests share the test database's tables, so the tests run one at a time even under `ctest -j` (all of them:
# the others take milliseconds, so they aren't worth discovering apart).
catch_discover_tests(miniverse_tests PROPERTIES RESOURCE_LOCK miniverse_test_db)

# ==============================================================================
# miniverse::gdal, if it was built: its own executable, so the tests above stay free of GDAL
# ==============================================================================
if(TARGET miniverse_gdal)
  add_executable(
    miniverse_gdal_tests
    gdal/dataset_test.cpp
    gdal/raster_read_test.cpp
    gdal/raster_file_test.cpp
    gdal/raster_equivalence_test.cpp
    gdal/vector_read_test.cpp
    gdal/vector_file_test.cpp
    gdal/vector_equivalence_test.cpp
  )
  target_include_directories(miniverse_gdal_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(miniverse_gdal_tests PRIVATE miniverse::gdal GDAL::GDAL Catch2::Catch2WithMain)
  miniverse_checks(miniverse_gdal_tests)
  catch_discover_tests(miniverse_gdal_tests PROPERTIES RESOURCE_LOCK miniverse_test_db)
endif()

# ==============================================================================
# The command-line tool, if it was built with both file components: its commands, called as the command line calls them,
# and the program itself, run as a user runs it
# ==============================================================================
if(TARGET miniverse_tool_commands AND TARGET miniverse_gdal AND TARGET miniverse_osm)
  add_executable(miniverse_tool_tests tool/commands_test.cpp tool/command_line_test.cpp)
  target_include_directories(miniverse_tool_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(
    miniverse_tool_tests PRIVATE MINIVERSE_EXAMPLE_DATA="${PROJECT_SOURCE_DIR}/examples/data" MINIVERSE_TOOL="$<TARGET_FILE:miniverse_tool>"
  )
  add_dependencies(miniverse_tool_tests miniverse_tool)
  target_link_libraries(miniverse_tool_tests PRIVATE miniverse_tool_commands GDAL::GDAL Catch2::Catch2WithMain)
  miniverse_checks(miniverse_tool_tests)
  catch_discover_tests(miniverse_tool_tests PROPERTIES RESOURCE_LOCK miniverse_test_db)
endif()

# ==============================================================================
# miniverse::osm, if it was built: its own executable too
# ==============================================================================
if(TARGET miniverse_osm)
  add_executable(miniverse_osm_tests osm/ways_test.cpp)
  target_include_directories(miniverse_osm_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  # The tests make a PBF file with libosmium themselves, so they need its headers; a program that only reads ways does not.
  target_include_directories(miniverse_osm_tests SYSTEM PRIVATE ${MINIVERSE_OSMIUM_INCLUDE_DIR} ${MINIVERSE_PROTOZERO_INCLUDE_DIR})
  target_link_libraries(miniverse_osm_tests PRIVATE miniverse::osm Catch2::Catch2WithMain)
  miniverse_checks(miniverse_osm_tests)
  catch_discover_tests(miniverse_osm_tests PROPERTIES RESOURCE_LOCK miniverse_test_db)
endif()

# ==============================================================================
# The examples, those that were built: each run against the test database
# ==============================================================================
# Skipped, as the integration tests are, unless MINIVERSE_TEST_DB is set. Each makes and drops its own tables
# (miniverse_example_*), but they take the integration tests' lock all the same: a raster example pushes with the merge
# function that a raster test drops.
foreach(example features road_network elevation own_model import_vector import_raster import_osm local_files)
  if(TARGET miniverse_example_${example})
    add_test(NAME example.${example} COMMAND ${CMAKE_COMMAND} -DEXAMPLE=$<TARGET_FILE:miniverse_example_${example}> -P
                                             ${CMAKE_CURRENT_SOURCE_DIR}/run_example.cmake)
    set_tests_properties(
      example.${example} PROPERTIES SKIP_REGULAR_EXPRESSION "MINIVERSE_TEST_DB is not set: skipped" RESOURCE_LOCK miniverse_test_db
    )
  endif()
endforeach()

# ==============================================================================
# Compile-fail tests
# ==============================================================================
# Each builds compile_fail/cases.cpp with one case defined, as a target left out of the default build, and passes if the
# compiler's output has the expected message (which it can only print by failing).
function(miniverse_compile_fail case regex)
  add_library(compile_fail_${case} OBJECT EXCLUDE_FROM_ALL compile_fail/cases.cpp)
  target_link_libraries(compile_fail_${case} PRIVATE miniverse::miniverse)
  target_compile_definitions(compile_fail_${case} PRIVATE CASE_${case})
  # GCC names the failing requirement of a nested concept only when asked; clang does so by default.
  target_compile_options(compile_fail_${case} PRIVATE $<$<CXX_COMPILER_ID:GNU>:-fconcepts-diagnostics-depth=3>)
  miniverse_checks(compile_fail_${case})

  add_test(NAME compile_fail.${case} COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG> --target compile_fail_${case})
  set_tests_properties(compile_fail.${case} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}")
endfunction()

miniverse_compile_fail(LOAD_LAYER_NOT_HELD "this miniverse has no such layer")
miniverse_compile_fail(STREAM_LAYER_NOT_HELD "this miniverse has no such layer")
miniverse_compile_fail(PUSH_LAYER_NOT_HELD "this miniverse has no such layer")
miniverse_compile_fail(DUPLICATE_NAME "two layers of the miniverse have one name")
miniverse_compile_fail(DUPLICATE_NAME_ACROSS_MODELS "two layers of the miniverse have one name")
miniverse_compile_fail(NOT_A_LAYER "IsLayer")
miniverse_compile_fail(NO_LAYERS "a miniverse holds at least one layer")
miniverse_compile_fail(PUSH_NEEDS_DATASET "rows belong to a dataset")
miniverse_compile_fail(PUSH_TAKES_NO_DATASET "has no datasets")
miniverse_compile_fail(CREATE_TABLES_NEED_SETTINGS "call create_table<.name.>\\(settings\\) for")
miniverse_compile_fail(NO_LAYOUT "LAYOUT")
miniverse_compile_fail(NO_WRITES "model_t::writes")
miniverse_compile_fail(DATASETS_WITHOUT_COLUMNS "has_column")

# ==============================================================================
# Formatting
# ==============================================================================
# One test, if clang-format is found: every source and header of the project, checked against .clang-format.
find_program(MINIVERSE_CLANG_FORMAT NAMES clang-format-18 clang-format DOC "clang-format, for the test `format`")
if(MINIVERSE_CLANG_FORMAT)
  set(MINIVERSE_FORMATTED_FILES)
  foreach(directory include src tests examples tools)
    file(GLOB_RECURSE MINIVERSE_FORMATTED_IN CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/${directory}/*.hpp ${PROJECT_SOURCE_DIR}/${directory}/*.cpp)
    list(APPEND MINIVERSE_FORMATTED_FILES ${MINIVERSE_FORMATTED_IN})
  endforeach()
  add_test(NAME format COMMAND ${MINIVERSE_CLANG_FORMAT} --dry-run --Werror ${MINIVERSE_FORMATTED_FILES})
endif()
message(STATUS "format test (MINIVERSE_CLANG_FORMAT): ${MINIVERSE_CLANG_FORMAT}")
