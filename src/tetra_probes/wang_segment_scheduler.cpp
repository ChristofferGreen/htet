#include "tetra_probes/wang_segment_scheduler.hpp"

#include "tetra_probes/wang_local_segment_recovery.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <unordered_map>

namespace tetra::probes {
namespace {

using EdgeKey=std::array<std::uint64_t,2>;

EdgeKey edge_key(std::uint64_t first,std::uint64_t second) {
  if(second<first)std::swap(first,second);
  return {{first,second}};
}

bool is_mesh_edge(const WangOrderedTetMesh& mesh,
                  std::uint32_t first,std::uint32_t second) {
  return mesh.find_edge_cell(first,second).has_value();
}

// The ordered-topology audit deliberately does not prove that a finite star
// is geometrically embedded.  Keep this opt-in check at the recoverEdge
// boundaries so a real-fixture trace attributes the first bad state to one
// directional pass, rather than to the later scheduler hand-off.
void trace_finite_embedding(const CanonicalPlcConstraintSet& constraints,
                            const WangOrderedTetMesh& mesh,
                            const std::array<std::uint64_t,2>& edge,
                            const char* phase) {
  if(std::getenv("WANG_OWNED_GEOMETRY_TRACE")==nullptr)return;
  using Face=std::array<std::uint32_t,3>;
  const auto face_key=[](Face face) {
    std::sort(face.begin(),face.end());
    return face;
  };
  std::map<Face,std::vector<std::uint32_t>> opposites;
  const auto ghost=mesh.ghost_vertex();
  for(const auto& cell:mesh.cells()) {
    if(cell.deleted||(ghost>=0&&std::find(cell.vertices.begin(),cell.vertices.end(),
        static_cast<std::uint32_t>(ghost))!=cell.vertices.end()))continue;
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face face{}; unsigned cursor{};
      for(unsigned corner=0U;corner<4U;++corner)
        if(corner!=omitted)face[cursor++]=cell.vertices[corner];
      opposites[face_key(face)].push_back(cell.vertices[omitted]);
    }
  }
  for(const auto& [face,uses]:opposites) {
    if(uses.size()!=2U)continue;
    const auto& origin=constraints.vertices[face[0]].position;
    const auto relative=[&](std::uint32_t vertex) {
      const auto& p=constraints.vertices[vertex].position;
      return std::array<long double,3>{{p.x-origin.x,p.y-origin.y,p.z-origin.z}};
    };
    const auto b=relative(face[1]),c=relative(face[2]);
    const std::array<long double,3> normal{{b[1]*c[2]-b[2]*c[1],
                                             b[2]*c[0]-b[0]*c[2],
                                             b[0]*c[1]-b[1]*c[0]}};
    const auto side=[&](std::uint32_t vertex) {
      const auto p=relative(vertex);
      return normal[0]*p[0]+normal[1]*p[1]+normal[2]*p[2];
    };
    const auto left=side(uses[0]),right=side(uses[1]);
    if(left==0.0L||right==0.0L||(left>0.0L)==(right>0.0L)) {
      std::cerr<<"owned_geometry_invalid scheduler-"<<phase<<" edge "
               <<edge[0]<<' '<<edge[1]<<" face "<<face[0]<<' '<<face[1]
               <<' '<<face[2]<<" opposites "<<uses[0]<<' '<<uses[1]<<'\n';
      return;
    }
  }
}

} // namespace

WangOwnedSegmentSchedulerResult run_scheduler_prefix(
    const CanonicalPlcConstraintSet& constraints,WangOrderedTetMesh& mesh,
    bool stop_before_full_search,
    const WangOwnedSegmentSchedulerState* resume=nullptr,
    bool capture_oracle_trace=true) {
  WangOwnedSegmentSchedulerResult result;
  const WangLocalSegmentRecoveryWorkspace local_workspace(constraints);
  std::unordered_map<std::uint64_t,std::uint32_t> index_for_id;
  for(std::size_t index=0;index<constraints.vertices.size();++index)
    index_for_id.emplace(constraints.vertices[index].id,
                         static_cast<std::uint32_t>(index));

  std::unordered_map<std::uint64_t,int> previous_lost_count;
  std::vector<std::size_t> carried_failed;
  std::size_t first_round=1U;
  if(resume) {
    result.surface_edges=resume->surface_edges;
    result.lost_edges=resume->remaining_round;
    carried_failed=resume->failed_earlier_this_round;
    first_round=resume->round;
    for(const auto& [vertex,count]:resume->previous_lost_count)
      previous_lost_count.emplace(vertex,count);
  } else {
  std::map<EdgeKey,std::size_t> surface_edge_for_key;
  for(const auto& facet:constraints.facets) {
    const auto& triangle=facet.vertices;
    for(const auto corners:std::array<std::array<unsigned,2>,3>{
            {{{1U,2U}},{{2U,0U}},{{0U,1U}}}}) {
      const std::array<std::uint64_t,2> directed{
          {triangle[corners[0]],triangle[corners[1]]}};
      const auto key=edge_key(directed[0],directed[1]);
      if(surface_edge_for_key.contains(key))continue;
      const auto first=index_for_id.find(directed[0]);
      const auto second=index_for_id.find(directed[1]);
      if(first==index_for_id.end()||second==index_for_id.end()) {
        result.stop=WangOwnedSegmentSchedulerStop::invalid_constraint;
        return result;
      }
      surface_edge_for_key.emplace(key,result.surface_edges.size());
      result.surface_edges.push_back(
          {directed,{{first->second,second->second}},0});
    }
  }

  for(std::size_t edge=0;edge<result.surface_edges.size();++edge) {
    const auto& item=result.surface_edges[edge];
    if(is_mesh_edge(mesh,item.indices[0],item.indices[1])) {
      result.surface_edges[edge].info=1;
      continue;
    }
    result.lost_edges.push_back(edge);
    ++previous_lost_count[item.vertices[0]];
    ++previous_lost_count[item.vertices[1]];
  }
  }

  for(std::size_t round=first_round;!result.lost_edges.empty()||
      !carried_failed.empty();++round) {
    const auto round_size=result.lost_edges.size();
    std::vector<std::size_t> next_lost=std::move(carried_failed);
    carried_failed.clear();
    next_lost.reserve(round_size);
    for(std::size_t queued=0;queued<round_size;++queued) {
      const auto edge_index=result.lost_edges[queued];
      auto& edge=result.surface_edges[edge_index];
      const int raw_depth=static_cast<int>(round)-edge.info*10;
      const bool full_search=edge.info<=-3;
      const std::uint8_t steiner_mode=edge.info<=-5?2U:
                                      edge.info<=-4?1U:0U;
      const auto depth=static_cast<std::size_t>(
          full_search?std::max(1000,raw_depth):raw_depth);
      if(full_search&&stop_before_full_search) {
        result.stop=WangOwnedSegmentSchedulerStop::full_search_required;
        result.lost_edges.assign(result.lost_edges.begin()+
                                     static_cast<std::ptrdiff_t>(queued),
                                 result.lost_edges.end());
        result.next_round=round;
        return result;
      }

      bool recovered=is_mesh_edge(mesh,edge.indices[0],edge.indices[1]);
      std::uint64_t obstructing_vertex{};
      bool saw_vertex_obstruction=false;
      std::vector<std::vector<WangOrderedTetMesh::Tet>> local_passes;
      std::vector<std::vector<WangOwnedLocalMutation>> local_mutations;
      std::vector<std::vector<WangEndpointStarFeatureDiagnostic>> local_features;
      std::vector<std::vector<std::vector<WangOrderedTetMesh::Tet>>> local_p2t;
      if(!recovered) {
        const auto forward=recover_wang_segment_by_local_flips(
            constraints,edge.vertices,false,depth,mesh,local_workspace,
            capture_oracle_trace);
        trace_finite_embedding(constraints,mesh,edge.vertices,"forward");
        recovered=forward.recovered;
        if(forward.failure==WangOwnedLocalRecoveryFailure::vertex_obstruction) {
          obstructing_vertex=forward.obstructing_vertex;
          saw_vertex_obstruction=true;
        }
        if(capture_oracle_trace) {
          std::vector<WangOrderedTetMesh::Tet> cells_after_forward;
          for(const auto& cell:mesh.cells())
            if(!cell.deleted)cells_after_forward.push_back(cell.vertices);
          local_passes.push_back(std::move(cells_after_forward));
          local_mutations.push_back(forward.mutations);
          local_features.push_back(forward.selected_features);
          local_p2t.push_back(forward.p2t_after_mutations);
        }
      }
      if(!recovered) {
        const auto reverse=recover_wang_segment_by_local_flips(
            constraints,edge.vertices,true,depth,mesh,local_workspace,
            capture_oracle_trace);
        trace_finite_embedding(constraints,mesh,edge.vertices,"reverse");
        recovered=reverse.recovered;
        if(reverse.failure==WangOwnedLocalRecoveryFailure::vertex_obstruction) {
          obstructing_vertex=reverse.obstructing_vertex;
          saw_vertex_obstruction=true;
        }
        if(capture_oracle_trace) {
          std::vector<WangOrderedTetMesh::Tet> cells_after_reverse;
          for(const auto& cell:mesh.cells())
            if(!cell.deleted)cells_after_reverse.push_back(cell.vertices);
          local_passes.push_back(std::move(cells_after_reverse));
          local_mutations.push_back(reverse.mutations);
          local_features.push_back(reverse.selected_features);
          local_p2t.push_back(reverse.p2t_after_mutations);
        }
      }
      if(!recovered&&full_search) {
        const auto full=recover_wang_segment_by_full_search(
            constraints,edge.vertices,depth,mesh);
        trace_finite_embedding(constraints,mesh,edge.vertices,"full-search");
        // DT::AutorecoverEdges does not turn a failed full-search walk into
        // a scheduler terminal state.  recoverEdge returns zero, the edge is
        // requeued, and updateFliptype advances its info to the source's
        // mode-one/mode-two FHC hand-off.  The walk result remains useful
        // diagnostics, but must not bypass that control flow.
        recovered=full.recovered;
      }
      result.attempts.push_back(
          {edge.vertices,edge_index,round,edge.info,depth,full_search,
           static_cast<std::uint8_t>(recovered?0U:steiner_mode),
           recovered?WangOwnedSchedulerAttemptOutcome::recovered:
                     WangOwnedSchedulerAttemptOutcome::failed});
      if(capture_oracle_trace) {
        std::vector<WangOrderedTetMesh::Tet> cells_after;
        for(const auto& cell:mesh.cells())
          if(!cell.deleted)cells_after.push_back(cell.vertices);
        result.cells_after_attempt.push_back(std::move(cells_after));
        result.cells_after_local_pass.push_back(std::move(local_passes));
        result.mutations_after_local_pass.push_back(std::move(local_mutations));
        result.features_after_local_pass.push_back(std::move(local_features));
        result.p2t_after_local_mutation.push_back(std::move(local_p2t));
        std::vector<WangOrderedTetMesh::Tet> p2t;
        p2t.reserve(mesh.vertex_count());
        for(std::size_t vertex=0;vertex<mesh.vertex_count();++vertex) {
          const auto carrier=mesh.point_to_cell()[vertex];
          p2t.push_back(carrier>=0?mesh.cells()[static_cast<std::size_t>(carrier)].vertices:
                                    WangOrderedTetMesh::Tet{{0U,0U,0U,0U}});
        }
        result.p2t_after_attempt.push_back(std::move(p2t));
      }
      if(recovered)edge.info=1;
      // DT::recoverEdgebyFlip handles a free point hit before its FHC mode.
      // Preserve the exact live queue boundary here; the driver owns the PLC
      // mutation performed by the negative-point split fallback.
      if(!recovered&&saw_vertex_obstruction) {
        const std::vector<std::size_t> remaining(
            result.lost_edges.begin()+static_cast<std::ptrdiff_t>(queued+1U),
            result.lost_edges.end());
        result.stop=WangOwnedSegmentSchedulerStop::interior_vertex_obstruction;
        result.obstructing_vertex=obstructing_vertex;
        result.continuation=WangOwnedSegmentSchedulerState{};
        result.continuation->surface_edges=result.surface_edges;
        result.continuation->remaining_round=remaining;
        result.continuation->failed_earlier_this_round=next_lost;
        result.continuation->previous_lost_count.reserve(previous_lost_count.size());
        for(const auto& [vertex,count]:previous_lost_count)
          result.continuation->previous_lost_count.push_back({vertex,count});
        result.continuation->round=round;
        result.lost_edges.assign(1U,edge_index);
        result.lost_edges.insert(result.lost_edges.end(),remaining.begin(),remaining.end());
        result.lost_edges.insert(result.lost_edges.end(),next_lost.begin(),next_lost.end());
        result.next_round=round;
        return result;
      }
      // DT::recoverEdge always completes forward, reverse, and (when
      // selected) full-search recovery before it enters addinnerSteiner_Edge.
      // Stop only at that hand-off: the caller owns the mutable mesh needed
      // to commit the FHC transaction, while this result preserves the exact
      // current edge and the queue order at the hand-off.
      if(!recovered&&steiner_mode!=0U) {
        const std::vector<std::size_t> remaining(
            result.lost_edges.begin()+static_cast<std::ptrdiff_t>(queued+1U),
            result.lost_edges.end());
        result.stop=WangOwnedSegmentSchedulerStop::steiner_insertion_required;
        result.pending_steiner_mode=steiner_mode;
        result.continuation=WangOwnedSegmentSchedulerState{};
        result.continuation->surface_edges=result.surface_edges;
        result.continuation->remaining_round=remaining;
        result.continuation->failed_earlier_this_round=next_lost;
        result.continuation->previous_lost_count.reserve(
            previous_lost_count.size());
        for(const auto& [vertex,count]:previous_lost_count)
          result.continuation->previous_lost_count.push_back({vertex,count});
        result.continuation->round=round;
        result.lost_edges.clear();
        result.lost_edges.push_back(edge_index);
        result.lost_edges.insert(result.lost_edges.end(),remaining.begin(),
                                 remaining.end());
        result.lost_edges.insert(result.lost_edges.end(),next_lost.begin(),
                                 next_lost.end());
        result.next_round=round;
        return result;
      }
      if(!recovered)next_lost.push_back(edge_index);
    }

    std::unordered_map<std::uint64_t,int> current_lost_count;
    for(const auto edge_index:next_lost) {
      const auto& edge=result.surface_edges[edge_index];
      ++current_lost_count[edge.vertices[0]];
      ++current_lost_count[edge.vertices[1]];
    }
    for(const auto edge_index:next_lost) {
      auto& edge=result.surface_edges[edge_index];
      const int old_first=previous_lost_count[edge.vertices[0]];
      const int old_second=previous_lost_count[edge.vertices[1]];
      const int new_first=current_lost_count[edge.vertices[0]];
      const int new_second=current_lost_count[edge.vertices[1]];
      std::swap(edge.vertices[0],edge.vertices[1]);
      std::swap(edge.indices[0],edge.indices[1]);
      if(old_first<=new_first&&old_second<=new_second) {
        edge.info=std::max(edge.info-1,-90);
        if(new_first+new_second>100)edge.info=std::min(edge.info,-4);
      } else edge.info=std::min(edge.info+1,0);
    }
    previous_lost_count=std::move(current_lost_count);
    result.lost_edges=std::move(next_lost);
    if(round>=1000U) {
      result.stop=WangOwnedSegmentSchedulerStop::round_limit;
      result.next_round=round+1U;
      return result;
    }
  }
  result.next_round=result.attempts.empty()?
      (resume?first_round+1U:1U):result.attempts.back().round+1U;
  return result;
}

WangOwnedSegmentSchedulerResult run_wang_segment_scheduler_local_prefix(
    const CanonicalPlcConstraintSet& constraints,WangOrderedTetMesh& mesh,
    bool capture_oracle_trace) {
  return run_scheduler_prefix(constraints,mesh,true,nullptr,capture_oracle_trace);
}

WangOwnedSegmentSchedulerResult run_wang_segment_scheduler_pre_steiner(
    const CanonicalPlcConstraintSet& constraints,WangOrderedTetMesh& mesh,
    bool capture_oracle_trace) {
  return run_scheduler_prefix(constraints,mesh,false,nullptr,capture_oracle_trace);
}

WangOwnedSegmentSchedulerResult resume_wang_segment_scheduler_after_fhc(
    const CanonicalPlcConstraintSet& constraints,WangOrderedTetMesh& mesh,
    const WangOwnedSegmentSchedulerState& continuation,
    bool capture_oracle_trace) {
  return run_scheduler_prefix(constraints,mesh,false,&continuation,
                              capture_oracle_trace);
}

} // namespace tetra::probes
