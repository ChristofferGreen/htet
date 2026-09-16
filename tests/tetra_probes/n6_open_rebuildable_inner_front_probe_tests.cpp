#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define N6_OPEN_REBUILDABLE_INNER_FRONT_TEST
#include "../../artifacts/dc-viability-2026-09-09/n6_open_rebuildable_inner_front_probe.cpp"
#undef N6_OPEN_REBUILDABLE_INNER_FRONT_TEST

TEST_CASE("N6 rebuildable collar underside derives one open joint front") {
  CHECK(n6_open_rebuildable_inner_front_main() == 0);
  const auto forward = build_open_front_attempt(false);
  const auto reverse = build_open_front_attempt(true);
  CHECK(forward.visible_exact);
  CHECK(forward.fixture_exterior_exact);
  CHECK(forward.artificial_inner_excluded);
  CHECK(forward.open_single_component);
  CHECK(forward.rim_edges > 0U);
  CHECK(forward.core_exact);
  CHECK(forward.core_tets == 96U);
  CHECK(forward.core_boundary_faces == 104U);
  CHECK(forward.literal_front_core_faces == 0U);
  CHECK(forward.collar_core_strict_overlaps == 0U);
  CHECK(forward.rim_edges == reverse.rim_edges);
}
