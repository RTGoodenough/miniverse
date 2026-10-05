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
  compile/own_kind_test.cpp
  unit/wkb_test.cpp
  unit/column_type_test.cpp
  unit/layer_test.cpp
  unit/feature_layer_test.cpp
  unit/raster_test.cpp
  unit/raster_layer_test.cpp
  unit/reader_test.cpp
  integration/feature_layer_test.cpp
  integration/geometry_test.cpp
  integration/miniverse_test.cpp
  integration/raster_layer_test.cpp
  integration/stream_test.cpp
  integration/verify_test.cpp
)
target_include_directories(miniverse_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(miniverse_tests PRIVATE miniverse::miniverse Catch2::Catch2WithMain)
# The integration tests share the test database's tables, so the tests run one at a time even under `ctest -j` (all of them:
# the others take milliseconds, so they aren't worth discovering apart).
catch_discover_tests(miniverse_tests PROPERTIES RESOURCE_LOCK miniverse_test_db)

# ==============================================================================
# miniverse::gdal, if it was built: its own executable, so the tests above stay free of GDAL
# ==============================================================================
if(TARGET miniverse_gdal)
  add_executable(miniverse_gdal_tests gdal/dataset_test.cpp gdal/raster_test.cpp gdal/vector_test.cpp)
  target_include_directories(miniverse_gdal_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(miniverse_gdal_tests PRIVATE miniverse::gdal GDAL::GDAL Catch2::Catch2WithMain)
  catch_discover_tests(miniverse_gdal_tests PROPERTIES RESOURCE_LOCK miniverse_test_db)
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
  catch_discover_tests(miniverse_osm_tests PROPERTIES RESOURCE_LOCK miniverse_test_db)
endif()

# ==============================================================================
# The examples, those that were built: each run against the test database
# ==============================================================================
# Skipped, as the integration tests are, unless MINIVERSE_TEST_DB is set. Each makes and drops its own tables
# (miniverse_example_*), but they take the integration tests' lock all the same: a raster example pushes with the merge
# function that a raster test drops.
foreach(example features road_network elevation own_kind import_vector import_raster import_osm local_files)
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

  add_test(NAME compile_fail.${case} COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG> --target compile_fail_${case})
  set_tests_properties(compile_fail.${case} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}")
endfunction()

miniverse_compile_fail(LOAD_LAYER_NOT_HELD "this miniverse has no such layer")
miniverse_compile_fail(STREAM_LAYER_NOT_HELD "this miniverse has no such layer")
miniverse_compile_fail(PUSH_LAYER_NOT_HELD "this miniverse has no such layer")
miniverse_compile_fail(DUPLICATE_KIND "a miniverse holds one layer of each kind")
miniverse_compile_fail(CREATE_TABLES_NEED_SETTINGS "call create_table<Kind>\\(settings\\) for it")
