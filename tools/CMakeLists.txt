# The command-line tool `miniverse`: makes, fills and checks the tables of a database, for the built-in kinds of layer. It
# reads vector and raster files when miniverse::gdal is built, and OpenStreetMap files when miniverse::osm is.

# CLI11, the command-line parser, as its release's single header (third_party/CLI11). It is a system header here: the
# warnings are about the tool.
add_library(miniverse_cli11 INTERFACE)
target_include_directories(miniverse_cli11 SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/third_party/CLI11)

# The commands, a library of their own, which the tool and its tests link.
add_library(miniverse_tool_commands STATIC create.cpp import.cpp remove.cpp verify.cpp settings.cpp datasets.cpp)
target_include_directories(miniverse_tool_commands PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(miniverse_tool_commands PUBLIC miniverse::miniverse)
if(TARGET miniverse_gdal)
  target_link_libraries(miniverse_tool_commands PUBLIC miniverse::gdal)
  target_compile_definitions(miniverse_tool_commands PRIVATE MINIVERSE_TOOL_GDAL=1)
endif()
if(TARGET miniverse_osm)
  target_link_libraries(miniverse_tool_commands PUBLIC miniverse::osm)
  target_compile_definitions(miniverse_tool_commands PRIVATE MINIVERSE_TOOL_OSM=1)
endif()
miniverse_checks(miniverse_tool_commands)

add_executable(miniverse_tool main.cpp)
set_target_properties(miniverse_tool PROPERTIES OUTPUT_NAME miniverse)
target_link_libraries(miniverse_tool PRIVATE miniverse_tool_commands miniverse_cli11)
miniverse_checks(miniverse_tool)

if(MINIVERSE_INSTALL)
  install(TARGETS miniverse_tool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  # CLI11's license asks that a binary made from it carry its notice.
  install(FILES ${PROJECT_SOURCE_DIR}/third_party/CLI11/LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR} RENAME CLI11-LICENSE)
endif()
