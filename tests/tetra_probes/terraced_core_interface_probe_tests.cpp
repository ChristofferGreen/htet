#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#define TERRACED_CORE_INTERFACE_PROBE_TEST
#include "../../artifacts/dc-viability-2026-09-09/terraced_core_interface_probe.cpp"
#undef TERRACED_CORE_INTERFACE_PROBE_TEST

TEST_CASE("actual exposed Freudenthal-core faces retain terraces but reject a zero-buffer collar attachment") {
  for(const char* fixture:{"n6","n8","n8-nearzero","n8-phase2","n8-phase3"})
    CHECK(terraced_core_interface_probe_main(fixture)==0);
}

TEST_CASE("terraced interface retains distinct lattice layers rather than hiding the old flat ij collapse") {
  const auto result=run_terraced_probe(bridge_fixture("n8"));
  CHECK(result.collar_qualified);
  CHECK(result.core.exact_lattice_positions);
  CHECK(result.core.tetrahedra.size()>0U);
  CHECK(result.core.exposed_faces.size()>0U);
  CHECK(result.core.vertical_exposed_faces>0U);
  CHECK(result.core.flattened_face_collapses>0U);
  CHECK(result.core.distinct_xy_columns_with_layers>0U);
  CHECK(result.flat_control_rejected);
  CHECK(result.direct.directly_paired_faces==0U);
  CHECK(result.direct.rejected_for_unfilled_transition);
  CHECK(result.direct.input_order_deterministic);
  CHECK(result.local_source_bounded);
}
