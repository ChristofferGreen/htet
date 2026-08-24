#pragma once

#include "tetra_core/implicit_surface.hpp"

#include <cstdint>
#include <vector>

namespace tetra {

enum class ParallelCommitPolicy : std::uint8_t {
  serial_oracle,deterministic_cavity_batches,optimistic_cavity_locks,
};

[[nodiscard]] constexpr std::string_view strategy_key(ParallelCommitPolicy policy){
  switch(policy){
    case ParallelCommitPolicy::serial_oracle:return "serial-oracle";
    case ParallelCommitPolicy::deterministic_cavity_batches:
      return "deterministic-cavity-batches";
    case ParallelCommitPolicy::optimistic_cavity_locks:return "optimistic-cavity-locks";
  }
  return "unknown";
}

struct ConflictFreeBatches {
  std::vector<std::uint32_t> offsets;
  std::vector<std::uint32_t> command_indices;
};

struct ParallelCommitMetrics {
  std::size_t thread_count{};
  std::size_t batch_count{};
  std::size_t attempted_operations{};
  std::size_t successful_commits{};
  std::size_t excluded_operations{};
  std::size_t conflicts{};
  std::size_t rollbacks{};
  std::size_t reschedules{};
  std::size_t idle_slots{};
  std::uint64_t command_log_hash{};
  double scheduling_ms{};
};

struct ParallelCommitResult {
  AdaptationCommitResult commit;
  ParallelCommitMetrics metrics;
};

[[nodiscard]] ConflictFreeBatches partition_conflict_free_cavities(
    const TetMesh& mesh,std::span<const AdaptationCommand> commands);

[[nodiscard]] ParallelCommitResult commit_adaptation_parallel(
    TetMesh& mesh,const AdaptationPlan& plan,
    const AdaptationConfiguration& configuration,std::uint64_t field_revision,
    ParallelCommitPolicy policy,std::size_t thread_count);

} // namespace tetra
