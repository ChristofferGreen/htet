#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#define TWO_FRONT_TRANSITION_PROBE_TEST
#include "../../artifacts/dc-viability-2026-09-09/two_front_transition_probe.cpp"
#undef TWO_FRONT_TRANSITION_PROBE_TEST

TEST_CASE("bounded two-front DC collar qualifies the canonical five-fixture surface corpus") {
  for(const char* fixture:{"n6","n8","n8-nearzero","n8-phase2","n8-phase3"})
    CHECK(two_front_transition_probe_main(fixture)==0);
}

TEST_CASE("fixed globally-derived collar depth joins independently generated chunks") {
  for(const char* fixture:{"n6","n8","n8-nearzero","n8-phase2","n8-phase3"}) {
    const auto result=run_canonical_chunk_collar_probe(fixture_config(fixture));
    CHECK(result.qualified);
    CHECK(result.source_partition_matches);
    CHECK(result.inner_front_faces_exact);
    CHECK(result.collar_emission_matches_monolithic);
    CHECK(result.shared_inner_vertices_identical);
    CHECK(result.seam_topology_identical);
    CHECK(result.reverse_request_order_identical);
    CHECK(result.joined_audit.valid());
    CHECK(result.joined_quality.passes);
  }
}

TEST_CASE("two-front collar rejects a collapsed zero-thickness front") {
  const auto config=fixture_config("n8");
  const auto surface=dual_contour_surface(config,0U,2U*config.resolution);
  const auto invalid=make_candidate(config,surface,0.0);
  CHECK_FALSE(invalid.audit.positive);
  CHECK_FALSE(invalid.audit.valid());
  CHECK_FALSE(invalid.accepted);
}
