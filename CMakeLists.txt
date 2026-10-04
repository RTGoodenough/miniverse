cmake_minimum_required(VERSION 3.21)

project(miniverse VERSION 0.1.0 LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

option(MINIVERSE_INSTALL "Generate the install rules: the library, its headers and a CMake package (find_package(miniverse))" ${PROJECT_IS_TOP_LEVEL})
option(MINIVERSE_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
option(MINIVERSE_BUILD_EXAMPLES "Build the examples" ${PROJECT_IS_TOP_LEVEL})
set(MINIVERSE_BUILD_GDAL "AUTO" CACHE STRING "Build miniverse::gdal, which reads files with GDAL: AUTO (if GDAL is found), ON or OFF")
set_property(CACHE MINIVERSE_BUILD_GDAL PROPERTY STRINGS AUTO ON OFF)
set(MINIVERSE_BUILD_OSM "AUTO" CACHE STRING "Build miniverse::osm, which reads OpenStreetMap files with libosmium: AUTO (if it is found), ON or OFF")
set_property(CACHE MINIVERSE_BUILD_OSM PROPERTY STRINGS AUTO ON OFF)
set(MINIVERSE_SCHEMACHT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../schemacht" CACHE PATH "schemacht's source tree, used when no installed schemacht is found")

# ==============================================================================
# Dependencies
# ==============================================================================
# schemacht: installed (find_package), or else built from its source tree beside this one.
find_package(schemacht CONFIG QUIET)
if(NOT schemacht_FOUND)
  if(NOT EXISTS "${MINIVERSE_SCHEMACHT_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "schemacht was not found: install it (find_package(schemacht)), or set MINIVERSE_SCHEMACHT_DIR to its source tree.")
  endif()
  # Installing miniverse needs schemacht installed beside it: its package refers to schemacht's.
  if(NOT DEFINED SCHEMACHT_INSTALL)  # a -D of its own wins
    set(SCHEMACHT_INSTALL ${MINIVERSE_INSTALL})
  endif()
  add_subdirectory("${MINIVERSE_SCHEMACHT_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/schemacht")
endif()

# Boost.Geometry: header-only, the geometry types (geo/types.hpp).
find_package(Boost 1.74 REQUIRED)

# ==============================================================================
# Library
# ==============================================================================
add_library(
  miniverse
  STATIC
  src/geo/raster.cpp
  src/geo/wkb.cpp
  src/layer/feature_layer.cpp
  src/layer/raster_layer.cpp
  src/layer/road_layer.cpp
)

target_include_directories(
  miniverse
  PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(miniverse PUBLIC schemacht::schemacht Boost::headers)
target_compile_features(miniverse PUBLIC cxx_std_23)

add_library(miniverse::miniverse ALIAS miniverse)
set_target_properties(miniverse PROPERTIES EXPORT_NAME miniverse)

# ==============================================================================
# Optional: miniverse::gdal, files read with GDAL
# ==============================================================================
# Its own library, so that the core never needs GDAL. Its headers are in include/miniverse/gdal.
set(MINIVERSE_HAS_GDAL FALSE)
string(TOUPPER "${MINIVERSE_BUILD_GDAL}" MINIVERSE_BUILD_GDAL_CHOICE)
if(MINIVERSE_BUILD_GDAL_CHOICE STREQUAL "ON")
  find_package(GDAL CONFIG REQUIRED)
elseif(MINIVERSE_BUILD_GDAL_CHOICE STREQUAL "AUTO")
  find_package(GDAL CONFIG QUIET)
elseif(NOT MINIVERSE_BUILD_GDAL_CHOICE STREQUAL "OFF")
  message(FATAL_ERROR "MINIVERSE_BUILD_GDAL is '${MINIVERSE_BUILD_GDAL}': AUTO, ON or OFF")
endif()

# Not for OFF, even if a project around this one has found GDAL for itself.
if(GDAL_FOUND AND NOT MINIVERSE_BUILD_GDAL_CHOICE STREQUAL "OFF")
  set(MINIVERSE_HAS_GDAL TRUE)

  add_library(miniverse_gdal STATIC src/gdal/raster.cpp src/gdal/vector.cpp)
  target_link_libraries(miniverse_gdal PUBLIC miniverse::miniverse PRIVATE GDAL::GDAL)

  add_library(miniverse::gdal ALIAS miniverse_gdal)
  set_target_properties(miniverse_gdal PROPERTIES EXPORT_NAME gdal)
endif()
message(STATUS "miniverse::gdal (MINIVERSE_BUILD_GDAL=${MINIVERSE_BUILD_GDAL}): ${MINIVERSE_HAS_GDAL}")

# ==============================================================================
# Optional: miniverse::osm, OpenStreetMap files read with libosmium
# ==============================================================================
# Its own library too. libosmium is headers only, and has no CMake package: it is found by its headers, with protozero's and
# the libraries its file formats need. Its tags are written as JSON with Boost.JSON, which Boost has from 1.75, compiled
# into this library (src/osm/boost_json.cpp) so that it needs no Boost library to link.
set(MINIVERSE_HAS_OSM FALSE)
string(TOUPPER "${MINIVERSE_BUILD_OSM}" MINIVERSE_BUILD_OSM_CHOICE)
if(NOT MINIVERSE_BUILD_OSM_CHOICE MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "MINIVERSE_BUILD_OSM is '${MINIVERSE_BUILD_OSM}': AUTO, ON or OFF")
endif()

if(NOT MINIVERSE_BUILD_OSM_CHOICE STREQUAL "OFF")
  find_path(MINIVERSE_OSMIUM_INCLUDE_DIR osmium/version.hpp)
  find_path(MINIVERSE_PROTOZERO_INCLUDE_DIR protozero/version.hpp)
  find_package(ZLIB QUIET)
  find_package(EXPAT QUIET)
  find_package(BZip2 QUIET)
  find_package(Threads QUIET)

  if(MINIVERSE_OSMIUM_INCLUDE_DIR AND MINIVERSE_PROTOZERO_INCLUDE_DIR AND ZLIB_FOUND AND EXPAT_FOUND AND BZIP2_FOUND AND Threads_FOUND
     AND Boost_VERSION VERSION_GREATER_EQUAL 1.75)
    set(MINIVERSE_HAS_OSM TRUE)
  elseif(MINIVERSE_BUILD_OSM_CHOICE STREQUAL "ON")
    message(FATAL_ERROR "MINIVERSE_BUILD_OSM is ON, but miniverse::osm needs libosmium, protozero, zlib, expat, bzip2, threads and Boost 1.75 or newer: one was not found.")
  endif()
endif()

if(MINIVERSE_HAS_OSM)
  add_library(miniverse_osm STATIC src/osm/boost_json.cpp src/osm/ways.cpp)
  target_include_directories(miniverse_osm SYSTEM PRIVATE ${MINIVERSE_OSMIUM_INCLUDE_DIR} ${MINIVERSE_PROTOZERO_INCLUDE_DIR})
  target_link_libraries(miniverse_osm PUBLIC miniverse::miniverse PRIVATE ZLIB::ZLIB EXPAT::EXPAT BZip2::BZip2 Threads::Threads)

  add_library(miniverse::osm ALIAS miniverse_osm)
  set_target_properties(miniverse_osm PROPERTIES EXPORT_NAME osm)
endif()
message(STATUS "miniverse::osm (MINIVERSE_BUILD_OSM=${MINIVERSE_BUILD_OSM}): ${MINIVERSE_HAS_OSM}")

# ==============================================================================
# Examples and tests
# ==============================================================================
# The examples first: the tests run each one that was built.
if(MINIVERSE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(MINIVERSE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ==============================================================================
# Install
# ==============================================================================
# `cmake --install <build> --prefix <dir>` puts under <dir>:
#   lib/libminiverse.a        the library (and lib/libminiverse_gdal.a and lib/libminiverse_osm.a, if built)
#   include/miniverse/...     its headers
#   lib/cmake/miniverse/      the package: find_package(miniverse) gives miniverse::miniverse (and miniverse::gdal and
#                             miniverse::osm, if built)
if(MINIVERSE_INSTALL)
  set(MINIVERSE_INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/miniverse)

  install(TARGETS miniverse EXPORT miniverse-targets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  if(MINIVERSE_HAS_GDAL)
    install(TARGETS miniverse_gdal EXPORT miniverse-targets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  endif()
  if(MINIVERSE_HAS_OSM)
    install(TARGETS miniverse_osm EXPORT miniverse-targets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  endif()
  install(EXPORT miniverse-targets NAMESPACE miniverse:: DESTINATION ${MINIVERSE_INSTALL_CMAKEDIR})
  # Without the headers of a library that was not built.
  set(MINIVERSE_HEADERS_LEFT_OUT)
  if(NOT MINIVERSE_HAS_GDAL)
    list(APPEND MINIVERSE_HEADERS_LEFT_OUT REGEX "/miniverse/gdal$" EXCLUDE)
  endif()
  if(NOT MINIVERSE_HAS_OSM)
    list(APPEND MINIVERSE_HEADERS_LEFT_OUT REGEX "/miniverse/osm$" EXCLUDE)
  endif()
  install(DIRECTORY include/miniverse DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.hpp" ${MINIVERSE_HEADERS_LEFT_OUT})

  configure_package_config_file(
    cmake/miniverse-config.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/miniverse-config.cmake
    INSTALL_DESTINATION ${MINIVERSE_INSTALL_CMAKEDIR}
  )
  # 0.x: a new minor version may change anything, so only a patch release is compatible.
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/miniverse-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
  )
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/miniverse-config.cmake ${CMAKE_CURRENT_BINARY_DIR}/miniverse-config-version.cmake
          DESTINATION ${MINIVERSE_INSTALL_CMAKEDIR})
endif()
