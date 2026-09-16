#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "tetra_core/bounded_front_buffer.hpp"

using namespace tetra;
TEST_CASE("bounded front buffer preserves arbitrary outer sheet and has conforming shared sides") {
  const std::vector<BoundedBufferPoint> outer{{0,0,1},{1,0,1.1},{1,1,.9},{0,1,1.2}};
  const std::vector<BoundedBufferPoint> inner{{0,0,0},{1,0,0},{1,1,0},{0,1,0}};
  const auto buffer=build_bounded_front_buffer(outer,inner,{{{0,1,2}},{{0,2,3}}});
  REQUIRE(buffer.accepted()); CHECK(buffer.vertices.size()==8U); CHECK(buffer.tetrahedra.size()==6U);
  CHECK(buffer.minimum_mean_ratio>0.01); CHECK(buffer.maximum_edge_ratio<20.0);
  CHECK(buffer.minimum_dihedral_degrees>0.0); CHECK(buffer.maximum_dihedral_degrees<180.0);
  auto reversed=build_bounded_front_buffer(outer,inner,{{{3,2,0}},{{2,1,0}}});
  REQUIRE(reversed.accepted()); CHECK(reversed.tetrahedra==buffer.tetrahedra);
}
TEST_CASE("bounded front buffer rejects coincident material even with distinct IDs") {
  const std::vector<BoundedBufferPoint> outer{{0,0,1},{1,0,1},{0,1,1},{0,0,1},{1,0,1},{0,1,1}};
  auto inner=outer; for(auto& point:inner) point.z=0;
  const auto result=build_bounded_front_buffer(outer,inner,{{{0,1,2}},{{3,4,5}}});
  CHECK(result.refusal==BoundedFrontBufferRefusal::overlapping_tetrahedra);
  CHECK(result.vertices.empty()); CHECK(result.tetrahedra.empty());
}
TEST_CASE("bounded front buffer refuses degenerate or malformed fronts transactionally") {
  const std::vector<BoundedBufferPoint> points{{0,0,0},{1,0,0},{0,1,0}};
  const auto flat=build_bounded_front_buffer(points,points,{{{0,1,2}}});
  CHECK(flat.refusal==BoundedFrontBufferRefusal::nonpositive_tetrahedron); CHECK(flat.vertices.empty());
  CHECK(build_bounded_front_buffer(points,points,{{{0,0,2}}}).refusal==BoundedFrontBufferRefusal::bad_triangle);
}
