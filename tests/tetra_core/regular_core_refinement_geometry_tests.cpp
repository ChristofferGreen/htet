#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "tetra_core/regular_core_refinement_geometry.hpp"
#include <algorithm>
#include <map>
#include <limits>
#include <set>

namespace { using namespace tetra;
std::vector<RegularCoreParent> topo(bool reverse=false) {
  RegularCoreParent a{11},b{29};
  a.face_vertices={{{2,3,4},{1,3,4},{1,2,4},{1,2,3}}};
  b.face_vertices={{{1,2,5},{1,3,5},{2,3,5},{3,2,1}}};
  a.neighbors[3]=RegularCoreNeighbor{{29,3},{2,1,0}}; b.neighbors[3]=RegularCoreNeighbor{{11,3},{2,1,0}};
  std::vector<RegularCoreParent> r{a,b};if(reverse)std::reverse(r.begin(),r.end());return r;
}
RegularCoreGeometryDescriptor geometry(bool reverse=false) {
  RegularCoreGeometryDescriptor d;
  d.vertices={{1,{0,0,0}},{2,{1,0,0}},{3,{.5,.8660254037844386,0}},{4,{.5,.2886751345948129,.816496580927726}},{5,{.5,.2886751345948129,-.816496580927726}}};
  d.parents={{11,{{1,2,3,4}}},{29,{{3,2,1,5}}}};if(reverse)std::reverse(d.parents.begin(),d.parents.end());return d;
}
RegularCoreRefinementLimits lim(){return {1,2,16,1};}
}
TEST_CASE("geometry materialization shares canonical red midpoints across rotated seam") {
  const auto cut=refine_regular_core(topo(),{1,{11,3}},lim()); REQUIRE(cut.accepted());
  const auto a=materialize_regular_core_red(topo(),cut,geometry());
  const auto opposite=materialize_regular_core_red(topo(true),refine_regular_core(topo(true),{1,{29,3}},lim()),geometry(true));
  REQUIRE(a.accepted()); REQUIRE(opposite.accepted());
  CHECK(a.children.size()==16U); CHECK(a.vertices.size()==14U); CHECK(a.minimum_dihedral_degrees>=5.0);
  CHECK(a.maximum_dihedral_degrees<=175.0);
  CHECK(a.vertices==opposite.vertices); CHECK(a.children==opposite.children);
  // Six seam-edge midpoints are globally deduplicated: each id appears in
  // children belonging to both parents, proving the common child faces agree.
  std::size_t shared_midpoints{};for(const auto& v:a.vertices)if(v.id>= (std::uint64_t{1}<<63U)){bool left=false,right=false;for(const auto& t:a.children)if(std::find(t.vertices.begin(),t.vertices.end(),v.id)!=t.vertices.end()){left|=t.address.parent==11;right|=t.address.parent==29;}if(left&&right)++shared_midpoints;}
  CHECK(shared_midpoints==3U);
  std::map<std::array<std::uint64_t,3>,std::set<RegularCoreParentId>> face_owners;
  constexpr std::array<std::array<int,3>,4> faces{{{{1,2,3}},{{0,3,2}},{{0,1,3}},{{0,2,1}}}};
  for(const auto& t:a.children)for(const auto& local:faces) { auto f=std::array<std::uint64_t,3>{{t.vertices[static_cast<std::size_t>(local[0])],t.vertices[static_cast<std::size_t>(local[1])],t.vertices[static_cast<std::size_t>(local[2])]}};std::sort(f.begin(),f.end());face_owners[f].insert(t.address.parent); }
  std::size_t common_child_faces{};for(const auto& [face,owners]:face_owners)if(owners.size()==2U)++common_child_faces;
  CHECK(common_child_faces==4U);
}
TEST_CASE("geometry materialization refuses malformed stable identity transactionally") {
  const auto cut=refine_regular_core(topo(),{1,{11,3}},lim());
  auto expect=[](const auto& r,RegularCoreMaterializationRefusal why){CHECK(r.refusal==why);CHECK(r.vertices.empty());CHECK(r.children.empty());};
  auto bad=geometry();bad.parents[1].vertices[0]=77;
  expect(materialize_regular_core_red(topo(),cut,bad),RegularCoreMaterializationRefusal::stable_id_mismatch);
  auto duplicate=geometry();duplicate.vertices.push_back(duplicate.vertices.front());
  expect(materialize_regular_core_red(topo(),cut,duplicate),RegularCoreMaterializationRefusal::malformed_geometry);
  auto nonfinite=geometry();nonfinite.vertices[0].point.x=std::numeric_limits<double>::infinity();
  expect(materialize_regular_core_red(topo(),cut,nonfinite),RegularCoreMaterializationRefusal::malformed_geometry);
}
