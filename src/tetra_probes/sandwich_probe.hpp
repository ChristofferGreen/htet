#pragma once

#include "tetra_core/world_hierarchy.hpp"
#include "tetra_probes/surface_core_contract.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tetra::probes {

enum class SandwichField : std::uint8_t { planar, perlin_height };

struct SandwichConfig {
  unsigned int resolution{8U};
  SandwichField field{SandwichField::perlin_height};
  double amplitude{0.14};
  double frequency{1.75};
  double phase_x{0.23};
  double phase_y{0.41};
};

struct SandwichQuality {
  double minimum_normalized_volume{};
  double minimum_mean_ratio{};
  double minimum_scaled_jacobian{};
  double minimum_dihedral_degrees{};
  double maximum_dihedral_degrees{};
  double maximum_edge_ratio{};
  double percentile1_mean_ratio{};
  double percentile5_mean_ratio{};
  std::size_t slivers_below_mean_ratio_001{};
  std::size_t elements_below_mean_ratio_01{};
  std::size_t dihedrals_below_1_degree{};
  std::size_t dihedrals_below_5_degrees{};
  std::size_t dihedrals_above_175_degrees{};
  // This is a deliberately conservative diagnostic screen, not a claim that
  // an eventual collision or FEM solver is safe at these values.
  bool diagnostic_thresholds_met{};
};

// Quality of the immutable DC triangles before they become PLC constraints.
// The shape quality is 4*sqrt(3)*area / sum(edge_length^2), i.e. 1 for an
// equilateral triangle and 0 for a degenerate one.
struct FrozenSurfaceQuality {
  double minimum_triangle_angle_degrees{};
  double maximum_triangle_angle_degrees{};
  double minimum_shape_quality{};
  double maximum_edge_ratio{};
  std::size_t triangles_below_1_degree{};
  std::size_t triangles_below_5_degrees{};
  std::size_t triangles_below_shape_quality_001{};
  // A PLC may be topologically valid while this remains false.  These are
  // screening thresholds for this research corpus, not physics guarantees.
  bool diagnostic_thresholds_met{};
};

// Evidence that the DC Hermite samples came from the canonical bounded root
// policy, rather than an interpolation of endpoint field values.  The counts
// intentionally include a sample once for every incident active cell: that is
// the work performed by the mass-point builder and makes a chunk report
// independently auditable.
struct HermiteCrossingQuality {
  std::size_t sign_changing_edges{};
  std::size_t exact_zero_endpoint_roots{};
  std::size_t bracketed_roots{};
  std::size_t maximum_bisection_iterations{};
  double maximum_absolute_field_residual{};
  // Final bracket length divided by the original edge length.  Exact endpoint
  // roots report zero; bracketed roots are bounded by 2^-iterations.
  double maximum_bracket_fraction{};
  bool deterministic_bounded_policy{};
};

struct SandwichStorage {
  std::size_t vertices{};
  std::size_t tetrahedra{};
  std::size_t core_tetrahedra{};
  std::size_t transition_tetrahedra{};
  std::size_t surface_triangles{};
  std::size_t explicit_live_bytes{};
  std::size_t explicit_core_bytes{};
  std::size_t transition_bytes{};
  std::size_t implicit_core_descriptor_bytes{};
};

struct SandwichTimings {
  double field_and_surface_ms{};
  double count_ms{};
  double scan_ms{};
  double emit_ms{};
  double validation_ms{};
  double total_ms{};
};

struct SandwichValidation {
  bool finite_distinct_tetrahedra{};
  bool unique_tetrahedra{};
  bool nondegenerate_background_tetrahedra{};
  bool source_containment{};
  bool no_tetrahedron_overlap{};
  bool positive_tetrahedra{};
  bool face_incidence{};
  bool frozen_surface_preserved{};
  bool artificial_boundary_only{};
  bool sampled_partition{};
  bool surface_manifold{};
  bool valid{};
  std::size_t nonmanifold_faces{};
  std::size_t unmatched_non_surface_faces{};
  std::size_t sampled_gaps{};
  std::size_t sampled_overlaps{};
  std::size_t duplicate_tetrahedra{};
  std::size_t source_containment_failures{};
  std::size_t tetrahedron_overlap_pairs{};
};

struct SandwichBuildReport {
  SandwichStorage storage;
  SandwichQuality quality;
  SandwichTimings timings;
  SandwichValidation validation;
  std::uint64_t surface_hash{};
  std::uint64_t tetrahedron_hash{};
};

struct DualContourValidation {
  bool finite_vertices{};
  bool nondegenerate_triangles{};
  bool unique_triangles{};
  bool manifold_edges{};
  bool consistently_oriented{};
  // Edge incidence does not prove a triangle sheet is an embedded surface.
  // This catches strict intersections between non-adjacent triangles before a
  // caller freezes the sheet as a PLC boundary.
  bool no_strict_triangle_intersections{};
  bool valid{};
  std::size_t boundary_edges{};
  std::size_t nonmanifold_edges{};
  std::size_t degenerate_triangles{};
  std::size_t duplicate_triangles{};
  std::size_t strict_triangle_intersections{};
};

// Exact frozen output of the existing bounded dual-contouring producer. This
// is intentionally surface-only: callers must supply their own explicit core
// and may not mistake the historical extruded-prism diagnostic for a volume.
struct SharedLatticePrimalEdgeOwner {
  std::array<std::uint32_t,3> lower_lattice_vertex{};
  std::uint8_t axis{};
  friend constexpr bool operator==(const SharedLatticePrimalEdgeOwner&,
                                   const SharedLatticePrimalEdgeOwner&)=default;
  friend constexpr auto operator<=>(const SharedLatticePrimalEdgeOwner&,
                                    const SharedLatticePrimalEdgeOwner&)=default;
};
struct FrozenDualContourSurface {
  unsigned int lattice_resolution{};
  std::vector<std::uint64_t> stable_vertex_ids;
  std::vector<std::array<double,3>> vertices;
  std::vector<std::array<std::uint32_t,3>> triangles;
  // Parallel to `triangles`.  The two triangles emitted for a DC quad carry
  // the same source primal edge, including its axis.  This is the canonical
  // local owner used by a lattice zipper; it survives rigid transforms and
  // avoids reconstructing ownership from floating-point projection.
  std::vector<SharedLatticePrimalEdgeOwner> triangle_primal_edge_owners;
  // Directed according to the frozen sheet winding. Each appears exactly once
  // in the triangle ledger and is the sole authority for artificial curtains.
  std::vector<std::array<std::uint32_t,2>> boundary_edges;
  DualContourValidation validation;
};

// The explicit near-core selection paired with a frozen DC sheet.  These are
// wholly-material Freudenthal tetrahedra addressed by the original lattice;
// the unselected far lattice remains implicit by construction.
struct FrozenRegularCore {
  unsigned int lattice_resolution{};
  std::vector<std::uint64_t> stable_vertex_ids;
  std::vector<std::array<double,3>> vertices;
  std::vector<std::array<std::uint32_t,4>> tetrahedra;
  // Faces on the selected outer interface of this independently generated
  // regular volume.  These index `vertices`, use the regular-grid
  // triangulation, and are the only faces a transition constructor may
  // locally refine.  An empty list means that a legacy selector did not
  // designate one particular side of its closed boundary.
  std::vector<std::array<std::uint32_t,3>> interface_triangles;
  // Parallel to `interface_triangles`: minimum (i,j) lattice coordinate of
  // the Freudenthal square containing the interface face.
  std::vector<std::array<std::uint32_t,2>> interface_square_owners;
};

// The actual background volume used by the pre-atmosphere prototype.  Core
// geometry is deliberately absent: each logical owner is a root/depth/base-8
// address whose four corners are reconstructed by world_tetrahedron_geometry.
// Only the active cut and its finite transition boundary are retained.
struct FrozenBccHierarchyCore {
  unsigned int red_depth{};
  std::vector<WorldTetAddress> logical_owners;
  std::vector<std::array<WorldVertexKey,3>> interface_faces;
  // Parallel canonical owner ledger for hierarchy-local transition work.
  std::vector<WorldTetAddress> interface_face_owners;
};

struct BccHexCellAddress {
  WorldTetAddress owner{};
  std::uint8_t local_hexahedron{};
  auto operator<=>(const BccHexCellAddress&) const = default;
};

// Dual-contouring surface generated on the four-hexahedra complex derived
// directly from a uniform BCC hierarchy cut.  Hex edges are retained only as
// probe/viewer evidence; the core tetrahedra remain address-reconstructed.
struct BccHierarchyDualSurface {
  unsigned int red_depth{};
  std::size_t source_tetrahedra{};
  std::size_t source_hexahedra{};
  std::vector<BccHexCellAddress> vertex_owners;
  std::vector<std::array<double,3>> vertices;
  std::vector<std::array<std::uint32_t,3>> triangles;
  // Canonical hierarchy owner of each final triangle, derived solely from
  // its incident hex-cell addresses after all diagonal/orientation repairs.
  // This is the partition key for hierarchy-local transition work.
  std::vector<WorldTetAddress> triangle_owners;
  std::vector<std::array<double,6>> active_hexahedron_edges;
  // Number of active primal edges by incident dual-cell ring size. A regular
  // Cartesian DC grid uses valence four; other nonzero bins identify coarse
  // multi-hexahedron singularities where the surface is not quad-only.
  std::array<std::size_t,9> active_ring_valences{};
  DualContourValidation validation;
};

// Minimal corrected experiment: two face-sharing children of one tetrahedron,
// each carrying an ordinary structured hexahedral grid.  `quads` is the
// authoritative DC topology; `triangles` is only its render triangulation.
struct StructuredTwoHexTetAddress {
  std::array<std::uint32_t,3> primal_node{};
  std::uint8_t freudenthal_permutation{};
  auto operator<=>(const StructuredTwoHexTetAddress&) const = default;
};

struct StructuredTwoHexDualSurface {
  unsigned int resolution{};
  std::array<std::array<double,3>,4> parent_tetrahedron{};
  std::array<std::array<std::array<double,3>,8>,2> parent_hexahedra{};
  std::vector<std::array<double,3>> grid_vertices;
  std::vector<std::array<std::uint32_t,2>> grid_edges;
  std::vector<std::array<double,3>> dual_vertices;
  std::vector<std::array<std::uint32_t,3>> dual_vertex_addresses;
  std::vector<std::array<std::uint32_t,4>> quads;
  std::vector<std::array<std::uint32_t,3>> triangles;
  // One reconstructible dual-grid point per structured primal cell. Material
  // dual hexahedra are split with the canonical six-tet Freudenthal pattern.
  std::vector<std::array<double,3>> volume_vertices;
  std::vector<std::array<std::uint32_t,3>> volume_vertex_addresses;
  std::vector<std::array<std::uint32_t,4>> transition_tetrahedra;
  std::vector<std::array<std::uint32_t,4>> core_tetrahedra;
  // Authoritative background core from the pre-atmosphere world hierarchy.
  // Unlike `volume_vertices`/`core_tetrahedra` above, these coordinates are
  // reconstructed only from a WorldTetAddress and the parent-tetrahedron
  // root transform.  Hexahedra merely own whole tets for generation; they do
  // not contribute to vertex placement or clip tets at their shared face.
  unsigned int global_core_red_depth{};
  std::vector<std::array<double,3>> global_core_vertices;
  std::vector<WorldVertexKey> global_core_vertex_addresses;
  std::vector<std::array<std::uint32_t,4>> global_core_tetrahedra;
  std::vector<WorldTetAddress> global_core_tet_addresses;
  std::vector<std::uint8_t> global_core_hexahedron_owners;
  std::size_t global_core_shared_border_crossing_tetrahedra{};
  bool has_dc_ghost_halo{};
  std::size_t frozen_dc_vertex_prefix{};
  std::size_t frozen_dc_quad_prefix{};
  std::size_t frozen_dc_triangle_prefix{};
  // Unchanged address-reconstructible Freudenthal tetrahedra selected wholly
  // behind an SDF-space moat. This is the active nonmatching PLC core input;
  // `core_tetrahedra` above remains the rejected logical-shift diagnostic.
  std::vector<std::array<std::uint32_t,4>> eroded_core_tetrahedra;
  std::vector<StructuredTwoHexTetAddress> eroded_core_tet_addresses;
  std::vector<StructuredTwoHexTetAddress> transition_tet_addresses;
  std::vector<StructuredTwoHexTetAddress> core_tet_addresses;
  std::size_t shared_face_grid_vertices{};
  std::size_t crossed_primal_edges{};
  std::size_t boundary_crossed_edges{};
  std::size_t seam_quads{};
  std::size_t eroded_core_boundary_faces{};
  double eroded_core_moat{};
  double eroded_core_maximum_field_value{};
  std::size_t active_logical_columns{};
  std::size_t duplicate_active_logical_columns{};
  std::array<std::size_t,3> crossed_edge_axis_quads{};
  double maximum_dual_vertex_field_residual{};
  double minimum_surface_triangle_angle_degrees{180.0};
  bool exact_shared_face_identity{};
  bool independently_reproduced_shared_face{};
  bool implicit_address_reconstruction_exact{};
  bool global_core_address_reconstruction_exact{};
  bool global_core_unique_ownership{};
  bool qef_surface_placement{};
  bool every_interior_crossing_is_quad{};
  bool every_quad_has_four_distinct_vertices{};
  bool deterministic{};
  bool positive_volume_tetrahedra{};
  bool volume_face_incidence{};
  bool exact_dc_quad_boundary{};
  bool dual_vertices_cell_contained{};
  bool no_tetrahedron_overlap{};
  bool exact_boundary_volume_agreement{};
  bool tetrahedron_centroids_within_surface_error{};
  bool complete_volume_valid{};
  std::size_t nonpositive_volume_tetrahedra{};
  std::size_t unpaired_internal_volume_faces{};
  std::size_t missing_dc_boundary_triangles{};
  std::size_t out_of_cell_dual_vertices{};
  std::size_t tetrahedron_overlap_pairs{};
  std::size_t transition_overlap_pairs{};
  std::size_t same_transition_cell_overlap_pairs{};
  std::size_t adjacent_transition_cell_overlap_pairs{};
  std::size_t nonadjacent_transition_cell_overlap_pairs{};
  std::size_t separating_diagonal_faces{};
  std::size_t diagnostic_centre_fan_faces{};
  std::size_t nonseparating_fixed_faces{};
  std::size_t worst_transition_tetrahedron{};
  std::size_t core_overlap_pairs{};
  std::size_t transition_core_overlap_pairs{};
  std::size_t centroids_beyond_surface_error{};
  double maximum_centroid_field_overshoot{};
  double tetrahedral_volume{};
  double boundary_volume{};
  SandwichQuality volume_quality;
  SandwichQuality transition_volume_quality;
  SandwichQuality core_volume_quality;
  DualContourValidation validation;
};

// Ordinary Cartesian-topology DC ghost cells around the two displayed
// hexahedra. The authoritative two-hexahedron vertices/quads/triangles are
// retained as an exact prefix; halo cells only supply neighboring dual cells
// needed to close chunk-owned primal-edge rings.
struct StructuredTwoHexDcHalo {
  StructuredTwoHexDualSurface surface;
  unsigned int halo_cells{};
  std::size_t original_vertices{};
  std::size_t original_quads{};
  std::size_t original_triangles{};
  std::size_t halo_vertices{};
  std::size_t halo_quads{};
  std::size_t halo_triangles{};
  std::size_t incomplete_outer_rings{};
  std::size_t original_active_mismatches{};
  std::size_t missing_original_quads{};
  bool original_surface_preserved{};
  bool every_complete_ring_is_quad{};
  bool valid{};
};

enum class SharedLatticeOwnershipFailure : std::uint8_t {
  none, invalid_input, resolution_mismatch, incomplete_dc_quad,
  incomplete_core_square, missing_vertical_partner,
};
struct SharedLatticeOwnershipReport {
  SharedLatticeOwnershipFailure failure{SharedLatticeOwnershipFailure::invalid_input};
  unsigned int resolution{};
  std::size_t dc_primal_edges{};
  std::size_t dc_vertical_edges{};
  std::size_t dc_horizontal_edges{};
  std::size_t core_interface_squares{};
  std::size_t paired_vertical_squares{};
  std::size_t unpaired_core_squares{};
  bool exact_two_triangles_per_dc_quad{};
  bool exact_two_triangles_per_core_square{};
  bool deterministic_local_ownership{};
  [[nodiscard]] bool accepted() const noexcept {
    return failure==SharedLatticeOwnershipFailure::none;
  }
};

enum class SharedLatticeLocalTransitionFailure : std::uint8_t {
  none, invalid_input, missing_core_vertex, mismatched_interface_face,
  ambiguous_boundary_column, nonpositive_tetrahedron, invalid_face_incidence,
  overlapping_tetrahedra,
};
struct SharedLatticeLocalTransitionReport {
  SharedLatticeLocalTransitionFailure failure{SharedLatticeLocalTransitionFailure::invalid_input};
  std::vector<std::uint64_t> stable_vertex_ids;
  std::vector<std::array<double,3>> vertices;
  std::vector<std::array<std::uint64_t,4>> transition_tetrahedra;
  std::vector<std::array<std::uint64_t,3>> surface_faces;
  std::vector<std::array<std::uint64_t,3>> interface_faces;
  std::vector<std::array<std::uint64_t,3>> side_faces;
  std::size_t paired_quads{};
  std::size_t step_quads{};
  std::size_t perimeter_squares{};
  std::size_t perimeter_tetrahedra{};
  std::size_t surface_triangles{};
  std::size_t interface_triangles{};
  std::size_t tetrahedra{};
  std::size_t open_boundary_faces{};
  std::size_t combinatorially_collapsed_tetrahedra{};
  std::size_t overlap_pairs{};
  SandwichQuality quality;
  bool exact_surface_subset_preserved{};
  bool exact_core_faces_preserved{};
  bool positive_tetrahedra{};
  bool valid_face_incidence{};
  bool no_strict_overlap{};
  [[nodiscard]] bool accepted() const noexcept {
    return failure==SharedLatticeLocalTransitionFailure::none;
  }
};

// Height-field preflight for the independent surface/core pair.  Projection
// uses a geometry-derived orthonormal frame, so coverage and clearance do not
// depend on world-axis alignment and can be repeated after a rigid transform.
struct IndependentCoreInterfacePreflight {
  bool finite{};
  bool nondegenerate_projection{};
  bool all_surface_vertices_covered{};
  bool surface_strictly_above_interface{};
  bool valid{};
  std::size_t uncovered_surface_vertices{};
  std::size_t degenerate_interface_triangles{};
  double minimum_normal_clearance{};
  double surface_projected_area{};
  double interface_projected_area{};
};

enum class SurfaceGridOverlayFailure : std::uint8_t {
  none, invalid_input, invalid_projection, uncovered_surface,
  unclassified_vertex, stable_id_collision, nonpositive_separation,
};
struct SurfaceGridOverlayVertex {
  std::uint64_t stable_id{};
  std::array<double,3> surface_point{};
  std::array<double,3> interface_point{};
  // When an overlay corner is an authoritative input vertex, retain that
  // identity so the zipper can share it with the untouched surface/core.
  // Edge intersections have no source vertex and use the overlay stable id.
  bool has_surface_source_vertex{};
  bool has_interface_source_vertex{};
  std::uint64_t surface_source_vertex_id{};
  std::uint64_t interface_source_vertex_id{};
};
struct SurfaceGridOverlayTriangle {
  std::array<std::uint64_t,3> vertices{};
  std::uint64_t surface_parent_id{};
  std::uint64_t interface_parent_id{};
  friend constexpr bool operator==(const SurfaceGridOverlayTriangle&,const SurfaceGridOverlayTriangle&)=default;
};
struct SurfaceGridOverlay {
  SurfaceGridOverlayFailure failure{SurfaceGridOverlayFailure::invalid_input};
  std::vector<SurfaceGridOverlayVertex> vertices;
  std::vector<SurfaceGridOverlayTriangle> triangles;
  double surface_projected_area{};
  double overlay_projected_area{};
  double minimum_normal_separation{};
  double minimum_surface_triangle_angle_degrees{};
  [[nodiscard]] bool accepted() const noexcept { return failure==SurfaceGridOverlayFailure::none; }
};
enum class SurfaceGridTransitionFailure : std::uint8_t {
  none, rejected_overlay, stable_id_collision, nonpositive_tetrahedron,
  invalid_face_incidence, quality_refused,
};
struct SurfaceGridTransitionTetrahedron {
  std::array<std::uint64_t,4> vertices{};
  friend constexpr bool operator==(const SurfaceGridTransitionTetrahedron&,const SurfaceGridTransitionTetrahedron&)=default;
};
struct SurfaceGridTransitionLayer {
  SurfaceGridTransitionFailure failure{SurfaceGridTransitionFailure::rejected_overlay};
  std::vector<std::uint64_t> stable_vertex_ids;
  std::vector<std::array<double,3>> vertices;
  std::vector<SurfaceGridTransitionTetrahedron> tetrahedra;
  std::vector<std::array<std::uint64_t,3>> surface_triangles;
  std::vector<std::array<std::uint64_t,3>> interface_triangles;
  SandwichQuality quality;
  [[nodiscard]] bool accepted() const noexcept { return failure==SurfaceGridTransitionFailure::none; }
};

enum class SharedLatticeZipperFailure : std::uint8_t {
  none,
  resource_limit,
  rejected_input,
  rejected_overlay,
  stable_id_collision,
  incomplete_interface_coverage,
  nonpositive_tetrahedron,
  invalid_face_incidence,
  overlapping_tetrahedra,
  volume_mismatch,
  quality_refused,
};
enum class SharedLatticeZipperRegion : std::uint8_t {
  transition,
  refined_core_interface,
  retained_core,
};
struct SharedLatticeZipperLimits {
  std::size_t maximum_surface_vertices{1U<<20U};
  std::size_t maximum_surface_triangles{1U<<21U};
  std::size_t maximum_core_vertices{1U<<20U};
  std::size_t maximum_core_tetrahedra{1U<<21U};
  std::size_t maximum_overlay_triangles{1U<<22U};
  std::size_t maximum_output_tetrahedra{1U<<23U};
  std::size_t maximum_overlap_candidates{1U<<25U};
};
struct SharedLatticeLoopVertex {
  std::uint64_t stable_id{};
  std::array<double,3> position{};
};
enum class SharedLatticeSideWallFailure : std::uint8_t {
  none, invalid_loop, resource_limit, degenerate_triangle, invalid_incidence,
};
struct SharedLatticeSideWall {
  SharedLatticeSideWallFailure failure{SharedLatticeSideWallFailure::invalid_loop};
  std::vector<std::array<std::uint64_t,3>> triangles;
  double cost{};
  std::size_t dynamic_programming_states{};
  [[nodiscard]] bool accepted() const noexcept { return failure==SharedLatticeSideWallFailure::none; }
};
enum class SharedLatticeStarGapFailure : std::uint8_t {
  none, invalid_front, side_wall_refused, non_star_gap, invalid_coning,
};
struct SharedLatticeStarGap {
  SharedLatticeStarGapFailure failure{SharedLatticeStarGapFailure::invalid_front};
  std::size_t surface_loop_vertices{};
  std::size_t interface_loop_vertices{};
  std::size_t side_triangles{};
  std::size_t boundary_triangles{};
  std::size_t tetrahedra{};
  std::size_t overlap_pairs{};
  double kernel_margin{};
  double tetrahedral_volume{};
  double boundary_volume{};
  SandwichQuality quality;
  bool closed_boundary{};
  bool star_shaped{};
  bool positive_tetrahedra{};
  bool no_strict_overlap{};
  bool exact_volume_agreement{};
  [[nodiscard]] bool accepted() const noexcept { return failure==SharedLatticeStarGapFailure::none; }
};
enum class SharedLatticePartitionFailure : std::uint8_t {
  none, invalid_input, clipping_failed, patch_refused, seam_mismatch,
};
struct SharedLatticePartitionProbe {
  SharedLatticePartitionFailure failure{SharedLatticePartitionFailure::invalid_input};
  unsigned int divisions{};
  std::size_t patches{};
  std::size_t total_tetrahedra{};
  std::size_t surface_cut_edges{};
  std::size_t interface_cut_edges{};
  double minimum_kernel_margin{};
  SandwichQuality quality;
  bool all_closed{};
  bool all_star_shaped{};
  bool all_geometry_valid{};
  bool front_seams_identical{};
  bool s4_passed{};
  [[nodiscard]] bool accepted() const noexcept { return failure==SharedLatticePartitionFailure::none; }
};
struct SharedLatticeZipperRequest {
  FrozenDualContourSurface surface;
  FrozenRegularCore core;
  SharedLatticeZipperLimits limits;
  double minimum_dihedral_degrees{5.0};
  double maximum_dihedral_degrees{175.0};
  double minimum_mean_ratio{0.01};
  double maximum_edge_ratio{20.0};
};
struct SharedLatticeZipperTetrahedron {
  std::array<std::uint64_t,4> vertices{};
  SharedLatticeZipperRegion region{SharedLatticeZipperRegion::transition};
  friend constexpr bool operator==(const SharedLatticeZipperTetrahedron&,const SharedLatticeZipperTetrahedron&)=default;
};
struct SharedLatticeZipperResult {
  SharedLatticeZipperFailure failure{SharedLatticeZipperFailure::rejected_input};
  std::vector<std::uint64_t> stable_vertex_ids;
  std::vector<std::array<double,3>> vertices;
  std::vector<SharedLatticeZipperTetrahedron> tetrahedra;
  // Exact geometric subdivisions of the two authoritative fronts.
  std::vector<std::array<std::uint64_t,3>> surface_triangles;
  std::vector<std::array<std::uint64_t,3>> interface_triangles;
  std::vector<std::array<std::uint64_t,3>> side_triangles;
  SandwichQuality quality;
  std::size_t transition_tetrahedra{};
  std::size_t refined_core_interface_tetrahedra{};
  std::size_t retained_core_tetrahedra{};
  std::size_t overlap_candidates{};
  std::size_t overlap_pairs{};
  std::size_t boundary_faces{};
  std::size_t nonmanifold_faces{};
  std::size_t same_sided_shared_faces{};
  double tetrahedral_volume{};
  double boundary_volume{};
  bool exact_surface_preserved{};
  bool exact_interface_preserved{};
  bool positive_tetrahedra{};
  bool face_incidence_valid{};
  bool no_strict_overlap{};
  bool exact_volume_agreement{};
  bool geometry_valid{};
  bool s4_passed{};
  [[nodiscard]] bool accepted() const noexcept { return failure==SharedLatticeZipperFailure::none; }
};

struct SharedLatticeAdjacentChunkReport {
  unsigned int resolution{};
  std::size_t left_tetrahedra{};
  std::size_t right_tetrahedra{};
  std::size_t shared_boundary_faces{};
  bool left_accepted{};
  bool right_accepted{};
  bool byte_identical_shared_faces{};
  bool monolithic_topology_reproduced{};
  bool valid{};
};

// Validation for the volume constructed from the frozen dual-contour sheet.
// "artificial" means the finite probe's side, lower, or independent-chunk
// cut boundary; the real exterior is required to be exactly the DC triangles.
struct DualVolumeValidation {
  bool finite_distinct_tetrahedra{};
  bool unique_tetrahedra{};
  bool positive_tetrahedra{};
  bool face_incidence{};
  bool frozen_surface_preserved{};
  bool artificial_boundary_only{};
  bool no_tetrahedron_overlap{};
  bool opposing_shared_faces{};
  bool valid{};
  std::size_t degenerate_tetrahedra{};
  std::size_t duplicate_tetrahedra{};
  std::size_t nonmanifold_faces{};
  std::size_t missing_frozen_surface_faces{};
  std::size_t unmatched_non_surface_faces{};
  std::size_t tetrahedron_overlap_pairs{};
  std::size_t same_side_shared_faces{};
};

struct DualVolumeStorage {
  std::size_t vertices{};
  std::size_t tetrahedra{};
  std::size_t transition_tetrahedra{};
  std::size_t core_tetrahedra{};
  std::size_t explicit_live_bytes{};
  // This describes a regenerable, layer-indexed prism/tet core.  It is not
  // claimed to be the eventual regular background tet-grid core.
  std::size_t implicit_core_descriptor_bytes{};
};

struct DualVolumeTimings {
  double emit_ms{};
  double validation_ms{};
  double total_ms{};
};

struct DualVolumeReport {
  DualVolumeValidation monolithic;
  DualVolumeValidation left_chunk;
  DualVolumeValidation right_chunk;
  DualVolumeValidation joined_chunks;
  DualVolumeStorage storage;
  SandwichQuality quality;
  DualVolumeTimings timings;
  bool height_field_precondition{};
  bool shared_halo_positions_identical{};
  bool partition_independent{};
  bool valid{};
  std::uint64_t monolithic_hash{};
  std::uint64_t joined_hash{};
};

// A deliberately narrower candidate which connects the DC collar to a
// translated, regular parametric tet lattice.  It is currently planar-only:
// varying-height DC cells are not silently projected onto this grid.
struct DualGridBridgeReport {
  bool attempted{};
  bool planar_only{};
  bool noisy_diagnostic_attempted{};
  bool noisy_explicitly_unsupported{};
  bool exact_regular_grid_reconstruction{};
  bool transition_core_interface_paired{};
  bool partition_independent{};
  bool valid{};
  std::size_t coincident_regular_core_vertex_pairs{};
  std::size_t degenerate_transition_tetrahedra{};
  std::size_t degenerate_core_tetrahedra{};
  DualVolumeValidation monolithic;
  DualVolumeValidation left_chunk;
  DualVolumeValidation right_chunk;
  DualVolumeValidation joined_chunks;
  DualVolumeStorage storage;
  SandwichQuality quality;
  std::uint64_t monolithic_hash{};
  std::uint64_t joined_hash{};
};

// One bounded connector layer whose inner front is keyed by the full 3-D
// address of every active DC cell.  It is diagnostic evidence for a future
// cleaved grid-front bridge, not by itself a claim of a filled grid core.
struct DualGridAttachmentReport {
  bool attempted{};
  unsigned int connector_segments{};
  bool exact_3d_grid_address_reconstruction{};
  bool inner_front_on_material_side{};
  bool partition_independent{};
  bool valid{};
  bool has_first_strict_overlap{};
  std::array<std::uint64_t,4> first_overlap_left{};
  std::array<std::uint64_t,4> first_overlap_right{};
  DualVolumeValidation monolithic;
  DualVolumeValidation left_chunk;
  DualVolumeValidation right_chunk;
  DualVolumeValidation joined_chunks;
  DualVolumeStorage storage;
  SandwichQuality quality;
  std::uint64_t monolithic_hash{};
  std::uint64_t joined_hash{};
};

struct DualStepPatchReport {
  bool attempted{};
  bool has_vertical_2_to_1_step{};
  bool grid_front_on_material_side{};
  bool plc_self_intersection{};
  bool kernel_feasible{};
  double kernel_margin{};
  // A deliberately finite CPU PLC oracle. It is only run for an embedded PLC;
  // a negative result says nothing beyond the stated finite candidate family.
  bool plc_oracle_attempted{};
  bool plc_oracle_candidate_family_exhausted{};
  bool plc_oracle_found_fill{};
  std::size_t plc_oracle_steiner_vertices{};
  std::size_t plc_oracle_candidate_tetrahedra{};
  std::size_t plc_oracle_unfillable_prescribed_faces{};
  std::size_t plc_oracle_search_states{};
  std::size_t plc_oracle_rejected_outside{};
  std::size_t plc_oracle_rejected_overlap{};
  double plc_oracle_boundary_volume{};
  double plc_oracle_tetrahedron_volume{};
  double plc_oracle_volume_error{};
  bool valid{};
  std::size_t tetrahedra{};
  DualVolumeValidation validation;
};

struct DualContourReport {
  SandwichConfig config;
  DualContourValidation monolithic;
  DualContourValidation left_chunk;
  DualContourValidation right_chunk;
  FrozenSurfaceQuality monolithic_surface_quality;
  FrozenSurfaceQuality left_surface_quality;
  FrozenSurfaceQuality right_surface_quality;
  HermiteCrossingQuality monolithic_crossing_quality;
  std::size_t monolithic_vertices{};
  std::size_t monolithic_triangles{};
  std::size_t shared_halo_vertices{};
  std::size_t seam_crossing_triangles{};
  bool shared_halo_positions_identical{};
  bool partition_independent{};
  bool valid{};
  // The robust emitted policy is Hermite mass-point DC placement.  The local
  // QEF is retained only as an unqualified diagnostic because it can fold a
  // neighbouring coarse cell collar in the adversarial corpus.
  bool hermite_mass_point_placement{};
  bool qef_placement_qualified{};
  std::uint64_t monolithic_hash{};
  std::uint64_t joined_hash{};
  DualVolumeReport volume;
  DualGridBridgeReport regular_grid_bridge;
  DualGridAttachmentReport identity_grid_attachment;
  DualStepPatchReport isolated_step_patch;
  DualStepPatchReport stepped_edge_union_patch;
};

// Fast, surface-only qualification for sampling changes.  It deliberately
// does not construct any collar or candidate tet volume, keeping the Hermite
// accuracy corpus independent of the later shell-quality gate.
struct HermiteSurfaceProbeReport {
  DualContourValidation monolithic;
  FrozenSurfaceQuality monolithic_surface_quality;
  HermiteCrossingQuality monolithic_crossing_quality;
  bool shared_halo_positions_identical{};
  bool partition_independent{};
  std::uint64_t monolithic_hash{};
  std::uint64_t joined_hash{};
};

// A comparative, deliberately non-production diagnostic.  It proves whether
// the old independent local-QEF placement can support the same collar contract
// for a supplied fixture; it never changes the emitted mass-point policy.
struct DualContourQefDiagnostic {
  DualContourValidation qef_surface;
  DualVolumeValidation qef_volume;
  bool qef_volume_attempted{};
  bool qef_volume_qualified{};
};

// This is the *interface precondition* for the independently meshed shell,
// rather than a shell result.  It fixes the ownership of whole DC triangles
// before any chunk-local tetrahedralizer is allowed to run.  A triangle that
// crosses the original hexahedral cut belongs to the chunk containing its
// lexicographically first dual-cell address; its neighbour receives the
// vertex halo needed to evaluate it, but must not recreate or clip it.
//
// Complete, strictly-material Freudenthal tets are selected by their source
// hexahedron address using the same rule.  The selected core is consequently
// independent of the DC vertex placement and has exact, globally named faces
// on the shared cut.  The report deliberately does not claim that the shell
// between these two prescribed boundaries has been tetrahedralized.
struct DualChunkInterfaceReport {
  bool whole_triangle_ownership{};
  bool halo_positions_identical{};
  bool chunk_results_generated_independently{};
  bool canonical_surface_partition{};
  bool assembly_order_and_permutation_independent{};
  bool canonical_seam_edge_ids{};
  bool automatic_retained_core_selection{};
  bool retained_core_partition_independent{};
  bool retained_core_interface_paired{};
  bool independent_shell_meshing_completed{};
  // Source generation is evaluated from the owner interval plus a fixed
  // one-cell vertex halo and at most one adjacent owner strip per side for
  // seam-edge classification.  These counters intentionally include that
  // temporary seam support so a caller can audit the actual request bound.
  bool local_source_matches_monolithic{};
  bool bounded_local_source_work{};
  std::size_t left_owned_triangles{};
  std::size_t right_owned_triangles{};
  std::size_t crossing_owned_by_left{};
  std::size_t canonical_seam_edges{};
  std::size_t left_retained_core_tetrahedra{};
  std::size_t right_retained_core_tetrahedra{};
  std::size_t paired_core_interface_faces{};
  std::size_t left_requested_cells{};
  std::size_t right_requested_cells{};
  std::size_t left_halo_cells{};
  std::size_t right_halo_cells{};
  std::size_t left_seam_dependency_cells{};
  std::size_t right_seam_dependency_cells{};
  std::uint64_t monolithic_surface_hash{};
  std::uint64_t joined_owned_surface_hash{};
  std::uint64_t monolithic_core_hash{};
  std::uint64_t joined_core_hash{};
  std::string conclusion;
};

// Qualification for the actual local source contract used by the external
// chunk-shell oracle.  The fixed corpus is compared with a deliberately
// monolithic control, then the same request is made while the unrelated
// positive-x world extent grows.  It does not qualify adaptive DC or a shell
// tetrahedralizer; it only establishes bounded source construction here.
struct DualChunkLocalityReport {
  bool local_outputs_match_monolithic{};
  bool reverse_request_order_independent{};
  bool remote_domain_growth_bounded{};
  std::size_t fixture_count{};
  std::size_t remote_domain_cases{};
  std::size_t maximum_requested_cells{};
  std::size_t maximum_halo_cells{};
  std::size_t maximum_seam_dependency_cells{};
  std::size_t maximum_temporary_cells{};
  std::string conclusion;
};

struct SandwichReport {
  SandwichConfig config;
  SandwichBuildReport monolithic;
  SandwichBuildReport left_chunk;
  SandwichBuildReport right_chunk;
  SandwichBuildReport joined_chunks;
  bool shared_surface_interface_identical{};
  bool shared_volume_interface_paired{};
  bool partition_independent{};
  std::size_t shared_surface_edges{};
  std::size_t shared_volume_faces{};
  bool valid{};
  std::string conclusion;
};

[[nodiscard]] const char* sandwich_field_name(SandwichField field);
[[nodiscard]] double evaluate_sandwich_field(
    const SandwichConfig& config,std::array<double,3> position);
[[nodiscard]] SandwichReport run_sandwich_probe(const SandwichConfig& config = {});
[[nodiscard]] HermiteCrossingQuality sample_hermite_crossings(const SandwichConfig& config = {});
[[nodiscard]] HermiteSurfaceProbeReport run_hermite_surface_probe(const SandwichConfig& config = {});
[[nodiscard]] DualContourReport run_dual_contour_probe(const SandwichConfig& config = {});
[[nodiscard]] FrozenDualContourSurface extract_frozen_dual_contour_surface(const SandwichConfig& config = {});
[[nodiscard]] FrozenRegularCore extract_selected_regular_core(const SandwichConfig& config = {});
// Fixture-only conservative core: it is a fixed moat inside the two-hexahedron
// domain and is independent of DC vertex positions.  Unlike the historical
// all-corners-inside export, it intentionally leaves a mutable band for the
// transition constructor and therefore provides an explicit/far-core boundary
// candidate for the first noisy-volume transaction.
[[nodiscard]] FrozenRegularCore extract_conservative_regular_core(const SandwichConfig& config = {});
// Complete full-footprint Freudenthal volume below a fixed logical layer.
// Generation depends only on resolution: field, amplitude, frequency and
// phase cannot change either its topology or coordinates.  The default
// interface leaves one logical cell of clearance below the fixture's middle
// plane and is the independent grid input for the surface-overlay candidate.
[[nodiscard]] FrozenRegularCore extract_independent_regular_core(const SandwichConfig& config = {});
// Same Freudenthal construction, restricted to the finite DC sheet's owned
// projected cell footprint.  This is the production pairing for the local
// zipper; `extract_independent_regular_core` deliberately retains its larger
// historical annulus for mismatch diagnostics.
[[nodiscard]] FrozenRegularCore extract_shared_lattice_regular_core(const SandwichConfig& config = {});
// Selects wholly-material cells from a uniform cut of the exact 12-root BCC
// hierarchy used by the first prototype.  The selected core stores hierarchy
// addresses, not tetrahedron coordinates; interface_faces is the bounded
// explicit hand-off to a future DC-to-hierarchy transition constructor.
[[nodiscard]] FrozenBccHierarchyCore extract_implicit_bcc_hierarchy_core(
    const SandwichConfig& config = {});
[[nodiscard]] BccHierarchyDualSurface extract_bcc_hierarchy_dual_surface(
    const SandwichConfig& config = {});
[[nodiscard]] StructuredTwoHexDualSurface extract_structured_two_hex_dual_surface(
    const SandwichConfig& config = {},bool reverse_parent_build_order = false);
[[nodiscard]] StructuredTwoHexDcHalo extract_structured_two_hex_dc_halo(
    const SandwichConfig& config = {},unsigned int halo_cells = 2U);
[[nodiscard]] SharedLatticeOwnershipReport inspect_shared_lattice_ownership(
    const FrozenDualContourSurface& surface,const FrozenRegularCore& core);
[[nodiscard]] SharedLatticeLocalTransitionReport probe_shared_lattice_local_transition(
    const FrozenDualContourSurface& surface,const FrozenRegularCore& core);
[[nodiscard]] IndependentCoreInterfacePreflight preflight_independent_core_interface(
    const FrozenDualContourSurface& surface,const FrozenRegularCore& core);
// Canonical common refinement of the frozen DC sheet and independent grid
// interface.  Every output triangle has the same stable topology on its exact
// lifted surface and interface realizations; no float-position identity is
// used.  This is the input to the transition-prism and top-parent refinement
// stages, not itself a volume.
[[nodiscard]] SurfaceGridOverlay construct_surface_grid_overlay(
    const FrozenDualContourSurface& surface,const FrozenRegularCore& core);
[[nodiscard]] SurfaceGridTransitionLayer construct_surface_grid_transition_layer(
    const SurfaceGridOverlay& overlay);
[[nodiscard]] SharedLatticeSideWall construct_shared_lattice_side_wall(
    std::vector<SharedLatticeLoopVertex> surface_loop,
    std::vector<SharedLatticeLoopVertex> interface_loop,
    std::size_t maximum_triangles=1U<<20U);

// Dependency-free tetrahedralization of one already-closed triangular PLC.
// This is the bounded local kernel used by BCC root-star transition regions:
// it preserves every input triangle literally, first takes the deterministic
// common-kernel cone fast path, and otherwise searches a finite advancing
// front with deterministic backtracking.  A refusal is never a partial mesh.
enum class ClosedPlcTetrahedralizationFailure : std::uint8_t {
  none,
  invalid_input,
  self_intersection,
  candidate_limit,
  search_limit,
  no_fill,
  audit_failed,
};

struct ClosedPlcTetrahedralizationOptions {
  std::size_t maximum_candidate_tetrahedra{1U<<18U};
  std::size_t maximum_search_states{1U<<20U};
  bool allow_interior_steiner{true};
  // When a common kernel exists, insert this many homothetic interior shells
  // between the immutable boundary and the kernel point.  This is a bounded
  // star-shaped refinement experiment, not a general sizing or CDT scheme.
  unsigned int common_kernel_radial_layers{};
};

struct ClosedPlcTetrahedralizationResult {
  ClosedPlcTetrahedralizationFailure failure{
      ClosedPlcTetrahedralizationFailure::invalid_input};
  std::vector<FrozenFacetVertex> vertices;
  std::vector<std::array<std::uint64_t,4>> tetrahedra;
  std::size_t candidate_tetrahedra{};
  std::size_t search_states{};
  std::size_t rejected_outside{};
  std::size_t rejected_overlap{};
  double boundary_volume{};
  double tetrahedron_volume{};
  bool used_common_kernel{};
  unsigned int common_kernel_radial_layers{};
  bool exact_boundary{};
  bool positive{};
  bool no_strict_overlap{};
  bool exact_volume{};
  [[nodiscard]] bool accepted() const noexcept {
    return failure==ClosedPlcTetrahedralizationFailure::none;
  }
};

[[nodiscard]] ClosedPlcTetrahedralizationResult tetrahedralize_closed_plc(
    std::span<const FrozenFacetVertex> vertices,
    std::span<const std::array<std::uint64_t,3>> faces,
    const ClosedPlcTetrahedralizationOptions& options={});
[[nodiscard]] SharedLatticeStarGap probe_shared_lattice_star_gap(
    const FrozenDualContourSurface& surface,const FrozenRegularCore& core,
    std::size_t maximum_side_triangles=1U<<20U);
[[nodiscard]] SharedLatticePartitionProbe probe_shared_lattice_star_partition(
    const FrozenDualContourSurface& surface,const FrozenRegularCore& core,
    unsigned int divisions);
// Builds the complete finite DC -> refined Freudenthal interface -> unchanged
// regular-core transaction. Geometry is retained even when the independent S4
// gate refuses it, so quality work can inspect the real failing elements.
[[nodiscard]] SharedLatticeZipperResult construct_shared_lattice_zipper(
    const SharedLatticeZipperRequest& request);
// Builds two adjacent transactions independently from canonical primal-edge,
// interface-square, and Freudenthal-cell ownership.  It then compares their
// stable-ID boundary ledgers and their assembled tetrahedra to a monolithic
// build; no floating-point seam classification participates in the result.
[[nodiscard]] SharedLatticeAdjacentChunkReport validate_shared_lattice_adjacent_chunks(
    const SandwichConfig& config = {});
[[nodiscard]] DualContourQefDiagnostic run_dual_contour_qef_diagnostic(const SandwichConfig& config = {});
[[nodiscard]] DualChunkInterfaceReport run_dual_chunk_interface_probe(const SandwichConfig& config = {});
[[nodiscard]] std::string make_dual_chunk_interface_report_json(const DualChunkInterfaceReport& report);
[[nodiscard]] DualChunkLocalityReport run_dual_chunk_locality_probe();
[[nodiscard]] std::string make_dual_chunk_locality_report_json(const DualChunkLocalityReport& report);
[[nodiscard]] std::string make_sandwich_report_json(const SandwichReport& report);
[[nodiscard]] std::string make_dual_contour_report_json(const DualContourReport& report);
[[nodiscard]] std::string make_sandwich_svg(const SandwichConfig& config = {});
[[nodiscard]] std::string make_sandwich_viewer_data(const SandwichConfig& config = {});

} // namespace tetra::probes
