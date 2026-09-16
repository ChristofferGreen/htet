#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define N6_FROZEN_BOUNDARY_S4_PREFLIGHT_TEST
#include "../../artifacts/dc-viability-2026-09-09/n6_frozen_boundary_s4_preflight_probe.cpp"
#undef N6_FROZEN_BOUNDARY_S4_PREFLIGHT_TEST

TEST_CASE("an interior face fan preserves unsplit boundary facets while partitioning their wedge") {
  CHECK(n6_frozen_boundary_s4_preflight_main() == 0);
  const auto control=boundary_edge_fan_control();
  const Face first=key({{0U,1U,2U}}), second=key({{0U,1U,3U}}), divider=key({{0U,1U,4U}});
  CHECK(audit(control,2U).geometry);
  CHECK(face_use_count(control,first) == 1U);
  CHECK(face_use_count(control,second) == 1U);
  CHECK(face_use_count(control,divider) == 2U);
  CHECK(interior_wedge_degrees(control,oriented_owner_face(control,first,0U),oriented_owner_face(control,divider,0U)) > 5.0);
  CHECK(interior_wedge_degrees(control,oriented_owner_face(control,divider,1U),oriented_owner_face(control,second,1U)) > 5.0);
}
