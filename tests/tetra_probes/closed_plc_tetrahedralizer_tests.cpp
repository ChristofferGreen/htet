#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_probes/bcc_transition_request.hpp"

TEST_CASE("closed PLC tetrahedralizer preserves a literal tetrahedron shell") {
  using namespace tetra::probes;
  const std::vector<FrozenFacetVertex> vertices{
      {10U,{0.0,0.0,0.0}}, {20U,{1.0,0.0,0.0}},
      {30U,{0.0,1.0,0.0}}, {40U,{0.0,0.0,1.0}}};
  const std::vector<std::array<std::uint64_t,3>> faces{
      {{10U,30U,20U}},{{10U,20U,40U}},
      {{20U,30U,40U}},{{30U,10U,40U}}};
  const auto result=tetrahedralize_closed_plc(vertices,faces);
  REQUIRE(result.accepted());
  CHECK(result.used_common_kernel);
  CHECK(result.exact_boundary);
  CHECK(result.positive);
  CHECK(result.no_strict_overlap);
  CHECK(result.exact_volume);
  CHECK(result.tetrahedra.size()==faces.size());
}

TEST_CASE("actual planar BCC root cavity enters the bounded non-star path") {
  using namespace tetra::probes;
  SandwichConfig config;config.resolution=4U;config.field=SandwichField::planar;
  const auto surface=extract_bcc_hierarchy_dual_surface(config);
  const auto core=extract_implicit_bcc_hierarchy_core(config);
  const auto roots=probe_bcc_transition_star_cones(surface,core,0U);
  REQUIRE_FALSE(roots.closed_patch_boundaries.empty());
  const auto& root=roots.closed_patch_boundaries.front();
  REQUIRE_FALSE(root.star_shaped);
  ClosedPlcTetrahedralizationOptions options;
  options.maximum_candidate_tetrahedra=1U<<18U;
  options.maximum_search_states=1U<<18U;
  const auto result=tetrahedralize_closed_plc(root.vertices,root.faces,options);
  INFO("vertices="<<root.vertices.size()<<" faces="<<root.faces.size()
       <<" candidates="<<result.candidate_tetrahedra
       <<" states="<<result.search_states
       <<" failure="<<static_cast<unsigned int>(result.failure));
  CHECK(result.failure!=ClosedPlcTetrahedralizationFailure::invalid_input);
  CHECK(result.failure!=ClosedPlcTetrahedralizationFailure::self_intersection);
  if(result.accepted()) {
    CHECK(result.exact_boundary);
    CHECK(result.positive);
    CHECK(result.no_strict_overlap);
    CHECK(result.exact_volume);
  }
}

TEST_CASE("DC surface is completely partitioned by addressed BCC scaffold cells") {
  using namespace tetra::probes;
  for(const auto field:{SandwichField::planar,SandwichField::perlin_height}) {
    SandwichConfig config;config.resolution=4U;config.field=field;
    const auto surface=extract_bcc_hierarchy_dual_surface(config);
    const auto core=extract_implicit_bcc_hierarchy_core(config);
    const auto partition=partition_bcc_surface_over_transition_scaffold(surface,core);
    INFO("field="<<static_cast<unsigned int>(field)
         <<" sources="<<partition.source_triangles
         <<" covered="<<partition.covered_source_triangles
         <<" fragments="<<partition.triangles.size()
         <<" cut_owners="<<partition.cut_owners.size()
         <<" canonical_vertices="<<partition.vertices.size()
         <<" canonical_edges="<<partition.canonical_edges
         <<" owner_seams="<<partition.shared_owner_edges
         <<" boundary_edges="<<partition.boundary_edges
         <<" duplicates="<<partition.duplicate_coplanar_fragments
         <<" area_error="<<partition.area_error);
    CHECK(partition.finite);
    CHECK(partition.barycentrics_valid);
    CHECK(partition.canonical_keys_valid);
    CHECK(partition.canonical_positions_consistent);
    CHECK(partition.canonical_edge_incidence);
    CHECK(partition.source_boundary_preserved);
    CHECK(partition.covered_source_triangles==partition.source_triangles);
    CHECK(partition.exact_coverage);
    CHECK(partition.disjoint_from_retained_core);
    CHECK(partition.valid);
    CHECK_FALSE(partition.triangles.empty());
    CHECK_FALSE(partition.vertices.empty());
    CHECK(partition.canonical_edges>partition.vertices.size());
    CHECK(partition.shared_owner_edges>0U);
    CHECK_FALSE(partition.cut_owners.empty());
    for(const auto& triangle:partition.triangles)
      for(const auto vertex:triangle.canonical_vertex_indices)
        CHECK(vertex<partition.vertices.size());
    const auto repeated=partition_bcc_surface_over_transition_scaffold(surface,core);
    REQUIRE(repeated.valid);
    CHECK(repeated.vertices==partition.vertices);
    REQUIRE(repeated.triangles.size()==partition.triangles.size());
    for(std::size_t i=0U;i<partition.triangles.size();++i) {
      CHECK(repeated.triangles[i].owner==partition.triangles[i].owner);
      CHECK(repeated.triangles[i].source_triangle==partition.triangles[i].source_triangle);
      CHECK(repeated.triangles[i].canonical_vertex_indices==
            partition.triangles[i].canonical_vertex_indices);
    }
  }
}

TEST_CASE("BCC scaffold reports whether exact local DC pieces are convex-coneable") {
  using namespace tetra::probes;
  for(const auto field:{SandwichField::planar,SandwichField::perlin_height}) {
    SandwichConfig config;config.resolution=4U;config.field=field;
    const auto surface=extract_bcc_hierarchy_dual_surface(config);
    const auto core=extract_implicit_bcc_hierarchy_core(config);
    const auto report=inspect_bcc_scaffold_convex_material_cells(config,surface,core);
    INFO("field="<<static_cast<unsigned int>(field)
         <<" owners="<<report.cut_owners
         <<" inside="<<report.owners_with_inside_vertex
         <<" convex="<<report.convex_material_owners
         <<" nonconvex="<<report.nonconvex_material_owners
         <<" supporting="<<report.supporting_surface_fragments
         <<"/"<<report.surface_fragments
         <<" max_vertices="<<report.maximum_material_vertices);
    CHECK(report.partition_valid);
    CHECK(report.cut_owners>0U);
    CHECK(report.convex_material_owners+report.nonconvex_material_owners==
          report.cut_owners);
    CHECK(report.supporting_surface_fragments<=report.surface_fragments);
    if(field==SandwichField::planar)CHECK(report.complete_convex_route);
  }
}

TEST_CASE("planar convex-cell attempt truthfully exposes its finite-boundary gap") {
  using namespace tetra::probes;
  SandwichConfig config;config.resolution=4U;config.field=SandwichField::planar;
  const auto surface=extract_bcc_hierarchy_dual_surface(config);
  const auto core=extract_implicit_bcc_hierarchy_core(config);
  const auto volume=construct_bcc_scaffold_convex_transition(config,surface,core);
  INFO("vertices="<<volume.vertices.size()
       <<" cut_tets="<<volume.cut_owner_tetrahedra
       <<" eroded_full="<<volume.eroded_full_tetrahedra
       <<" core_tets="<<volume.retained_core_tetrahedra.size()
       <<" surface="<<volume.exact_surface_triangles.size()
       <<" local_open_edges="<<volume.local_boundary_open_edges
       <<" unmatched_surface="<<volume.unmatched_surface_edges
       <<" unmatched_bcc="<<volume.unmatched_bcc_face_edges
       <<" open_components="<<volume.open_boundary_components
       <<" bad_degree="<<volume.open_boundary_bad_degree_vertices
       <<" max_open_vertices="<<volume.maximum_open_boundary_vertices
       <<" closures="<<volume.closure_components
       <<" refused_closures="<<volume.refused_closure_components
       <<" closure_triangles="<<volume.closure_triangles
       <<" overlap_candidates="<<volume.overlap_candidates
       <<" overlaps="<<volume.strict_overlap_pairs
       <<" same_owner_overlaps="<<volume.same_owner_overlap_pairs
       <<" cross_owner_overlaps="<<volume.cross_owner_overlap_pairs
       <<" unpaired="<<volume.unpaired_non_domain_faces
       <<" same_sided="<<volume.same_sided_shared_faces
       <<" volume_error="<<volume.volume_error
       <<" dihedral="<<volume.minimum_dihedral_degrees<<".."
       <<volume.maximum_dihedral_degrees
       <<" bytes="<<volume.retained_bytes);
  CHECK(volume.convex_route_applicable);
  CHECK_FALSE(volume.positive);
  CHECK(volume.exact_surface_preserved);
  CHECK_FALSE(volume.face_incidence_valid);
  CHECK(volume.exact_volume);
  CHECK_FALSE(volume.no_overlap_by_scaffold_partition);
  CHECK_FALSE(volume.valid);
  CHECK(volume.local_boundary_open_edges==544U);
  CHECK(volume.unmatched_surface_edges==342U);
  CHECK(volume.unmatched_bcc_face_edges==202U);
  CHECK(volume.open_boundary_components>0U);
  CHECK(volume.open_boundary_bad_degree_vertices==4U);
  CHECK(volume.closure_components>0U);
  CHECK(volume.refused_closure_components>0U);
  CHECK(volume.unpaired_non_domain_faces>0U);
  CHECK(volume.unpaired_non_domain_faces<volume.local_boundary_open_edges);
  CHECK(volume.same_sided_shared_faces>0U);
  CHECK(volume.strict_overlap_pairs>0U);
  CHECK(volume.same_owner_overlap_pairs==volume.strict_overlap_pairs);
  CHECK(volume.cross_owner_overlap_pairs==0U);
  CHECK(volume.minimum_dihedral_degrees<5.0);
  CHECK(volume.maximum_dihedral_degrees>175.0);
  CHECK(volume.retained_bytes>0U);
  CHECK_FALSE(volume.transition_tetrahedra.empty());
  CHECK_FALSE(volume.retained_core_tetrahedra.empty());
  CHECK(volume.exact_surface_triangles.size()>0U);
  const auto repeated=construct_bcc_scaffold_convex_transition(config,surface,core);
  CHECK(repeated.vertices==volume.vertices);
  CHECK(repeated.transition_tetrahedra==volume.transition_tetrahedra);
  CHECK(repeated.transition_tetrahedron_owners==volume.transition_tetrahedron_owners);
  CHECK(repeated.retained_core_tetrahedra==volume.retained_core_tetrahedra);
  CHECK(repeated.exact_surface_triangles==volume.exact_surface_triangles);
  CHECK(repeated.finite_closure_triangles==volume.finite_closure_triangles);
  CHECK(repeated.strict_overlap_pairs==volume.strict_overlap_pairs);
  CHECK(repeated.unpaired_non_domain_faces==volume.unpaired_non_domain_faces);
  CHECK(repeated.retained_bytes==volume.retained_bytes);
}
