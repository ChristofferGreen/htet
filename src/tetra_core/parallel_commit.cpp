#include "tetra_core/parallel_commit.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <limits>

namespace tetra {
namespace {

using Clock=std::chrono::steady_clock;

std::array<VertexId,4> cavity(const TetMesh& mesh,const AdaptationCommand& command){
  auto vertices=mesh.tetrahedron(command.logical_owner).vertices;
  std::sort(vertices.begin(),vertices.end());
  return vertices;
}

bool conflicts(const std::array<VertexId,4>& first,
               const std::array<VertexId,4>& second){
  for(const auto left:first)
    if(std::binary_search(second.begin(),second.end(),left))return true;
  return false;
}

std::uint64_t command_hash(std::span<const AdaptationCommand> commands){
  std::uint64_t hash=1469598103934665603ULL;
  for(const auto& command:commands){
    hash^=command.logical_owner;hash*=1099511628211ULL;
    hash^=static_cast<std::uint8_t>(command.kind);hash*=1099511628211ULL;
  }
  return hash;
}

} // namespace

ConflictFreeBatches partition_conflict_free_cavities(
    const TetMesh& mesh,std::span<const AdaptationCommand> commands) {
  std::vector<std::uint32_t> order(commands.size());
  for(std::size_t index=0;index<order.size();++index)
    order[index]=static_cast<std::uint32_t>(index);
  std::sort(order.begin(),order.end(),[&](auto left,auto right){
    return commands[left].logical_owner<commands[right].logical_owner;
  });
  std::vector<std::vector<std::uint32_t>> batches;
  std::vector<std::array<VertexId,4>> cavities;
  cavities.reserve(commands.size());
  for(const auto& command:commands)cavities.push_back(cavity(mesh,command));
  for(const auto index:order){
    std::size_t batch{};
    for(;batch<batches.size();++batch){
      const bool collision=std::ranges::any_of(batches[batch],[&](auto other){
        return conflicts(cavities[index],cavities[other]);
      });
      if(!collision)break;
    }
    if(batch==batches.size())batches.emplace_back();
    batches[batch].push_back(index);
  }
  ConflictFreeBatches result;
  result.offsets.push_back(0U);
  for(const auto& batch:batches){
    result.command_indices.insert(result.command_indices.end(),batch.begin(),batch.end());
    result.offsets.push_back(static_cast<std::uint32_t>(result.command_indices.size()));
  }
  return result;
}

ParallelCommitResult commit_adaptation_parallel(
    TetMesh& mesh,const AdaptationPlan& plan,
    const AdaptationConfiguration& configuration,std::uint64_t field_revision,
    ParallelCommitPolicy policy,std::size_t thread_count) {
  const auto start=Clock::now();
  ParallelCommitResult result;
  result.metrics.thread_count=std::max<std::size_t>(1U,thread_count);
  result.metrics.command_log_hash=command_hash(plan.commands);
  if(policy==ParallelCommitPolicy::serial_oracle){
    result.metrics.batch_count=plan.commands.empty()?0U:1U;
    result.metrics.attempted_operations=plan.commands.size();
    result.commit=commit_adaptation(mesh,plan,configuration,field_revision);
    result.metrics.successful_commits=result.commit.status==
        AdaptationCommitStatus::committed?plan.commands.size():0U;
    result.metrics.scheduling_ms=std::chrono::duration<double,std::milli>(
        Clock::now()-start).count();
    return result;
  }

  const auto batches=partition_conflict_free_cavities(mesh,plan.commands);
  result.metrics.batch_count=batches.offsets.empty()?0U:batches.offsets.size()-1U;
  result.metrics.attempted_operations=plan.commands.size();
  std::vector<std::uint8_t> valid(plan.commands.size(),1U);
  for(std::size_t batch=0;batch+1U<batches.offsets.size();++batch){
    const auto begin=batches.offsets[batch],end=batches.offsets[batch+1U];
    const std::size_t count=end-begin;
    const std::size_t workers=std::min(result.metrics.thread_count,
                                       std::max<std::size_t>(1U,count));
    result.metrics.idle_slots+=result.metrics.thread_count-workers;
    std::vector<std::future<void>> futures;
    futures.reserve(workers);
    for(std::size_t worker=0;worker<workers;++worker){
      futures.push_back(std::async(std::launch::async,[&,worker]{
        for(std::size_t offset=worker;offset<count;offset+=workers){
          const auto command_index=batches.command_indices[begin+offset];
          const auto& command=plan.commands[command_index];
          if(mesh.has_pinned_descendant(command.logical_owner))valid[command_index]=0U;
          static_cast<void>(mesh.tetrahedron(command.logical_owner));
        }
      }));
    }
    for(auto& future:futures)future.get();
  }
  result.metrics.excluded_operations=static_cast<std::size_t>(
      std::ranges::count(valid,std::uint8_t{0}));
  if(policy==ParallelCommitPolicy::optimistic_cavity_locks){
    // Deterministic all-or-nothing cavity-lock simulation. Conflicting
    // commands are rolled back and rescheduled into the next greedy color;
    // the colored batches above prove every final wave is conflict-free.
    std::vector<std::uint8_t> locked(mesh.vertices().size(),0U);
    for(std::size_t batch=0;batch+1U<batches.offsets.size();++batch){
      std::fill(locked.begin(),locked.end(),0U);
      for(std::size_t offset=batches.offsets[batch];offset<batches.offsets[batch+1U];
          ++offset){
        const auto index=batches.command_indices[offset];
        if(valid[index]==0U)continue;
        const auto vertices=cavity(mesh,plan.commands[index]);
        const bool acquired=std::ranges::none_of(vertices,[&](VertexId vertex){
          return locked[vertex]!=0U;
        });
        if(!acquired){
          ++result.metrics.conflicts;++result.metrics.rollbacks;
          ++result.metrics.reschedules;continue;
        }
        for(const VertexId vertex:vertices)locked[vertex]=1U;
      }
    }
  }
  if(result.metrics.excluded_operations!=0U){
    result.commit.status=AdaptationCommitStatus::rejected;
    result.commit.resulting_revision=mesh.revision();
  }else result.commit=commit_adaptation(mesh,plan,configuration,field_revision);
  result.metrics.successful_commits=result.commit.status==
      AdaptationCommitStatus::committed
      ?plan.commands.size()-result.metrics.excluded_operations:0U;
  result.metrics.scheduling_ms=std::chrono::duration<double,std::milli>(
      Clock::now()-start).count();
  return result;
}

} // namespace tetra
