// Each block must fail to compile, with the message tests/CMakeLists.txt expects for it. Compiled one case at a time.

#include <cstdint>
#include <string>
#include <tuple>

#include "miniverse/miniverse.hpp"

struct Roads : miniverse::RoadLayer {};
struct Tracks : miniverse::RoadLayer {};
struct Elevation : miniverse::RasterLayer<std::int16_t> {};

#if defined(CASE_LOAD_LAYER_NOT_HELD)
void load_tracks(miniverse::Miniverse<Roads>& world) { std::ignore = world.load<Tracks>(miniverse::geo::Polygon()); }
#endif

#if defined(CASE_STREAM_LAYER_NOT_HELD)
void stream_tracks(miniverse::Miniverse<Roads>& world) {
  std::ignore = world.stream<Tracks>(miniverse::geo::Polygon(), [](const miniverse::Ways& /*chunk*/) {});
}
#endif

#if defined(CASE_PUSH_LAYER_NOT_HELD)
void push_tracks(miniverse::Miniverse<Roads>& world) { std::ignore = world.push<Tracks>(miniverse::Ways()); }
#endif

#if defined(CASE_CREATE_TABLES_NEED_SETTINGS)
void make(miniverse::Miniverse<Elevation>& world) { world.create_tables(); }
#endif

#if defined(CASE_DUPLICATE_KIND)
void open(const std::string& conninfo) {
  miniverse::Miniverse<Roads, Roads> world(conninfo, miniverse::Layer<Roads>("roads"), miniverse::Layer<Roads>("more_roads"));
}
#endif
