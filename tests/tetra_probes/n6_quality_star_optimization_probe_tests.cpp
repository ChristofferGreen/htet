#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define N6_QUALITY_STAR_OPTIMIZATION_TEST
#include "../../artifacts/dc-viability-2026-09-09/n6_quality_star_optimization_probe.cpp"
#undef N6_QUALITY_STAR_OPTIMIZATION_TEST

TEST_CASE("N6 local quality-star candidates retain the complete-domain contract") {
  CHECK(n6_quality_star_optimization_main()==0);
  for(int family=0;family<8;++family) {
    const auto candidate=flip_candidate(family);
    CHECK(candidate.declared);
    CHECK(audit(candidate.domain,candidate.shell).geometry);
    CHECK_FALSE(audit(candidate.domain,candidate.shell).quality);
  }
}
