#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define N6_FULL_RIM_CYCLE_CAVITY_TEST
#include "../../artifacts/dc-viability-2026-09-09/n6_full_rim_cycle_cavity_probe.cpp"
#undef N6_FULL_RIM_CYCLE_CAVITY_TEST

TEST_CASE("N6 complete rebuildable rim rejects an unmatched exact-core patch") {
  CHECK(n6_full_rim_cycle_cavity_main() == 0);
  const auto forward = build_full_rim_attempt(false);
  const auto reversed = build_full_rim_attempt(true);
  CHECK(forward.rim_edges == 34U);
  CHECK(forward.rim_cycles == 1U);
  CHECK(forward.largest_cycle_edges == forward.rim_edges);
  CHECK(forward.collar_neighbourhood_tets > 0U);
  CHECK(forward.collar_neighbourhood_inner_faces > 0U);
  CHECK(forward.core_faces == 104U);
  CHECK(forward.exact_core_patch_connected);
  CHECK(forward.core_patch_cycles > 1U);
  CHECK_FALSE(forward.loops_compatible);
  CHECK(forward.emitted_tets == 0U);
  CHECK(forward.core_patch_boundary_edges == reversed.core_patch_boundary_edges);
}
