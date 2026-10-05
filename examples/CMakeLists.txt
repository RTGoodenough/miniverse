# Each example is a program of its own, run with a libpq connection string to a database with PostGIS (and postgis_raster,
# for the raster ones). Each makes its own tables (miniverse_example_*) and drops them when it is done. The examples that
# read files read the small ones of data/.
function(miniverse_example name)
  add_executable(miniverse_example_${name} ${name}.cpp)
  target_link_libraries(miniverse_example_${name} PRIVATE ${ARGN})
  target_compile_definitions(miniverse_example_${name} PRIVATE MINIVERSE_EXAMPLE_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
endfunction()

miniverse_example(features miniverse::miniverse)
miniverse_example(road_network miniverse::miniverse)
miniverse_example(elevation miniverse::miniverse)
miniverse_example(own_kind miniverse::miniverse)

if(TARGET miniverse_gdal)
  miniverse_example(import_vector miniverse::gdal)
  miniverse_example(import_raster miniverse::gdal)
endif()

if(TARGET miniverse_osm)
  miniverse_example(import_osm miniverse::osm)
endif()

if(TARGET miniverse_gdal AND TARGET miniverse_osm)
  miniverse_example(local_files miniverse::gdal miniverse::osm)
endif()
