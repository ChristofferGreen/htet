#pragma once

#include "tetra_core/tet_mesh.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <compare>
#include <span>
#include <vector>

namespace tetra::probes {

// A frozen facet normally remains literal: the transition must use the input
// triangle verbatim.  Geometric mode is an explicit future escape hatch for a
// conforming transition layer: it may replace the parent by coplanar subfaces,
// but only through the exact contract below.  This enum is intentionally not
// connected to tetrahedron generation.
enum class FacetPreservationMode : std::uint8_t { literal, geometric };

// Stable identity is independent of local triangle winding and local vertex
// indices.  It is the key that adjacent chunks use to derive identical splits.
struct FrozenFacetIdentity {
  std::array<std::uint64_t, 3> vertex_ids{};
  friend bool operator==(const FrozenFacetIdentity&, const FrozenFacetIdentity&) = default;
  friend auto operator<=>(const FrozenFacetIdentity&, const FrozenFacetIdentity&) = default;
};

// Exact affine coordinates on `parent`: numerator[0..2] / denominator.  The
// values are canonical non-negative integer barycentrics, not sampled float
// positions; hence an adjacent chunk cannot drift off the parent plane.
struct FacetBarycentricPoint {
  std::array<std::uint64_t, 3> numerator{};
  std::uint64_t denominator{1U};
  friend bool operator==(const FacetBarycentricPoint&, const FacetBarycentricPoint&) = default;
};

struct FrozenFacetVertex {
  std::uint64_t id{};
  Vec3 position{};
};

// The exact construction that defines a semantic affine plane.  The value is
// an exact rational coordinate in either world space or the structured source
// reference space.  The latter deliberately retains the source affine map's
// meaning instead of reverse-engineering a plane from evaluated binary64
// positions.
enum class ExactAffinePlaneConstructionKind : std::uint8_t {
  world_axis_rational,
  structured_reference_axis,
};
struct ExactAffinePlaneConstruction {
  ExactAffinePlaneConstructionKind kind{
      ExactAffinePlaneConstructionKind::world_axis_rational};
  std::uint8_t axis{};
  std::int64_t numerator{};
  std::uint64_t denominator{1U};
  friend bool operator==(const ExactAffinePlaneConstruction&,
                         const ExactAffinePlaneConstruction&)=default;
};

// A semantic affine-plane declaration for frozen input vertices. Membership
// is carried by stable identity, while construction retains the exact source
// geometry needed by seed and recovery predicates.
struct ExactAffinePlaneProvenance {
  ExactAffinePlaneConstruction construction;
  std::vector<std::uint64_t> vertex_ids;
};

[[nodiscard]] bool is_semantically_coplanar(
    const std::array<std::uint64_t,4>& stable_vertex_ids,
    std::span<const ExactAffinePlaneProvenance> planes);

struct OwnedFacetSubface {
  FrozenFacetIdentity parent;
  std::array<FacetBarycentricPoint, 3> corners{};
  std::uint32_t ordinal{};
  std::uint64_t owner_chunk{};
  // Both sides derive the subface, but only the canonical owner emits it.
  bool emitted_by_local_chunk{};
  friend bool operator==(const OwnedFacetSubface&, const OwnedFacetSubface&) = default;
};

struct FrozenFacetSplit {
  FrozenFacetIdentity parent;
  FacetPreservationMode mode{FacetPreservationMode::literal};
  std::uint64_t owner_chunk{};
  std::vector<OwnedFacetSubface> subfaces;
};

// Canonical split used by every chunk.  `local_chunk` and `neighbour_chunk`
// participate only in ownership: the lower stable id owns all shared
// subfaces, while both chunks derive byte-identical geometry.
[[nodiscard]] FrozenFacetSplit split_frozen_facet(
    FrozenFacetIdentity parent, FacetPreservationMode mode,
    std::uint64_t local_chunk, std::uint64_t neighbour_chunk);

// Exact integer-barycentric validation: all subfaces must lie in the parent,
// have positive common orientation, be pairwise non-overlapping, and have
// summed oriented area exactly equal to the parent.  This proves no gaps or
// positive-area overlap without comparing floating-point transformed points.
[[nodiscard]] bool validate_frozen_facet_split(const FrozenFacetSplit& split);
// `parent` may have either local winding.  Coordinates are first associated
// with their stable ids and sorted to the FrozenFacetIdentity order.
[[nodiscard]] Vec3 evaluate_facet_barycentric(
    std::array<FrozenFacetVertex, 3> parent, const FacetBarycentricPoint& point);

// This is the deliberately small, format-independent boundary presented to a
// future generic transition constructor.  The outer sheet is the frozen DC
// sheet plus the finite chunk curtain; the core tetrahedra are generated from
// an implicit regular-core descriptor before this function is called.  No
// shell tetrahedra are accepted here.
struct SurfaceCoreTransitionInput {
  std::vector<Vec3> vertices;
  // Stable across independently generated chunks. Empty is the controlled
  // legacy literal mode and derives ids from input vertex indices.
  std::vector<std::uint64_t> stable_vertex_ids;
  std::vector<std::array<std::uint32_t, 3>> outer_faces;
  std::vector<std::array<std::uint32_t, 4>> retained_core_tetrahedra;
  struct ParentFacet { FrozenFacetIdentity identity; FacetPreservationMode mode{FacetPreservationMode::literal}; };
  // When supplied, outer entries are parallel to outer_faces. Core entries
  // describe precisely the boundary faces of retained_core_tetrahedra. Empty
  // lists are backward-compatible literal contracts derived from topology.
  std::vector<ParentFacet> outer_parent_facets;
  std::vector<ParentFacet> core_parent_facets;
  // Optional exact source semantics for frozen vertices.  Empty preserves the
  // generic legacy contract.  Each entry must be a sorted, duplicate-free set
  // of at least four stable vertex ids from this input.
  std::vector<ExactAffinePlaneProvenance> exact_affine_planes;
  double coordinate_scale{1.0};
  double minimum_outer_triangle_angle_degrees{5.0};
  std::size_t maximum_vertices{1U << 20U};
  std::size_t maximum_outer_faces{1U << 21U};
  std::size_t maximum_core_tetrahedra{1U << 21U};
};

enum class SurfaceCoreInputFailure : std::uint8_t {
  none,
  resource_limit,
  empty_outer_boundary,
  empty_core,
  non_finite_vertex,
  invalid_index,
  repeated_vertex,
  degenerate_outer_face,
  outer_quality_below_contract,
  duplicate_outer_face,
  outer_not_closed_two_manifold,
  outer_inconsistent_orientation,
  outer_self_intersection,
  degenerate_core_tetrahedron,
  duplicate_core_tetrahedron,
  core_not_two_manifold,
  core_inconsistent_orientation,
  core_not_strictly_nested,
  core_touches_or_intersects_outer,
  invalid_facet_contract,
};

struct SurfaceCoreTransitionContract {
  SurfaceCoreInputFailure failure{SurfaceCoreInputFailure::none};
  std::size_t failing_element{};
  // Pairwise input failures retain the second offending element so a frozen
  // PLC rejection can be reproduced without another quadratic search.
  std::size_t related_element{};
  std::size_t outer_boundary_edges{};
  std::size_t outer_nonmanifold_edges{};
  double minimum_outer_triangle_angle_degrees{};
  std::size_t core_boundary_faces{};
  std::size_t core_nonmanifold_faces{};
  // The core must be separated from the frozen outer surface by this
  // scale-relative distance.  A smaller gap can create a tet below the
  // output contract's positive-volume tolerance even when the two surfaces
  // do not exactly intersect in binary64 geometry.
  double minimum_core_outer_clearance{};
  bool accepted{};
};

struct OutputFacetSubface {
  OwnedFacetSubface exact;
  // Indices address input vertices first, then owned_vertices. Every exact
  // barycentric corner must coincide with this actual emitted output vertex.
  std::array<std::uint32_t,3> vertices{};
};

// The complete output includes the retained core unchanged. Owned vertices
// make constrained Steiner/subface points explicit rather than silently
// mutating input geometry.
struct SurfaceCoreTransitionOutput {
  std::vector<std::array<std::uint32_t, 4>> tetrahedra;
  std::vector<Vec3> owned_vertices;
  std::vector<std::uint64_t> owned_vertex_ids;
  std::vector<OutputFacetSubface> outer_preserved_facets;
  std::vector<OutputFacetSubface> core_preserved_facets;
};

enum class SurfaceCoreOutputFailure : std::uint8_t {
  none, rejected_input_contract, invalid_owned_vertex, invalid_owned_vertex_id,
  invalid_output_index, missing_facet_preservation, invalid_facet_preservation,
  literal_facet_changed, geometric_facet_not_emitted, unsupported_geometric_core,
  non_positive_tetrahedra, duplicate_tetrahedra, tetrahedron_overlap,
  nonmanifold_output, inconsistent_shared_face, missing_retained_core,
};

enum class SurfaceCoreConstructionFailure : std::uint8_t {
  none,
  rejected_input_contract,
  missing_or_invalid_correspondence,
  core_front_not_homologous,
  geometry_gate_rejected,
  quality_gate_rejected,
};

struct SurfaceCoreConstructionOptions {
  // `outer_to_core_vertex[i]` identifies the inner-front/core vertex
  // corresponding to frozen outer vertex i.  This is a temporary, explicitly
  // narrow constructor: it accepts only a core boundary with identical face
  // connectivity.  It never guesses a correspondence from geometry.
  std::vector<std::uint32_t> outer_to_core_vertex;
  double minimum_dihedral_degrees{5.0};
  double maximum_dihedral_degrees{175.0};
};

struct SurfaceCoreTransitionValidation {
  SurfaceCoreOutputFailure failure{SurfaceCoreOutputFailure::none};
  bool positive_tetrahedra{};
  bool unique_tetrahedra{};
  bool no_strict_tetrahedron_overlap{};
  bool closed_two_manifold{};
  bool consistently_oriented_shared_faces{};
  bool frozen_outer_faces_preserved{};
  bool retained_core_preserved{};
  bool valid{};
  std::size_t degenerate_tetrahedra{};
  std::size_t duplicate_tetrahedra{};
  std::size_t tetrahedron_overlap_pairs{};
  std::size_t nonmanifold_faces{};
  std::size_t same_sided_shared_faces{};
  std::size_t missing_outer_faces{};
  std::size_t unexpected_boundary_faces{};
  std::size_t missing_core_tetrahedra{};
  std::size_t missing_outer_parent_facets{};
  std::size_t missing_core_parent_facets{};
  std::size_t invalid_preserved_subfaces{};
};

struct SurfaceCoreConstructionResult {
  SurfaceCoreConstructionFailure failure{SurfaceCoreConstructionFailure::rejected_input_contract};
  SurfaceCoreTransitionOutput output;
  SurfaceCoreTransitionValidation validation;
  double minimum_dihedral_degrees{};
  double maximum_dihedral_degrees{};
  bool succeeded{};
};

[[nodiscard]] SurfaceCoreTransitionContract validate_surface_core_transition_input(
    const SurfaceCoreTransitionInput& input);
[[nodiscard]] SurfaceCoreTransitionValidation validate_surface_core_transition_output(
    const SurfaceCoreTransitionInput& input, const SurfaceCoreTransitionOutput& output);
// Use only when the identical input contract has already passed
// validate_surface_core_transition_input in the current transaction.
[[nodiscard]] SurfaceCoreTransitionValidation
validate_surface_core_transition_output_assuming_valid_input(
    const SurfaceCoreTransitionInput& input,
    const SurfaceCoreTransitionOutput& output);
[[nodiscard]] bool strict_tetrahedra_overlap(
    const std::array<Vec3,4>& first,const std::array<Vec3,4>& second);
[[nodiscard]] SurfaceCoreConstructionResult construct_homologous_surface_core_transition(
    const SurfaceCoreTransitionInput& input, const SurfaceCoreConstructionOptions& options);
[[nodiscard]] const char* surface_core_input_failure_name(SurfaceCoreInputFailure failure);
[[nodiscard]] const char* surface_core_construction_failure_name(SurfaceCoreConstructionFailure failure);

}  // namespace tetra::probes
