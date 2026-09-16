#pragma once

#include "tetra_probes/canonical_delaunay_seed.hpp"
#include "tetra_probes/wang_local_segment_recovery.hpp"
#include "tetra_probes/wang_ordered_tet_mesh.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace tetra::probes {

struct WangOwnedSurfaceEdge {
  std::array<std::uint64_t,2> vertices{};
  std::array<std::uint32_t,2> indices{};
  int info{};
};

// The live AutorecoverEdges state at an FHC hand-off.  `remaining_round`
// contains entries which have not yet been popped this round; entries in
// `failed_earlier_this_round` have already been popped and returned zero.
// They must be appended only after the remaining entries, exactly as the
// source queue does.  Counts are retained by stable vertex ID so new interior
// FHC vertices do not perturb the next updateFliptype pass.
struct WangOwnedSegmentSchedulerState {
  std::vector<WangOwnedSurfaceEdge> surface_edges;
  std::vector<std::size_t> remaining_round;
  std::vector<std::size_t> failed_earlier_this_round;
  std::vector<std::pair<std::uint64_t,int>> previous_lost_count;
  std::size_t round{};
};

enum class WangOwnedSchedulerAttemptOutcome : std::uint8_t {
  failed,
  recovered,
};

struct WangOwnedSchedulerAttempt {
  std::array<std::uint64_t,2> edge{};
  std::size_t surface_edge{};
  std::size_t round{};
  int info_before{};
  std::size_t search_depth{};
  bool full_search{};
  std::uint8_t steiner_mode{};
  WangOwnedSchedulerAttemptOutcome outcome{
      WangOwnedSchedulerAttemptOutcome::failed};
};

enum class WangOwnedSegmentSchedulerStop : std::uint8_t {
  complete,
  full_search_required,
  steiner_insertion_required,
  interior_vertex_obstruction,
  unsupported_contact,
  round_limit,
  invalid_constraint,
};

struct WangOwnedSegmentSchedulerResult {
  WangOwnedSegmentSchedulerStop stop{WangOwnedSegmentSchedulerStop::complete};
  std::vector<WangOwnedSurfaceEdge> surface_edges;
  std::vector<WangOwnedSchedulerAttempt> attempts;
  // Oracle-only trace: cells in their owned vertex order after each attempt.
  // It makes the first divergent scheduler operation observable without
  // incorporating author code into production.
  std::vector<std::vector<WangOrderedTetMesh::Tet>> cells_after_attempt;
  // Oracle-only sub-attempt snapshots, in source order: forward local flip
  // pass, then reverse pass only when the forward pass did not recover.
  // These expose the first mutation that changes corner order without
  // changing the scheduler's production decisions.
  std::vector<std::vector<std::vector<WangOrderedTetMesh::Tet>>>
      cells_after_local_pass;
  std::vector<std::vector<std::vector<WangOwnedLocalMutation>>>
      mutations_after_local_pass;
  std::vector<std::vector<std::vector<WangEndpointStarFeatureDiagnostic>>>
      features_after_local_pass;
  std::vector<std::vector<std::vector<std::vector<WangOrderedTetMesh::Tet>>>>
      p2t_after_local_mutation;
  std::vector<std::vector<WangOrderedTetMesh::Tet>> p2t_after_attempt;
  std::vector<std::size_t> lost_edges;
  std::size_t next_round{};
  // `info == -4` selects addinnerSteiner_Edge; `info <= -5` selects the
  // paper's constraint-splitting fallback after that operation has failed.
  // Retain the selected mode when stopping so production can report the
  // exact unimplemented branch rather than a generic scheduler failure.
  std::uint8_t pending_steiner_mode{};
  // The stable point ID from the exact directed local walk that stopped the
  // queue.  This is consumed by the owning recovery driver, which alone owns
  // the mutable PLC topology.
  std::uint64_t obstructing_vertex{};
  std::optional<WangOwnedSegmentSchedulerState> continuation;
};

// Section 4.2 / AutorecoverEdges, limited at present to the owned local-flip
// stage. The queue stops before the first operation that requires the still
// unimplemented full intersection search or Steiner insertion; it never
// reports that missing operation as an ordinary recovery failure.
[[nodiscard]] WangOwnedSegmentSchedulerResult
run_wang_segment_scheduler_local_prefix(
    const CanonicalPlcConstraintSet& constraints,
    WangOrderedTetMesh& mesh);

// Continue the same scheduler through the owned full-search branch and stop
// before the first FHC/Steiner operation.
[[nodiscard]] WangOwnedSegmentSchedulerResult
run_wang_segment_scheduler_pre_steiner(
    const CanonicalPlcConstraintSet& constraints,
    WangOrderedTetMesh& mesh);

// Continue exactly after a successful mode-one FHC transaction.  The FHC
// edge itself has recovered and is therefore not requeued; this consumes the
// rest of its original round before calling updateFliptype and proceeding to
// later rounds.
[[nodiscard]] WangOwnedSegmentSchedulerResult
resume_wang_segment_scheduler_after_fhc(
    const CanonicalPlcConstraintSet& constraints,WangOrderedTetMesh& mesh,
    const WangOwnedSegmentSchedulerState& continuation);

} // namespace tetra::probes
