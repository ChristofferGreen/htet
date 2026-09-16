#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <algorithm>
#include "tetra_core/regular_core_refinement_geometry.hpp"
#include "tetra_probes/refined_core_transaction.hpp"

TEST_CASE("the control's red cut is materialized from the regular-core descriptor") {
  using namespace tetra;
  RegularCoreParent parent{11U};
  parent.face_vertices={{{2U,3U,4U},{1U,3U,4U},{1U,2U,4U},{1U,2U,3U}}};
  const std::vector<RegularCoreParent> topology{parent};
  const RegularCoreRefinementLimits limits{regular_core_red_grammar_version,1U,8U,1U};
  const auto cut=refine_regular_core(topology,{regular_core_red_grammar_version,{11U,0U}},limits);
  REQUIRE(cut.accepted());
  CHECK(cut.active_leaves.size()==8U);
  CHECK(cut.interface_subfaces.size()==16U);
  const double s3=std::sqrt(3.0),s23=std::sqrt(2.0/3.0);
  RegularCoreGeometryDescriptor geometry;
  geometry.vertices={{1U,{0,0,0}},{2U,{1,0,0}},{3U,{.5,s3/2,0}},{4U,{.5,s3/6,s23}}};
  geometry.parents={{11U,{{1U,2U,3U,4U}}}};
  const auto materialized=materialize_regular_core_red(topology,cut,geometry);
  REQUIRE(materialized.accepted());
  CHECK(materialized.children.size()==8U);
  CHECK(materialized.minimum_dihedral_degrees>=5.0);
}

TEST_CASE("reusable refined-core transaction is descriptor driven and fail closed") {
  using namespace tetra; using namespace tetra::probes;
  RegularCoreParent parent{11U}; parent.face_vertices={{{2U,3U,4U},{1U,3U,4U},{1U,2U,4U},{1U,2U,3U}}};
  const double s3=std::sqrt(3.0),s23=std::sqrt(2.0/3.0);
  RefinedCoreTransactionInput input{{parent},{regular_core_red_grammar_version,{11U,0U}},{regular_core_red_grammar_version,1U,8U,1U},{{{1U,{0,0,0}},{2U,{1,0,0}},{3U,{.5,s3/2,0}},{4U,{.5,s3/6,s23}}},{{11U,{{1U,2U,3U,4U}}}}},{{1U,{-1,-1,-1}},{2U,{2,-1,-1}},{3U,{.5,2,-1}},{4U,{.5,.0,2}}}};
  // The extracted constructor intentionally accepts only matching red faces
  // of a homothetic regular outer descriptor.
  const RegularCorePoint center{.5,s3/6.,s23/4.};
  input.outer_root_vertices.clear();
  for(const auto& vertex:input.core_geometry.vertices) {
    const auto p=vertex.point;
    input.outer_root_vertices.push_back({vertex.id,{center.x+(p.x-center.x)*3.,center.y+(p.y-center.y)*3.,center.z+(p.z-center.z)*3.}});
  }
  const auto result=begin_refined_core_transaction(input);
  INFO("failure="<<static_cast<int>(result.failure)<<" validation_failure="<<static_cast<int>(result.validation.failure)<<" positive="<<result.positive<<" overlap="<<result.no_strict_overlap<<" closed="<<result.closed_oriented_boundary<<" interface="<<result.interface_two_sided<<" valid="<<result.validation.valid<<" frozen="<<result.validation.frozen_outer_faces_preserved<<" core="<<result.validation.retained_core_preserved<<" unique="<<result.validation.unique_tetrahedra<<" sides="<<result.validation.consistently_oriented_shared_faces<<" unexpected="<<result.validation.unexpected_boundary_faces);
  REQUIRE(result.accepted()); CHECK(result.cut.active_leaves.size()==8U); CHECK(result.core.children.size()==8U); CHECK(result.outer_red_vertices.size()==10U);
  CHECK(result.combined_core_tetrahedra.size()==8U); CHECK(result.shell_tetrahedra.size()==48U); CHECK(result.interface_subfaces.size()==16U);
  CHECK(result.core_subface_provenance.size()==16U); CHECK(result.outer_subface_provenance.size()==16U);
  CHECK(result.positive); CHECK(result.no_strict_overlap); CHECK(result.closed_oriented_boundary); CHECK(result.interface_two_sided); CHECK(result.s4);
  CHECK(result.minimum_dihedral_degrees>=5.0); CHECK(result.maximum_dihedral_degrees<=175.0);
  const auto repeat=begin_refined_core_transaction(input);
  CHECK(repeat.accepted()); CHECK(repeat.shell_tetrahedra==result.shell_tetrahedra); CHECK(repeat.combined_core_tetrahedra==result.combined_core_tetrahedra);
  auto reordered=input;
  std::reverse(reordered.core_geometry.vertices.begin(),reordered.core_geometry.vertices.end());
  std::reverse(reordered.outer_root_vertices.begin(),reordered.outer_root_vertices.end());
  const auto reordered_result=begin_refined_core_transaction(reordered);
  REQUIRE(reordered_result.accepted());
  CHECK(reordered_result.combined_vertices==result.combined_vertices);
  CHECK(reordered_result.combined_core_tetrahedra==result.combined_core_tetrahedra);
  CHECK(reordered_result.shell_tetrahedra==result.shell_tetrahedra);
  CHECK(reordered_result.interface_subfaces==result.interface_subfaces);
  CHECK(reordered_result.core_subface_provenance==result.core_subface_provenance);
  CHECK(reordered_result.outer_subface_provenance==result.outer_subface_provenance);
  input.limits.maximum_depth=0U; const auto refused=begin_refined_core_transaction(input); CHECK_FALSE(refused.accepted()); CHECK(refused.core.children.empty());
  CHECK(refused.cut.active_leaves.empty()); CHECK(refused.outer_red_vertices.empty()); CHECK(refused.combined_vertices.empty()); CHECK(refused.combined_core_tetrahedra.empty()); CHECK(refused.shell_tetrahedra.empty());
  input.limits.maximum_depth=1U; input.outer_root_vertices.pop_back(); const auto malformed=begin_refined_core_transaction(input); CHECK_FALSE(malformed.accepted());
  CHECK(malformed.core.children.empty()); CHECK(malformed.outer_red_vertices.empty()); CHECK(malformed.combined_vertices.empty()); CHECK(malformed.shell_tetrahedra.empty());
  auto two_parent=input; two_parent.topology.push_back(parent); two_parent.topology.back().id=12U;
  two_parent.core_geometry.parents.push_back(two_parent.core_geometry.parents.front()); two_parent.core_geometry.parents.back().id=12U;
  two_parent.topology.push_back(parent); two_parent.topology.back().id=13U;
  two_parent.core_geometry.parents.push_back(two_parent.core_geometry.parents.front()); two_parent.core_geometry.parents.back().id=13U;
  const auto unsupported=begin_refined_core_transaction(two_parent);
  CHECK_FALSE(unsupported.accepted()); CHECK(unsupported.failure==RefinedCoreTransactionFailure::unsupported_parent_topology);
  CHECK(unsupported.combined_vertices.empty()); CHECK(unsupported.shell_tetrahedra.empty());
}

TEST_CASE("two-parent rotated regular core emits shell only on six external parent faces") {
  using namespace tetra; using namespace tetra::probes;
  RegularCoreParent a{11U},b{29U};
  a.face_vertices={{{2,3,4},{1,3,4},{1,2,4},{1,2,3}}};
  b.face_vertices={{{1,2,5},{1,3,5},{2,3,5},{3,2,1}}};
  a.neighbors[3]=RegularCoreNeighbor{{29U,3U},{2U,1U,0U}}; b.neighbors[3]=RegularCoreNeighbor{{11U,3U},{2U,1U,0U}};
  const double s3=std::sqrt(3.0),h=std::sqrt(2.0/3.0);
  RefinedCoreTransactionInput input{{a,b},{1U,{11U,3U}},{1U,2U,16U,1U},{{{1,{0,0,0}},{2,{1,0,0}},{3,{.5,s3/2,0}},{4,{.5,s3/6,h}},{5,{.5,s3/6,-h}}},{{11,{{1,2,3,4}}},{29,{{3,2,1,5}}}}},{}};
  const RegularCorePoint center{.5,s3/6,0};
  for(const auto& v:input.core_geometry.vertices){const auto p=v.point;input.outer_root_vertices.push_back({v.id,{center.x+(p.x-center.x)*3.,center.y+(p.y-center.y)*3.,center.z+(p.z-center.z)*3.}});}
  const auto result=begin_refined_core_transaction(input);
  INFO("failure="<<static_cast<int>(result.failure));
  REQUIRE(result.accepted());
  CHECK(result.core.children.size()==16U); CHECK(result.interface_subfaces.size()==24U);
  CHECK(result.shell_tetrahedra.size()==72U); CHECK(result.expected_outer_boundary.size()==24U);
  CHECK(result.core_subface_provenance.size()==24U); CHECK(result.outer_subface_provenance.size()==24U);
  CHECK(result.positive); CHECK(result.no_strict_overlap); CHECK(result.interface_two_sided); CHECK(result.s4);
  auto reordered=input;std::reverse(reordered.topology.begin(),reordered.topology.end());std::reverse(reordered.core_geometry.parents.begin(),reordered.core_geometry.parents.end());std::reverse(reordered.core_geometry.vertices.begin(),reordered.core_geometry.vertices.end());std::reverse(reordered.outer_root_vertices.begin(),reordered.outer_root_vertices.end());reordered.request.face={29U,3U};
  const auto repeat=begin_refined_core_transaction(reordered); REQUIRE(repeat.accepted());
  CHECK(repeat.combined_vertices==result.combined_vertices); CHECK(repeat.combined_core_tetrahedra==result.combined_core_tetrahedra); CHECK(repeat.shell_tetrahedra==result.shell_tetrahedra); CHECK(repeat.interface_subfaces==result.interface_subfaces);
}
