#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define N6_RIM_TO_CORE_CAVITY_TEST
#include "../../artifacts/dc-viability-2026-09-09/n6_rim_to_core_cavity_probe.cpp"
#undef N6_RIM_TO_CORE_CAVITY_TEST

TEST_CASE("N6 rim-to-core prism exposes the remaining open-rim obstruction") {
  CHECK(n6_rim_to_core_cavity_main() == 0);
  const auto forward = build_rim_core_attempt(false);
  const auto reversed = build_rim_core_attempt(true);
  CHECK(forward.visible_exact); CHECK(forward.fixture_exact); CHECK(forward.core_exact);
  CHECK(forward.rim_edges == 34U); CHECK(forward.core_tets == 96U); CHECK(forward.core_faces == 104U);
  CHECK(forward.bridge_tets == 3U); CHECK(forward.bridge_core_faces == 1U); CHECK(forward.bridge_rim_edges == 1U);
  CHECK(forward.nonpositive == 0U); CHECK(forward.boundary_invalid_edges > 0U);
  CHECK(forward.boundary_invalid_edges == reversed.boundary_invalid_edges);
}
