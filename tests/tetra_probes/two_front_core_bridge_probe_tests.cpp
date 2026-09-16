#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#define TWO_FRONT_CORE_BRIDGE_PROBE_TEST
#include "../../artifacts/dc-viability-2026-09-09/two_front_core_bridge_probe.cpp"
#undef TWO_FRONT_CORE_BRIDGE_PROBE_TEST

TEST_CASE("fixed regular core interface rejects every canonical noisy two-front collar") {
  for(const char* fixture:{"n6","n8","n8-nearzero","n8-phase2","n8-phase3"})
    CHECK(two_front_core_bridge_probe_main(fixture)==0);
}

TEST_CASE("fixed core interface does not hide the failure by duplicating a regular node") {
  const auto result=run_bridge(bridge_fixture("n6"));
  CHECK(result.collar_qualified);
  CHECK(result.exact_regular_positions);
  CHECK(result.bridge.frozen_faces_exact);
  CHECK(result.bridge.collapsed_grid_triangles>0U);
  CHECK(result.bridge.degenerate_bridge_tetrahedra>0U);
  CHECK(result.bridge.duplicate_tetrahedra>0U);
  CHECK(result.rejected_for_topological_collapse);
  CHECK(result.zero_thickness_control_rejected);
  CHECK(result.boundary_cavity_control_rejected);
  CHECK(result.reversed_input_deterministic);
  CHECK(result.remote_growth_bounded);
}
