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
  set(SCHEMACHT_INSTALL ${MINIVERSE_INSTALL} CACHE BOOL "" FORCE)
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
  src/geo/wkb.cpp
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
# Tests
# ==============================================================================
if(MINIVERSE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ==============================================================================
# Install
# ==============================================================================
# `cmake --install <build> --prefix <dir>` puts under <dir>:
#   lib/libminiverse.a        the library
#   include/miniverse/...     its headers
#   lib/cmake/miniverse/      the package: find_package(miniverse) gives miniverse::miniverse
if(MINIVERSE_INSTALL)
  set(MINIVERSE_INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/miniverse)

  install(TARGETS miniverse EXPORT miniverse-targets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(EXPORT miniverse-targets NAMESPACE miniverse:: DESTINATION ${MINIVERSE_INSTALL_CMAKEDIR})
  install(DIRECTORY include/miniverse DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.hpp")

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
