#pragma once

#include "tetra_core/implicit_surface.hpp"

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

struct MixedDepthDualPatchDependency {
  TetId patch_owner{invalid_tet};
  TetId incident_owner{invalid_tet};
  auto operator<=>(const MixedDepthDualPatchDependency&) const = default;
};

struct MixedDepthDualPatchTriangle {
  TetId patch_owner{invalid_tet};
  Triangle triangle{};
};

struct MixedDepthDualPatchMetrics {
  std::size_t accepted_candidates{};
  std::size_t flag_tetrahedra{};
  std::size_t evaluated_samples{};
  std::size_t output_triangles{};
};

// Retains the complete packed vertex-star index. Each accepted candidate is
// decomposed into six fixed barycentric flag tetrahedra per incident cell;
// selected patches are keyed by the candidate's Wald-equivalent owner.
class MixedDepthDualPatchBuilder {
 public:
  void rebuild_index(const TetMesh& mesh);
  void generate_patches(
      const TetMesh& mesh,const Sphere& surface,
      std::span<const TetId> selected_patch_owners,
      std::vector<MixedDepthDualPatchTriangle>& output);
  [[nodiscard]] const MixedDepthDualIndex& index() const noexcept{return index_;}
  [[nodiscard]] std::span<const MixedDepthDualPatchDependency> dependencies()
      const noexcept{return dependencies_;}
  [[nodiscard]] const MixedDepthDualPatchMetrics& metrics() const noexcept{
    return metrics_;
  }
  [[nodiscard]] std::size_t retained_bytes() const noexcept;

 private:
  MixedDepthDualIndex index_;
  std::vector<MixedDepthDualPatchDependency> dependencies_;
  MixedDepthDualPatchMetrics metrics_;
};

[[nodiscard]] std::vector<Triangle> extract_mixed_depth_dual_isosurface(
    const TetMesh& mesh,const Sphere& surface);

} // namespace tetra
