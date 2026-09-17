#pragma once

#include "tetra_probes/canonical_delaunay_seed.hpp"
#include "tetra_probes/wang_ordered_tet_mesh.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace tetra::probes {

enum class WangOwnedLocalRecoveryFailure : std::uint8_t {
  none,
  missing_endpoint,
  walk_failed,
  vertex_obstruction,
  boundary_obstruction,
  local_flip_failed,
  iteration_limit,
};

struct WangOwnedLocalMutation {
  enum class Kind : std::uint8_t { flip32,flip23 };
  Kind kind{Kind::flip32};
  std::array<std::uint32_t,3> feature{};
  std::uint32_t apex{};
  std::size_t active_cells_before{};
  std::size_t active_cells_after{};
  std::size_t recursion_depth{};
};

struct WangOwnedLocalRecoveryResult {
  WangOwnedLocalRecoveryFailure failure{WangOwnedLocalRecoveryFailure::none};
  bool recovered{};
  std::vector<WangOwnedLocalMutation> mutations;
  // Diagnostic snapshots immediately after each source-shaped local mutation.
  std::vector<std::vector<WangOrderedTetMesh::Tet>> p2t_after_mutations;
  // Source-direction features visited by this directed pass, in order.
  std::vector<WangEndpointStarFeatureDiagnostic> selected_features;
  // Set only for the literal `Across Vertex` arm.  The scheduler needs the
  // stable identity to run DT::removePnt / disturbPnt / splitBndEdge in that
  // order; inferring it from a later mesh state would be a different walk.
  std::uint64_t obstructing_vertex{};
};

enum class WangOwnedFullSearchFeatureKind : std::uint8_t { face,edge };

struct WangOwnedFullSearchFeature {
  WangOwnedFullSearchFeatureKind kind{WangOwnedFullSearchFeatureKind::face};
  std::array<std::uint32_t,3> vertices{};
};

enum class WangOwnedFullSearchFailure : std::uint8_t {
  none,
  missing_endpoint,
  walk_failed,
  unsupported_edge_contact,
  vertex_obstruction_requires_remove_point,
  iteration_limit,
};

struct WangOwnedFullSearchResult {
  WangOwnedFullSearchFailure failure{WangOwnedFullSearchFailure::none};
  bool recovered{};
  std::size_t successful_removals{};
  std::vector<WangOwnedFullSearchFeature> features;
  std::vector<WangOwnedLocalMutation> mutations;
};

struct WangOwnedFullSearchTrace {
  WangOwnedFullSearchFailure failure{WangOwnedFullSearchFailure::none};
  std::size_t failure_step{};
  // Stable ID of the exact vertex selected by the directed source walk when
  // it stops at an Across-Vertex obstruction.
  std::uint64_t obstructing_vertex{};
  std::vector<WangOwnedFullSearchFeature> features;
};

struct WangOwnedInteriorVertexDisturbanceResult {
  bool attempted{};
  bool moved{};
  std::size_t samples{};
  Vec3 original_position{};
  Vec3 final_position{};
};

// Source-shaped DT::disturbPnt: draw at most ten independent positive
// [0,1e-6) offsets from the original position and keep the first one whose
// finite point star has positive signed volumes. The source intentionally
// seeds from random_device, so callers must not expect a fixed coordinate.
[[nodiscard]] WangOwnedInteriorVertexDisturbanceResult
disturb_wang_owned_interior_vertex(CanonicalPlcConstraintSet& constraints,
                                   std::uint64_t vertex_id,
                                   WangOrderedTetMesh& mesh);

// The flip-only arm of DT::recoverFacebyFlip_Split.  It follows each
// directed boundary edge around its ordered tetrahedron ring and applies the
// existing source-shaped removeEdge/flipnm transaction to an intersecting
// free edge.  Interior-point and boundary-split arms are deliberately not
// folded into this result.
struct WangOwnedFacetFlipRecoveryResult {
  bool recovered{};
  bool changed{};
  std::size_t edge_removal_attempts{};
  std::size_t edge_removals{};
};

[[nodiscard]] WangOwnedFacetFlipRecoveryResult
recover_wang_facet_by_flip_split(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,3> facet,
    std::size_t flip_depth,
    WangOrderedTetMesh& mesh);

// The full-depth arm of DT::recoverFacebyLocalFlips. Starting from the
// target facet's three vertex stars, it grows only across faces incident to
// an intersecting mesh edge, then applies the owned flipnm transaction to
// each encountered free edge.
[[nodiscard]] WangOwnedFacetFlipRecoveryResult
recover_wang_facet_by_local_flips(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,3> facet,
    std::size_t flip_depth,
    WangOrderedTetMesh& mesh);

enum class WangOwnedLockedFhcFailure : std::uint8_t {
  none,
  missing_endpoint,
  walk_failed,
  first_feature_not_face,
  face_not_found,
  face_was_removable,
  no_locking_edge,
  vertex_id_exhausted,
  insertion_failed,
};

enum class WangOwnedInteriorSteinerFailure : std::uint8_t {
  none,
  walk_failed,
  edge_feature_not_implemented,
  cavity_commit_failed,
  resource_limit,
  iteration_limit,
};

struct WangOwnedInteriorSteinerResult {
  struct Placement {
    std::array<std::uint32_t,3> face{};
    std::array<std::uint32_t,2> locking_edge{};
    Vec3 segment_face_hit{};
    std::array<double,2> segment_face_weights{};
    Vec3 point{};
  };
  WangOwnedInteriorSteinerFailure failure{WangOwnedInteriorSteinerFailure::none};
  WangOwnedFullSearchFailure walk_failure{WangOwnedFullSearchFailure::none};
  std::size_t walk_failure_step{};
  std::uint64_t obstructing_vertex{};
  bool recovered{};
  std::size_t recursion_levels{};
  std::vector<Vec3> inserted_points;
  std::vector<Placement> placements;
};

// One exact edge-feature application of DT::addinnerSteiner_Edge's
// Cascade-FHC arm. The edge is an ordered-mesh vertex pair; the returned
// cavity/replacement is not committed until the caller appends `point` to
// both the PLC vertex list and WangOrderedTetMesh.
struct WangOwnedCascadeFhcInsertionResult {
  bool inserted{};
  Vec3 point{};
  std::vector<WangOrderedTetMesh::Tet> cavity;
  std::vector<WangOrderedTetMesh::Tet> replacement;
};
[[nodiscard]] WangOwnedCascadeFhcInsertionResult
insert_wang_owned_cascade_fhc_point(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> directed_segment,
    std::array<std::uint32_t,2> intersecting_edge,
    WangOrderedTetMesh& mesh);

struct WangOwnedLockedFhcInsertionResult {
  WangOwnedLockedFhcFailure failure{WangOwnedLockedFhcFailure::walk_failed};
  bool inserted{};
  std::array<std::uint32_t,3> intersecting_face{};
  std::array<std::uint32_t,2> locking_edge{};
  Vec3 segment_face_hit{};
  std::array<double,2> segment_face_weights{};
  Vec3 inserted_point{};
  std::vector<WangOrderedTetMesh::Tet> cavity_tetrahedra;
  std::vector<WangOrderedTetMesh::Tet> ordered_cavity_tetrahedra;
  std::vector<WangOrderedTetMesh::Tet> replacement_tetrahedra;
  std::vector<WangOrderedTetMesh::Tet> tetrahedra;
};

// Immutable lookup data shared by all directed segment attempts against one
// constraint set. Building it once avoids reconstructing the same point,
// stable-ID and boundary tables for every edge in the Wang scheduler.
class WangLocalSegmentRecoveryWorkspace {
 public:
  explicit WangLocalSegmentRecoveryWorkspace(
      const CanonicalPlcConstraintSet& constraints);
  ~WangLocalSegmentRecoveryWorkspace();
  WangLocalSegmentRecoveryWorkspace(WangLocalSegmentRecoveryWorkspace&&) noexcept;
  WangLocalSegmentRecoveryWorkspace& operator=(
      WangLocalSegmentRecoveryWorkspace&&) noexcept;
  WangLocalSegmentRecoveryWorkspace(
      const WangLocalSegmentRecoveryWorkspace&)=delete;
  WangLocalSegmentRecoveryWorkspace& operator=(
      const WangLocalSegmentRecoveryWorkspace&)=delete;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  friend WangOwnedLocalRecoveryResult recover_wang_segment_by_local_flips(
      const CanonicalPlcConstraintSet&,std::array<std::uint64_t,2>,bool,
      std::size_t,WangOrderedTetMesh&,
      const WangLocalSegmentRecoveryWorkspace&,bool);
};

// Paper Section 3.1 local segment recovery. This is the prototype-owned
// finddirection/removeface/removeEdge/flipnm path; boundary insertion and FHC
// fallbacks belong to later stages.
[[nodiscard]] WangOwnedLocalRecoveryResult recover_wang_segment_by_local_flips(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,
    bool reverse_direction,
    std::size_t search_depth,
    WangOrderedTetMesh& mesh);

[[nodiscard]] WangOwnedLocalRecoveryResult recover_wang_segment_by_local_flips(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,
    bool reverse_direction,
    std::size_t search_depth,
    WangOrderedTetMesh& mesh,
    const WangLocalSegmentRecoveryWorkspace& workspace,
    bool capture_oracle_trace=true);

// The full-search branch of recoverEdgebyFlip. It first repeats the directed
// local path, then walks every intersected mesh feature in segment order,
// attempts each removal with the source's 32-level cap, and retries after any
// successful removal. Edge-contact traversal is reported explicitly until its
// shell walk is implemented; it is never silently replaced by a global scan.
[[nodiscard]] WangOwnedFullSearchResult recover_wang_segment_by_full_search(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> directed_segment,
    std::size_t search_depth,
    WangOrderedTetMesh& mesh);

[[nodiscard]] WangOwnedFullSearchTrace inspect_wang_full_search_features(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> directed_segment,
    WangOrderedTetMesh& mesh);

// First face branch of Section 3.1's addinnerSteiner_Edge mode.  A failed
// constrained face removal identifies the locking boundary edge; the new
// point is the barycenter of that edge and the segment/face intersection,
// then the two incident face cells seed Wang's constrained BW insertion.
// On success the new vertex is appended to constraints and the returned
// tetrahedra describe the transaction; ordered-state application is separate.
[[nodiscard]] WangOwnedLockedFhcInsertionResult
insert_first_wang_locked_fhc_point(
    CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> directed_segment,
    WangOrderedTetMesh& mesh);

// Recursive `addinnerSteiner_Edge` mode used after full flip search.  Each
// level processes one fixed feature snapshot, retries two-direction local
// recovery after every feature, and recurses only when the next snapshot is
// strictly shorter, exactly as the paper implementation's `info` guard.
[[nodiscard]] WangOwnedInteriorSteinerResult
recover_wang_segment_with_interior_steiner_mode1(
    CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> directed_segment,
    WangOrderedTetMesh& mesh,std::size_t maximum_levels=32U,
    std::size_t maximum_insertions=(std::numeric_limits<std::size_t>::max)());

} // namespace tetra::probes
