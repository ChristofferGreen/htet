#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define BOUNDED_N6_JOINT_RETRIANGULATOR_TEST
#include "../../artifacts/dc-viability-2026-09-09/bounded_n6_joint_retriangulator_probe.cpp"
#undef BOUNDED_N6_JOINT_RETRIANGULATOR_TEST

TEST_CASE("N6 quotient-prism repair removes collapse but rejects an unjointed cavity") {
  CHECK(bounded_n6_joint_retriangulator_main()==0);
  const auto attempt=build_quotient_attempt(fixture_config("n6"),false);
  CHECK(attempt.frozen_dc_exact);
  CHECK(attempt.retained_core_exact);
  CHECK(attempt.collapsed_prisms==4U);
  CHECK(attempt.nonpositive==0U);
  CHECK(attempt.duplicate_tets==2U);
  CHECK(attempt.nonmanifold_faces>0U);
  CHECK(attempt.strict_overlaps==330U);
}
