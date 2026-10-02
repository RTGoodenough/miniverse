// Each block must fail to compile, with the message tests/CMakeLists.txt expects for it. Compiled one case at a time.

#include <string>

#include "miniverse/miniverse.hpp"

struct Roads : miniverse::RoadLayer<"roads"> {};
struct Tracks : miniverse::RoadLayer<"tracks"> {};

#if defined(CASE_LOAD_LAYER_NOT_HELD)
void load_tracks(miniverse::Miniverse<Roads>& world) { std::ignore = world.load<Tracks>(miniverse::geo::Polygon()); }
#endif

#if defined(CASE_PUSH_LAYER_NOT_HELD)
void push_tracks(miniverse::Miniverse<Roads>& world) { std::ignore = world.push<Tracks>(miniverse::Ways()); }
#endif

#if defined(CASE_SAME_TABLE)
struct MoreRoads : miniverse::RoadLayer<"roads"> {};
void open(const std::string& conninfo) { miniverse::Miniverse<Roads, MoreRoads> world(conninfo); }
#endif

#if defined(CASE_NOT_A_LAYER_KIND)
struct NotAKind {
  using result_type = int;
};
void open(const std::string& conninfo) { miniverse::Miniverse<Roads, NotAKind> world(conninfo); }
#endif
