#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_probes/nonmatching_plc_manifest.hpp"
#include "tetra_probes/canonical_delaunay_seed.hpp"

#include <algorithm>

namespace {
using namespace tetra;
using namespace tetra::probes;

std::array<std::uint64_t,3> face(std::array<std::uint64_t,3> ids) {
  std::sort(ids.begin(),ids.end()); return ids;
}

RegularCoreParent parent(std::uint64_t id, std::array<std::uint64_t,4> vertices) {
  RegularCoreParent result; result.id=id;
  for(std::uint8_t f=0;f<4U;++f) {
    std::array<std::uint64_t,3> triangle{}; std::size_t j=0U;
    for(std::size_t i=0U;i<4U;++i) if(i!=f) triangle[j++]=vertices[i];
    result.face_vertices[f]=triangle;
  }
  return result;
}

NonmatchingPlcManifestInput fixture() {
  NonmatchingPlcManifestInput result;
  // A red 1-to-4 subdivision of the independent outer tetrahedron.
  result.outer.vertices={
    {-3,-3,-3},{3,-3,-3},{-3,3,-3},{-3,-3,3},
    {0,-3,-3},{0,0,-3},{-3,0,-3},{-3,-3,0},{0,-3,0},{-3,0,0},
    {-2,-2,-2},{-1.5,-2,-2},{-2,-1.5,-2},{-2,-2,-2.5},{-2.6,-2,-1.5}};
  result.outer.stable_vertex_ids={1,2,3,4,5,6,7,8,9,10,101,102,103,104,105};
  result.outer.outer_faces={
    {{0,6,4}},{{6,2,5}},{{4,5,1}},{{6,5,4}},
    {{0,4,7}},{{4,1,8}},{{7,8,3}},{{4,8,7}},
    {{0,7,6}},{{7,3,9}},{{6,9,2}},{{7,9,6}},
    {{1,5,8}},{{5,2,9}},{{8,9,3}},{{5,9,8}}};
  result.outer.retained_core_tetrahedra={{{10,12,11,13}},{{10,11,12,14}}};
  result.outer.coordinate_scale=6.0;
  for(const auto triangle:result.outer.outer_faces) {
    FrozenFacetIdentity identity{{result.outer.stable_vertex_ids[triangle[0]],result.outer.stable_vertex_ids[triangle[1]],result.outer.stable_vertex_ids[triangle[2]]}};
    std::sort(identity.vertex_ids.begin(),identity.vertex_ids.end());
    result.outer.outer_parent_facets.push_back({identity,FacetPreservationMode::literal});
  }
  result.core_geometry.vertices={
    {101,{-2,-2,-2}},{102,{-1.5,-2,-2}},{103,{-2,-1.5,-2}},
    {104,{-2,-2,-2.5}},{105,{-2.6,-2,-1.5}}};
  // The second parent deliberately has a rotated/permuted local order.
  result.core_geometry.parents={{11,{{101,103,102,104}}},{12,{{101,102,103,105}}}};
  auto first=parent(11,{{101,103,102,104}});
  auto second=parent(12,{{101,102,103,105}});
  first.neighbors[3]=RegularCoreNeighbor{{12,3},{{0,2,1}}};
  second.neighbors[3]=RegularCoreNeighbor{{11,3},{{0,2,1}}};
  result.core_topology={first,second};
  return result;
}

bool empty(const NonmatchingPlcManifest& manifest) {
  return manifest.vertex_geometry.empty() && manifest.outer_parent_coverage.empty() &&
    manifest.external_core_coverage.empty() && manifest.internal_core_faces.empty() && manifest.materialized_core_tetrahedra.empty();
}
}

TEST_CASE("nonmatching PLC manifest canonically describes a rotated two-parent red core") {
  const auto input=fixture();
  const auto built=build_nonmatching_plc_manifest(input);
  REQUIRE(built.accepted());
  CHECK(built.manifest.outer_parent_coverage.size()==16U);
  CHECK(built.manifest.vertex_geometry.size()==24U);
  CHECK(built.manifest.external_core_coverage.size()==6U);
  CHECK(built.manifest.internal_core_faces.size()==1U);
  CHECK(built.manifest.materialized_core_tetrahedra.size()==16U);
  for(const auto& tetrahedron:built.manifest.materialized_core_tetrahedra)
    for(const auto id:tetrahedron)
      CHECK(std::any_of(built.manifest.vertex_geometry.begin(),built.manifest.vertex_geometry.end(),[id](const auto& vertex){return vertex.id==id;}));
  std::size_t external_subfaces=0U;
  for(const auto& split:built.manifest.outer_parent_coverage) CHECK(validate_frozen_facet_split(split));
  for(const auto& split:built.manifest.external_core_coverage) {
    CHECK(validate_frozen_facet_split(split)); external_subfaces+=split.subfaces.size();
  }
  CHECK(external_subfaces==24U);

  auto reordered=input;
  std::reverse(reordered.core_topology.begin(),reordered.core_topology.end());
  std::reverse(reordered.core_geometry.parents.begin(),reordered.core_geometry.parents.end());
  std::reverse(reordered.core_geometry.vertices.begin(),reordered.core_geometry.vertices.end());
  std::reverse(reordered.outer.outer_faces.begin(),reordered.outer.outer_faces.end());
  std::reverse(reordered.outer.outer_parent_facets.begin(),reordered.outer.outer_parent_facets.end());
  const auto repeat=build_nonmatching_plc_manifest(reordered);
  REQUIRE(repeat.accepted());
  CHECK(serialize_nonmatching_plc_manifest(repeat)==serialize_nonmatching_plc_manifest(built));

  // The connected constructor path consumes this real manifest. Its initial
  // unconstrained seed must refuse publication until every geometric core
  // subface has been materialized and recovered.
  const auto seed=inspect_canonical_plc_seed(built,1024U);
  CHECK(seed.failure==CanonicalPlcSeedFailure::unrecovered_constraint);
  CHECK(seed.candidate_tetrahedra>0U);
  CHECK(seed.required_facets==40U);
  CHECK(seed.unresolved_facet_vertices==0U);
  CHECK(seed.required_edges==60U);
  // Co-spherical regular-core points are resolved by the stable symbolic
  // predicate.  The contract is missing versus recovered constraints, not a
  // historical choice of Delaunay diagonals.
  CHECK(seed.recovered_edges<seed.required_edges);
  CHECK_FALSE(seed.missing_edges.empty());
  CHECK(seed.recovered_facets<seed.required_facets);
  CHECK(seed.recovered_facets<seed.required_facets);
  CHECK_FALSE(seed.missing_facets.empty());

  const auto constraints=materialize_canonical_plc_constraints(built);
  REQUIRE(constraints.accepted());
  CHECK(constraints.constraints.vertices.size()==24U);
  CHECK(constraints.constraints.facets.size()==40U);
  const auto split=split_canonical_plc_constraint_edge(constraints.constraints,seed.missing_edges.front(),64U,128U);
  REQUIRE(split.accepted());
  CHECK(split.constraints.vertices.size()==25U);
  CHECK(split.constraints.facets.size()>constraints.constraints.facets.size());
  for(const auto& facet:split.constraints.facets) for(const auto& corner:facet.corners)
    CHECK(corner.numerator[0]+corner.numerator[1]+corner.numerator[2]==corner.denominator);

  const auto recovered=recover_canonical_plc_edges(built);
  CAPTURE(recovered.two_sided_facet_attempts);
  CAPTURE(recovered.two_sided_facets_recovered);
  CAPTURE(recovered.two_sided_cavity_expansions);
  CAPTURE(recovered.last_two_sided_intersected_tetrahedra);
  CAPTURE(recovered.last_two_sided_top_tetrahedra);
  CAPTURE(recovered.last_two_sided_bottom_tetrahedra);
  CAPTURE(recovered.last_two_sided_retriangulation_trials);
  CAPTURE(static_cast<int>(recovered.last_two_sided_facet_failure));
  CHECK(recovered.accepted());
  CHECK(recovered.edge_splits==0U);
  CHECK(recovered.edges_recovered_before_facet_stage);
  CHECK(recovered.attempted_edge_recoveries>0U);
  CHECK(recovered.advancing_ridge_insertions==0U);
  CHECK(recovered.advancing_ridge_refusals[static_cast<std::size_t>(
            CanonicalAdvancingRidgeFailure::none)]==0U);
  CHECK(recovered.inspection.accepted());
  CHECK_FALSE(recovered.constraints.vertices.empty());
  CanonicalPlcRecoveryOptions capped_options;
  capped_options.maximum_vertices=64U;
  capped_options.maximum_facets=128U;
  capped_options.maximum_tetrahedra=4096U;
  capped_options.maximum_edge_splits=0U;
  const auto capped=recover_canonical_plc_edges(built,capped_options);
  CHECK(capped.accepted());
  CHECK(capped.edge_splits==0U);
  CHECK(capped.edges_recovered_before_facet_stage);
  CHECK(capped.advancing_ridge_attempts==0U);
  CHECK_FALSE(capped.constraints.vertices.empty());

  const auto volume=construct_canonical_plc_volume(built);
  CAPTURE(static_cast<int>(volume.failure));
  CAPTURE(static_cast<int>(volume.validation.failure));
  CAPTURE(volume.validation.missing_outer_faces);
  CAPTURE(volume.validation.unexpected_boundary_faces);
  CAPTURE(volume.validation.missing_outer_parent_facets);
  CAPTURE(volume.validation.invalid_preserved_subfaces);
  CAPTURE(volume.validation.nonmanifold_faces);
  CAPTURE(volume.validation.same_sided_shared_faces);
  CAPTURE(volume.validation.tetrahedron_overlap_pairs);
  CAPTURE(volume.shell_tetrahedra);
  CAPTURE(volume.core_tetrahedra);
  CHECK_FALSE(volume.accepted());
  CHECK(volume.failure==CanonicalPlcVolumeFailure::quality_rejected);
  CHECK(volume.validation.valid);
  CHECK(volume.shell_tetrahedra>0U);
  CHECK(volume.core_tetrahedra>0U);
  // The bounded topology-independent stage considers at most four face,
  // six 3→2-edge, two 4→4-edge, one centroid split, and at most two
  // free two-tet cavity cones per shell tet.
  CHECK(volume.quality_repair_candidates<=15U*volume.shell_tetrahedra);
  // The selected repair must be the mesh subsequently audited, even though
  // S4 still refuses this control. The installed transaction is measured as
  // 3.94519°..167.435°; a merely counted-but-lost candidate cannot satisfy
  // this regression. Edge-first recovery supplies this control's constrained
  // topology without invoking the legacy advancing-ridge stage. The locally
  // recovered private dividers leave 60 shell cells after repair.
  CHECK(volume.quality_repair_accepted==1U);
  CHECK(volume.shell_tetrahedra==60U);
  CHECK(volume.minimum_dihedral_degrees==doctest::Approx(3.94519).epsilon(1e-5));
  CHECK(volume.maximum_dihedral_degrees==doctest::Approx(167.435).epsilon(1e-5));
  CHECK((volume.minimum_dihedral_degrees<5.0 || volume.maximum_dihedral_degrees>175.0));
  CHECK(volume.tetrahedra.empty());
  CHECK(volume.vertices.empty());

  // Reordering the full input may not alter a generated assembly's audit or
  // refusal reason. This remains useful evidence even while S4 withholds it.
  const auto reordered_volume=construct_canonical_plc_volume(repeat);
  CHECK(reordered_volume.failure==volume.failure);
  CHECK(reordered_volume.validation.valid==volume.validation.valid);
  CHECK(reordered_volume.shell_tetrahedra==volume.shell_tetrahedra);
  CHECK(reordered_volume.core_tetrahedra==volume.core_tetrahedra);
  CHECK(reordered_volume.minimum_dihedral_degrees==doctest::Approx(volume.minimum_dihedral_degrees));
  CHECK(reordered_volume.maximum_dihedral_degrees==doctest::Approx(volume.maximum_dihedral_degrees));
}

TEST_CASE("manifest materializes independent geometric outer-facet points") {
  auto input=fixture();
  for(auto& parent:input.outer.outer_parent_facets)
    parent.mode=FacetPreservationMode::geometric;
  const auto built=build_nonmatching_plc_manifest(input);
  REQUIRE(built.accepted());
  // The original control has 24 point records. A geometric outer sheet adds
  // its independently owned midpoint coordinates; none are expected from the
  // red core materializer itself.
  CHECK(built.manifest.vertex_geometry.size()>24U);
  const auto constraints=materialize_canonical_plc_constraints(built);
  REQUIRE(constraints.accepted());
  CHECK(constraints.constraints.facets.size()>40U);
}

TEST_CASE("nonmatching PLC manifest refuses invalid controls transactionally") {
  SUBCASE("malformed reciprocal adjacency") {
    auto input=fixture(); input.core_topology[1].neighbors[3]->face.face=2U;
    const auto result=build_nonmatching_plc_manifest(input);
    CHECK(result.failure==NonmatchingPlcManifestFailure::malformed_core_adjacency); CHECK(empty(result.manifest));
    CHECK(serialize_nonmatching_plc_manifest(result).empty());
  }
  SUBCASE("moved outer vertex no longer nests the core") {
    auto input=fixture(); input.outer.vertices[0]={0.0,0.0,0.0};
    const auto result=build_nonmatching_plc_manifest(input);
    CHECK(result.failure==NonmatchingPlcManifestFailure::invalid_outer_plc); CHECK(empty(result.manifest));
  }
  SUBCASE("open outer surface") {
    auto input=fixture(); input.outer.outer_faces.pop_back(); input.outer.outer_parent_facets.pop_back();
    const auto result=build_nonmatching_plc_manifest(input);
    CHECK(result.failure==NonmatchingPlcManifestFailure::invalid_outer_plc); CHECK(empty(result.manifest));
  }
  SUBCASE("self intersecting closed outer surface") {
    auto input=fixture(); input.outer.outer_parent_facets.clear();
    input.outer.vertices.insert(input.outer.vertices.end(),{{-2,-2,-2},{4,-2,-2},{-2,4,-2},{-2,-2,4}});
    input.outer.stable_vertex_ids.insert(input.outer.stable_vertex_ids.end(),{201,202,203,204});
    input.outer.outer_faces.insert(input.outer.outer_faces.end(),{{{15,17,16}},{{15,16,18}},{{15,18,17}},{{16,17,18}}});
    const auto result=build_nonmatching_plc_manifest(input);
    CHECK(result.failure==NonmatchingPlcManifestFailure::invalid_outer_plc); CHECK(empty(result.manifest));
  }
  SUBCASE("parent resource limit") {
    auto input=fixture(); input.maximum_parents=1U;
    const auto result=build_nonmatching_plc_manifest(input);
    CHECK(result.failure==NonmatchingPlcManifestFailure::resource_limit); CHECK(empty(result.manifest));
  }
}
