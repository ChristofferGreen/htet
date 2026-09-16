#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define BOUNDED_N6_AUTHORITATIVE_JOINT_TEST
#include "../../artifacts/dc-viability-2026-09-09/bounded_n6_authoritative_joint_probe.cpp"
#undef BOUNDED_N6_AUTHORITATIVE_JOINT_TEST

TEST_CASE("bounded N6 candidate uses the authoritative geometry and S4 gates") {
  CHECK(bounded_n6_authoritative_joint_main()==0);
  const auto forward=build_candidate(false), reversed=build_candidate(true);
  const auto result=audit(forward.domain,forward.shell_tets);
  CHECK(result.geometry);
  CHECK_FALSE(result.quality);
  CHECK(forward.canonical_hash==reversed.canonical_hash);
  CHECK(result.min_dihedral<diagnostic_min_tet_dihedral_degrees);
}
