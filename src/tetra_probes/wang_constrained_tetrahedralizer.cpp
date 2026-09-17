#include "tetra_probes/wang_constrained_tetrahedralizer.hpp"

#if defined(TETRA_ENABLE_WANG_AUTHOR_ORACLE)
#include "dt.h"
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace tetra::probes {
namespace {

using StableFace=std::array<std::uint64_t,3>;

StableFace face_key(StableFace face) {
  std::sort(face.begin(),face.end());
  return face;
}

std::set<StableFace> literal_faces(const CanonicalPlcConstraintSet& plc) {
  std::set<StableFace> result;
  for(const auto& facet:plc.facets)result.insert(face_key(facet.vertices));
  return result;
}

std::set<std::uint64_t> boundary_vertices(const CanonicalPlcConstraintSet& plc) {
  std::set<std::uint64_t> result;
  for(const auto& facet:plc.facets)result.insert(facet.vertices.begin(),facet.vertices.end());
  return result;
}

// Algorithm 2 requires an embedded PLC.  A vertex geometrically in the open
// interior of a literal constraint edge without being an endpoint is a
// nonconforming T-contact, not a valid input feature.  The pinned source's
// AttachPnt2Seg is a mutable surface-topology repair for that state; admitting
// it at this public immutable-PLC boundary would change the requested input.
bool has_open_boundary_vertex_on_literal_edge(const CanonicalPlcConstraintSet& plc) {
  std::map<std::uint64_t,Vec3> positions;
  for(const auto& vertex:plc.vertices)
    if(!positions.emplace(vertex.id,vertex.position).second)return true;
  const auto vertices=boundary_vertices(plc);
  std::set<std::array<std::uint64_t,2>> edges;
  for(const auto& facet:plc.facets)
    for(unsigned corner=0U;corner<3U;++corner) {
      std::array<std::uint64_t,2> edge{{
          facet.vertices[corner],facet.vertices[(corner+1U)%3U]}};
      std::sort(edge.begin(),edge.end());edges.insert(edge);
    }
  for(const auto& edge:edges) {
    const auto first=positions.find(edge[0]),second=positions.find(edge[1]);
    if(first==positions.end()||second==positions.end())return true;
    const auto direction=second->second-first->second;
    const auto length2=direction.x*direction.x+direction.y*direction.y+
        direction.z*direction.z;
    if(!(length2>0.0))return true;
    for(const auto id:vertices) {
      if(id==edge[0]||id==edge[1])continue;
      const auto point=positions.find(id);if(point==positions.end())return true;
      const auto offset=point->second-first->second;
      const auto parameter=(offset.x*direction.x+offset.y*direction.y+
          offset.z*direction.z)/length2;
      if(!(parameter>0.0&&parameter<1.0))continue;
      const auto residual=offset-direction*parameter;
      const auto residual2=residual.x*residual.x+residual.y*residual.y+
          residual.z*residual.z;
      if(residual2<=64.0*std::numeric_limits<double>::epsilon()*length2)
        return true;
    }
  }
  return false;
}

WangBoundaryAudit audit_boundary(
    const CanonicalPlcConstraintSet& original,
    const CanonicalPlcConstraintSet& recovered,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra) {
  WangBoundaryAudit audit;
  const auto original_faces=literal_faces(original);
  const auto final_faces=literal_faces(recovered);
  const auto original_boundary=boundary_vertices(original);
  const auto final_boundary=boundary_vertices(recovered);
  audit.original_triangles=original_faces.size();
  audit.final_triangles=final_faces.size();
  audit.original_boundary_vertices=original_boundary.size();
  audit.final_boundary_vertices=final_boundary.size();
  audit.original_triangles_unchanged=original_faces==final_faces;
  audit.no_boundary_steiner_points=original_boundary==final_boundary&&
      recovered.split_vertices.empty()&&recovered.facet_split_vertices.empty();
  std::map<std::uint64_t,Vec3> original_positions,final_positions;
  for(const auto& vertex:original.vertices)original_positions.emplace(vertex.id,vertex.position);
  for(const auto& vertex:recovered.vertices)final_positions.emplace(vertex.id,vertex.position);
  audit.original_vertices_unchanged=true;
  for(const auto id:original_boundary) {
    const auto before=original_positions.find(id),after=final_positions.find(id);
    audit.original_vertices_unchanged=audit.original_vertices_unchanged&&
        before!=original_positions.end()&&after!=final_positions.end()&&
        before->second.x==after->second.x&&before->second.y==after->second.y&&
        before->second.z==after->second.z;
  }
  std::set<StableFace> mesh_faces;
  for(const auto& tet:tetrahedra)for(unsigned omit=0U;omit<4U;++omit) {
    StableFace face{};unsigned cursor{};
    for(unsigned i=0U;i<4U;++i)if(i!=omit)
      face[cursor++]=recovered.vertices[tet[i]].id;
    mesh_faces.insert(face_key(face));
  }
  audit.every_constraint_is_a_mesh_face=std::all_of(
      original_faces.begin(),original_faces.end(),
      [&](const auto& face){return mesh_faces.contains(face);});
  return audit;
}

} // namespace

const char* wang_unsupported_branch_name(WangUnsupportedBranch branch) {
  switch(branch) {
    case WangUnsupportedBranch::none:return "none";
    case WangUnsupportedBranch::remove_point_or_randomized_disturbance:
      return "remove_point_or_randomized_disturbance";
    case WangUnsupportedBranch::original_interior_vertex_promotion:
      return "original_interior_vertex_promotion";
    case WangUnsupportedBranch::segment_full_search_walk:
      return "segment_full_search_walk";
    case WangUnsupportedBranch::cascade_fhc_edge_feature:
      return "cascade_fhc_edge_feature";
    case WangUnsupportedBranch::segment_contact:return "segment_contact";
    case WangUnsupportedBranch::segment_boundary_split:
      return "segment_boundary_split";
    case WangUnsupportedBranch::facet_recovery:return "facet_recovery";
    case WangUnsupportedBranch::facet_no_intersecting_mesh_edge:
      return "facet_no_intersecting_mesh_edge";
    case WangUnsupportedBranch::reverse_boundary_removal:
      return "reverse_boundary_removal";
  }
  return "unknown";
}

#if defined(TETRA_ENABLE_WANG_AUTHOR_ORACLE)
WangAuthorLocalEdgeSequence trace_wang_author_local_edge_sequence(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> edge) {
  WangAuthorLocalEdgeSequence result;
  if(constraints.vertices.size()<4U||constraints.facets.empty())return result;

  // This deliberately constructs a fresh author DT, as does the reference
  // harness.  The forward walk mutates and recycles slots, so it cannot share
  // the scheduler's DT instance or the prototype's geometry-only seed.
  std::vector<std::size_t> order(constraints.vertices.size());
  std::iota(order.begin(),order.end(),0U);
  std::sort(order.begin(),order.end(),[&](std::size_t a,std::size_t b) {
    return constraints.vertices[a].id<constraints.vertices[b].id;
  });
  std::map<std::uint64_t,int> node;
  dt::Mesh mesh;
  mesh.V.reserve(order.size());
  for(std::size_t i=0;i<order.size();++i) {
    const auto& vertex=constraints.vertices[order[i]];
    node.emplace(vertex.id,static_cast<int>(i));
    mesh.V.push_back({vertex.position.x,vertex.position.y,vertex.position.z});
  }
  mesh.F.reserve(constraints.facets.size());
  for(const auto& facet:constraints.facets) {
    const auto a=node.find(facet.vertices[0]),b=node.find(facet.vertices[1]),
               c=node.find(facet.vertices[2]);
    if(a==node.end()||b==node.end()||c==node.end())return {};
    mesh.F.push_back({a->second,b->second,c->second,0});
  }
  const auto start=node.find(edge[0]),end=node.find(edge[1]);
  if(start==node.end()||end==node.end())return result;
  dt::Args args;
  args.constrain=1; args.ignoreIntersect=0; args.autoflip=1;
  args.refine=0; args.optlevel=0; args.nthread=1; args.infolevel=2;
  dt::DT source;
  if(!source.dt_init(mesh,args)||!source.BndPntInst(mesh,args))return result;
  source.buildBndInfo(mesh,args);
  int* target=source.BndEdg.find(start->second,end->second);
  if(!target)return result;
  result.target_found=true;
  source.seg[0]=start->second;
  source.seg[1]=end->second;
  result.forward_result=source.recoverEdgebyFlip(*target,0,2);
  for(std::size_t index=0;index<source.Elems.size();++index) {
    if(source.isDelEle(static_cast<int>(index))||
       source.ishulltet(static_cast<int>(index)))continue;
    std::array<Vec3,4> cell{};
    bool valid=true;
    for(unsigned corner=0;corner<4U;++corner) {
      const int vertex=source.Elems[index].form[corner];
      if(vertex<0||vertex==source.ghost||
         static_cast<std::size_t>(vertex)>=source.Nodes.size()) {valid=false;break;}
      const double* point=source.Nodes[vertex].pt;
      cell[corner]={point[0],point[1],point[2]};
    }
    if(valid)result.after_forward_cells.push_back(cell);
  }
  int reverse_source=-1;
  result.reverse_direction=source.finddirection(end->second,start->second,reverse_source);
  if(reverse_source>=0&&static_cast<std::size_t>(reverse_source)<source.Elems.size()) {
    result.reverse_source_found=true;
    for(unsigned corner=0;corner<4U;++corner) {
      const double* point=source.Nodes[source.Elems[reverse_source].form[corner]].pt;
      result.reverse_source[corner]={point[0],point[1],point[2]};
    }
  }
  result.reverse_result=source.recoverEdgebyFlip(*target,1,2);
  result.recovered_after_reverse=source.isMeshEdge(start->second,end->second);
  return result;
}

CanonicalPlcRecoveryResult recover_wang_constraints_from_pinned_author_code(
    const CanonicalPlcConstraintSet& initial,
    const CanonicalPlcRecoveryOptions& options) {
  CanonicalPlcRecoveryResult result;
  result.constraints=initial;
  if(initial.vertices.size()<4U||initial.facets.empty())return result;

  // Preserve the input's stable ordering when entering the author DT.  DT
  // owns the ordered tetrahedra, neighbour encodings, P2T links, hull cells,
  // and vacancy reuse for the entire recovery operation.
  std::vector<std::size_t> order(initial.vertices.size());
  std::iota(order.begin(),order.end(),0U);
  std::sort(order.begin(),order.end(),[&](std::size_t a,std::size_t b) {
    return initial.vertices[a].id<initial.vertices[b].id;
  });
  std::map<std::uint64_t,int> node;
  std::uint64_t next_id{};
  for(const auto& vertex:initial.vertices)next_id=std::max(next_id,vertex.id);
  struct AuthorPointIdentity { Vec3 point; std::uint64_t id; };
  std::vector<AuthorPointIdentity> identities;
  identities.reserve(initial.vertices.size()+32U);
  for(const auto& vertex:initial.vertices)
    identities.push_back({vertex.position,vertex.id});
  const auto identity_for=[&](const double* point) -> std::uint64_t {
    const Vec3 value{point[0],point[1],point[2]};
    for(const auto& known:identities)
      if(known.point.x==value.x&&known.point.y==value.y&&known.point.z==value.z)
        return known.id;
    identities.push_back({value,++next_id});
    return next_id;
  };
  dt::Mesh mesh;
  mesh.V.reserve(order.size());
  for(std::size_t i=0U;i<order.size();++i) {
    const auto& vertex=initial.vertices[order[i]];
    node.emplace(vertex.id,static_cast<int>(i));
    mesh.V.push_back({vertex.position.x,vertex.position.y,vertex.position.z});
  }
  mesh.F.reserve(initial.facets.size());
  for(const auto& facet:initial.facets) {
    const auto a=node.find(facet.vertices[0]),b=node.find(facet.vertices[1]),
               c=node.find(facet.vertices[2]);
    if(a==node.end()||b==node.end()||c==node.end())return {};
    mesh.F.push_back({a->second,b->second,c->second,0});
  }
  dt::Args args;
  args.constrain=1; args.ignoreIntersect=0; args.autoflip=1;
  args.refine=0; args.optlevel=0; args.nthread=1; args.infolevel=2;
  dt::DT source;
  if(!source.dt_init(mesh,args)||!source.BndPntInst(mesh,args)) {
    result.failure=CanonicalPlcRecoveryFailure::seed_failed;
    return result;
  }
  result.seed_failure=CanonicalDelaunaySeedFailure::none;
  // Capture the seed before any recovery can recycle a DT node slot.  These
  // identities are converted to the adapter's stable indices after outMesh.
  std::vector<std::array<std::uint64_t,4>> initial_cells;
  result.initial_tetrahedra.clear();
  for(std::size_t t=0;t<source.Elems.size();++t)
    if(!source.isDelEle(static_cast<int>(t))&&!source.ishulltet(static_cast<int>(t))) {
      std::array<std::uint64_t,4> cell{};
      for(unsigned corner=0;corner<4U;++corner)
        cell[corner]=identity_for(source.Nodes[source.Elems[t].form[corner]].pt);
      initial_cells.push_back(cell);
    }

  // This is DT::AutorecoverEdges copied at its queue boundary so the retained
  // differential trace observes the source calls.  Each individual mutation
  // remains the author implementation: recoverEdge -> recoverEdgebyFlip ->
  // removeface/removeEdge -> findShell -> flipnm -> flip23/flip32.
  source.buildBndInfo(mesh,args);
  std::queue<int> lost;
  std::unordered_map<int,int> previous;
  for(int i=0;i<static_cast<int>(source.Elems.size());++i) {
    if(source.isDelEle(i))continue;
    for(int j=0;j<6;++j) {
      int* edge=source.BndEdg.find(source.Elems[i].form[dt::Egid[j][0]],
                                   source.Elems[i].form[dt::Egid[j][1]]);
      if(edge)source.SurEdgs[*edge].info=1;
    }
  }
  auto stable_for_node=[&](int index) {
    return identity_for(source.Nodes[index].pt);
  };
  for(int i=0;i<static_cast<int>(source.SurEdgs.size());++i) {
    if(source.isRecBndEdg(i))continue;
    lost.push(i); ++previous[source.SurEdgs[i].iStart];
    ++previous[source.SurEdgs[i].iEnd];
  }
  for(std::size_t round=1U;!lost.empty()&&round<=1000U;++round) {
    const auto count=lost.size(); result.segment_local_flip_round_attempts.emplace_back();
    for(std::size_t i=0;i<count;++i) {
      const int target=lost.front();lost.pop(); if(source.isDelSurEdg(target))continue;
      const int info=source.SurEdgs[target].info;
      source.fliplevel=static_cast<int>(round)-info*10;
      const bool full=info<=-3; if(full)source.fliplevel=std::max(1000,source.fliplevel);
      const std::uint8_t steiner=static_cast<std::uint8_t>(info<=-5?2:(info<=-4?1:0));
      std::array<std::uint64_t,2> edge{{stable_for_node(source.SurEdgs[target].iStart),
                                        stable_for_node(source.SurEdgs[target].iEnd)}};
      std::sort(edge.begin(),edge.end());
      result.segment_local_flip_round_attempts.back().push_back(edge);
      ++result.attempted_edge_recoveries;
      const int outcome=source.recoverEdge(target,full,steiner);
      const auto kind=outcome==0?CanonicalPlcRecoveryResult::SegmentSchedulerOutcome::failed:
          outcome==1?CanonicalPlcRecoveryResult::SegmentSchedulerOutcome::recovered:
          CanonicalPlcRecoveryResult::SegmentSchedulerOutcome::split;
      result.segment_scheduler_attempt_trace.push_back(
          {edge,round,info,static_cast<std::size_t>(source.fliplevel),full,steiner,kind});
      if(outcome==0)lost.push(target);
      else if(outcome>1) {
        std::vector<std::array<std::uint64_t,2>> children;
        for(int child=outcome;child<static_cast<int>(source.SurEdgs.size());++child) {
          std::array<std::uint64_t,2> child_edge{{stable_for_node(source.SurEdgs[child].iStart),
              stable_for_node(source.SurEdgs[child].iEnd)}}; std::sort(child_edge.begin(),child_edge.end());
          children.push_back(child_edge);
          const int child_outcome=source.recoverEdge(child,1,0);
          result.segment_scheduler_attempt_trace.push_back({child_edge,round,-3,
              static_cast<std::size_t>(source.fliplevel),true,0,
              child_outcome==0?CanonicalPlcRecoveryResult::SegmentSchedulerOutcome::failed:
              child_outcome==1?CanonicalPlcRecoveryResult::SegmentSchedulerOutcome::recovered:
              CanonicalPlcRecoveryResult::SegmentSchedulerOutcome::split});
          if(child_outcome==0) {source.SurEdgs[child].info=-3;lost.push(child);}
        }
        result.segment_post_split_child_edge_calls.push_back(std::move(children));
      }
    }
    source.updateFliptype(previous,lost);
  }
  std::vector<std::array<std::uint64_t,4>> segment_cells;
  for(std::size_t t=0;t<source.Elems.size();++t) {
    if(source.isDelEle(static_cast<int>(t))||source.ishulltet(static_cast<int>(t)))continue;
    std::array<std::uint64_t,4> cell{};
    for(unsigned corner=0;corner<4U;++corner)
      cell[corner]=identity_for(source.Nodes[source.Elems[t].form[corner]].pt);
    segment_cells.push_back(cell);
  }
  // The remaining two source stages are invoked without reinterpretation.
  source.recoverFacesPass(args);
  source.removeStPass(args);
  bool all_segments=true,all_facets=true;
  for(std::size_t e=0;e<source.SurEdgs.size();++e)
    if(!source.isDelSurEdg(static_cast<int>(e))&&!source.isMeshEdge(
        source.SurEdgs[e].iStart,source.SurEdgs[e].iEnd)) all_segments=false;
  for(std::size_t f=0;f<source.SurTris.size();++f)
    if(!source.isDelSurTri(static_cast<int>(f))&&!source.isMeshFace(
        source.SurTris[f].form[0],source.SurTris[f].form[1],
        source.SurTris[f].form[2])) all_facets=false;
  result.edges_recovered_before_facet_stage=all_segments;
  if(source.ColorVirtualTet(args)<0||source.RemoveTet(args)!=0||
     source.outMesh(mesh,args)!=1) return result;

  // The author is free to reuse node slots.  Export immutable point identity,
  // rather than treating a transient DT slot as a durable PLC vertex.
  result.constraints.vertices.clear();
  for(const auto& cell:mesh.T)for(unsigned corner=0;corner<4U;++corner)
    (void)identity_for(source.Nodes[cell[corner]].pt);
  result.constraints.vertices.reserve(identities.size());
  std::map<std::uint64_t,std::uint32_t> exported;
  for(const auto& identity:identities) {
    exported.emplace(identity.id,static_cast<std::uint32_t>(result.constraints.vertices.size()));
    result.constraints.vertices.push_back({identity.id,identity.point});
  }
  result.constraints.facets=initial.facets;
  result.initial_tetrahedra.reserve(initial_cells.size());
  for(const auto& cell:initial_cells) {
    std::array<std::uint32_t,4> converted{};
    for(unsigned corner=0;corner<4U;++corner)
      converted[corner]=exported.at(cell[corner]);
    result.initial_tetrahedra.push_back(converted);
  }
  result.segment_stage_tetrahedra.reserve(segment_cells.size());
  for(const auto& cell:segment_cells) {
    std::array<std::uint32_t,4> converted{};
    for(unsigned corner=0;corner<4U;++corner)
      converted[corner]=exported.at(cell[corner]);
    result.segment_stage_tetrahedra.push_back(converted);
  }
  for(const auto& cell:mesh.T) {
    std::array<std::uint32_t,4> converted{};
    for(unsigned corner=0;corner<4U;++corner) {
      if(cell[corner]<0||static_cast<std::size_t>(cell[corner])>=source.Nodes.size())return {};
      converted[corner]=exported.at(identity_for(source.Nodes[cell[corner]].pt));
    }
    result.tetrahedra.push_back(converted);
  }
  result.segment_stage_constraints=result.constraints;
  result.inspection=inspect_canonical_plc_tetrahedra(
      result.constraints,result.tetrahedra);
  if(all_segments&&all_facets&&result.inspection.accepted()) {
    result.failure=CanonicalPlcRecoveryFailure::none;
  }
  return result;
}

#else
WangAuthorLocalEdgeSequence trace_wang_author_local_edge_sequence(
    const CanonicalPlcConstraintSet&,std::array<std::uint64_t,2>) {
  return {};
}

CanonicalPlcRecoveryResult recover_wang_constraints_from_pinned_author_code(
    const CanonicalPlcConstraintSet& initial,const CanonicalPlcRecoveryOptions&) {
  CanonicalPlcRecoveryResult result;
  result.constraints=initial;
  result.failure=CanonicalPlcRecoveryFailure::seed_failed;
  return result;
}
#endif

WangReverseBoundaryRemovalResult run_wang_reverse_boundary_removal(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra) {
  WangReverseBoundaryRemovalResult result;
  result.constraints=constraints;
  result.tetrahedra=tetrahedra;
  const auto insertion_order=constraints.recovery_journal;
  std::vector<CanonicalPlcRecoveryInsertion> failed_reverse_order;
  failed_reverse_order.reserve(insertion_order.size());

  // Algorithm 2 line 23 comes before reverse removal.  This is deliberately
  // not folded into the removePnt fallback below: that fallback only smooths
  // a point which could not be removed, whereas this pass optimizes every
  // disposable point that exists after facet recovery.
  result.pre_removal_volume_optimization_invoked=true;
  const auto pre_removal_interior_order=
      result.constraints.interior_steiner_vertices;
  result.pre_removal_volume_optimization_attempts.reserve(
      pre_removal_interior_order.size());
  for(const auto& vertex:pre_removal_interior_order) {
    auto smoothed=smooth_canonical_interior_steiner_volume(
        result.constraints,result.tetrahedra,vertex.id);
    result.pre_removal_volume_optimization_attempts.push_back(
        {vertex,smoothed.attempted,smoothed.moved,smoothed.converged,
         smoothed.descent_steps});
    result.constraints=std::move(smoothed.constraints);
    result.tetrahedra=std::move(smoothed.tetrahedra);
  }

  const auto try_interior=[&](const CanonicalInteriorSteinerVertex& vertex,
                              std::size_t maximum_edge_attempts,
                              WangInteriorRemovalPhase phase) {
    auto removed=remove_canonical_interior_steiner_point(
        result.constraints,result.tetrahedra,vertex.id,maximum_edge_attempts);
    WangInteriorRemovalAttempt attempt{
        vertex,phase,removed.removed(),removed.failure,removed.edge_attempts,
        removed.edge_removals,removed.used_four_to_one,false,false};
    // removePnt commits the first successful directional destroyShortEdge;
    // otherwise the point and its star remain available to smooth_volume.
    result.constraints=std::move(removed.constraints);
    result.tetrahedra=std::move(removed.tetrahedra);
    if(removed.removed()) {
      ++result.interior_points_removed;
    } else {
      auto smoothed=smooth_canonical_interior_steiner_volume(
          result.constraints,result.tetrahedra,vertex.id);
      attempt.smoothing_attempted=smoothed.attempted;
      attempt.smoothing_succeeded=smoothed.moved;
      result.constraints=std::move(smoothed.constraints);
      result.tetrahedra=std::move(smoothed.tetrahedra);
    }
    result.interior_attempts.push_back(attempt);
  };

  for(auto at=insertion_order.rbegin();at!=insertion_order.rend();++at) {
    // Restoration primitives dispatch on journal.back(). Journal entries are
    // metadata only, so expose just this target while leaving every retained
    // boundary point and its topology in the working mesh.
    result.constraints.recovery_journal={*at};
    std::set<std::uint64_t> interior_before;
    for(const auto& vertex:result.constraints.interior_steiner_vertices)
      interior_before.insert(vertex.id);
    auto restored=restore_last_canonical_boundary_steiner_point(
        result.constraints,result.tetrahedra);
    result.attempts.push_back(
        {*at,restored.accepted(),restored.failure,
         restored.last_retriangulation_rejection,
         restored.retriangulation_old_boundary_only,
         restored.retriangulation_new_boundary_only,
         restored.relocation_regions,
         restored.retriangulation_nonmanifold_face,
         restored.retriangulation_nonmanifold_face_uses,
         restored.retry_attempts});
    if(!restored.accepted()) {
      failed_reverse_order.push_back(*at);
      // removeEdgStiner's tiny-tet repair is not transactional.  A failed
      // recursive retry retains any removebadtet topology changes (and later,
      // its repair-point insertions) before the scheduler advances.
      if(!restored.retry_attempts.empty()) {
        result.constraints=std::move(restored.constraints);
        result.tetrahedra=std::move(restored.tetrahedra);
      }
      continue;
    }
    result.restored_points+=restored.restored_points;
    result.interior_points_inserted+=restored.interior_points_inserted;
    result.constraints=std::move(restored.constraints);
    result.tetrahedra=std::move(restored.tetrahedra);
    const auto after_restoration=result.constraints.interior_steiner_vertices;
    for(const auto& vertex:after_restoration)
      if(vertex.kind==CanonicalInteriorSteinerKind::boundary_relocation&&
         !interior_before.contains(vertex.id))
        try_interior(vertex,10U,WangInteriorRemovalPhase::boundary_relocation);
  }

  std::reverse(failed_reverse_order.begin(),failed_reverse_order.end());
  result.constraints.recovery_journal=std::move(failed_reverse_order);

  // Pinned removeInteriorSteiner visits disposable nodes in creation order,
  // retains successful local edge mutations, and uses a deeper removePnt
  // limit than the immediate boundary-removal call.
  result.interior_removal_stage_invoked=true;
  const auto interior_order=result.constraints.interior_steiner_vertices;
  for(const auto& vertex:interior_order) {
    try_interior(vertex,100U,WangInteriorRemovalPhase::global);
  }
  return result;
}

WangConstrainedTetrahedralizationResult tetrahedralize_wang_constrained_plc(
    const CanonicalPlcConstraintSet& plc,
    const WangConstrainedTetrahedralizationOptions& options) {
  WangConstrainedTetrahedralizationResult result;
  if(plc.vertices.size()<4U||plc.facets.size()<4U||literal_faces(plc).size()!=plc.facets.size())
    return result;
  if(has_open_boundary_vertex_on_literal_edge(plc))return result;
  // Wang et al. (2026), Algorithm 2 lines 1-22. This dedicated entry point
  // deliberately refuses incomplete paper stages rather than falling through
  // to the repository's experimental constrained-recovery algorithms.
  auto recovery_options=options.recovery;
  // R5 now executes the source-shaped removal and disturbance attempts
  // before the pre-existing negative-point split.  The restricted experiment
  // may therefore continue through that exact fallback; later unimplemented
  // branches remain explicit below.
  if(options.restricted_viability_experiment)
    recovery_options.allow_incomplete_obstruction_promotion=true;
  result.recovery=recover_wang_constraints(plc,recovery_options);
  if(options.restricted_viability_experiment) {
    switch(result.recovery.failure) {
      case CanonicalPlcRecoveryFailure::owned_segment_remove_or_disturb_point_required:
        result.unsupported_branch=WangUnsupportedBranch::remove_point_or_randomized_disturbance;
        break;
      case CanonicalPlcRecoveryFailure::owned_segment_original_vertex_promotion_required:
        result.unsupported_branch=WangUnsupportedBranch::original_interior_vertex_promotion;
        break;
      case CanonicalPlcRecoveryFailure::owned_segment_full_search_walk_required:
        result.unsupported_branch=WangUnsupportedBranch::segment_full_search_walk;
        break;
      case CanonicalPlcRecoveryFailure::owned_segment_cascade_fhc_required:
        result.unsupported_branch=WangUnsupportedBranch::cascade_fhc_edge_feature;
        break;
      case CanonicalPlcRecoveryFailure::owned_segment_contact_unsupported:
        result.unsupported_branch=WangUnsupportedBranch::segment_contact;
        break;
      case CanonicalPlcRecoveryFailure::owned_segment_boundary_split_required:
        result.unsupported_branch=WangUnsupportedBranch::segment_boundary_split;
        break;
      case CanonicalPlcRecoveryFailure::facet_recovery_required:
        result.unsupported_branch=
            result.recovery.last_facet_boundary_failure==
                    WangFacetBoundaryInsertionFailure::no_intersecting_mesh_edge?
                WangUnsupportedBranch::facet_no_intersecting_mesh_edge:
                WangUnsupportedBranch::facet_recovery;
        break;
      default:break;
    }
  }
  result.initial_tetrahedralization_complete=
      result.recovery.failure!=CanonicalPlcRecoveryFailure::seed_failed&&
      result.recovery.failure!=CanonicalPlcRecoveryFailure::materialization_failed&&
      !(result.recovery.failure==CanonicalPlcRecoveryFailure::resource_limit&&
        result.recovery.resource_limit==WangRecoveryResourceLimit::initial_vertices);
  // An empty missing-edge inspection alone is not a successful Algorithm 2
  // handoff: a bounded segment transaction may hit its resource limit after
  // mutating the mesh.  The owned scheduler's `facet_recovery_required`
  // result is the explicit proof that its complete segment queue drained.
  // Keep `none` here for the forward-compatible case where a later owned
  // facet stage completes in the same recovery transaction.
  result.segment_recovery_complete=
      result.recovery.edges_recovered_before_facet_stage&&
      (result.recovery.failure==CanonicalPlcRecoveryFailure::facet_recovery_required||
       result.recovery.failure==CanonicalPlcRecoveryFailure::none);
  result.facet_recovery_complete=result.recovery.accepted();
  if(!result.initial_tetrahedralization_complete) {
    result.failure=WangConstrainedTetrahedralizationFailure::initial_tetrahedralization_failed;
    return result;
  }
  if(!result.segment_recovery_complete) {
    result.failure=WangConstrainedTetrahedralizationFailure::segment_recovery_failed;
    return result;
  }
  if(!result.facet_recovery_complete) {
    result.failure=WangConstrainedTetrahedralizationFailure::facet_recovery_failed;
    return result;
  }
  // Wang et al. (2026), Algorithm 2 lines 23--29.  The owned recovery stage
  // deliberately stops after facet recovery, so execute the pinned
  // DT::removeStPass order here: reverse boundary insertions (with only its
  // local repair retries), followed by disposable interior-point removal.
  // This must precede both the boundary audit and region extraction.
  auto removal=run_wang_reverse_boundary_removal(result.recovery.constraints,
                                                  result.recovery.tetrahedra);
  const bool all_boundary_points_restored=removal.all_boundary_points_restored();
  result.boundary_points_restored=removal.restored_points;
  result.boundary_relocation_interior_points=removal.interior_points_inserted;
  result.boundary_removal_attempts=std::move(removal.attempts);
  result.pre_removal_volume_optimization_attempts=
      std::move(removal.pre_removal_volume_optimization_attempts);
  result.interior_removal_attempts=std::move(removal.interior_attempts);
  result.interior_points_removed=removal.interior_points_removed;
  result.pre_removal_volume_optimization_invoked=
      removal.pre_removal_volume_optimization_invoked;
  result.interior_removal_stage_invoked=removal.interior_removal_stage_invoked;
  result.recovery.constraints=std::move(removal.constraints);
  result.recovery.tetrahedra=std::move(removal.tetrahedra);
  result.recovery.inspection=inspect_canonical_plc_tetrahedra(
      result.recovery.constraints,result.recovery.tetrahedra);
  result.boundary_audit=audit_boundary(plc,result.recovery.constraints,
                                       result.recovery.tetrahedra);
  result.reverse_boundary_restoration_complete=
      all_boundary_points_restored&&
      result.boundary_audit.no_boundary_steiner_points;
  if(!result.reverse_boundary_restoration_complete) {
    result.failure=WangConstrainedTetrahedralizationFailure::boundary_restoration_failed;
    return result;
  }
  if(!result.boundary_audit.accepted()) {
    result.failure=WangConstrainedTetrahedralizationFailure::final_audit_failed;
    return result;
  }
  result.vertices=result.recovery.constraints.vertices;
  CanonicalPlcRegionInput region_input;
  for(const auto& vertex:result.vertices)region_input.vertices.push_back(vertex.position);
  region_input.tetrahedra=result.recovery.tetrahedra;
  region_input.core_witnesses=options.core_witnesses;
  std::map<std::uint64_t,std::uint32_t> index;
  for(std::size_t i=0U;i<result.vertices.size();++i)
    index.emplace(result.vertices[i].id,static_cast<std::uint32_t>(i));
  for(const auto& facet:result.recovery.constraints.facets) {
    std::array<std::uint32_t,3> face{};
    for(unsigned i=0U;i<3U;++i)face[i]=index.at(facet.vertices[i]);
    (facet.core_interface?region_input.core_faces:region_input.outer_faces).push_back(face);
  }
  const auto regions=classify_canonical_plc_regions(region_input);
  result.region_failure=regions.failure;
  if(!regions.accepted()) {
    result.failure=WangConstrainedTetrahedralizationFailure::final_audit_failed;
    return result;
  }
  result.outside_tetrahedra=regions.outside_cells;
  result.transition_tetrahedra=regions.shell_cells;
  result.core_tetrahedra=regions.core_cells;
  for(std::size_t cell=0U;cell<result.recovery.tetrahedra.size();++cell) {
    if(regions.regions[cell]!=CanonicalPlcCellRegion::shell)continue;
    const auto& tet=result.recovery.tetrahedra[cell];
    result.tetrahedra.push_back({{result.vertices[tet[0]].id,
        result.vertices[tet[1]].id,result.vertices[tet[2]].id,
        result.vertices[tet[3]].id}});
  }
  result.failure=WangConstrainedTetrahedralizationFailure::none;
  return result;
}

CanonicalPlcConstraintResult materialize_wang_planar_fixture_plc(
    const AdvancingFrontFixture& fixture) {
  CanonicalPlcConstraintResult result;
  if(!fixture.audit.accepted)return result;
  result.constraints.vertices.reserve(fixture.outer_vertices.size()+
                                      fixture.core_vertices.size());
  for(std::size_t i=0U;i<fixture.outer_vertices.size();++i)
    result.constraints.vertices.push_back({i+1U,fixture.outer_vertices[i]});
  const auto core_offset=fixture.outer_vertices.size();
  for(std::size_t i=0U;i<fixture.core_vertices.size();++i)
    result.constraints.vertices.push_back({core_offset+i+1U,fixture.core_vertices[i]});
  const auto add_face=[&](const std::array<std::uint32_t,3>& input,
                          std::uint64_t offset,bool core) {
    CanonicalPlcConstraintFacet facet;
    for(unsigned i=0U;i<3U;++i)facet.vertices[i]=offset+input[i]+1U;
    facet.parent={facet.vertices};facet.source_vertices=facet.vertices;
    facet.corners={{{{1U,0U,0U},1U},{{0U,1U,0U},1U},{{0U,0U,1U},1U}}};
    facet.core_interface=core;
    result.constraints.facets.push_back(facet);
  };
  for(const auto face:fixture.outer_triangles)add_face(face,0U,false);
  for(const auto face:fixture.core_boundary_triangles)add_face(face,core_offset,true);
  result.failure=CanonicalPlcConstraintFailure::none;
  return result;
}

WangConstrainedTetrahedralizationResult tetrahedralize_wang_planar_fixture(
    const AdvancingFrontFixture& fixture,
    const WangConstrainedTetrahedralizationOptions& options) {
  const auto plc=materialize_wang_planar_fixture_plc(fixture);
  if(!plc.accepted())return {};
  auto configured=options;
  configured.core_witnesses.reserve(fixture.core_tetrahedra.size());
  for(const auto& tet:fixture.core_tetrahedra)
    configured.core_witnesses.push_back((fixture.core_vertices[tet[0]]+
        fixture.core_vertices[tet[1]]+fixture.core_vertices[tet[2]]+
        fixture.core_vertices[tet[3]])/4.0);
  auto result=tetrahedralize_wang_constrained_plc(plc.constraints,configured);
  if(!result.accepted())return result;
  const auto core_offset=fixture.outer_vertices.size();
  for(const auto& tet:fixture.core_tetrahedra)
    result.tetrahedra.push_back({{core_offset+tet[0]+1U,core_offset+tet[1]+1U,
        core_offset+tet[2]+1U,core_offset+tet[3]+1U}});
  return result;
}

} // namespace tetra::probes
