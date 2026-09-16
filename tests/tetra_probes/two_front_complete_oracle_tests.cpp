#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// TetGen is supplied only to the reproduction runner.  Keep its deterministic
// input contract in the focused suite: a fixed collar and exact retained core.
#define main two_front_complete_exporter_main
#include "../../artifacts/dc-viability-2026-09-09/export_two_front_complete.cpp"
#undef main

namespace {
using namespace tetra::probes;

SandwichConfig fixture(const char* name) {
  SandwichConfig config;config.resolution=std::string_view{name}=="n6"?6U:8U;
  if(std::string_view{name}=="nearzero")config.phase_x=config.phase_y=0.0001;
  if(std::string_view{name}=="phase2"){config.phase_x=0.5;config.phase_y=0.0001;}
  if(std::string_view{name}=="phase3"){config.phase_x=0.73;config.phase_y=0.91;}
  return config;
}
} // namespace

TEST_CASE("complete external oracle inputs retain a fixed healthy collar and exact core") {
  for(const auto name:{"n6","n8","nearzero","phase2","phase3"}) {
    const auto config=fixture(name);
    const auto collar=run_probe(config);
    CHECK(collar.selected.accepted);
    CHECK(collar.deterministic);
    const auto core=conservative_core(config);
    CHECK_FALSE(core.empty());
    for(const auto& tet:core) for(const auto id:tet) {
      const auto p=cartesian_lattice_position({KeyKind::lattice,id},config.resolution);
      CHECK(std::isfinite(p.x));
      CHECK(std::isfinite(p.y));
      CHECK(std::isfinite(p.z));
    }
  }
}
