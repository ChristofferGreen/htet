#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#define ARRANGEMENT_AWARE_CLEAVING_PROBE_TEST
#include "../../artifacts/dc-viability-2026-09-09/arrangement_aware_cleaving_probe.cpp"
#undef ARRANGEMENT_AWARE_CLEAVING_PROBE_TEST

TEST_CASE("finite DC triangle is stitched across its shared grid face without plane extension") {
  const auto result=run_arrangement_aware_cleaving_probe();
  CHECK(arrangement_aware_cleaving_probe_main()==0);
  CHECK(result.shared_cut_bit_identical);
  CHECK(result.owner_order_identical);
  CHECK(result.seam_owner_rule);
  CHECK(result.clipped_patch_control_rejected);
  CHECK(result.audit.frozen_triangle_exact);
  CHECK(result.audit.coplanar_extension_faces==0U);
  CHECK(result.audit.frozen_triangle_uses==2U);
  CHECK(result.audit.closed_boundary);
}

TEST_CASE("finite-triangle cross-face cavity is a valid quality-qualified tet complex") {
  const auto result=run_arrangement_aware_cleaving_probe();
  CHECK(result.geometry_valid);
  CHECK(result.audit.positive);
  CHECK(result.audit.unique);
  CHECK(result.audit.manifold);
  CHECK(result.audit.opposite_sides);
  CHECK(result.audit.no_overlap);
  CHECK(result.audit.volume_conserved);
  CHECK(result.audit.volume_error<1.0e-12);
  CHECK(result.quality_pass);
  CHECK(result.below_five==0U);
}
