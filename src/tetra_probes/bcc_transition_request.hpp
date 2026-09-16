#pragma once

#include "tetra_probes/sandwich_probe.hpp"
#include "tetra_probes/surface_core_contract.hpp"

#include <cstddef>
#include <cstdint>
#include <compare>
#include <vector>

namespace tetra::probes {

// Canonical finite adapter between the two authoritative BCC fronts and the
// generic in-house PLC contract.  Only hierarchy owners incident to the
// exposed interface are materialized; all deeper owners remain address-only.
enum class BccTransitionRequestFailure : std::uint8_t {
  none,
  invalid_surface,
  incompatible_hierarchy,
  unsupported_address_depth,
  malformed_interface,
  stable_id_collision,
  invalid_closed_contract,
};

struct BccSurfaceCoreTransitionRequest {
  SurfaceCoreTransitionInput input;
  SurfaceCoreTransitionContract validation;
  std::vector<WorldTetAddress> materialized_interface_owners;
  std::size_t frozen_surface_faces{};
  std::size_t curtain_faces{};
  std::size_t finite_cap_faces{};
  std::size_t hierarchy_interface_faces{};
  std::size_t implicit_far_core_owners{};
  BccTransitionRequestFailure failure{
      BccTransitionRequestFailure::invalid_surface};

  [[nodiscard]] bool accepted() const noexcept {
    return failure==BccTransitionRequestFailure::none&&validation.accepted;
  }
};

struct BccTransitionOwnerPartition {
  std::size_t surface_owners{};
  std::size_t interface_owners{};
  std::size_t exact_shared_owners{};
  std::size_t parent_surface_owners{};
  std::size_t parent_interface_owners{};
  std::size_t shared_parent_owners{};
  std::size_t maximum_surface_faces_per_parent{};
  std::size_t maximum_interface_faces_per_parent{};
  bool deterministic_bounded_groups{};
};

struct BccTransitionParentPatch {
  WorldTetAddress parent{};
  std::vector<std::uint32_t> surface_triangles;
  std::vector<std::uint32_t> interface_faces;
  std::vector<std::array<std::uint64_t,2>> surface_boundary_edges;
  std::vector<std::array<WorldVertexKey,2>> interface_boundary_edges;
  std::size_t surface_boundary_components{};
  std::size_t interface_boundary_components{};
  bool surface_boundary_is_cycles{};
  bool interface_boundary_is_cycles{};
};

struct BccTransitionParentPartition {
  std::vector<BccTransitionParentPatch> patches;
  std::size_t surface_triangles{};
  std::size_t interface_faces{};
  std::size_t shared_surface_seams{};
  std::size_t shared_interface_seams{};
  std::size_t two_front_patches{};
  std::size_t single_loop_two_front_patches{};
  bool exact_partition{};
  bool canonical_shared_seams{};
};

struct BccParentStarConeProbe {
  std::size_t two_front_patches{};
  std::size_t single_loop_patches{};
  std::size_t side_walls_accepted{};
  std::size_t closed_boundaries{};
  std::size_t star_shaped_boundaries{};
  std::size_t candidate_tetrahedra{};
  std::size_t maximum_boundary_faces{};
  std::vector<WorldTetAddress> successful_parents;
  struct ClosedBoundary {
    WorldTetAddress owner{};
    std::vector<FrozenFacetVertex> vertices;
    std::vector<std::array<std::uint64_t,3>> faces;
    bool star_shaped{};
  };
  std::vector<ClosedBoundary> closed_patch_boundaries;
};

// First stage of the BCC-scaffolded constructor.  Each exact DC triangle is
// clipped by the uniform addressed hierarchy tetrahedra.  The resulting
// coplanar fragments retain barycentric coordinates on their immutable DC
// parent and identify the removed hierarchy cell that owns the material-side
// transition work.
struct BccScaffoldSurfaceTriangle {
  WorldTetAddress owner{};
  std::uint32_t source_triangle{};
  std::array<std::array<double,3>,3> positions{};
  std::array<std::array<double,3>,3> source_barycentrics{};
  std::array<std::uint32_t,3> canonical_vertex_indices{};
};

// An arrangement vertex is identified by the lowest-dimensional feature of
// each input complex that contains it.  This is traversal- and chunk-order
// independent: for example, an intersection of a DC edge and a BCC face has
// the two global DC endpoint indices and the three addressed BCC face keys.
// A pre-existing DC vertex deliberately carries no BCC feature, so it keeps
// one identity even when it lies exactly on a BCC face or edge.
struct BccScaffoldVertexKey {
  std::uint8_t surface_vertex_count{};
  std::uint8_t bcc_vertex_count{};
  std::array<std::uint32_t,3> surface_vertices{};
  std::array<WorldVertexKey,3> bcc_vertices{};
  auto operator<=>(const BccScaffoldVertexKey&) const =default;
};

struct BccScaffoldVertex {
  BccScaffoldVertexKey key{};
  std::array<double,3> position{};
  friend bool operator==(const BccScaffoldVertex&,const BccScaffoldVertex&)=default;
};

struct BccScaffoldSurfacePartition {
  std::vector<BccScaffoldSurfaceTriangle> triangles;
  std::vector<BccScaffoldVertex> vertices;
  std::vector<WorldTetAddress> cut_owners;
  // Cut owners touched by the artificial boundary of the finite DC patch.
  // Their material region is not defined by the surface fragments plus the
  // four hierarchy faces alone; a halo or explicit domain closure is needed.
  std::vector<WorldTetAddress> source_boundary_owners;
  std::size_t source_triangles{};
  std::size_t covered_source_triangles{};
  std::size_t duplicate_coplanar_fragments{};
  std::size_t maximum_fragments_per_source{};
  std::size_t canonical_edges{};
  std::size_t shared_owner_edges{};
  std::size_t boundary_edges{};
  double source_area{};
  double fragment_area{};
  double area_error{};
  bool finite{};
  bool barycentrics_valid{};
  bool canonical_keys_valid{};
  bool canonical_positions_consistent{};
  bool canonical_edge_incidence{};
  bool source_boundary_preserved{};
  bool exact_coverage{};
  bool disjoint_from_retained_core{};
  bool valid{};
};

// Bounded viability audit for the first local construction: if all exact DC
// facets in an owner support one convex material polyhedron, that polyhedron
// can be triangulated on its BCC faces and coned without a general cavity
// tetrahedralizer. Rejected owners identify where local decomposition is
// genuinely required; they are never silently omitted.
struct BccScaffoldConvexCellReport {
  std::size_t cut_owners{};
  std::size_t owners_with_inside_vertex{};
  std::size_t convex_material_owners{};
  std::size_t nonconvex_material_owners{};
  std::size_t surface_fragments{};
  std::size_t supporting_surface_fragments{};
  std::size_t maximum_material_vertices{};
  bool partition_valid{};
  bool complete_convex_route{};
};

struct BccScaffoldTransitionVolume {
  std::vector<std::array<double,3>> vertices;
  std::vector<std::array<std::uint32_t,4>> transition_tetrahedra;
  std::vector<WorldTetAddress> transition_tetrahedron_owners;
  std::vector<std::array<std::uint32_t,4>> retained_core_tetrahedra;
  std::vector<WorldTetAddress> retained_core_tetrahedron_owners;
  std::vector<std::array<std::uint32_t,3>> exact_surface_triangles;
  std::vector<std::array<std::uint32_t,3>> finite_closure_triangles;
  std::vector<WorldTetAddress> locally_open_owners;
  std::vector<std::array<double,6>> unmatched_surface_edge_geometry;
  std::vector<std::array<double,6>> unmatched_hierarchy_edge_geometry;
  std::size_t cut_owner_tetrahedra{};
  std::size_t eroded_full_tetrahedra{};
  std::size_t local_boundary_open_edges{};
  std::size_t unmatched_surface_edges{};
  std::size_t unmatched_bcc_face_edges{};
  std::size_t geometrically_matching_open_edge_pairs{};
  std::size_t surface_edges_contained_in_hierarchy_edges{};
  std::size_t hierarchy_edges_contained_in_surface_edges{};
  std::size_t open_surface_edges_on_hierarchy_faces{};
  std::size_t open_boundary_components{};
  std::size_t open_boundary_bad_degree_vertices{};
  std::size_t maximum_open_boundary_vertices{};
  std::size_t closure_components{};
  std::size_t refused_closure_components{};
  std::size_t closure_triangles{};
  std::size_t overlap_candidates{};
  std::size_t strict_overlap_pairs{};
  std::size_t same_owner_overlap_pairs{};
  std::size_t cross_owner_overlap_pairs{};
  std::size_t unpaired_non_domain_faces{};
  std::size_t chunk_interface_faces{};
  std::size_t same_sided_shared_faces{};
  double tetrahedron_volume{};
  double boundary_volume{};
  double volume_error{};
  double minimum_dihedral_degrees{180.0};
  double maximum_dihedral_degrees{};
  std::size_t retained_bytes{};
  bool convex_route_applicable{};
  bool positive{};
  bool exact_surface_preserved{};
  bool face_incidence_valid{};
  bool exact_volume{};
  bool no_overlap_by_scaffold_partition{};
  bool valid{};
};

[[nodiscard]] BccScaffoldSurfacePartition
partition_bcc_surface_over_transition_scaffold(
    const BccHierarchyDualSurface& surface,
    const FrozenBccHierarchyCore& core);

// Clips the corrected structured two-hexahedra DC sheet against the genuine
// root-zero WorldTetAddress hierarchy. The calculation is performed in the
// canonical hierarchy frame and mapped back through the parent/root affine
// transform, so hexahedron-local coordinates never define transition cells.
[[nodiscard]] BccScaffoldSurfacePartition
partition_structured_surface_over_global_core(
    const StructuredTwoHexDualSurface& surface);

[[nodiscard]] BccScaffoldConvexCellReport
inspect_structured_global_cut_cells(
    const SandwichConfig& config,
    const StructuredTwoHexDualSurface& surface);

// Fills the material portion of every cut global hierarchy tet while leaving
// all retained hierarchy tets untouched. Transition ownership is inherited
// from the WorldTetAddress owner, never recomputed from a hexahedron-local
// cell, so independently generated hexahedra cannot duplicate border cells.
[[nodiscard]] BccScaffoldTransitionVolume
construct_structured_global_cut_transition(
    const SandwichConfig& config,
    const StructuredTwoHexDualSurface& surface);

// Diagnostic subset used to qualify the actual shared-chunk construction
// independently of the finite viewer window: owners touched by the source
// sheet's artificial outer boundary are omitted, never reported as filled.
[[nodiscard]] BccScaffoldTransitionVolume
construct_structured_global_interior_cut_transition(
    const SandwichConfig& config,
    const StructuredTwoHexDualSurface& surface);

[[nodiscard]] BccScaffoldConvexCellReport
inspect_bcc_scaffold_convex_material_cells(
    const SandwichConfig& config,
    const BccHierarchyDualSurface& surface,
    const FrozenBccHierarchyCore& core);

[[nodiscard]] BccScaffoldTransitionVolume
construct_bcc_scaffold_convex_transition(
    const SandwichConfig& config,
    const BccHierarchyDualSurface& surface,
    const FrozenBccHierarchyCore& core);

[[nodiscard]] std::uint64_t bcc_transition_surface_vertex_id(
    BccHexCellAddress address);
[[nodiscard]] std::uint64_t bcc_transition_hierarchy_vertex_id(
    WorldVertexKey key);

[[nodiscard]] BccTransitionParentPartition partition_bcc_transition_parent_stars(
    const BccHierarchyDualSurface& surface,
    const FrozenBccHierarchyCore& core);
[[nodiscard]] BccTransitionParentPartition partition_bcc_transition_stars(
    const BccHierarchyDualSurface& surface,
    const FrozenBccHierarchyCore& core,
    unsigned int grouping_depth);

// Dependency-free bounded control for the simple parent stars.  It zips the
// two unequal boundary cycles, proves a closed face ledger, solves the common
// face-halfspace kernel, and cones the boundary.  The candidates are not a
// complete volume until neighboring multi-component stars share the same
// side complexes.
[[nodiscard]] BccParentStarConeProbe probe_bcc_parent_star_cones(
    const BccHierarchyDualSurface& surface,
    const FrozenBccHierarchyCore& core);
[[nodiscard]] BccParentStarConeProbe probe_bcc_transition_star_cones(
    const BccHierarchyDualSurface& surface,
    const FrozenBccHierarchyCore& core,
    unsigned int grouping_depth);

// Measures whether the free band can be scheduled by the current leaf owner
// or needs the one-red-generation parent star as its smallest common unit.
[[nodiscard]] BccTransitionOwnerPartition inspect_bcc_transition_owner_partition(
    const BccHierarchyDualSurface& surface,
    const FrozenBccHierarchyCore& core);

// The finite envelope consists of the exact DC sheet, a deterministic skirt
// around its directed boundary, and a topologically identical cap below the
// unit root complex.  The cap/skirt are research-fixture closure only; the
// frozen DC triangles and hierarchy interface are never moved.
[[nodiscard]] BccSurfaceCoreTransitionRequest
make_bcc_surface_core_transition_request(
    const BccHierarchyDualSurface& surface,
    const FrozenBccHierarchyCore& core,
    double skirt_expansion=0.125,
    double cap_clearance=0.25);

[[nodiscard]] const char* bcc_transition_request_failure_name(
    BccTransitionRequestFailure failure);

} // namespace tetra::probes
