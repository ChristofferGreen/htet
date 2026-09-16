#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_probes/exact_binary_predicates.hpp"

#include <limits>

TEST_CASE("exact binary predicates retain signs below long-double tolerance") {
  using namespace tetra;
  using namespace tetra::probes;
  const auto tiny=std::numeric_limits<double>::denorm_min();
  CHECK(exact_orientation_3d({0,0,0},{1,0,0},{0,1,0},{0,0,tiny})==ExactPredicateSign::positive);
  CHECK(exact_orientation_3d({0,0,0},{1,0,0},{0,1,0},{0,0,-tiny})==ExactPredicateSign::negative);
  CHECK(exact_orientation_3d({0,0,0},{1,0,0},{0,1,0},{1,1,0})==ExactPredicateSign::zero);
}

TEST_CASE("exact binary in-sphere distinguishes interior exterior and cospherical inputs") {
  using namespace tetra;
  using namespace tetra::probes;
  const Vec3 a{0,0,0},b{1,0,0},c{0,1,0},d{0,0,1};
  CHECK(exact_in_sphere(a,b,c,d,{.5,.5,.5})==ExactPredicateSign::negative);
  CHECK(exact_in_sphere(a,b,c,d,{2,2,2})==ExactPredicateSign::positive);
  CHECK(exact_in_sphere(a,b,c,d,{1,1,1})==ExactPredicateSign::zero);
}
