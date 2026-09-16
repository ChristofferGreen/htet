#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define N6_WORST_QUALITY_CAVITY_TEST
#include "../../artifacts/dc-viability-2026-09-09/n6_worst_quality_cavity_probe.cpp"
#undef N6_WORST_QUALITY_CAVITY_TEST

TEST_CASE("N6 worst quality cavity is exact and reproducible") {
  CHECK(n6_worst_quality_cavity_main()==0);
  const auto candidate=build_candidate();
  CHECK(candidate.cavity_tets==1U);
  CHECK(candidate.prescribed_incident_faces<=4U);
  CHECK(audit(candidate.domain,candidate.shell_tets).geometry);
  CHECK_FALSE(audit(candidate.domain,candidate.shell_tets).quality);
}
