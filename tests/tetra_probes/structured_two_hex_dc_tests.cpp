#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_probes/sandwich_probe.hpp"
#include "tetra_probes/terrain_volume_request.hpp"
#include "tetra_probes/canonical_delaunay_seed.hpp"
#include "tetra_probes/bcc_transition_request.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace {
template<std::size_t N>
auto canonical_geometry(
    const std::vector<std::array<double,3>>& vertices,
    const std::vector<std::array<std::uint32_t,N>>& elements) {
  std::vector<std::array<std::array<double,3>,N>> result;
  result.reserve(elements.size());
  for(const auto& element:elements) {
    std::array<std::array<double,3>,N> geometry{};
    for(std::size_t vertex=0U;vertex<N;++vertex)
      geometry[vertex]=vertices.at(element[vertex]);
    std::ranges::sort(geometry);
    result.push_back(geometry);
  }
  std::ranges::sort(result);
  return result;
}

auto canonical_vertices(std::vector<std::array<double,3>> vertices) {
  std::ranges::sort(vertices);
  return vertices;
}

auto root_transform(const std::array<std::array<double,3>,4>& target) {
  const auto root=tetra::world_tetrahedron_geometry(
      tetra::WorldTetAddress::root(0U));
  const auto subtract=[](tetra::Vec3 a,tetra::Vec3 b) {
    return tetra::Vec3{a.x-b.x,a.y-b.y,a.z-b.z};
  };
  const auto cross=[](tetra::Vec3 a,tetra::Vec3 b) {
    return tetra::Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,
                       a.x*b.y-a.y*b.x};
  };
  const auto dot=[](tetra::Vec3 a,tetra::Vec3 b) {
    return a.x*b.x+a.y*b.y+a.z*b.z;
  };
  return [=](tetra::Vec3 point) {
    const auto a=subtract(root[1],root[0]);
    const auto b=subtract(root[2],root[0]);
    const auto c=subtract(root[3],root[0]);
    const auto d=subtract(point,root[0]);
    const double determinant=dot(a,cross(b,c));
    const double u=dot(d,cross(b,c))/determinant;
    const double v=dot(a,cross(d,c))/determinant;
    const double w=dot(a,cross(b,d))/determinant;
    const std::array<double,4> weights{{1.0-u-v-w,u,v,w}};
    std::array<double,3> result{};
    for(std::size_t corner=0U;corner<4U;++corner)
      for(std::size_t axis=0U;axis<3U;++axis)
        result[axis]+=target[corner][axis]*weights[corner];
    return result;
  };
}
} // namespace

TEST_CASE("structured hexahedra select one unchanged global hierarchy core") {
  tetra::probes::SandwichConfig planar;
  planar.resolution=8U;
  planar.field=tetra::probes::SandwichField::planar;
  const auto first=tetra::probes::extract_structured_two_hex_dual_surface(planar);
  const auto reversed=tetra::probes::extract_structured_two_hex_dual_surface(
      planar,true);
  INFO("global vertices="<<first.global_core_vertices.size()
       <<" tets="<<first.global_core_tetrahedra.size()
       <<" shared-border-crossers="
       <<first.global_core_shared_border_crossing_tetrahedra);
  REQUIRE_FALSE(first.global_core_tetrahedra.empty());
  CHECK(first.global_core_red_depth==3U);
  CHECK(first.global_core_address_reconstruction_exact);
  CHECK(first.global_core_unique_ownership);
  CHECK(first.global_core_shared_border_crossing_tetrahedra>0U);
  CHECK(first.global_core_tet_addresses.size()==
        first.global_core_tetrahedra.size());
  CHECK(first.global_core_hexahedron_owners.size()==
        first.global_core_tetrahedra.size());
  CHECK(std::ranges::is_sorted(first.global_core_tet_addresses));
  CHECK(std::adjacent_find(first.global_core_tet_addresses.begin(),
                           first.global_core_tet_addresses.end())==
        first.global_core_tet_addresses.end());
  CHECK(first.global_core_vertices==reversed.global_core_vertices);
  CHECK(first.global_core_vertex_addresses==
        reversed.global_core_vertex_addresses);
  CHECK(first.global_core_tetrahedra==reversed.global_core_tetrahedra);
  CHECK(first.global_core_tet_addresses==reversed.global_core_tet_addresses);
  CHECK(first.global_core_hexahedron_owners==
        reversed.global_core_hexahedron_owners);

  // N6 and N8 use different hexahedron-local structured parameterizations,
  // but both select hierarchy depth three. The authoritative core must
  // therefore be byte-identical rather than following either local grid.
  auto different_local_grid=planar;
  different_local_grid.resolution=6U;
  const auto reparameterized=
      tetra::probes::extract_structured_two_hex_dual_surface(
          different_local_grid);
  CHECK(first.global_core_vertices==reparameterized.global_core_vertices);
  CHECK(first.global_core_vertex_addresses==
        reparameterized.global_core_vertex_addresses);
  CHECK(first.global_core_tetrahedra==
        reparameterized.global_core_tetrahedra);
  CHECK(first.global_core_tet_addresses==
        reparameterized.global_core_tet_addresses);
  CHECK(first.global_core_hexahedron_owners==
        reparameterized.global_core_hexahedron_owners);

  std::array<std::vector<tetra::WorldTetAddress>,2> independent_chunks;
  for(std::size_t tet=0U;tet<first.global_core_tet_addresses.size();++tet)
    independent_chunks[first.global_core_hexahedron_owners[tet]].push_back(
        first.global_core_tet_addresses[tet]);
  CHECK_FALSE(independent_chunks[0].empty());
  CHECK_FALSE(independent_chunks[1].empty());
  std::vector<tetra::WorldTetAddress> merged=independent_chunks[0];
  merged.insert(merged.end(),independent_chunks[1].begin(),
                independent_chunks[1].end());
  std::ranges::sort(merged);
  CHECK(merged==first.global_core_tet_addresses);

  const auto transform=root_transform(first.parent_tetrahedron);
  for(std::size_t tet=0U;tet<first.global_core_tetrahedra.size();++tet) {
    const auto address=first.global_core_tet_addresses[tet];
    const auto geometry=tetra::world_tetrahedron_geometry(address);
    const auto keys=tetra::world_tetrahedron_vertex_keys(address);
    CHECK(address.red_depth()==first.global_core_red_depth);
    CHECK(first.global_core_hexahedron_owners[tet]<2U);
    for(std::size_t corner=0U;corner<4U;++corner) {
      const auto vertex=first.global_core_tetrahedra[tet][corner];
      REQUIRE(vertex<first.global_core_vertices.size());
      CHECK(first.global_core_vertex_addresses[vertex]==keys[corner]);
      CHECK(first.global_core_vertices[vertex]==transform(geometry[corner]));
    }
  }

  auto noisy=planar;
  noisy.field=tetra::probes::SandwichField::perlin_height;
  noisy.amplitude=0.22;
  const auto changed_field=
      tetra::probes::extract_structured_two_hex_dual_surface(noisy);
  std::map<tetra::WorldVertexKey,std::array<double,3>> planar_positions;
  for(std::size_t vertex=0U;vertex<first.global_core_vertices.size();++vertex)
    planar_positions.emplace(first.global_core_vertex_addresses[vertex],
                             first.global_core_vertices[vertex]);
  std::size_t common_vertices{};
  for(std::size_t vertex=0U;vertex<changed_field.global_core_vertices.size();
      ++vertex) {
    const auto found=planar_positions.find(
        changed_field.global_core_vertex_addresses[vertex]);
    if(found==planar_positions.end())continue;
    ++common_vertices;
    CHECK(found->second==changed_field.global_core_vertices[vertex]);
  }
  CHECK(common_vertices>0U);
}

TEST_CASE("frozen structured DC is exactly partitioned over global cut tets") {
  for(const auto field:{tetra::probes::SandwichField::planar,
                        tetra::probes::SandwichField::perlin_height}) {
    tetra::probes::SandwichConfig config;
    config.resolution=8U;
    config.field=field;
    config.amplitude=0.22;
    const auto surface=
        tetra::probes::extract_structured_two_hex_dual_surface(config);
    const auto partition=
        tetra::probes::partition_structured_surface_over_global_core(surface);
    INFO("field="<<static_cast<unsigned int>(field)
         <<" source="<<partition.source_triangles
         <<" covered="<<partition.covered_source_triangles
         <<" fragments="<<partition.triangles.size()
         <<" cut_tets="<<partition.cut_owners.size()
         <<" shared_edges="<<partition.shared_owner_edges
         <<" duplicates="<<partition.duplicate_coplanar_fragments
         <<" area_error="<<partition.area_error);
    CHECK(partition.valid);
    CHECK(partition.finite);
    CHECK(partition.barycentrics_valid);
    CHECK(partition.canonical_keys_valid);
    CHECK(partition.canonical_positions_consistent);
    CHECK(partition.canonical_edge_incidence);
    CHECK(partition.source_boundary_preserved);
    CHECK(partition.exact_coverage);
    CHECK(partition.disjoint_from_retained_core);
    CHECK(partition.covered_source_triangles==surface.triangles.size());
    CHECK_FALSE(partition.cut_owners.empty());
    CHECK_FALSE(partition.source_boundary_owners.empty());
    CHECK(partition.source_boundary_owners.size()<partition.cut_owners.size());
    for(const auto owner:partition.source_boundary_owners)
      CHECK(std::ranges::binary_search(partition.cut_owners,owner));
    CHECK(partition.shared_owner_edges>0U);

    const auto repeated=
        tetra::probes::partition_structured_surface_over_global_core(surface);
    CHECK(repeated.vertices==partition.vertices);
    CHECK(repeated.cut_owners==partition.cut_owners);
    CHECK(repeated.source_boundary_owners==partition.source_boundary_owners);
    REQUIRE(repeated.triangles.size()==partition.triangles.size());
    for(std::size_t triangle=0U;triangle<partition.triangles.size();++triangle) {
      CHECK(repeated.triangles[triangle].owner==
            partition.triangles[triangle].owner);
      CHECK(repeated.triangles[triangle].source_triangle==
            partition.triangles[triangle].source_triangle);
      CHECK(repeated.triangles[triangle].canonical_vertex_indices==
            partition.triangles[triangle].canonical_vertex_indices);
      CHECK(repeated.triangles[triangle].positions==
            partition.triangles[triangle].positions);
    }
  }
}

TEST_CASE("structured ghost halo preserves the frozen two-hex DC prefix") {
  for(const auto field:{tetra::probes::SandwichField::planar,
                        tetra::probes::SandwichField::perlin_height}) {
    tetra::probes::SandwichConfig config;
    config.resolution=8U;
    config.field=field;
    config.amplitude=0.22;
    const auto frozen=
        tetra::probes::extract_structured_two_hex_dual_surface(config);
    const auto halo=tetra::probes::extract_structured_two_hex_dc_halo(config,3U);
    INFO("field="<<static_cast<unsigned int>(field)
         <<" halo_vertices="<<halo.halo_vertices
         <<" halo_quads="<<halo.halo_quads
         <<" halo_triangles="<<halo.halo_triangles
         <<" outer_incomplete="<<halo.incomplete_outer_rings
         <<" active_mismatch="<<halo.original_active_mismatches
         <<" missing_quads="<<halo.missing_original_quads
         <<" boundary="<<halo.surface.validation.boundary_edges
         <<" nonmanifold="<<halo.surface.validation.nonmanifold_edges
         <<" intersections="<<halo.surface.validation.strict_triangle_intersections);
    REQUIRE(halo.surface.dual_vertices.size()>=frozen.dual_vertices.size());
    REQUIRE(halo.surface.quads.size()>=frozen.quads.size());
    REQUIRE(halo.surface.triangles.size()>=frozen.triangles.size());
    CHECK(std::equal(frozen.dual_vertices.begin(),frozen.dual_vertices.end(),
                     halo.surface.dual_vertices.begin()));
    CHECK(std::equal(frozen.quads.begin(),frozen.quads.end(),
                     halo.surface.quads.begin()));
    CHECK(std::equal(frozen.triangles.begin(),frozen.triangles.end(),
                     halo.surface.triangles.begin()));
    CHECK(halo.original_surface_preserved);
    CHECK(halo.every_complete_ring_is_quad);
    CHECK(halo.halo_vertices>0U);
    CHECK(halo.halo_quads>0U);
    CHECK(halo.halo_triangles==2U*halo.halo_quads);
    CHECK(halo.surface.validation.valid);
    CHECK(halo.valid);
    const auto partition=
        tetra::probes::partition_structured_surface_over_global_core(
            halo.surface);
    INFO("partition_boundary_owners="<<partition.source_boundary_owners.size()
         <<" covered="<<partition.covered_source_triangles<<'/'
         <<partition.source_triangles
         <<" finite="<<partition.finite
         <<" bary="<<partition.barycentrics_valid
         <<" keys="<<partition.canonical_keys_valid
         <<" positions="<<partition.canonical_positions_consistent
         <<" incidence="<<partition.canonical_edge_incidence
         <<" coverage="<<partition.exact_coverage
         <<" disjoint="<<partition.disjoint_from_retained_core);
    CHECK(partition.valid);
    CHECK_FALSE(partition.source_boundary_owners.empty());
    if(field==tetra::probes::SandwichField::planar) {
      const auto volume=
          tetra::probes::construct_structured_global_cut_transition(
              config,halo.surface);
      std::size_t open_on_root_boundary{};
      for(const auto owner:volume.locally_open_owners) {
        const auto keys=tetra::world_tetrahedron_vertex_keys(owner);
        bool owner_on_boundary{};
        for(std::size_t omitted=0U;omitted<4U;++omitted)
          for(std::size_t axis=0U;axis<3U;++axis) {
            bool low=true,high=true;
            for(std::size_t corner=0U;corner<4U;++corner)
              if(corner!=omitted) {
                const auto coordinate=axis==0U?keys[corner].x:
                    axis==1U?keys[corner].y:keys[corner].z;
                const auto denominator=std::int64_t{1}<<
                    keys[corner].denominator_exponent;
                low=low&&coordinate==0;high=high&&coordinate==denominator;
              }
            owner_on_boundary=owner_on_boundary||low||high;
          }
        if(owner_on_boundary)++open_on_root_boundary;
      }
      INFO("halo_transition="<<volume.transition_tetrahedra.size()
           <<" retained="<<volume.retained_core_tetrahedra.size()
           <<" open_edges="<<volume.local_boundary_open_edges
           <<" unmatched_surface="<<volume.unmatched_surface_edges
           <<" unmatched_hierarchy="<<volume.unmatched_bcc_face_edges
           <<" open_owners="<<volume.locally_open_owners.size()
           <<" open_root_owners="<<open_on_root_boundary
           <<" geometric_pairs="<<volume.geometrically_matching_open_edge_pairs
           <<" surface_in_hierarchy="
                <<volume.surface_edges_contained_in_hierarchy_edges
           <<" hierarchy_in_surface="
                <<volume.hierarchy_edges_contained_in_surface_edges
           <<" surface_on_owner_face="
                <<volume.open_surface_edges_on_hierarchy_faces
           <<" refusals="<<volume.refused_closure_components
           <<" unpaired="<<volume.unpaired_non_domain_faces
           <<" chunk_interfaces="<<volume.chunk_interface_faces
           <<" overlaps="<<volume.strict_overlap_pairs
           <<" volume_error="<<volume.volume_error
           <<" first_surface="<<(volume.unmatched_surface_edge_geometry.empty()?999.0:
                volume.unmatched_surface_edge_geometry.front()[0])<<','
                <<(volume.unmatched_surface_edge_geometry.empty()?999.0:
                volume.unmatched_surface_edge_geometry.front()[1])<<','
                <<(volume.unmatched_surface_edge_geometry.empty()?999.0:
                volume.unmatched_surface_edge_geometry.front()[2])<<" -> "
                <<(volume.unmatched_surface_edge_geometry.empty()?999.0:
                volume.unmatched_surface_edge_geometry.front()[3])<<','
                <<(volume.unmatched_surface_edge_geometry.empty()?999.0:
                volume.unmatched_surface_edge_geometry.front()[4])<<','
                <<(volume.unmatched_surface_edge_geometry.empty()?999.0:
                volume.unmatched_surface_edge_geometry.front()[5])
           <<" first_hierarchy="<<(volume.unmatched_hierarchy_edge_geometry.empty()?999.0:
                volume.unmatched_hierarchy_edge_geometry.front()[0])<<','
                <<(volume.unmatched_hierarchy_edge_geometry.empty()?999.0:
                volume.unmatched_hierarchy_edge_geometry.front()[1])<<','
                <<(volume.unmatched_hierarchy_edge_geometry.empty()?999.0:
                volume.unmatched_hierarchy_edge_geometry.front()[2])<<" -> "
                <<(volume.unmatched_hierarchy_edge_geometry.empty()?999.0:
                volume.unmatched_hierarchy_edge_geometry.front()[3])<<','
                <<(volume.unmatched_hierarchy_edge_geometry.empty()?999.0:
                volume.unmatched_hierarchy_edge_geometry.front()[4])<<','
                <<(volume.unmatched_hierarchy_edge_geometry.empty()?999.0:
                volume.unmatched_hierarchy_edge_geometry.front()[5]));
      CHECK(volume.convex_route_applicable);
      CHECK(volume.local_boundary_open_edges==0U);
      CHECK(volume.refused_closure_components==0U);
      CHECK(volume.positive);
      CHECK(volume.exact_surface_preserved);
      CHECK(volume.face_incidence_valid);
      CHECK(volume.unpaired_non_domain_faces==0U);
      CHECK(volume.chunk_interface_faces>0U);
      CHECK(volume.no_overlap_by_scaffold_partition);
      CHECK(volume.exact_volume);
      CHECK(volume.valid);
    }
  }
}

TEST_CASE("global cut-cell material regions are classified without local grid warping") {
  for(const auto field:{tetra::probes::SandwichField::planar,
                        tetra::probes::SandwichField::perlin_height}) {
    tetra::probes::SandwichConfig config;
    config.resolution=8U;
    config.field=field;
    config.amplitude=0.22;
    const auto surface=
        tetra::probes::extract_structured_two_hex_dual_surface(config);
    const auto report=tetra::probes::inspect_structured_global_cut_cells(
        config,surface);
    INFO("field="<<static_cast<unsigned int>(field)
         <<" cut="<<report.cut_owners
         <<" inside="<<report.owners_with_inside_vertex
         <<" convex="<<report.convex_material_owners
         <<" nonconvex="<<report.nonconvex_material_owners
         <<" supporting="<<report.supporting_surface_fragments
         <<'/'<<report.surface_fragments
         <<" max_vertices="<<report.maximum_material_vertices);
    CHECK(report.partition_valid);
    CHECK(report.cut_owners>0U);
    CHECK(report.convex_material_owners+report.nonconvex_material_owners==
          report.cut_owners);
    CHECK(report.owners_with_inside_vertex>0U);
    CHECK(report.supporting_surface_fragments<=report.surface_fragments);
    CHECK(report.maximum_material_vertices>=4U);
    if(field==tetra::probes::SandwichField::perlin_height) {
      auto refined_transition=surface;
      ++refined_transition.global_core_red_depth;
      refined_transition.global_core_tet_addresses.clear();
      const auto refined=tetra::probes::inspect_structured_global_cut_cells(
          config,refined_transition);
      INFO("refined cut="<<refined.cut_owners
           <<" convex="<<refined.convex_material_owners
           <<" nonconvex="<<refined.nonconvex_material_owners
           <<" fragments="<<refined.surface_fragments);
      CHECK(refined.partition_valid);
      CHECK(refined.convex_material_owners*report.cut_owners>
            report.convex_material_owners*refined.cut_owners);
    }
  }
}

TEST_CASE("open two-hex DC patch is not mislabeled as a complete global volume") {
  tetra::probes::SandwichConfig config;
  config.resolution=8U;
  config.field=tetra::probes::SandwichField::planar;
  const auto surface=
      tetra::probes::extract_structured_two_hex_dual_surface(config);
  const auto volume=
      tetra::probes::construct_structured_global_cut_transition(config,surface);
  INFO("transition="<<volume.transition_tetrahedra.size()
       <<" retained="<<volume.retained_core_tetrahedra.size()
       <<" open_edges="<<volume.local_boundary_open_edges
       <<" unmatched_surface="<<volume.unmatched_surface_edges
       <<" unmatched_hierarchy="<<volume.unmatched_bcc_face_edges
       <<" closure_refusals="<<volume.refused_closure_components
       <<" unpaired_faces="<<volume.unpaired_non_domain_faces
       <<" overlaps="<<volume.strict_overlap_pairs
       <<" volume_error="<<volume.volume_error);
  CHECK(volume.convex_route_applicable);
  CHECK_FALSE(volume.transition_tetrahedra.empty());
  CHECK(volume.transition_tetrahedra.size()==
        volume.transition_tetrahedron_owners.size());
  CHECK(volume.retained_core_tetrahedra.size()==
        surface.global_core_tet_addresses.size());
  CHECK(volume.retained_core_tetrahedron_owners==
        surface.global_core_tet_addresses);
  // The frozen two-hexahedron surface intentionally stops at its outer
  // perimeter. A cut global tet can straddle that perimeter, so the DC sheet
  // alone does not bound its material side. Until a canonical DC halo (or an
  // explicit finite-domain closure) supplies those missing facets, publishing
  // this local construction as a complete volume would be false.
  CHECK(volume.local_boundary_open_edges>0U);
  CHECK(volume.refused_closure_components>0U);
  CHECK_FALSE(volume.face_incidence_valid);
  CHECK_FALSE(volume.valid);

  const auto interior=
      tetra::probes::construct_structured_global_interior_cut_transition(
          config,surface);
  INFO("interior_transition="<<interior.transition_tetrahedra.size()
       <<" local_open_edges="<<interior.local_boundary_open_edges
       <<" closure_refusals="<<interior.refused_closure_components
       <<" locally_open_owners="<<interior.locally_open_owners.size()
       <<" first_open_root="<<(interior.locally_open_owners.empty()?999U:
            interior.locally_open_owners.front().root_id())
       <<" first_open_low="<<(interior.locally_open_owners.empty()?0U:
            interior.locally_open_owners.front().low)
       <<" same_owner_overlaps="<<interior.same_owner_overlap_pairs
       <<" cross_owner_overlaps="<<interior.cross_owner_overlap_pairs);
  CHECK(interior.convex_route_applicable);
  CHECK_FALSE(interior.transition_tetrahedra.empty());
  CHECK(interior.local_boundary_open_edges<volume.local_boundary_open_edges);
  // Restricting the partition to the requested world root removes the former
  // spurious next-ring owner from a different BCC cube root. The deliberately
  // eroded central control is now locally closed.
  CHECK(interior.locally_open_owners.empty());
  CHECK(interior.refused_closure_components==0U);
  CHECK(interior.same_owner_overlap_pairs==0U);
  CHECK(interior.cross_owner_overlap_pairs==0U);
}

TEST_CASE("two face-sharing tetrahedron children carry one canonical structured DC grid") {
  for(const auto field:{tetra::probes::SandwichField::planar,
                        tetra::probes::SandwichField::perlin_height}) {
    tetra::probes::SandwichConfig config;
    config.resolution=8U;config.field=field;
    const auto first=tetra::probes::extract_structured_two_hex_dual_surface(config);
    const auto repeat=tetra::probes::extract_structured_two_hex_dual_surface(config);
    const auto reversed=tetra::probes::extract_structured_two_hex_dual_surface(config,true);
    INFO("field="<<tetra::probes::sandwich_field_name(field)
         <<" quads="<<first.quads.size()<<" seam="<<first.seam_quads
         <<" columns="<<first.active_logical_columns
         <<" duplicate_columns="<<first.duplicate_active_logical_columns
         <<" edge_axes="<<first.crossed_edge_axis_quads[0]<<','
         <<first.crossed_edge_axis_quads[1]<<','<<first.crossed_edge_axis_quads[2]
         <<" boundary="<<first.boundary_crossed_edges
         <<" residual="<<first.maximum_dual_vertex_field_residual
         <<" surface_min_angle="<<first.minimum_surface_triangle_angle_degrees
         <<" transition="<<first.transition_tetrahedra.size()
         <<" core="<<first.core_tetrahedra.size()
         <<" eroded_core="<<first.eroded_core_tetrahedra.size()
         <<" eroded_boundary="<<first.eroded_core_boundary_faces
         <<" eroded_moat="<<first.eroded_core_moat
         <<" eroded_max_field="<<first.eroded_core_maximum_field_value
         <<" nonpositive="<<first.nonpositive_volume_tetrahedra
         <<" incidence="<<first.volume_face_incidence
         <<" unpaired="<<first.unpaired_internal_volume_faces
         <<" missing_dc="<<first.missing_dc_boundary_triangles
         <<" finite="<<first.validation.finite_vertices
         <<" nondegenerate="<<first.validation.nondegenerate_triangles
         <<" unique="<<first.validation.unique_triangles
         <<" manifold="<<first.validation.manifold_edges
         <<" oriented="<<first.validation.consistently_oriented
         <<" intersections="<<first.validation.strict_triangle_intersections
         <<" cell_contained="<<first.dual_vertices_cell_contained
         <<" out_of_cell="<<first.out_of_cell_dual_vertices
         <<" no_overlap="<<first.no_tetrahedron_overlap
         <<" overlaps="<<first.tetrahedron_overlap_pairs
         <<" transition_overlaps="<<first.transition_overlap_pairs
         <<" same_transition_cell_overlaps="<<first.same_transition_cell_overlap_pairs
         <<" adjacent_transition_cell_overlaps="<<first.adjacent_transition_cell_overlap_pairs
         <<" nonadjacent_transition_cell_overlaps="<<first.nonadjacent_transition_cell_overlap_pairs
         <<" separating_diagonal_faces="<<first.separating_diagonal_faces
         <<" diagnostic_centre_fans="<<first.diagnostic_centre_fan_faces
         <<" nonseparating_fixed_faces="<<first.nonseparating_fixed_faces
         <<" core_overlaps="<<first.core_overlap_pairs
         <<" transition_core_overlaps="<<first.transition_core_overlap_pairs
         <<" boundary_volume="<<first.exact_boundary_volume_agreement
         <<" tet_volume="<<first.tetrahedral_volume
         <<" shell_volume="<<first.boundary_volume
         <<" centroids_bounded="<<first.tetrahedron_centroids_within_surface_error
         <<" centroid_overshoot="<<first.maximum_centroid_field_overshoot
         <<" centroids_beyond="<<first.centroids_beyond_surface_error
         <<" min_mean_ratio="<<first.volume_quality.minimum_mean_ratio
         <<" min_dihedral="<<first.volume_quality.minimum_dihedral_degrees
         <<" below5="<<first.volume_quality.dihedrals_below_5_degrees
         <<" max_dihedral="<<first.volume_quality.maximum_dihedral_degrees
         <<" transition_quality="<<first.transition_volume_quality.minimum_dihedral_degrees
         <<':'<<first.transition_volume_quality.minimum_mean_ratio
         <<" core_quality="<<first.core_volume_quality.minimum_dihedral_degrees
         <<':'<<first.core_volume_quality.minimum_mean_ratio
         <<" worst_transition="<<first.worst_transition_tetrahedron
         <<'@'<<first.transition_tet_addresses[first.worst_transition_tetrahedron].primal_node[0]
         <<','<<first.transition_tet_addresses[first.worst_transition_tetrahedron].primal_node[1]
         <<','<<first.transition_tet_addresses[first.worst_transition_tetrahedron].primal_node[2]
         <<':'<<static_cast<unsigned int>(first.transition_tet_addresses[first.worst_transition_tetrahedron].freudenthal_permutation)
         <<" volume="<<first.complete_volume_valid);
    const auto& worst=first.transition_tetrahedra[first.worst_transition_tetrahedron];
    INFO("worst transition geometry "
         <<first.volume_vertices[worst[0]][0]<<','<<first.volume_vertices[worst[0]][1]<<','<<first.volume_vertices[worst[0]][2]<<" | "
         <<first.volume_vertices[worst[1]][0]<<','<<first.volume_vertices[worst[1]][1]<<','<<first.volume_vertices[worst[1]][2]<<" | "
         <<first.volume_vertices[worst[2]][0]<<','<<first.volume_vertices[worst[2]][1]<<','<<first.volume_vertices[worst[2]][2]<<" | "
         <<first.volume_vertices[worst[3]][0]<<','<<first.volume_vertices[worst[3]][1]<<','<<first.volume_vertices[worst[3]][2]);
    CHECK(first.exact_shared_face_identity);
    CHECK(first.independently_reproduced_shared_face);
    CHECK(first.implicit_address_reconstruction_exact);
    CHECK(first.shared_face_grid_vertices==81U);
    CHECK(first.every_interior_crossing_is_quad);
    CHECK(first.every_quad_has_four_distinct_vertices);
    CHECK(first.seam_quads>0U);
    CHECK(std::isfinite(first.maximum_dual_vertex_field_residual));
    CHECK(first.maximum_dual_vertex_field_residual<0.1);
    CHECK(first.validation.valid);
    CHECK(first.validation.no_strict_triangle_intersections);
    CHECK(first.quads.size()*2U==first.triangles.size());
    CHECK_FALSE(first.transition_tetrahedra.empty());
    CHECK_FALSE(first.core_tetrahedra.empty());
    CHECK(first.positive_volume_tetrahedra);
    CHECK(first.nonpositive_volume_tetrahedra==0U);
    CHECK(first.volume_face_incidence);
    CHECK(first.unpaired_internal_volume_faces==0U);
    CHECK(first.exact_dc_quad_boundary);
    CHECK(first.missing_dc_boundary_triangles==0U);
    CHECK(first.dual_vertices_cell_contained);
    CHECK(first.out_of_cell_dual_vertices==0U);
    CHECK(first.no_tetrahedron_overlap);
    CHECK(first.tetrahedron_overlap_pairs==0U);
    CHECK(first.exact_boundary_volume_agreement);
    CHECK(first.tetrahedral_volume==doctest::Approx(first.boundary_volume));
    CHECK(first.tetrahedron_centroids_within_surface_error);
    CHECK(first.centroids_beyond_surface_error==0U);
    CHECK(first.volume_quality.diagnostic_thresholds_met);
    CHECK(first.complete_volume_valid);
    CHECK(first.grid_vertices==repeat.grid_vertices);
    CHECK(first.grid_edges==repeat.grid_edges);
    CHECK(first.dual_vertices==repeat.dual_vertices);
    CHECK(first.quads==repeat.quads);
    CHECK(first.triangles==repeat.triangles);
    CHECK(first.volume_vertices==repeat.volume_vertices);
    CHECK(first.transition_tetrahedra==repeat.transition_tetrahedra);
    CHECK(first.core_tetrahedra==repeat.core_tetrahedra);
    CHECK(first.volume_vertex_addresses==repeat.volume_vertex_addresses);
    CHECK(first.transition_tet_addresses==repeat.transition_tet_addresses);
    CHECK(first.core_tet_addresses==repeat.core_tet_addresses);
    CHECK(canonical_vertices(first.grid_vertices)==canonical_vertices(reversed.grid_vertices));
    CHECK(canonical_geometry(first.grid_vertices,first.grid_edges)==
          canonical_geometry(reversed.grid_vertices,reversed.grid_edges));
    CHECK(canonical_vertices(first.dual_vertices)==canonical_vertices(reversed.dual_vertices));
    CHECK(canonical_geometry(first.dual_vertices,first.quads)==
          canonical_geometry(reversed.dual_vertices,reversed.quads));
    CHECK(canonical_geometry(first.dual_vertices,first.triangles)==
          canonical_geometry(reversed.dual_vertices,reversed.triangles));
    CHECK(canonical_vertices(first.volume_vertices)==canonical_vertices(reversed.volume_vertices));
    CHECK(canonical_geometry(first.volume_vertices,first.transition_tetrahedra)==
          canonical_geometry(reversed.volume_vertices,reversed.transition_tetrahedra));
    CHECK(canonical_geometry(first.volume_vertices,first.core_tetrahedra)==
          canonical_geometry(reversed.volume_vertices,reversed.core_tetrahedra));
  }
}

TEST_CASE("viewer export distinguishes DC quad edges from render diagonals") {
  tetra::probes::SandwichConfig config;
  config.resolution=8U;
  const auto data=tetra::probes::make_sandwich_viewer_data(config);
  CHECK(data.find("\"buildRevision\":\"structured-two-hex-global-core-v5\"")!=std::string::npos);
  CHECK(data.find("\"surfaceAuthorityRevision\":\"frozen-before-volume-v1\"")!=std::string::npos);
  CHECK(data.find("\"dualQuads\":[")!=std::string::npos);
  CHECK(data.find("\"dualQuadEdges\":[")!=std::string::npos);
  CHECK(data.find("\"renderDiagonalEdges\":[")!=std::string::npos);
  CHECK(data.find("\"exactSharedFaceIdentity\":true")!=std::string::npos);
  CHECK(data.find("\"independentlyReproducedSharedFace\":true")!=std::string::npos);
  CHECK(data.find("\"implicitAddressReconstructionExact\":true")!=std::string::npos);
  CHECK(data.find("\"globalCoreUniqueOwnership\":true")!=std::string::npos);
  CHECK(data.find("\"everyInteriorCrossingIsQuad\":true")!=std::string::npos);
  CHECK(data.find("\"transitionConstructed\":false")!=std::string::npos);
  CHECK(data.find("\"dualVerticesCellContained\":true")!=std::string::npos);
  CHECK(data.find("\"noTetrahedronOverlap\":true")!=std::string::npos);
  CHECK(data.find("\"exactBoundaryVolumeAgreement\":true")!=std::string::npos);
  CHECK(data.find("\"centroidsWithinSurfaceError\":true")!=std::string::npos);
  CHECK(data.find("\"volumeQualityScreenPassed\":true")!=std::string::npos);
  CHECK(data.find("\"completeVolumeValid\":false")!=std::string::npos);
  CHECK(data.find("\"transitionEdges\":[]")!=std::string::npos);
  CHECK(data.find("\"implicitCoreEdges\":[]")==std::string::npos);
}

TEST_CASE("frozen structured DC and global hierarchy core form a generic PLC request") {
  for(const auto field:{tetra::probes::SandwichField::planar,
                        tetra::probes::SandwichField::perlin_height}) {
    tetra::probes::SandwichConfig config;config.resolution=8U;config.field=field;
    const auto request=tetra::probes::make_structured_two_hex_terrain_volume_request(config);
    INFO("field="<<tetra::probes::sandwich_field_name(field)
         <<" failure="<<static_cast<unsigned int>(request.failure)
         <<" validation_failure="<<static_cast<unsigned int>(request.validation.failure)
         <<" failing="<<request.validation.failing_element
         <<" related="<<request.validation.related_element
         <<" outer="<<request.request.contract.outer_faces.size()
         <<" core="<<request.request.contract.retained_core_tetrahedra.size());
    REQUIRE(request.accepted());
    CHECK(request.validation.accepted);
    CHECK(request.request.frozen_dc_faces>0U);
    CHECK(request.request.artificial_closure_faces>0U);
    CHECK(request.request.explicit_local_core_tetrahedra>0U);
    const auto manifest=tetra::probes::build_terrain_volume_plc_manifest(request.request);
    REQUIRE(manifest.accepted());
    const auto constraints=tetra::probes::materialize_canonical_plc_constraints(manifest);
    INFO("constraints failure="<<static_cast<unsigned int>(constraints.failure)
         <<" vertices="<<constraints.constraints.vertices.size()
         <<" facets="<<constraints.constraints.facets.size());
    REQUIRE(constraints.accepted());
    const auto inspection=tetra::probes::inspect_canonical_plc_constraints(
        constraints.constraints,1U<<18U);
    INFO("seed failure="<<static_cast<unsigned int>(inspection.failure)
         <<" seed reason="<<static_cast<unsigned int>(inspection.seed_invalid_reason)
         <<" stellar="<<inspection.stellar_fallback_used
         <<" hull_faces="<<inspection.diagnostic_hull_faces
         <<" cone_cells="<<inspection.diagnostic_cone_cells
         <<" witness="<<inspection.invalid_hull_witness
         <<" witness_id="<<(inspection.invalid_hull_witness<constraints.constraints.vertices.size()
             ?constraints.constraints.vertices[inspection.invalid_hull_witness].id:0U)
         <<" witness_position="<<(inspection.invalid_hull_witness<constraints.constraints.vertices.size()
             ?constraints.constraints.vertices[inspection.invalid_hull_witness].position.x:0.0)<<','
         <<(inspection.invalid_hull_witness<constraints.constraints.vertices.size()
             ?constraints.constraints.vertices[inspection.invalid_hull_witness].position.y:0.0)<<','
         <<(inspection.invalid_hull_witness<constraints.constraints.vertices.size()
             ?constraints.constraints.vertices[inspection.invalid_hull_witness].position.z:0.0)
         <<" candidates="<<inspection.candidate_tetrahedra
         <<" recovered facets="<<inspection.recovered_facets<<'/'<<inspection.required_facets
         <<" recovered edges="<<inspection.recovered_edges<<'/'<<inspection.required_edges);
    CHECK(inspection.failure!=tetra::probes::CanonicalPlcSeedFailure::rejected_manifest);
    tetra::probes::CanonicalPlcRecoveryOptions recovery_options;
    recovery_options.maximum_vertices=1U<<14U;
    recovery_options.maximum_facets=1U<<15U;
    recovery_options.maximum_tetrahedra=1U<<18U;
    recovery_options.maximum_edge_splits=64U;
    const auto recovery=tetra::probes::recover_canonical_plc_edges(
        constraints.constraints,recovery_options);
    INFO("recovery failure="<<static_cast<unsigned int>(recovery.failure)
         <<" seed_tets="<<inspection.candidate_tetrahedra
         <<" attempted_edges="<<recovery.attempted_edge_recoveries
         <<" accepted_edges="<<recovery.accepted_edge_recoveries
         <<" edge_flips="<<recovery.edge_flips
         <<" edge_splits="<<recovery.edge_splits
         <<" first_edge="<<recovery.first_unrecovered_edge[0]<<','<<recovery.first_unrecovered_edge[1]
         <<" first_edge_core="<<recovery.first_unrecovered_edge_is_core
         <<" edge_failure="<<static_cast<unsigned int>(recovery.last_edge_failure)
         <<" cavity_cells="<<recovery.last_cavity_cell_count
         <<" retriangulation_trials="<<recovery.last_retriangulation_trials
         <<" split_ratio="<<recovery.last_split_numerator<<'/'<<recovery.last_split_denominator
         <<" split_insert_failure="<<static_cast<unsigned int>(recovery.last_split_insertion_failure)
         <<" edges_ready="<<recovery.edges_recovered_before_facet_stage
         <<" missing_edges="<<recovery.inspection.missing_edges.size()
         <<" missing_facets="<<recovery.inspection.missing_facets.size()
         <<" facet_attempts="<<recovery.two_sided_facet_attempts
         <<" facets_recovered="<<recovery.two_sided_facets_recovered
         <<" last_facet_failure="<<static_cast<unsigned int>(recovery.last_two_sided_facet_failure));
    CHECK(recovery.failure!=tetra::probes::CanonicalPlcRecoveryFailure::materialization_failed);
    CHECK(recovery.failure!=tetra::probes::CanonicalPlcRecoveryFailure::seed_failed);
    const auto volume=tetra::probes::construct_canonical_plc_volume(
        manifest,recovery_options);
    INFO("volume failure="<<static_cast<unsigned int>(volume.failure)
         <<" validation="<<static_cast<unsigned int>(volume.validation.failure)
         <<" shell="<<volume.shell_tetrahedra
         <<" core="<<volume.core_tetrahedra
         <<" min_dihedral="<<volume.minimum_dihedral_degrees
         <<" max_dihedral="<<volume.maximum_dihedral_degrees);
    CHECK(volume.failure!=tetra::probes::CanonicalPlcVolumeFailure::seed_failed);
  }
}
