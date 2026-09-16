#include "tetra_probes/wang_constrained_tetrahedralizer.hpp"
#include "tetra_probes/exact_binary_predicates.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>

int main(int argc,char** argv) {
  std::cout<<std::setprecision(17);
  tetra::probes::AdvancingFrontFixtureConfig config;config.noise_amplitude=0.0;
  if(argc>2)config.grid_resolution=static_cast<unsigned>(std::stoul(argv[2]));
  if(argc>3)config.noise_amplitude=std::stod(argv[3]);
  const auto fixture=tetra::probes::build_advancing_front_fixture(config);
  tetra::probes::WangConstrainedTetrahedralizationOptions options;
  options.recovery.maximum_vertices=1U<<13U;
  options.recovery.maximum_facets=1U<<14U;
  options.recovery.maximum_tetrahedra=1U<<18U;
  options.recovery.maximum_edge_splits=256U;
  options.recovery.maximum_fhc_steiner_insertions=64U;
  if(argc>1)options.recovery.maximum_edge_recovery_attempts=
      static_cast<std::size_t>(std::stoull(argv[1]));
  const bool reverse_input=argc>4&&std::string(argv[4])=="reverse";
  tetra::probes::WangConstrainedTetrahedralizationResult result;
  if(!reverse_input) {
    result=tetra::probes::tetrahedralize_wang_planar_fixture(fixture,options);
  } else {
    auto plc=tetra::probes::materialize_wang_planar_fixture_plc(fixture);
    std::reverse(plc.constraints.vertices.begin(),plc.constraints.vertices.end());
    std::reverse(plc.constraints.facets.begin(),plc.constraints.facets.end());
    auto reordered=options;
    for(auto tet=fixture.core_tetrahedra.rbegin();
        tet!=fixture.core_tetrahedra.rend();++tet)
      reordered.core_witnesses.push_back((fixture.core_vertices[(*tet)[0]]+
          fixture.core_vertices[(*tet)[1]]+fixture.core_vertices[(*tet)[2]]+
          fixture.core_vertices[(*tet)[3]])/4.0);
    result=tetra::probes::tetrahedralize_wang_constrained_plc(
        plc.constraints,reordered);
    if(result.accepted()) {
      const auto core_offset=fixture.outer_vertices.size();
      for(auto tet=fixture.core_tetrahedra.rbegin();
          tet!=fixture.core_tetrahedra.rend();++tet)
        result.tetrahedra.push_back({{core_offset+(*tet)[0]+1U,
            core_offset+(*tet)[1]+1U,core_offset+(*tet)[2]+1U,
            core_offset+(*tet)[3]+1U}});
    }
  }
  auto canonical_cells=result.tetrahedra;
  for(auto& cell:canonical_cells)std::sort(cell.begin(),cell.end());
  std::sort(canonical_cells.begin(),canonical_cells.end());
  auto mix=[](std::uint64_t& hash,std::uint64_t value) {
    hash^=value;hash*=1099511628211ULL;
  };
  std::uint64_t topology_hash=1469598103934665603ULL;
  for(const auto& cell:canonical_cells) {
    for(const auto id:cell)mix(topology_hash,id);
    mix(topology_hash,std::numeric_limits<std::uint64_t>::max());
  }
  auto canonical_vertices=result.vertices;
  std::sort(canonical_vertices.begin(),canonical_vertices.end(),
            [](const auto& left,const auto& right){return left.id<right.id;});
  std::uint64_t vertex_hash=1469598103934665603ULL;
  for(const auto& vertex:canonical_vertices) {
    mix(vertex_hash,vertex.id);
    mix(vertex_hash,std::bit_cast<std::uint64_t>(vertex.position.x));
    mix(vertex_hash,std::bit_cast<std::uint64_t>(vertex.position.y));
    mix(vertex_hash,std::bit_cast<std::uint64_t>(vertex.position.z));
  }
  std::uint64_t closest_id{};double closest_parameter{};
  double closest_relative_distance=std::numeric_limits<double>::infinity();
  const auto endpoint=[&](std::uint64_t id)->tetra::Vec3 {
    for(const auto& vertex:result.recovery.constraints.vertices)
      if(vertex.id==id)return vertex.position;
    return {};
  };
  const auto a=endpoint(result.recovery.first_unrecovered_edge[0]);
  const auto b=endpoint(result.recovery.first_unrecovered_edge[1]);
  const auto delta=b-a;
  const auto length2=delta.x*delta.x+delta.y*delta.y+delta.z*delta.z;
  if(length2>0.0)for(const auto& vertex:result.recovery.constraints.vertices) {
    if(vertex.id==result.recovery.first_unrecovered_edge[0]||
       vertex.id==result.recovery.first_unrecovered_edge[1])continue;
    const auto offset=vertex.position-a;
    const auto parameter=(offset.x*delta.x+offset.y*delta.y+offset.z*delta.z)/length2;
    if(!(parameter>0.0&&parameter<1.0))continue;
    const auto projected=a+delta*parameter;
    const auto residual=vertex.position-projected;
    const auto relative=(residual.x*residual.x+residual.y*residual.y+
                         residual.z*residual.z)/length2;
    if(relative<closest_relative_distance){closest_relative_distance=relative;
      closest_parameter=parameter;closest_id=vertex.id;}
  }
  double best_crossing_min_barycentric=-std::numeric_limits<double>::infinity();
  std::size_t plane_crossing_faces{};
  std::array<std::uint64_t,3> best_crossing_face{};
  std::map<std::uint64_t,std::uint32_t> vertex_index;
  for(std::size_t i=0U;i<result.recovery.constraints.vertices.size();++i)
    vertex_index.emplace(result.recovery.constraints.vertices[i].id,
                         static_cast<std::uint32_t>(i));
  const auto first=vertex_index.find(result.recovery.first_unrecovered_edge[0]);
  const auto second=vertex_index.find(result.recovery.first_unrecovered_edge[1]);
  if(first!=vertex_index.end()&&second!=vertex_index.end()) {
    const auto dot=[](tetra::Vec3 x,tetra::Vec3 y){return x.x*y.x+x.y*y.y+x.z*y.z;};
    const auto cross=[](tetra::Vec3 x,tetra::Vec3 y){return tetra::Vec3{
        x.y*y.z-x.z*y.y,x.z*y.x-x.x*y.z,x.x*y.y-x.y*y.x};};
    std::set<std::array<std::uint32_t,3>> faces;
    for(const auto& cell:result.recovery.tetrahedra)for(unsigned omitted=0U;omitted<4U;++omitted){
      std::array<std::uint32_t,3> face{};unsigned n{};
      for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[n++]=cell[i];
      std::sort(face.begin(),face.end());faces.insert(face);
    }
    for(const auto& face:faces) {
      if(std::find(face.begin(),face.end(),first->second)!=face.end()||
         std::find(face.begin(),face.end(),second->second)!=face.end())continue;
      const auto p0=result.recovery.constraints.vertices[face[0]].position;
      const auto p1=result.recovery.constraints.vertices[face[1]].position;
      const auto p2=result.recovery.constraints.vertices[face[2]].position;
      const auto normal=cross(p1-p0,p2-p0);
      const auto da=dot(normal,a-p0),db=dot(normal,b-p0);
      if(!(da*db<0.0))continue;
      ++plane_crossing_faces;
      const auto hit=a+(b-a)*(da/(da-db));
      const auto v0=p1-p0,v1=p2-p0,v2=hit-p0;
      const auto d00=dot(v0,v0),d01=dot(v0,v1),d11=dot(v1,v1);
      const auto d20=dot(v2,v0),d21=dot(v2,v1);
      const auto determinant=d00*d11-d01*d01;if(!(determinant>0.0))continue;
      const auto w1=(d11*d20-d01*d21)/determinant;
      const auto w2=(d00*d21-d01*d20)/determinant;
      const auto minimum=std::min({1.0-w1-w2,w1,w2});
      if(minimum>best_crossing_min_barycentric) {
        best_crossing_min_barycentric=minimum;
        for(unsigned i=0U;i<3U;++i)
          best_crossing_face[i]=result.recovery.constraints.vertices[face[i]].id;
      }
    }
  }
  std::map<std::array<std::uint32_t,3>,unsigned> face_uses;
  std::map<std::array<std::uint32_t,3>,std::vector<std::uint32_t>> face_opposites;
  std::set<std::array<std::uint32_t,4>> unique_cells;
  std::size_t duplicate_cells{},nonmanifold_faces{},degenerate_cells{};
  std::vector<std::array<std::uint64_t,4>> degenerate_cell_ids;
  std::size_t midpoint_containing_cells{};
  std::vector<std::array<std::uint32_t,4>> midpoint_cells;
  const auto midpoint=(a+b)/2.0;
  const auto triple=[](tetra::Vec3 p,tetra::Vec3 q,tetra::Vec3 r,tetra::Vec3 s){
    const auto x=q-p,y=r-p,z=s-p;
    return x.x*(y.y*z.z-y.z*z.y)-x.y*(y.x*z.z-y.z*z.x)+
           x.z*(y.x*z.y-y.y*z.x);
  };
  for(const auto& cell:result.recovery.tetrahedra) {
    auto key=cell;std::sort(key.begin(),key.end());
    if(!unique_cells.insert(key).second)++duplicate_cells;
    const auto p0=result.recovery.constraints.vertices[cell[0]].position;
    const auto p1=result.recovery.constraints.vertices[cell[1]].position;
    const auto p2=result.recovery.constraints.vertices[cell[2]].position;
    const auto p3=result.recovery.constraints.vertices[cell[3]].position;
    if(tetra::probes::exact_orientation_3d(p0,p1,p2,p3)==
       tetra::probes::ExactPredicateSign::zero) {
      ++degenerate_cells;
      std::array<std::uint64_t,4> ids{};
      for(unsigned i=0U;i<4U;++i)
        ids[i]=result.recovery.constraints.vertices[cell[i]].id;
      std::sort(ids.begin(),ids.end());
      degenerate_cell_ids.push_back(ids);
    }
    bool inside=true;
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      std::array<std::uint32_t,3> face{};unsigned n{};
      for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[n++]=cell[i];
      auto face_key=face;std::sort(face_key.begin(),face_key.end());++face_uses[face_key];
      face_opposites[face_key].push_back(cell[omitted]);
      const auto f0=result.recovery.constraints.vertices[face[0]].position;
      const auto f1=result.recovery.constraints.vertices[face[1]].position;
      const auto f2=result.recovery.constraints.vertices[face[2]].position;
      const auto opposite=result.recovery.constraints.vertices[cell[omitted]].position;
      const auto reference=triple(f0,f1,f2,opposite);
      const auto query=triple(f0,f1,f2,midpoint);
      if(reference*query<0.0){inside=false;}
    }
    if(inside){++midpoint_containing_cells;midpoint_cells.push_back(cell);}
  }
  for(const auto& [face,count]:face_uses){(void)face;if(count>2U)++nonmanifold_faces;}
  std::size_t same_sided_shared_faces{};
  for(const auto& [face,opposites]:face_opposites)if(opposites.size()==2U) {
    const auto f0=result.recovery.constraints.vertices[face[0]].position;
    const auto f1=result.recovery.constraints.vertices[face[1]].position;
    const auto f2=result.recovery.constraints.vertices[face[2]].position;
    const auto o0=result.recovery.constraints.vertices[opposites[0]].position;
    const auto o1=result.recovery.constraints.vertices[opposites[1]].position;
    const auto side0=tetra::probes::exact_orientation_3d(f0,f1,f2,o0);
    const auto side1=tetra::probes::exact_orientation_3d(f0,f1,f2,o1);
    if(side0==tetra::probes::ExactPredicateSign::zero||
       side1==tetra::probes::ExactPredicateSign::zero||side0==side1)
      ++same_sided_shared_faces;
  }
  std::array<std::uint64_t,3> midpoint_shared_face{};
  if(midpoint_cells.size()==2U) {
    unsigned n{};
    for(const auto vertex:midpoint_cells[0])
      if(std::find(midpoint_cells[1].begin(),midpoint_cells[1].end(),vertex)!=midpoint_cells[1].end()&&n<3U)
        midpoint_shared_face[n++]=result.recovery.constraints.vertices[vertex].id;
    std::sort(midpoint_shared_face.begin(),midpoint_shared_face.end());
  }
  const auto final_flip=tetra::probes::try_recover_literal_edge_by_four_to_four(
      result.recovery.constraints,result.recovery.first_unrecovered_edge,
      result.recovery.tetrahedra);
  std::cout<<"accepted="<<result.accepted()
           <<" input_order="<<(reverse_input?"reverse":"forward")
           <<" topology_hash="<<topology_hash
           <<" vertex_hash="<<vertex_hash
           <<" limit_attempts="<<options.recovery.maximum_edge_recovery_attempts
           <<" limit_splits="<<options.recovery.maximum_edge_splits
           <<" failure="<<static_cast<unsigned>(result.failure)
           <<" resource_stage="<<static_cast<unsigned>(result.recovery.resource_limit)
           <<" resource_observed="<<result.recovery.resource_limit_observed
           <<" resource_configured="<<result.recovery.resource_limit_configured
           <<" seed="<<result.initial_tetrahedralization_complete
           <<" seed_failure="<<static_cast<unsigned>(result.recovery.seed_failure)
           <<" seed_reason="<<static_cast<unsigned>(result.recovery.seed_invalid_reason)
           <<" owned_scheduler="<<result.recovery.owned_segment_scheduler_invoked
           <<" owned_scheduler_round="<<result.recovery.owned_segment_scheduler_round
           <<" owned_scheduler_steiner_mode="
           <<static_cast<unsigned>(result.recovery.owned_segment_scheduler_steiner_mode)
           <<" segments="<<result.segment_recovery_complete
           <<" facets="<<result.facet_recovery_complete
           <<" boundary="<<result.reverse_boundary_restoration_complete
           <<" audit_vertices="<<result.boundary_audit.original_vertices_unchanged
           <<" audit_triangles="<<result.boundary_audit.original_triangles_unchanged
           <<" audit_mesh_faces="<<result.boundary_audit.every_constraint_is_a_mesh_face
           <<" audit_no_boundary_steiner="<<result.boundary_audit.no_boundary_steiner_points
           <<" region_failure="<<static_cast<unsigned>(result.region_failure)
           <<" missing_edges="<<result.recovery.inspection.missing_edges.size()
           <<" missing_facets="<<result.recovery.inspection.missing_facets.size()
           <<" two_sided_attempts="<<result.recovery.two_sided_facet_attempts
           <<" two_sided_recovered="<<result.recovery.two_sided_facets_recovered
           <<" two_sided_failure="
           <<static_cast<unsigned>(result.recovery.last_two_sided_facet_failure)
           <<" two_sided_cavity="<<result.recovery.last_two_sided_intersected_tetrahedra
           <<" two_sided_top="<<result.recovery.last_two_sided_top_tetrahedra
           <<" two_sided_bottom="<<result.recovery.last_two_sided_bottom_tetrahedra
           <<" two_sided_expansions="<<result.recovery.two_sided_cavity_expansions
           <<" ridge_attempts="<<result.recovery.advancing_ridge_attempts
           <<" ridge_insertions="<<result.recovery.advancing_ridge_insertions
           <<" intersection_attempts="<<result.recovery.intersection_steiner_attempts
           <<" intersection_insertions="<<result.recovery.intersection_steiner_insertions
           <<" fhc_insertions="<<result.recovery.fhc_steiner_insertions
           <<" fhc_cascade="<<result.recovery.fhc_cascade_configurations
           <<" fhc_locked="<<result.recovery.fhc_locked_configurations
           <<" fhc_generic="<<result.recovery.fhc_generic_cavity_configurations
           <<" fhc_attempts="<<result.recovery.fhc_steiner_attempts
           <<" facet_interior_attempts="<<result.recovery.facet_interior_steiner_attempts
           <<" facet_interior_insertions="<<result.recovery.facet_interior_steiner_insertions
           <<" facet_interior_failure="<<static_cast<unsigned>(
                result.recovery.last_facet_interior_failure)
           <<" fhc_failure="<<static_cast<unsigned>(result.recovery.last_fhc_insertion_failure)
           <<" fhc_cavity="<<result.recovery.last_fhc_forced_cavity_cells
           <<" fhc_obstructions="<<result.recovery.last_fhc_obstructions_before
           <<'/'<<result.recovery.last_fhc_obstructions_after
           <<" fhc_candidates="<<result.recovery.last_fhc_candidates_attempted
           <<'/'<<result.recovery.last_fhc_candidates_generated
           <<" fhc_forced_reject="<<result.recovery.last_fhc_forced_cavity_rejections
           <<" fhc_nonmonotonic="<<result.recovery.last_fhc_nonmonotonic_rejections
           <<" fhc_edge="<<result.recovery.last_fhc_had_blocking_edge
           <<" fhc_face="<<result.recovery.last_fhc_had_blocking_face
           <<" invalid_mesh_reject="<<result.recovery.invalid_candidate_mesh_rejections
           <<" boundary_splits="<<result.recovery.edge_splits
           <<" early_restorations="<<result.recovery.early_boundary_restorations
           <<" early_relocation_points="<<result.recovery.early_boundary_relocation_points
           <<" early_restoration_attempts="<<result.recovery.early_boundary_restoration_attempts
           <<" early_restoration_failure="
           <<static_cast<unsigned>(result.recovery.last_early_boundary_restoration_failure)
           <<" journal_last="<<(result.recovery.constraints.recovery_journal.empty()?0U:
                result.recovery.constraints.recovery_journal.back().vertex_id)
           <<" edge_flips="<<result.recovery.edge_flips
           <<" mesh_edge_removals="<<result.recovery.mesh_edge_removals
           <<" mesh_edge_removal_attempts="<<result.recovery.mesh_edge_removal_attempts
           <<" mesh_edge_trials="<<result.recovery.mesh_edge_retriangulation_trials
           <<" max_mesh_edge_degree="<<result.recovery.maximum_mesh_edge_degree_attempted
           <<" edge_attempts="<<result.recovery.attempted_edge_recoveries
           <<" last_cavity="<<result.recovery.last_cavity_cell_count
           <<" last_edge_failure="<<static_cast<unsigned>(result.recovery.last_edge_failure)
           <<" boundary_failure="<<static_cast<unsigned>(
                result.recovery.last_wang_segment_boundary_failure)
           <<" split_failure="<<static_cast<unsigned>(result.recovery.last_constraint_split_failure)
           <<" insertion_failure="<<static_cast<unsigned>(result.recovery.last_split_insertion_failure)
           <<" edge="<<result.recovery.first_unrecovered_edge[0]<<','<<result.recovery.first_unrecovered_edge[1]
           <<" ratio="<<result.recovery.last_split_numerator<<'/'<<result.recovery.last_split_denominator
           <<" recovery_vertices="<<result.recovery.constraints.vertices.size()
           <<" recovery_tets="<<result.recovery.tetrahedra.size()
           <<" journal="<<result.recovery.constraints.recovery_journal.size()
           <<" closest_segment_vertex="<<closest_id
           <<" edge_length2="<<length2
           <<" edge_a="<<a.x<<','<<a.y<<','<<a.z
           <<" edge_b="<<b.x<<','<<b.y<<','<<b.z
           <<" closest_t="<<closest_parameter
           <<" closest_rel2="<<closest_relative_distance
           <<" plane_crossings="<<plane_crossing_faces
           <<" best_crossing_min_bary="<<best_crossing_min_barycentric
           <<" best_crossing_face="<<best_crossing_face[0]<<','
           <<best_crossing_face[1]<<','<<best_crossing_face[2]
           <<" duplicate_cells="<<duplicate_cells
           <<" degenerate_cells="<<degenerate_cells
           <<" nonmanifold_faces="<<nonmanifold_faces
           <<" same_sided_faces="<<same_sided_shared_faces
           <<" midpoint_cells="<<midpoint_containing_cells
           <<" midpoint_face="<<midpoint_shared_face[0]<<','
           <<midpoint_shared_face[1]<<','<<midpoint_shared_face[2]
           <<" final_flip_failure="<<static_cast<unsigned>(final_flip.failure)
           <<" final_flip_cavity="<<final_flip.cavity_tetrahedra.size()
           <<" tets="<<result.tetrahedra.size()<<'\n';
  for(const auto& face:result.recovery.inspection.missing_facets)
    std::cout<<"missing_facet "<<face[0]<<' '<<face[1]<<' '<<face[2]<<'\n';
  for(const auto& cell:degenerate_cell_ids)
    std::cout<<"degenerate_cell "<<cell[0]<<' '<<cell[1]<<' '
             <<cell[2]<<' '<<cell[3]<<'\n';
  for(const auto& split:result.recovery.constraints.split_vertices) {
    auto split_edge=split.edge;
    auto failed_edge=result.recovery.first_unrecovered_edge;
    std::sort(split_edge.begin(),split_edge.end());
    std::sort(failed_edge.begin(),failed_edge.end());
    if(split_edge==failed_edge)
      std::cout<<"prior_split_same_edge "<<split.id<<' '<<split.numerator
               <<'/'<<split.denominator<<'\n';
  }
  for(const auto& facet:result.recovery.constraints.facets) {
    const auto has_first=std::find(
        facet.vertices.begin(),facet.vertices.end(),
        result.recovery.first_unrecovered_edge[0])!=facet.vertices.end();
    const auto has_second=std::find(
        facet.vertices.begin(),facet.vertices.end(),
        result.recovery.first_unrecovered_edge[1])!=facet.vertices.end();
    if(has_first&&has_second)
      std::cout<<"coarse_required_facet "<<facet.vertices[0]<<' '
               <<facet.vertices[1]<<' '<<facet.vertices[2]<<" source "
               <<facet.source_vertices[0]<<' '<<facet.source_vertices[1]<<' '
               <<facet.source_vertices[2]<<'\n';
  }
  auto canonical_split_id=[](std::array<std::uint64_t,2> edge,
                             std::uint32_t numerator,std::uint32_t denominator) {
    std::sort(edge.begin(),edge.end());
    std::uint64_t edge_id=1469598103934665603ULL;
    for(const auto word:edge){edge_id^=word;edge_id*=1099511628211ULL;}
    std::uint64_t value=1469598103934665603ULL;
    for(const auto word:{edge_id,static_cast<std::uint64_t>(numerator),
                         static_cast<std::uint64_t>(denominator)}) {
      value^=word;value*=1099511628211ULL;
    }
    return value|(std::uint64_t{1}<<63U);
  };
  const auto colliding_id=canonical_split_id(
      result.recovery.first_unrecovered_edge,result.recovery.last_split_numerator,
      result.recovery.last_split_denominator);
  for(const auto& vertex:result.recovery.constraints.vertices)
    if(vertex.id==colliding_id)
      std::cout<<"colliding_vertex "<<colliding_id<<' '<<vertex.position.x<<' '
               <<vertex.position.y<<' '<<vertex.position.z<<'\n';
  for(const auto& split:result.recovery.constraints.split_vertices)
    if(split.id==colliding_id)
      std::cout<<"colliding_split "<<split.edge[0]<<' '<<split.edge[1]<<' '
               <<split.numerator<<'/'<<split.denominator<<'\n';
  for(const auto& split:result.recovery.constraints.facet_split_vertices)
    if(split.id==colliding_id)
      std::cout<<"colliding_facet_split "<<split.parent.vertex_ids[0]<<' '
               <<split.parent.vertex_ids[1]<<' '<<split.parent.vertex_ids[2]
               <<'\n';
  for(const auto& cell:final_flip.cavity_tetrahedra) {
    std::cout<<"cavity";
    for(const auto vertex:cell)
      std::cout<<' '<<result.recovery.constraints.vertices[vertex].id;
    std::cout<<'\n';
  }
  for(const auto& attempt:result.boundary_removal_attempts) {
    std::cout<<"boundary_removal "<<attempt.insertion.vertex_id<<' '
             <<static_cast<unsigned>(attempt.insertion.kind)<<' '
             <<attempt.restored<<' '
             <<static_cast<unsigned>(attempt.failure)<<' '
             <<static_cast<unsigned>(attempt.retriangulation_rejection)<<' '
             <<attempt.relocation_regions<<' '
             <<attempt.retry_attempts.size()<<'\n';
    if(attempt.retriangulation_nonmanifold_face_uses!=0U)
      std::cout<<"boundary_nonmanifold "<<attempt.insertion.vertex_id<<' '
               <<attempt.retriangulation_nonmanifold_face[0]<<' '
               <<attempt.retriangulation_nonmanifold_face[1]<<' '
               <<attempt.retriangulation_nonmanifold_face[2]<<' '
               <<attempt.retriangulation_nonmanifold_face_uses<<'\n';
    for(const auto& split:result.recovery.constraints.split_vertices)
      if(split.id==attempt.insertion.vertex_id)
        std::cout<<"boundary_split_record "<<split.id<<' '
                 <<split.edge[0]<<' '<<split.edge[1]<<' '
                 <<split.numerator<<'/'<<split.denominator<<'\n';
    for(const auto& facet:attempt.insertion.replaced_facets)
      std::cout<<"boundary_predecessor "<<attempt.insertion.vertex_id<<' '
               <<facet.vertices[0]<<' '<<facet.vertices[1]<<' '
               <<facet.vertices[2]<<'\n';
    for(const auto& face:attempt.retriangulation_old_boundary_only) {
      std::cout<<"boundary_old_only "<<attempt.insertion.vertex_id;
      for(const auto id:face)std::cout<<' '<<id;
      std::cout<<'\n';
    }
    for(const auto& face:attempt.retriangulation_new_boundary_only) {
      std::cout<<"boundary_new_only "<<attempt.insertion.vertex_id;
      for(const auto id:face)std::cout<<' '<<id;
      std::cout<<'\n';
    }
    for(const auto& retry:attempt.retry_attempts)
      std::cout<<"boundary_retry "<<attempt.insertion.vertex_id<<' '
               <<retry.level<<' '
               <<retry.tiny_tetrahedra<<' '
               <<retry.accepted_topology_mutations<<' '
               <<retry.repair_point_insertions<<' '
               <<retry.recursive_retry_invoked<<'\n';
    if(!attempt.restored)
      for(const auto& facet:result.recovery.constraints.facets)
        if(std::find(facet.vertices.begin(),facet.vertices.end(),
                     attempt.insertion.vertex_id)!=facet.vertices.end())
          std::cout<<"failed_split_facet "<<attempt.insertion.vertex_id<<' '
                   <<facet.vertices[0]<<' '<<facet.vertices[1]<<' '
                   <<facet.vertices[2]<<" source "<<facet.source_vertices[0]<<' '
                   <<facet.source_vertices[1]<<' '<<facet.source_vertices[2]
                   <<" parent "<<facet.parent.vertex_ids[0]<<' '
                   <<facet.parent.vertex_ids[1]<<' '
                   <<facet.parent.vertex_ids[2]<<'\n';
  }
  return result.accepted()?0:1;
}
