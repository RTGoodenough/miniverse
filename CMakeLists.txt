cmake_minimum_required(VERSION 3.21)

project(miniverse VERSION 0.1.0 LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
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
set(MINIVERSE_BUILD_DOCS "AUTO" CACHE STRING "Add the target `miniverse_docs`, the API reference built from the headers with Doxygen: AUTO (if Doxygen is found), ON or OFF")
set_property(CACHE MINIVERSE_BUILD_DOCS PROPERTY STRINGS AUTO ON OFF)

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
  src/geo/raster_wkb.cpp
  src/geo/wkb.cpp
  src/layer/feature_layer.cpp
  src/layer/raster/reach.cpp
  src/layer/raster/tiles.cpp
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
target_compile_features(miniverse PUBLIC cxx_std_20)

add_library(miniverse::miniverse ALIAS miniverse)
set_target_properties(miniverse PROPERTIES EXPORT_NAME miniverse)

# ==============================================================================
# Optional: miniverse::gdal, files read with GDAL
# ==============================================================================
# Its own library, so that the core never needs GDAL. Its headers are in include/miniverse/gdal; one of them, dataset.hpp,
# hands out GDAL's own dataset, so GDAL's headers come with the library.
# GDAL 3.7 or newer: GDT_Int8 for rasters of std::int8_t (3.7), TransformBounds for a file's extent (3.4), prepared
# geometries in the public API (3.3), and GDAL's own CMake package (3.5). Built with GEOS, which the usual packages are.
# The version is checked after the find: GDAL's package only accepts a version of its own minor, so a minimum can't be
# asked of find_package.
set(MINIVERSE_MIN_GDAL 3.7)
set(MINIVERSE_HAS_GDAL FALSE)
string(TOUPPER "${MINIVERSE_BUILD_GDAL}" MINIVERSE_BUILD_GDAL_CHOICE)
if(MINIVERSE_BUILD_GDAL_CHOICE STREQUAL "ON")
  find_package(GDAL CONFIG REQUIRED)
elseif(MINIVERSE_BUILD_GDAL_CHOICE STREQUAL "AUTO")
  find_package(GDAL CONFIG QUIET)
elseif(NOT MINIVERSE_BUILD_GDAL_CHOICE STREQUAL "OFF")
  message(FATAL_ERROR "MINIVERSE_BUILD_GDAL is '${MINIVERSE_BUILD_GDAL}': AUTO, ON or OFF")
endif()

if(GDAL_FOUND AND GDAL_VERSION VERSION_LESS MINIVERSE_MIN_GDAL)
  if(MINIVERSE_BUILD_GDAL_CHOICE STREQUAL "ON")
    message(FATAL_ERROR "miniverse::gdal needs GDAL ${MINIVERSE_MIN_GDAL} or newer, found ${GDAL_VERSION}")
  endif()
  message(STATUS "GDAL ${GDAL_VERSION} found, but miniverse::gdal needs ${MINIVERSE_MIN_GDAL} or newer: not built")
  set(GDAL_FOUND FALSE)
endif()

# Not for OFF, even if a project around this one has found GDAL for itself.
if(GDAL_FOUND AND NOT MINIVERSE_BUILD_GDAL_CHOICE STREQUAL "OFF")
  set(MINIVERSE_HAS_GDAL TRUE)

  add_library(
    miniverse_gdal STATIC
    src/gdal/dataset.cpp
    src/gdal/raster.cpp
    src/gdal/raster/source.cpp
    src/gdal/vector.cpp
    src/gdal/vector/fields.cpp
    src/gdal/vector/filter.cpp
    src/gdal/vector/geometry.cpp
    src/gdal/vector/source.cpp
  )
  target_include_directories(miniverse_gdal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)  # its private headers, as "gdal/..."
  target_link_libraries(miniverse_gdal PUBLIC miniverse::miniverse GDAL::GDAL)

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
  add_library(miniverse_osm STATIC src/osm/boost_json.cpp src/osm/ways.cpp src/osm/ways_in_memory.cpp)
  target_include_directories(miniverse_osm SYSTEM PRIVATE ${MINIVERSE_OSMIUM_INCLUDE_DIR} ${MINIVERSE_PROTOZERO_INCLUDE_DIR})
  target_link_libraries(miniverse_osm PUBLIC miniverse::miniverse PRIVATE ZLIB::ZLIB EXPAT::EXPAT BZip2::BZip2 Threads::Threads)

  add_library(miniverse::osm ALIAS miniverse_osm)
  set_target_properties(miniverse_osm PROPERTIES EXPORT_NAME osm)
endif()
message(STATUS "miniverse::osm (MINIVERSE_BUILD_OSM=${MINIVERSE_BUILD_OSM}): ${MINIVERSE_HAS_OSM}")

# ==============================================================================
# Documentation: the target `miniverse_docs`, if Doxygen is found
# ==============================================================================
# The API reference, from the comments in include/, with README.md as its front page and docs/ as further pages, written to
# build/docs/html. No Doxyfile is kept: CMake's FindDoxygen writes one from the DOXYGEN_ variables below. Diagrams need
# Graphviz (dot); without it the reference is built without them.
string(TOUPPER "${MINIVERSE_BUILD_DOCS}" MINIVERSE_BUILD_DOCS_CHOICE)
if(MINIVERSE_BUILD_DOCS_CHOICE STREQUAL "ON")
  find_package(Doxygen REQUIRED OPTIONAL_COMPONENTS dot)
elseif(MINIVERSE_BUILD_DOCS_CHOICE STREQUAL "AUTO")
  find_package(Doxygen QUIET OPTIONAL_COMPONENTS dot)
elseif(NOT MINIVERSE_BUILD_DOCS_CHOICE STREQUAL "OFF")
  message(FATAL_ERROR "MINIVERSE_BUILD_DOCS is '${MINIVERSE_BUILD_DOCS}': AUTO, ON or OFF")
endif()

if(DOXYGEN_FOUND AND NOT MINIVERSE_BUILD_DOCS_CHOICE STREQUAL "OFF")
  set(DOXYGEN_PROJECT_BRIEF "layers of the world in PostGIS, loaded by polygon")
  set(DOXYGEN_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/docs)
  set(DOXYGEN_USE_MDFILE_AS_MAINPAGE ${CMAKE_CURRENT_SOURCE_DIR}/README.md)
  set(DOXYGEN_STRIP_FROM_PATH ${CMAKE_CURRENT_SOURCE_DIR}/include)
  set(DOXYGEN_STRIP_FROM_INC_PATH ${CMAKE_CURRENT_SOURCE_DIR}/include)
  set(DOXYGEN_EXTRACT_ALL NO)
  set(DOXYGEN_EXTRACT_PRIVATE NO)
  set(DOXYGEN_HIDE_UNDOC_MEMBERS YES)
  set(DOXYGEN_HIDE_UNDOC_CLASSES YES)
  set(DOXYGEN_EXCLUDE_SYMBOLS "detail" "*::detail" "*::detail::*" "schemacht" "schemacht::*")  # schemacht's own types are documented in schemacht
  set(DOXYGEN_MARKDOWN_ID_STYLE GITHUB)  # so that a link to a heading in docs/ resolves as it does on GitHub
  set(DOXYGEN_JAVADOC_AUTOBRIEF YES)
  set(DOXYGEN_GENERATE_LATEX NO)
  set(DOXYGEN_GENERATE_TREEVIEW YES)
  set(DOXYGEN_SHOW_INCLUDE_FILES NO)
  set(DOXYGEN_SORT_MEMBER_DOCS NO)
  set(DOXYGEN_WARN_IF_UNDOCUMENTED NO)
  set(DOXYGEN_WARN_AS_ERROR NO)
  set(DOXYGEN_QUIET YES)
  set(DOXYGEN_FILE_PATTERNS "*.hpp" "*.md")
  set(DOXYGEN_EXAMPLE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/examples)

  doxygen_add_docs(miniverse_docs ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/README.md ${CMAKE_CURRENT_SOURCE_DIR}/docs
                   COMMENT "The API reference, to ${CMAKE_BINARY_DIR}/docs/html")
endif()
message(STATUS "miniverse_docs target (MINIVERSE_BUILD_DOCS=${MINIVERSE_BUILD_DOCS}): ${DOXYGEN_FOUND}")

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
