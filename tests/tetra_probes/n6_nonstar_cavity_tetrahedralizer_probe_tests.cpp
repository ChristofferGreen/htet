#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define N6_NONSTAR_CAVITY_TETRAHEDRALIZER_TEST
#include "../../artifacts/dc-viability-2026-09-09/n6_nonstar_cavity_tetrahedralizer_probe.cpp"
#undef N6_NONSTAR_CAVITY_TETRAHEDRALIZER_TEST

TEST_CASE("N6 nonstar classifier retains a valid deterministic divider complex") {
  CHECK(n6_nonstar_cavity_tetrahedralizer_main()==0);
  const auto candidate=build_nonstar_candidate();
  const auto shell=candidate.domain.tets.size()-96U;
  CHECK(candidate.regions>0U);
  CHECK(candidate.nonstar_regions>0U);
  CHECK(candidate.star_regions+candidate.nonstar_regions==candidate.regions);
  CHECK(audit(candidate.domain,shell).geometry);
  CHECK(exact_interface(candidate.domain));
  REQUIRE(candidate.region_reports.size()>8U);
  const auto& region4=candidate.region_reports[4];
  CHECK(region4.fill=="quality_aware_bistellar_dividers");
  CHECK(region4.output_tets==9U);
  CHECK(region4.flips==4U);
  CHECK(region4.minimum>15.19);
  CHECK(region4.below_five==0U);
  // Region 8 is deliberately not promoted: its unconstrained 2->3 trial
  // creates overlaps, so the valid elementary-move family leaves its old
  // divider in place for the next, higher-order cavity experiment.
  const auto& region8=candidate.region_reports[8];
  CHECK(region8.fill=="canonical_existing_dividers");
  CHECK(region8.below_five==1U);
}
