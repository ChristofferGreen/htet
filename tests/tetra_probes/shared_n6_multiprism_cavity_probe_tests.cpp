#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define SHARED_N6_MULTIPRISM_CAVITY_TEST
#include "../../artifacts/dc-viability-2026-09-09/shared_n6_multiprism_cavity_probe.cpp"
#undef SHARED_N6_MULTIPRISM_CAVITY_TEST

TEST_CASE("N6 shared multi-prism cavity is locally valid but rejects the unconnected core") {
  CHECK(shared_n6_multiprism_cavity_main()==0);
  const auto attempt=build_shared_cavity_attempt(fixture_config("n6"),false);
  CHECK(attempt.frozen_dc_exact);
  CHECK(attempt.retained_core_exact);
  CHECK(attempt.collapsed_prisms==4U);
  CHECK(attempt.source_quotient_tets==12U);
  CHECK(attempt.cavity_boundary_closed);
  CHECK(attempt.nonpositive_tets==0U);
  CHECK(attempt.duplicate_tets==0U);
  CHECK(attempt.nonmanifold_faces==0U);
  CHECK(attempt.same_side_faces==0U);
  CHECK(attempt.strict_overlaps==0U);
  CHECK_FALSE(attempt.local_s4);
  CHECK(attempt.unmatched_retained_core_faces==104U);
  CHECK(attempt.unmatched_bridge_lower_faces==10U);
  const auto reversed=build_shared_cavity_attempt(fixture_config("n6"),true);
  CHECK(attempt.cavity_boundary_faces==reversed.cavity_boundary_faces);
  CHECK(attempt.boundary_nonmanifold_edges==reversed.boundary_nonmanifold_edges);
}
