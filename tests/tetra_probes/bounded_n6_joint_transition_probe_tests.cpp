#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define BOUNDED_N6_JOINT_TRANSITION_TEST
#include "../../artifacts/dc-viability-2026-09-09/bounded_n6_joint_transition_probe.cpp"
#undef BOUNDED_N6_JOINT_TRANSITION_TEST

TEST_CASE("complete N6 direct joint assembly rejects its smallest collapsed bridge") {
  CHECK(bounded_n6_joint_transition_main()==0);
  const auto attempt=build_attempt(fixture_config("n6"),false);
  CHECK(attempt.frozen_dc_exact);
  CHECK(attempt.retained_core_exact);
  CHECK(attempt.collapsed_triangles>0U);
  CHECK(attempt.nonpositive_tets>0U);
  CHECK(attempt.duplicate_tets==0U);
}
