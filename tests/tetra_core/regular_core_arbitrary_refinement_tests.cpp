#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_core/regular_core_arbitrary_refinement.hpp"

#include <algorithm>

namespace {
using namespace tetra;
RegularCoreArbitraryEdgeSplitRequest split(std::uint64_t edge, std::uint32_t n, std::uint32_t d) {
  return {{edge}, {n, d}};
}
RegularCoreArbitraryFaceTopology parent_face(std::uint64_t parent, std::uint8_t omitted,
                                             std::uint64_t split_id=0U,
                                             std::array<std::uint64_t,4> root={{1,2,3,4}}) {
  std::array<std::uint64_t,3> corners{}; std::size_t cursor{};
  for(std::size_t i=0;i<root.size();++i) if(i!=omitted) corners[cursor++]=root[i];
  std::array<RegularCoreArbitraryFaceEdge,3> edges{}; cursor=0;
  for(std::size_t i=0;i<3U;++i) for(std::size_t j=i+1U;j<3U;++j) {
    auto a=corners[i],b=corners[j];if(b<a)std::swap(a,b);
    edges[cursor++]=RegularCoreArbitraryFaceEdge{{a==1U&&b==2U?10U:100U+a*10U+b},{a,b}};
  }
  RegularCoreArbitraryFaceTopology result{{parent,omitted},corners,edges,{}};
  const bool contains_split=std::find(corners.begin(),corners.end(),1U)!=corners.end()&&std::find(corners.begin(),corners.end(),2U)!=corners.end();
  if(!contains_split) result.triangles.push_back(corners);
  else { const auto other=*std::find_if(corners.begin(),corners.end(),[](auto id){return id!=1U&&id!=2U;}); result.triangles={{{1U,split_id,other}},{{split_id,2U,other}}}; }
  return result;
}
}

TEST_CASE("arbitrary core-edge split ledger is exact, stable, and reorder independent") {
  std::vector requests{split(91, 1, 3), split(17, 7, 13), split(91, 2, 3)};
  const auto forward = plan_regular_core_arbitrary_edge_splits(requests, {3});
  std::reverse(requests.begin(), requests.end());
  const auto reverse = plan_regular_core_arbitrary_edge_splits(requests, {3});
  REQUIRE(forward.accepted());
  REQUIRE(reverse.accepted());
  CHECK(forward.vertices == reverse.vertices);
  CHECK(forward.vertices[0].edge == RegularCoreEdgeId{17});
  CHECK(forward.vertices[0].parameter == RegularCoreRational{7, 13});
  CHECK(forward.vertices[1].edge == RegularCoreEdgeId{91});
  CHECK(forward.vertices[1].parameter == RegularCoreRational{1, 3});
  CHECK(forward.vertices[2].parameter == RegularCoreRational{2, 3});
}

TEST_CASE("arbitrary split IDs and rational ordering survive unrelated requests") {
  const auto alone=plan_regular_core_arbitrary_edge_splits({split(7,1,3),split(7,1,2)},{3});
  const auto with_unrelated=plan_regular_core_arbitrary_edge_splits({split(2,3,7),split(7,1,2),split(7,1,3)},{3});
  REQUIRE(alone.accepted()); REQUIRE(with_unrelated.accepted());
  CHECK(alone.vertices[0].parameter == RegularCoreRational{1,3});
  CHECK(alone.vertices[1].parameter == RegularCoreRational{1,2});
  CHECK(alone.vertices[0].id == with_unrelated.vertices[1].id);
  CHECK(alone.vertices[1].id == with_unrelated.vertices[2].id);
}

TEST_CASE("arbitrary core-edge split ledger refuses transactionally") {
  auto expect = [](const auto& result, auto why) {
    CHECK(result.refusal == why); CHECK(result.vertices.empty());
  };
  expect(plan_regular_core_arbitrary_edge_splits({split(3, 0, 2)}, {1}), RegularCoreArbitraryRefinementRefusal::malformed_parameter);
  expect(plan_regular_core_arbitrary_edge_splits({split(3, 2, 2)}, {1}), RegularCoreArbitraryRefinementRefusal::malformed_parameter);
  expect(plan_regular_core_arbitrary_edge_splits({split(3, 2, 4)}, {1}), RegularCoreArbitraryRefinementRefusal::malformed_parameter);
  expect(plan_regular_core_arbitrary_edge_splits({split(3, 1, 3), split(3, 1, 3)}, {2}), RegularCoreArbitraryRefinementRefusal::duplicate_edge_parameter);
  expect(plan_regular_core_arbitrary_edge_splits({split(3, 1, 3), split(4, 1, 3)}, {1}), RegularCoreArbitraryRefinementRefusal::resource_limit);
}

TEST_CASE("face topology retains an explicit shared-edge triangulation") {
  const auto plan = plan_regular_core_arbitrary_edge_splits({split(10, 2, 5)}, {1});
  REQUIRE(plan.accepted());
  const auto midpoint = plan.vertices.front().id;
  RegularCoreArbitraryFaceTopology face{{42, 1}, {1, 2, 3}, {{{10, {1, 2}}, {11, {2, 3}}, {12, {3, 1}}}}, {{1, midpoint, 3}, {midpoint, 2, 3}}};
  const auto forward = validate_regular_core_face_topology(face, plan);
  std::reverse(face.triangles.begin(), face.triangles.end());
  for (auto& triangle : face.triangles) std::swap(triangle[0], triangle[2]);
  const auto reversed = validate_regular_core_face_topology(face, plan);
  REQUIRE(forward.accepted()); REQUIRE(reversed.accepted());
  CHECK(forward.canonical_triangles == reversed.canonical_triangles);
  face.triangles = {{1, midpoint, 3}};
  CHECK(validate_regular_core_face_topology(face, plan).refusal == RegularCoreFaceTopologyRefusal::nonconforming_boundary);
}

TEST_CASE("non-midpoint stable edge split materializes positive descendants") {
  const auto plan=plan_regular_core_arbitrary_edge_splits({split(10,2,5)},{1}); REQUIRE(plan.accepted());
  const std::vector<RegularCoreGeometryVertex> vertices{{1,{0,0,0}},{2,{1,0,0}},{3,{0,1,0}},{4,{0,0,1}},{5,{0,-1,0}}};
  const std::vector<RegularCoreGeometricParent> parents{{11,{1,2,3,4}},{29,{1,2,5,4}}};
  const auto forward=materialize_regular_core_single_edge_split(parents,vertices,{{10},{1,2}},plan,{11,29});
  REQUIRE(forward.accepted()); CHECK(forward.children.size()==4U); CHECK(forward.vertices.back().id==plan.vertices[0].id);
  const auto split_point=forward.vertices.back().point; CHECK(split_point.x==doctest::Approx(.4));
  auto bad=materialize_regular_core_single_edge_split(parents,vertices,{{10},{3,5}},plan,{11});
  CHECK(bad.refusal==RegularCoreArbitraryMaterializationRefusal::unsupported_pattern);
  const auto incomplete=materialize_regular_core_single_edge_split(parents,vertices,{{10},{1,2}},plan,{11});
  CHECK(incomplete.refusal==RegularCoreArbitraryMaterializationRefusal::incomplete_edge_star);
  auto forged=plan; forged.vertices[0].id+=1U;
  CHECK(materialize_regular_core_single_edge_split(parents,vertices,{{10},{1,2}},forged,{11,29}).refusal==RegularCoreArbitraryMaterializationRefusal::rejected_plan);
}

TEST_CASE("arbitrary split-face core patch preserves matching refined faces") {
  const auto plan=plan_regular_core_arbitrary_edge_splits({split(10,2,5)},{1}); REQUIRE(plan.accepted());
  const std::vector<RegularCoreGeometryVertex> vertices{{1,{0,0,0}},{2,{1,0,0}},{3,{0,1,0}},{4,{0,0,1}}};
  const std::vector<RegularCoreGeometricParent> parents{{11,{1,2,3,4}}};
  std::vector<RegularCoreArbitraryFaceTopology> faces;
  for(std::uint8_t i=0;i<4U;++i) faces.push_back(parent_face(11,i,plan.vertices.front().id));
  const auto materialized=materialize_regular_core_arbitrary_face_refinement(parents,vertices,plan,faces);
  REQUIRE(materialized.accepted()); CHECK(materialized.tetrahedra.size()==6U); CHECK(materialized.vertices.size()==6U);
  auto incompatible=faces; incompatible[2].triangles={{1,2,4}};
  CHECK(materialize_regular_core_arbitrary_face_refinement(parents,vertices,plan,incompatible).refusal==RegularCoreArbitraryFaceMaterializationRefusal::missing_face_topology);
}

TEST_CASE("adjacent arbitrary refined parents share an identical interface") {
  const auto plan=plan_regular_core_arbitrary_edge_splits({split(10,2,5)},{1}); REQUIRE(plan.accepted());
  std::vector<RegularCoreGeometryVertex> vertices{{1,{0,0,0}},{2,{1,0,0}},{3,{0,1,0}},{4,{0,0,1}},{5,{0,0,-1}}};
  std::vector<RegularCoreGeometricParent> parents{{11,{1,2,3,4}},{12,{1,2,3,5}}};
  std::vector<RegularCoreArbitraryFaceTopology> faces;
  for(std::uint8_t i=0;i<4U;++i) faces.push_back(parent_face(11,i,plan.vertices.front().id));
  for(std::uint8_t i=0;i<4U;++i) faces.push_back(parent_face(12,i,plan.vertices.front().id,{1,2,3,5}));
  const auto forward=materialize_regular_core_arbitrary_face_refinement(parents,vertices,plan,faces);
  std::reverse(parents.begin(),parents.end()); std::reverse(vertices.begin(),vertices.end()); std::reverse(faces.begin(),faces.end());
  const auto reversed=materialize_regular_core_arbitrary_face_refinement(parents,vertices,plan,faces);
  REQUIRE(forward.accepted()); REQUIRE(reversed.accepted());
  CHECK(forward.tetrahedra==reversed.tetrahedra); CHECK(forward.tetrahedra.size()==12U);
}

TEST_CASE("arbitrary core materialization supports two split edges on one face") {
  const auto plan=plan_regular_core_arbitrary_edge_splits({split(10,2,5),split(113,1,3)},{2}); REQUIRE(plan.accepted());
  const auto split_id=[&](std::uint64_t edge){return std::find_if(plan.vertices.begin(),plan.vertices.end(),[edge](const auto& vertex){return vertex.edge.value==edge;})->id;};
  const std::vector<RegularCoreGeometryVertex> vertices{{1,{0,0,0}},{2,{1,0,0}},{3,{0,1,0}},{4,{0,0,1}}};
  const std::vector<RegularCoreGeometricParent> parents{{11,{1,2,3,4}}};
  std::vector<RegularCoreArbitraryFaceTopology> faces;
  for(std::uint8_t i=0;i<4U;++i) faces.push_back(parent_face(11,i,split_id(10)));
  faces[1].triangles={{{1,split_id(113),4}},{{split_id(113),3,4}}};
  faces[3].triangles={{{1,split_id(10),split_id(113)}},{{split_id(10),2,3}},{{split_id(10),3,split_id(113)}}};
  const auto materialized=materialize_regular_core_arbitrary_face_refinement(parents,vertices,plan,faces);
  REQUIRE(materialized.accepted()); CHECK(materialized.tetrahedra.size()==8U);
}
