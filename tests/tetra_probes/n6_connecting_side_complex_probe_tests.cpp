#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define N6_CONNECTING_SIDE_COMPLEX_TEST
#include "../../artifacts/dc-viability-2026-09-09/n6_connecting_side_complex_probe.cpp"
#undef N6_CONNECTING_SIDE_COMPLEX_TEST

TEST_CASE("N6 smallest explicit connecting side complex rejects invalid joined volume") {
  CHECK(n6_connecting_side_complex_main()==0);
  const auto forward=build_side_attempt(false);const auto reversed=build_side_attempt(true);
  CHECK(forward.side_is_canonical);CHECK(forward.side_faces==6U);CHECK(forward.side_tets==3U);
  CHECK(forward.boundary_closed);CHECK(forward.nonpositive==0U);CHECK(forward.duplicates==0U);
  CHECK(forward.nonmanifold_faces==0U);CHECK(forward.same_side==0U);
  CHECK((forward.overlaps>0U || forward.minimum_dihedral<5.0));
  CHECK(forward.boundary_components==reversed.boundary_components);CHECK(forward.overlaps==reversed.overlaps);
}
