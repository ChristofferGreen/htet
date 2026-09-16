#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#define SCAFFOLDED_INTERFRONT_BUFFER_PROBE_TEST
#include "../../artifacts/dc-viability-2026-09-09/scaffolded_interfront_buffer_probe.cpp"
#undef SCAFFOLDED_INTERFRONT_BUFFER_PROBE_TEST

TEST_CASE("unaltered bounded regular-tet scaffold rings cannot pair to the qualified free collar front") {
  for(const char* fixture:{"n6","n8","n8-nearzero","n8-phase2","n8-phase3"})
    CHECK(scaffolded_interfront_buffer_probe_main(fixture)==0);
}

TEST_CASE("scaffold rejection preserves the actual core and cannot disguise an empty bridge as closed") {
  const auto result=run_scaffolded_buffer(bridge_fixture("n8"));
  CHECK(result.collar_qualified);
  CHECK(result.core_exact);
  CHECK(result.flat_control_rejected);
  CHECK(result.direct_control_rejected);
  CHECK(result.local_source_bounded);
  CHECK(result.deterministic);
  for(const auto& ring:result.rings) {
    CHECK(ring.tetrahedra.size()>0U);
    CHECK(ring.boundary_faces.size()>0U);
    CHECK(ring.interface_faces>0U);
    CHECK(ring.all_positive);
    CHECK(ring.nonmanifold_faces==0U);
    CHECK(ring.directly_matching_collar_faces==0U);
  }
  CHECK(result.rejected_for_nonconforming_front);
}
