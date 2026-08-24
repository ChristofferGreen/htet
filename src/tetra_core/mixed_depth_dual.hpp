#pragma once

#include "tetra_core/tet_mesh.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace tetra {

// Wald's ownership rejection rules adapted to a BCC primal-vertex star.
// The first three values describe a complete candidate. The remaining values
// reject the complete star before any owner may emit it.
enum class MixedDepthDualDecision : std::uint8_t {
  accepted,
  finer_level_owner,
  same_level_predecessor,
  missing_incident_cell,
  degenerate_star,
  nonmanifold_star,
  malformed_incident,
};

struct MixedDepthDualStarTopology {
  bool closed{};
  bool manifold{true};
  std::uint32_t incident_cell_count{};
  std::uint32_t unique_neighbour_count{};
};

struct MixedDepthDualResolution {
  TetId owner{invalid_tet};
  MixedDepthDualDecision decision{MixedDepthDualDecision::malformed_incident};
};

// Pure rule functions used by both packed mesh queries and synthetic proofs.
// logical_owners may be unordered and may contain duplicates.
[[nodiscard]] MixedDepthDualResolution resolve_mixed_depth_dual_owner(
    std::span<const TetId> logical_owners,
    MixedDepthDualStarTopology topology);
[[nodiscard]] MixedDepthDualDecision evaluate_mixed_depth_dual_contender(
    std::span<const TetId> logical_owners,TetId self,
    MixedDepthDualStarTopology topology);

struct MixedDepthDualIncident {
  TetId conforming_cell{invalid_tet};
  TetId logical_owner{invalid_tet};
  std::uint32_t owner_depth{};
  bool transition{};
};

struct MixedDepthDualContender {
  TetId logical_owner{invalid_tet};
  std::uint32_t owner_depth{};
  MixedDepthDualDecision decision{MixedDepthDualDecision::malformed_incident};
};

struct MixedDepthDualCandidate {
  VertexId primal_vertex{};
  std::uint32_t incident_begin{};
  std::uint32_t incident_count{};
  std::uint32_t contender_begin{};
  std::uint32_t contender_count{};
  std::uint32_t unique_neighbour_count{};
  TetId owner{invalid_tet};
  MixedDepthDualDecision decision{MixedDepthDualDecision::malformed_incident};
};

// One packed candidate array plus globally flat incident and contender arrays.
// No candidate owns an allocation. The index is a snapshot of one hierarchy
// revision and must be rebuilt after topology changes.
struct MixedDepthDualIndex {
  std::vector<MixedDepthDualCandidate> candidates;
  std::vector<MixedDepthDualIncident> incidents;
  std::vector<MixedDepthDualContender> contenders;
  std::uint64_t hierarchy_revision{};
};

[[nodiscard]] MixedDepthDualIndex build_mixed_depth_dual_index(
    const TetMesh& mesh);

} // namespace tetra
