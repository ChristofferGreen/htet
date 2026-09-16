#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define N6_DISJOINT_BUFFER_CORE_TEST
#include "../../artifacts/dc-viability-2026-09-09/n6_disjoint_buffer_core_probe.cpp"
#undef N6_DISJOINT_BUFFER_CORE_TEST

TEST_CASE("N6 shared buffer is ineligible as a whole-front core bridge") {
  CHECK(n6_disjoint_buffer_core_main()==0);
  const auto forward=build_disjoint_front_attempt(false);
  const auto reversed=build_disjoint_front_attempt(true);
  CHECK(forward.core_tets==96U);
  CHECK(forward.buffer_tets==14U);
  CHECK(forward.core_boundary_faces==104U);
  CHECK(forward.buffer_boundary_faces==14U);
  CHECK(forward.core_positive);
  CHECK(forward.buffer_positive);
  CHECK(forward.shared_boundary_faces==0U);
  CHECK(forward.strict_cross_component_overlaps==0U);
  CHECK(forward.strict_cross_component_overlaps==reversed.strict_cross_component_overlaps);
}
