#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_core/regular_core_refinement.hpp"

#include <algorithm>

namespace {
using namespace tetra;
std::vector<RegularCoreParent> adjacent_pair(bool reverse=false) {
  RegularCoreParent a{11U},b{29U};
  a.face_vertices[0]={1U,2U,3U}; a.face_vertices[1]={1U,3U,4U};
  a.face_vertices[2]={100U,101U,102U};
  a.face_vertices[3]={2U,3U,4U};
  b.face_vertices[0]={5U,6U,7U};
  b.face_vertices[1]={102U,100U,101U}; // deliberately rotated local order
  b.face_vertices[2]={5U,7U,8U}; b.face_vertices[3]={6U,7U,8U};
  a.neighbors[2]=RegularCoreNeighbor{{29U,1U},{1U,2U,0U}};
  b.neighbors[1]=RegularCoreNeighbor{{11U,2U},{2U,0U,1U}};
  std::vector<RegularCoreParent> r{a,b}; if(reverse)std::reverse(r.begin(),r.end()); return r;
}
RegularCoreRefinementLimits limits() { return {regular_core_red_grammar_version,2U,16U,1U}; }
}

TEST_CASE("regular core red closure is deterministic and conforming across a parent face") {
  const RegularCoreFaceSplitRequest request{regular_core_red_grammar_version,{11U,2U}};
  const auto forward=refine_regular_core(adjacent_pair(),request,limits());
  const auto reverse=refine_regular_core(adjacent_pair(true),request,limits());
  const auto opposite_request=refine_regular_core(adjacent_pair(true),{regular_core_red_grammar_version,{29U,1U}},limits());
  REQUIRE(forward.accepted()); REQUIRE(reverse.accepted()); REQUIRE(opposite_request.accepted());
  CHECK(forward.active_leaves==reverse.active_leaves);
  CHECK(forward.interface_subfaces==reverse.interface_subfaces);
  CHECK(forward.active_leaves==opposite_request.active_leaves);
  CHECK(forward.interface_subfaces==opposite_request.interface_subfaces);
  CHECK(forward.active_leaves.size()==16U);
  CHECK(std::all_of(forward.active_leaves.begin(),forward.active_leaves.end(),[](const auto& leaf){return leaf.grammar_version==regular_core_red_grammar_version;}));
  // Exactly one shared parent face appears, with all four canonical red subfaces.
  std::size_t shared{}; std::size_t boundary{};
  for(const auto& f:forward.interface_subfaces) if(f.second) {
    ++shared; CHECK(f.first==RegularCoreParentFaceId{11U,2U}); CHECK(*f.second==RegularCoreParentFaceId{29U,1U});
    CHECK(f.physical_face_vertices==std::array<std::uint64_t,3>{100U,101U,102U});
    CHECK(f.kind==RegularCoreInterfaceKind::internal_shared);
  } else {
    ++boundary; CHECK(f.kind==RegularCoreInterfaceKind::external_core_boundary);
    CHECK(f.physical_face_vertices[0]<f.physical_face_vertices[1]);
    CHECK(f.physical_face_vertices[1]<f.physical_face_vertices[2]);
  }
  CHECK(shared==4U);
  CHECK(boundary==24U);
}

TEST_CASE("regular core refinement refuses transactionally before emitting leaves") {
  const RegularCoreFaceSplitRequest request{regular_core_red_grammar_version,{11U,2U}};
  auto expect_empty=[](const auto& r,RegularCoreRefinementRefusal why) { CHECK(r.refusal==why); CHECK(r.active_leaves.empty()); CHECK(r.interface_subfaces.empty()); };
  auto bad_version=request; bad_version.grammar_version=7U;
  expect_empty(refine_regular_core(adjacent_pair(),bad_version,limits()),RegularCoreRefinementRefusal::bad_grammar_version);
  auto unsupported=request; unsupported.pattern=static_cast<RegularCoreSplitPattern>(99U);
  expect_empty(refine_regular_core(adjacent_pair(),unsupported,limits()),RegularCoreRefinementRefusal::unsupported_pattern);
  auto bad_face=request; bad_face.face={99U,0U};
  expect_empty(refine_regular_core(adjacent_pair(),bad_face,limits()),RegularCoreRefinementRefusal::missing_requested_face);
  auto shallow=limits(); shallow.maximum_depth=0U;
  expect_empty(refine_regular_core(adjacent_pair(),request,shallow),RegularCoreRefinementRefusal::depth_limit);
  auto small=limits(); small.maximum_halo_parents=1U;
  expect_empty(refine_regular_core(adjacent_pair(),request,small),RegularCoreRefinementRefusal::resource_limit);
  auto few_leaves=limits(); few_leaves.maximum_active_leaves=15U;
  expect_empty(refine_regular_core(adjacent_pair(),request,few_leaves),RegularCoreRefinementRefusal::resource_limit);
  auto missing=adjacent_pair(); missing.pop_back();
  expect_empty(refine_regular_core(missing,request,limits()),RegularCoreRefinementRefusal::insufficient_halo);
  auto malformed=adjacent_pair(); malformed[1].neighbors[1].reset();
  expect_empty(refine_regular_core(malformed,request,limits()),RegularCoreRefinementRefusal::malformed_adjacency);
  auto bad_permutation=adjacent_pair(); bad_permutation[0].neighbors[2]->vertex_permutation={0U,1U,2U};
  expect_empty(refine_regular_core(bad_permutation,request,limits()),RegularCoreRefinementRefusal::malformed_adjacency);
  auto duplicate_external=adjacent_pair(); duplicate_external[0].face_vertices[0]={1U,1U,2U};
  expect_empty(refine_regular_core(duplicate_external,request,limits()),RegularCoreRefinementRefusal::malformed_adjacency);
  auto repeated_external=adjacent_pair(); repeated_external[0].face_vertices[0]=repeated_external[0].face_vertices[1];
  expect_empty(refine_regular_core(repeated_external,request,limits()),RegularCoreRefinementRefusal::malformed_adjacency);
}

TEST_CASE("regular core local halo is one ring, deterministic, and transactional") {
  auto parents=adjacent_pair();
  const auto forward=select_regular_core_local_halo(parents,{11U},{2U});
  std::reverse(parents.begin(),parents.end());
  const auto reverse=select_regular_core_local_halo(parents,{11U},{2U});
  REQUIRE(forward.accepted()); REQUIRE(reverse.accepted());
  CHECK(forward.parents==std::vector<RegularCoreParentId>{11U});
  CHECK(forward.halo==std::vector<RegularCoreParentId>{29U});
  CHECK(forward.parents==reverse.parents); CHECK(forward.halo==reverse.halo);
  CHECK(select_regular_core_local_halo(adjacent_pair(),{11U},{1U}).refusal==RegularCoreLocalHaloRefusal::resource_limit);
  CHECK(select_regular_core_local_halo(adjacent_pair(),{99U},{2U}).refusal==RegularCoreLocalHaloRefusal::missing_selected_parent);
  auto broken=adjacent_pair(); broken[1].neighbors[1].reset();
  CHECK(select_regular_core_local_halo(broken,{11U},{2U}).refusal==RegularCoreLocalHaloRefusal::malformed_adjacency);
}
