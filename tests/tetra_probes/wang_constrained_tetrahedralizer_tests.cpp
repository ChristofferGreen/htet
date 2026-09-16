#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_probes/wang_constrained_tetrahedralizer.hpp"
#include "tetra_probes/wang_local_segment_recovery.hpp"
#include "tetra_probes/wang_ordered_tet_mesh.hpp"
#include "tetra_probes/wang_segment_scheduler.hpp"

#include <limits>
#include <cstdlib>

namespace {
using namespace tetra::probes;

CanonicalPlcConstraintFacet face(std::array<std::uint64_t,3> vertices) {
  CanonicalPlcConstraintFacet value;
  value.parent={vertices}; value.vertices=vertices; value.source_vertices=vertices;
  value.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  return value;
}

CanonicalPlcConstraintSet tetrahedron_plc() {
  CanonicalPlcConstraintSet plc;
  plc.vertices={{10,{0,0,0}},{20,{1,0,0}},{30,{0,1,0}},{40,{0,0,1}}};
  for(const auto vertices:std::array<std::array<std::uint64_t,3>,4>{{
      {{20,30,40}},{{10,40,30}},{{10,20,40}},{{10,30,20}}}})
    plc.facets.push_back(face(vertices));
  return plc;
}

CanonicalPlcConstraintSet cube_plc() {
  CanonicalPlcConstraintSet plc;
  plc.vertices={{10,{0,0,0}},{20,{1,0,0}},{30,{1,1,0}},{40,{0,1,0}},
                {50,{0,0,1}},{60,{1,0,1}},{70,{1,1,1}},{80,{0,1,1}}};
  for(const auto vertices:std::array<std::array<std::uint64_t,3>,12>{{
      {{10,30,20}},{{10,40,30}},{{50,60,70}},{{50,70,80}},
      {{10,20,60}},{{10,60,50}},{{40,80,70}},{{40,70,30}},
      {{10,50,80}},{{10,80,40}},{{20,30,70}},{{20,70,60}}}})
    plc.facets.push_back(face(vertices));
  return plc;
}

CanonicalPlcConstraintSet scheduler_plc() {
  CanonicalPlcConstraintSet plc;
  const std::array<tetra::Vec3,12> points{{
      {-1.294,10.0,4.83},{4.83,0.0,1.294},{4.83,10.0,-1.294},
      {-3.536,0.0,3.536},{4.253,6.532,-2.426},{-0.301,9.760,0.0},
      {3.117,2.999,-2.571},{-2.183,8.657,0.646},{1.874,1.002,-1.808},
      {-3.330,6.864,1.350},{0.163,-0.105,-0.366},{-4.051,3.184,2.242}}};
  for(std::size_t i=0;i<points.size();++i)plc.vertices.push_back({i+1U,points[i]});
  for(const auto vertices:std::array<std::array<std::uint64_t,3>,20>{{
      {{3,4,1}},{{3,5,1}},{{12,4,1}},{{5,6,1}},{{6,7,1}},{{7,8,1}},
      {{8,9,1}},{{9,10,1}},{{10,11,1}},{{11,12,1}},{{3,4,2}},{{3,5,2}},
      {{12,4,2}},{{5,6,2}},{{6,7,2}},{{7,8,2}},{{8,9,2}},{{9,10,2}},
      {{10,11,2}},{{11,12,2}}}})
    plc.facets.push_back(face(vertices));
  return plc;
}

std::pair<CanonicalPlcConstraintSet,WangReferenceSeedTrace> ordered_seed_state(
    CanonicalPlcConstraintSet plc) {
  std::sort(plc.vertices.begin(),plc.vertices.end(),
      [](const auto& left,const auto& right){return left.id<right.id;});
  const auto original_count=plc.vertices.size();
  auto low=plc.vertices.front().position,high=low;
  std::uint64_t next_id{};
  for(const auto& vertex:plc.vertices) {
    low.x=std::min(low.x,vertex.position.x);
    low.y=std::min(low.y,vertex.position.y);
    low.z=std::min(low.z,vertex.position.z);
    high.x=std::max(high.x,vertex.position.x);
    high.y=std::max(high.y,vertex.position.y);
    high.z=std::max(high.z,vertex.position.z);
    next_id=std::max(next_id,vertex.id);
  }
  const auto centre=(low+high)/2.0;
  const auto half=(high-low)/2.0;
  low=centre-half*2.0;high=centre+half*2.0;
  for(const auto point:std::array<tetra::Vec3,8>{
          {{low.x,low.y,low.z},{high.x,low.y,low.z},
           {high.x,high.y,low.z},{low.x,high.y,low.z},
           {low.x,low.y,high.z},{high.x,low.y,high.z},
           {high.x,high.y,high.z},{low.x,high.y,high.z}}})
    plc.vertices.push_back({++next_id,point});
  CanonicalDelaunaySeedInput input;
  input.maximum_vertices=plc.vertices.size();
  for(const auto& vertex:plc.vertices) {
    input.vertices.push_back(vertex.position);
    input.stable_vertex_ids.push_back(vertex.id);
  }
  return {std::move(plc),trace_wang_reference_seed(input,original_count)};
}

std::pair<std::size_t,WangReferenceSeedTrace> ordered_seed(
    CanonicalPlcConstraintSet plc) {
  auto [constraints,trace]=ordered_seed_state(std::move(plc));
  return {constraints.vertices.size(),std::move(trace)};
}
} // namespace

TEST_CASE("prototype-owned Wang state retains ordered scheduler seed topology") {
  const auto [vertex_count,trace]=ordered_seed(scheduler_plc());
  REQUIRE(trace.result.accepted());
  REQUIRE(!trace.stages.empty());
  WangOrderedTetMesh mesh(vertex_count,trace.stages.back());
  REQUIRE(mesh.set_point_incidence(trace.point_to_tetrahedron));
  const auto audit=mesh.audit();
  CHECK(audit.accepted());
  CHECK(audit.active_cells==73U);
  CHECK(mesh.hull_faces().size()==12U);
  CHECK(std::all_of(mesh.point_to_cell().begin(),mesh.point_to_cell().end(),
      [](std::int32_t cell){return cell>=0;}));
}

TEST_CASE("prototype-owned Wang state accepts tetrahedron and cube reference seeds") {
  const auto check=[](const CanonicalPlcConstraintSet& plc,
                      std::size_t expected_cells) {
    const auto [vertex_count,trace]=ordered_seed(plc);
    REQUIRE(trace.result.accepted());
    REQUIRE(!trace.stages.empty());
    WangOrderedTetMesh mesh(vertex_count,trace.stages.back());
    const auto audit=mesh.audit();
    CHECK(audit.accepted());
    CHECK(audit.active_cells==expected_cells);
    CHECK(mesh.hull_faces().size()==12U);
  };
  check(tetrahedron_plc(),26U);
  check(cube_plc(),42U);
}

TEST_CASE("prototype-owned Wang state recycles deleted cell slots in FIFO order") {
  std::vector<WangOrderedTetMesh::Tet> cells{
      {{0,1,2,3}},{{0,1,3,4}},{{0,1,4,5}}};
  WangOrderedTetMesh mesh(6U,cells);
  REQUIRE(mesh.audit().accepted());
  REQUIRE(mesh.erase_cell(0U));
  REQUIRE(mesh.erase_cell(2U));
  CHECK(mesh.available_slots()==2U);
  CHECK(mesh.add_cell({{0,2,3,4}})==0U);
  CHECK(mesh.add_cell({{1,2,4,5}})==2U);
  REQUIRE(mesh.rebuild_topology()==WangOrderedTetMesh::TopologyFailure::none);
  CHECK(mesh.audit().accepted());
}

TEST_CASE("prototype-owned Wang state applies the first two reference flip32 shells") {
  const auto [vertex_count,trace]=ordered_seed(scheduler_plc());
  REQUIRE(trace.result.accepted());
  WangOrderedTetMesh mesh(vertex_count,trace.stages.back());
  REQUIRE(mesh.audit().accepted());

  std::uint32_t first_shell_cell=std::numeric_limits<std::uint32_t>::max();
  for(std::size_t slot=0;slot<mesh.cells().size();++slot) {
    const auto& cell=mesh.cells()[slot];
    if(!cell.deleted&&
       std::find(cell.vertices.begin(),cell.vertices.end(),4U)!=cell.vertices.end()&&
       std::find(cell.vertices.begin(),cell.vertices.end(),7U)!=cell.vertices.end()) {
      first_shell_cell=static_cast<std::uint32_t>(slot);break;
    }
  }
  REQUIRE(first_shell_cell!=std::numeric_limits<std::uint32_t>::max());
  const auto first_shell=mesh.find_shell(first_shell_cell,4U,7U);
  CHECK(first_shell.closed);
  CHECK(first_shell.cells.size()==3U);
  auto first_ring=first_shell.ring_vertices;
  std::sort(first_ring.begin(),first_ring.end());
  CHECK(first_ring==std::vector<std::uint32_t>{{5U,6U,9U}});

  const auto first=mesh.flip32(4U,7U);
  REQUIRE(first.accepted);
  CHECK(mesh.audit().accepted());
  CHECK(mesh.audit().active_cells==72U);

  std::uint32_t second_shell_cell=std::numeric_limits<std::uint32_t>::max();
  for(std::size_t slot=0;slot<mesh.cells().size();++slot) {
    const auto& cell=mesh.cells()[slot];
    if(!cell.deleted&&
       std::find(cell.vertices.begin(),cell.vertices.end(),9U)!=cell.vertices.end()&&
       std::find(cell.vertices.begin(),cell.vertices.end(),4U)!=cell.vertices.end()) {
      second_shell_cell=static_cast<std::uint32_t>(slot);break;
    }
  }
  REQUIRE(second_shell_cell!=std::numeric_limits<std::uint32_t>::max());
  const auto second_shell=mesh.find_shell(second_shell_cell,9U,4U);
  CHECK(second_shell.closed);
  CHECK(second_shell.cells.size()==3U);
  auto second_ring=second_shell.ring_vertices;
  std::sort(second_ring.begin(),second_ring.end());
  CHECK(second_ring==std::vector<std::uint32_t>{{1U,5U,6U}});

  const auto second=mesh.flip32(9U,4U);
  REQUIRE(second.accepted);
  CHECK(mesh.audit().accepted());
  CHECK(mesh.audit().active_cells==71U);
  for(const auto& cell:mesh.cells())if(!cell.deleted) {
    const auto contains=[&](std::uint32_t vertex) {
      return std::find(cell.vertices.begin(),cell.vertices.end(),vertex)!=
          cell.vertices.end();
    };
    const bool has_first_removed_edge=contains(4U)&&contains(7U);
    const bool has_second_removed_edge=contains(9U)&&contains(4U);
    CHECK_FALSE(has_first_removed_edge);
    CHECK_FALSE(has_second_removed_edge);
  }

  std::uint32_t source=std::numeric_limits<std::uint32_t>::max();
  std::uint8_t opposite{};
  for(std::size_t slot=0;slot<mesh.cells().size();++slot) {
    const auto& cell=mesh.cells()[slot];
    if(cell.deleted)continue;
    const auto has=[&](std::uint32_t vertex) {
      return std::find(cell.vertices.begin(),cell.vertices.end(),vertex)!=
          cell.vertices.end();
    };
    if(has(9U)&&has(5U)&&has(1U)&&has(6U)) {
      source=static_cast<std::uint32_t>(slot);
      opposite=static_cast<std::uint8_t>(
          std::find(cell.vertices.begin(),cell.vertices.end(),6U)-
          cell.vertices.begin());
      break;
    }
  }
  REQUIRE(source!=std::numeric_limits<std::uint32_t>::max());
  const auto third=mesh.flip23(source,opposite);
  REQUIRE(third.accepted);
  CHECK(mesh.audit().accepted());
  CHECK(mesh.audit().active_cells==72U);
  CHECK(std::any_of(mesh.cells().begin(),mesh.cells().end(),[](const auto& cell) {
    return !cell.deleted&&
        std::find(cell.vertices.begin(),cell.vertices.end(),0U)!=cell.vertices.end()&&
        std::find(cell.vertices.begin(),cell.vertices.end(),6U)!=cell.vertices.end();
  }));
}

TEST_CASE("prototype-owned Wang flipnm driver recovers the retained local segment") {
  auto [constraints,trace]=ordered_seed_state(scheduler_plc());
  REQUIRE(trace.result.accepted());
  WangOrderedTetMesh mesh(constraints.vertices.size(),trace.stages.back());
  REQUIRE(mesh.set_point_incidence(trace.point_to_tetrahedron));
  const auto recovery=recover_wang_segment_by_local_flips(
      constraints,{{1U,7U}},true,1U,mesh);
  INFO("failure="<<static_cast<unsigned>(recovery.failure)
       <<" mutations="<<recovery.mutations.size());
  for(const auto& mutation:recovery.mutations)
    INFO("mutation kind="<<static_cast<unsigned>(mutation.kind)
         <<" feature="<<mutation.feature[0]<<','<<mutation.feature[1]<<','
         <<mutation.feature[2]<<" cells="<<mutation.active_cells_before
         <<"->"<<mutation.active_cells_after);
  REQUIRE(recovery.recovered);
  CHECK(recovery.failure==WangOwnedLocalRecoveryFailure::none);
  REQUIRE(recovery.mutations.size()==3U);
  CHECK(recovery.mutations[0].kind==WangOwnedLocalMutation::Kind::flip32);
  CHECK(recovery.mutations[0].feature[0]==4U);
  CHECK(recovery.mutations[0].feature[1]==7U);
  CHECK(recovery.mutations[1].kind==WangOwnedLocalMutation::Kind::flip32);
  CHECK(recovery.mutations[1].feature[0]==9U);
  CHECK(recovery.mutations[1].feature[1]==4U);
  CHECK(recovery.mutations[2].kind==WangOwnedLocalMutation::Kind::flip23);
  CHECK(mesh.audit().accepted());
  CHECK(mesh.audit().active_cells==72U);
  REQUIRE(mesh.point_to_cell()[6]>=0);
  CHECK(mesh.cells()[static_cast<std::size_t>(mesh.point_to_cell()[6])].vertices==
        std::array<std::uint32_t,4>{{0U,6U,9U,5U}});
  CHECK(mesh.point_to_cell()[0]==mesh.point_to_cell()[6]);
}

TEST_CASE("prototype-owned Wang scheduler matches the local-flip round prefix") {
  auto [constraints,trace]=ordered_seed_state(scheduler_plc());
  REQUIRE(trace.result.accepted());
  WangOrderedTetMesh mesh(constraints.vertices.size(),trace.stages.back());
  REQUIRE(mesh.set_point_incidence(trace.point_to_tetrahedron));

  const auto scheduler=run_wang_segment_scheduler_local_prefix(constraints,mesh);
  CHECK(scheduler.stop==WangOwnedSegmentSchedulerStop::full_search_required);
  CHECK(scheduler.next_round==4U);
  REQUIRE(scheduler.attempts.size()==8U);
  const std::array<std::array<std::uint64_t,2>,6> first_round{{
      {{3U,4U}},{{7U,1U}},{{9U,1U}},{{8U,9U}},{{11U,1U}},{{8U,2U}}}};
  for(std::size_t i=0;i<first_round.size();++i) {
    CHECK(scheduler.attempts[i].edge==first_round[i]);
    CHECK(scheduler.attempts[i].round==1U);
    CHECK(scheduler.attempts[i].info_before==0);
    CHECK(scheduler.attempts[i].search_depth==1U);
    CHECK_FALSE(scheduler.attempts[i].full_search);
    CHECK(scheduler.attempts[i].outcome==
          (i==0U?WangOwnedSchedulerAttemptOutcome::failed:
                  WangOwnedSchedulerAttemptOutcome::recovered));
  }
  CHECK(scheduler.attempts[6].edge==std::array<std::uint64_t,2>{{4U,3U}});
  CHECK(scheduler.attempts[6].round==2U);
  CHECK(scheduler.attempts[6].info_before==-1);
  CHECK(scheduler.attempts[6].search_depth==12U);
  CHECK(scheduler.attempts[6].outcome==WangOwnedSchedulerAttemptOutcome::failed);
  CHECK(scheduler.attempts[7].edge==std::array<std::uint64_t,2>{{3U,4U}});
  CHECK(scheduler.attempts[7].round==3U);
  CHECK(scheduler.attempts[7].info_before==-2);
  CHECK(scheduler.attempts[7].search_depth==23U);
  CHECK(scheduler.attempts[7].outcome==WangOwnedSchedulerAttemptOutcome::failed);
  REQUIRE(scheduler.lost_edges.size()==1U);
  const auto& pending=scheduler.surface_edges[scheduler.lost_edges.front()];
  CHECK(pending.vertices==std::array<std::uint64_t,2>{{4U,3U}});
  CHECK(pending.info==-3);
  CHECK(mesh.audit().accepted());
}

TEST_CASE("owned Wang scheduler preserves a free vertex obstruction for the negative-point arm") {
  // The required boundary edge 10--20 passes through disposable interior
  // point 50.  Each other literal boundary edge is already a mesh edge, so
  // this is a single, direct `Across Vertex` scheduler witness.
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={
      {10U,{0.0,0.0,0.0}},{20U,{2.0,0.0,0.0}},
      {30U,{1.0,1.0,0.0}},{40U,{1.0,0.0,1.0}},
      {50U,{1.0,0.0,0.0}}};
  constraints.facets={facet({{10U,20U,30U}}),facet({{20U,10U,40U}})};
  constraints.interior_steiner_vertices.push_back(
      {50U,CanonicalInteriorSteinerKind::cascade_fhc});
  WangOrderedTetMesh mesh(constraints.vertices.size(),{
      {{0U,2U,3U,4U}},{{1U,3U,2U,4U}}});
  REQUIRE(mesh.audit().accepted());

  const auto local=recover_wang_segment_by_local_flips(
      constraints,{{10U,20U}},false,1U,mesh);
  CHECK_FALSE(local.recovered);
  CHECK(local.failure==WangOwnedLocalRecoveryFailure::vertex_obstruction);
  CHECK(local.obstructing_vertex==50U);

  const auto scheduler=run_wang_segment_scheduler_local_prefix(constraints,mesh);
  CHECK(scheduler.stop==WangOwnedSegmentSchedulerStop::interior_vertex_obstruction);
  CHECK(scheduler.obstructing_vertex==50U);
  REQUIRE(scheduler.lost_edges.size()==1U);
  CHECK(scheduler.surface_edges[scheduler.lost_edges.front()].vertices==
        std::array<std::uint64_t,2>{{20U,10U}});
  REQUIRE(scheduler.continuation.has_value());
  CHECK(scheduler.continuation->remaining_round.empty());

  const auto promoted=promote_canonical_interior_steiner_point_to_segment(
      constraints,{{10U,20U}},50U);
  REQUIRE(promoted.accepted());
  CHECK(promoted.constraints.vertices[4].position.x==constraints.vertices[4].position.x);
  CHECK(promoted.constraints.vertices[4].position.y==constraints.vertices[4].position.y);
  CHECK(promoted.constraints.vertices[4].position.z==constraints.vertices[4].position.z);
  CHECK(promoted.constraints.interior_steiner_vertices.empty());
  REQUIRE(promoted.constraints.split_vertices.size()==1U);
  CHECK(promoted.constraints.split_vertices.front().id==50U);
}

TEST_CASE("prototype-owned Wang full search walks the retained eight faces") {
  auto [constraints,trace]=ordered_seed_state(scheduler_plc());
  REQUIRE(trace.result.accepted());
  WangOrderedTetMesh mesh(constraints.vertices.size(),trace.stages.back());
  REQUIRE(mesh.set_point_incidence(trace.point_to_tetrahedron));
  const auto scheduler=run_wang_segment_scheduler_local_prefix(constraints,mesh);
  REQUIRE(scheduler.stop==WangOwnedSegmentSchedulerStop::full_search_required);
  REQUIRE(scheduler.lost_edges.size()==1U);
  const auto edge=scheduler.surface_edges[scheduler.lost_edges.front()].vertices;

  CHECK_FALSE(recover_wang_segment_by_local_flips(
      constraints,edge,false,1000U,mesh).recovered);
  CHECK_FALSE(recover_wang_segment_by_local_flips(
      constraints,edge,true,1000U,mesh).recovered);
  const auto full=recover_wang_segment_by_full_search(
      constraints,edge,1000U,mesh);
  CHECK_FALSE(full.recovered);
  CHECK(full.failure==WangOwnedFullSearchFailure::none);
  CHECK(full.successful_removals==0U);
  REQUIRE(full.features.size()==8U);
  const std::array<std::array<std::uint32_t,3>,8> expected{{
      {{0U,1U,11U}},{{10U,0U,1U}},{{0U,1U,9U}},{{1U,8U,0U}},
      {{1U,7U,0U}},{{6U,0U,1U}},{{0U,1U,5U}},{{4U,0U,1U}}}};
  for(std::size_t i=0;i<expected.size();++i) {
    CHECK(full.features[i].kind==WangOwnedFullSearchFeatureKind::face);
    auto actual=full.features[i].vertices,want=expected[i];
    std::sort(actual.begin(),actual.end());
    std::sort(want.begin(),want.end());
    CHECK(actual==want);
  }
  CHECK(mesh.audit().accepted());
  CHECK(mesh.audit().active_cells==74U);
}

TEST_CASE("prototype-owned Wang scheduler reaches the first Steiner round") {
  auto [constraints,trace]=ordered_seed_state(scheduler_plc());
  REQUIRE(trace.result.accepted());
  WangOrderedTetMesh mesh(constraints.vertices.size(),trace.stages.back());
  REQUIRE(mesh.set_point_incidence(trace.point_to_tetrahedron));
  const auto scheduler=run_wang_segment_scheduler_pre_steiner(constraints,mesh);
  CHECK(scheduler.stop==WangOwnedSegmentSchedulerStop::steiner_insertion_required);
  CHECK(scheduler.next_round==5U);
  // AutorecoverEdges does not jump directly from its escalation state to
  // addinnerSteiner_Edge: recoverEdge first performs its two local passes
  // and its full-search pass for this same, FHC-enabled attempt.
  REQUIRE(scheduler.attempts.size()==10U);
  const auto& full=scheduler.attempts[8U];
  CHECK(full.edge==std::array<std::uint64_t,2>{{4U,3U}});
  CHECK(full.round==4U);
  CHECK(full.info_before==-3);
  CHECK(full.search_depth==1000U);
  CHECK(full.full_search);
  CHECK(full.steiner_mode==0U);
  CHECK(full.outcome==WangOwnedSchedulerAttemptOutcome::failed);
  const auto& fhc_handoff=scheduler.attempts.back();
  CHECK(fhc_handoff.edge==std::array<std::uint64_t,2>{{3U,4U}});
  CHECK(fhc_handoff.round==5U);
  CHECK(fhc_handoff.info_before==-4);
  CHECK(fhc_handoff.search_depth==1000U);
  CHECK(fhc_handoff.full_search);
  CHECK(fhc_handoff.steiner_mode==1U);
  CHECK(fhc_handoff.outcome==WangOwnedSchedulerAttemptOutcome::failed);
  REQUIRE(scheduler.lost_edges.size()==1U);
  const auto& pending=scheduler.surface_edges[scheduler.lost_edges.front()];
  CHECK(pending.vertices==std::array<std::uint64_t,2>{{3U,4U}});
  CHECK(pending.info==-4);
  CHECK(mesh.audit().accepted());
  CHECK(mesh.audit().active_cells==74U);
}

TEST_CASE("prototype-owned Wang scheduler resumes the live queue after FHC") {
  const auto constraints=tetrahedron_plc();
  WangOrderedTetMesh mesh(constraints.vertices.size(),
                          {{{0U,1U,2U,3U}}});
  REQUIRE(mesh.audit().accepted());
  // Model the precise queue boundary immediately after a successful FHC on
  // edge zero: edge one has not yet been popped in round five, while edge two
  // failed earlier in that same round and must remain behind it until the
  // round's updateFliptype pass.
  WangOwnedSegmentSchedulerState state;
  state.surface_edges={
      {{{10U,20U}},{{0U,1U}},-4},
      {{{20U,30U}},{{1U,2U}},0},
      {{{30U,40U}},{{2U,3U}},0}};
  state.remaining_round={1U};
  state.failed_earlier_this_round={2U};
  state.previous_lost_count={{10U,1},{20U,2},{30U,2},{40U,1}};
  state.round=5U;

  const auto resumed=resume_wang_segment_scheduler_after_fhc(
      constraints,mesh,state);
  CHECK(resumed.stop==WangOwnedSegmentSchedulerStop::complete);
  REQUIRE(resumed.attempts.size()==2U);
  CHECK(resumed.attempts[0].edge==std::array<std::uint64_t,2>{{20U,30U}});
  CHECK(resumed.attempts[0].round==5U);
  CHECK(resumed.attempts[0].info_before==0);
  CHECK(resumed.attempts[1].edge==std::array<std::uint64_t,2>{{40U,30U}});
  CHECK(resumed.attempts[1].round==6U);
  // updateFliptype sees endpoint 30 lose one incident unresolved edge, so
  // it increases (rather than decreases) this edge's escalation score.
  CHECK(resumed.attempts[1].info_before==0);
  CHECK(mesh.audit().accepted());

  auto final_edge_state=state;
  final_edge_state.remaining_round.clear();
  final_edge_state.failed_earlier_this_round.clear();
  final_edge_state.round=7U;
  const auto after_last_fhc=resume_wang_segment_scheduler_after_fhc(
      constraints,mesh,final_edge_state);
  CHECK(after_last_fhc.stop==WangOwnedSegmentSchedulerStop::complete);
  CHECK(after_last_fhc.next_round==8U);
}

TEST_CASE("prototype-owned Wang full search rotates through an edge shell") {
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={
      {1U,{-1.0,0.0,0.0}},{2U,{1.0,0.0,0.0}},
      {3U,{0.0,-1.0,0.0}},{4U,{0.0,1.0,0.0}},
      {5U,{0.0,0.0,1.0}},{6U,{0.0,0.0,-1.0}}};
  const std::vector<WangOrderedTetMesh::Tet> cells{
      {{0U,2U,3U,4U}},{{1U,4U,3U,2U}},
      {{1U,2U,3U,5U}},{{0U,5U,3U,2U}}};
  WangOrderedTetMesh mesh(constraints.vertices.size(),cells);
  REQUIRE(mesh.audit().accepted());
  const auto trace=inspect_wang_full_search_features(
      constraints,{{1U,2U}},mesh);
  CHECK(trace.failure==WangOwnedFullSearchFailure::none);
  REQUIRE(trace.features.size()==1U);
  CHECK(trace.features[0].kind==WangOwnedFullSearchFeatureKind::edge);
  auto edge=std::array<std::uint32_t,2>{{
      trace.features[0].vertices[0],trace.features[0].vertices[1]}};
  std::sort(edge.begin(),edge.end());
  CHECK(edge==std::array<std::uint32_t,2>{{2U,3U}});
}

TEST_CASE("prototype-owned Wang round-five full search exposes the first FHC") {
  auto [constraints,seed]=ordered_seed_state(scheduler_plc());
  REQUIRE(seed.result.accepted());
  WangOrderedTetMesh mesh(constraints.vertices.size(),seed.stages.back());
  REQUIRE(mesh.set_point_incidence(seed.point_to_tetrahedron));
  const auto scheduler=run_wang_segment_scheduler_pre_steiner(constraints,mesh);
  REQUIRE(scheduler.stop==WangOwnedSegmentSchedulerStop::steiner_insertion_required);
  const auto edge=scheduler.surface_edges[scheduler.lost_edges.front()].vertices;
  // The scheduler has already made this source-defined forward/reverse/full
  // attempt.  Replaying it here would test a different state than the FHC
  // hand-off receives.
  REQUIRE(!scheduler.attempts.empty());
  const auto& handoff=scheduler.attempts.back();
  CHECK(handoff.edge==edge);
  CHECK(handoff.full_search);
  CHECK(handoff.steiner_mode==1U);
  CHECK(handoff.outcome==WangOwnedSchedulerAttemptOutcome::failed);
  const auto trace=inspect_wang_full_search_features(constraints,edge,mesh);
  CHECK(trace.failure==WangOwnedFullSearchFailure::none);
  REQUIRE(trace.features.size()==8U);
  REQUIRE(trace.features[0].kind==WangOwnedFullSearchFeatureKind::face);
  auto face=trace.features[0].vertices;
  std::sort(face.begin(),face.end());
  CHECK(face==std::array<std::uint32_t,3>{{0U,1U,4U}});
  CHECK(mesh.audit().accepted());
  CHECK(mesh.audit().active_cells==74U);

  const tetra::Vec3 expected_point{
      std::strtod("0x1.d6a2fc60a59b8p+0",nullptr),
      std::strtod("0x1.fc092f811c5c5p+2",nullptr),
      std::strtod("0x1.9d79a55b5dac7p-1",nullptr)};
  const auto insertion=insert_first_wang_locked_fhc_point(
      constraints,edge,mesh);
  REQUIRE(insertion.inserted);
  CHECK(insertion.failure==WangOwnedLockedFhcFailure::none);
  auto inserted_face=insertion.intersecting_face;
  std::sort(inserted_face.begin(),inserted_face.end());
  CHECK(inserted_face==std::array<std::uint32_t,3>{{0U,1U,4U}});
  auto locking_edge=insertion.locking_edge;
  std::sort(locking_edge.begin(),locking_edge.end());
  CHECK(locking_edge==std::array<std::uint32_t,2>{{0U,4U}});
  CHECK(insertion.inserted_point.x==expected_point.x);
  CHECK(insertion.inserted_point.y==expected_point.y);
  CHECK(insertion.inserted_point.z==expected_point.z);
  CHECK(insertion.tetrahedra.size()==79U);
  const auto applied=mesh.replace_cavity_with_appended_vertex(
      insertion.ordered_cavity_tetrahedra,insertion.replacement_tetrahedra);
  REQUIRE(applied.accepted);
  CHECK(mesh.vertex_count()==constraints.vertices.size());
  CHECK(mesh.audit().accepted());
  CHECK(mesh.audit().active_cells==79U);
  std::vector<WangOrderedTetMesh::Tet> applied_cells;
  for(const auto& cell:mesh.cells())if(!cell.deleted) {
    auto key=cell.vertices;std::sort(key.begin(),key.end());
    applied_cells.push_back(key);
  }
  auto expected_cells=insertion.tetrahedra;
  for(auto& cell:expected_cells)std::sort(cell.begin(),cell.end());
  std::sort(applied_cells.begin(),applied_cells.end());
  std::sort(expected_cells.begin(),expected_cells.end());
  CHECK(applied_cells==expected_cells);
}

TEST_CASE("prototype-owned Wang mode-one recursion inserts the five retained points") {
  auto [constraints,seed]=ordered_seed_state(scheduler_plc());
  REQUIRE(seed.result.accepted());
  WangOrderedTetMesh mesh(constraints.vertices.size(),seed.stages.back());
  REQUIRE(mesh.set_point_incidence(seed.point_to_tetrahedron));
  const auto scheduler=run_wang_segment_scheduler_pre_steiner(constraints,mesh);
  REQUIRE(scheduler.stop==WangOwnedSegmentSchedulerStop::steiner_insertion_required);
  REQUIRE(scheduler.lost_edges.size()==1U);
  const auto edge=scheduler.surface_edges[scheduler.lost_edges.front()].vertices;
  const auto recovered=recover_wang_segment_with_interior_steiner_mode1(
      constraints,edge,mesh);
  CHECK_FALSE(recovered.recovered);
  CHECK(recovered.failure==WangOwnedInteriorSteinerFailure::none);
  REQUIRE(recovered.inserted_points.size()==5U);
  const std::array<tetra::Vec3,5> expected{{
      {std::strtod("0x1.d6a2fc60a59b8p+0",nullptr),
       std::strtod("0x1.fc092f811c5c5p+2",nullptr),
       std::strtod("0x1.9d79a55b5dac7p-1",nullptr)},
      {std::strtod("0x1.05213ae665309p+1",nullptr),
       std::strtod("0x1.52f538c78e8b7p+2",nullptr),
       std::strtod("0x1.3f1f7d28d253bp-1",nullptr)},
      {std::strtod("0x1.21db42b34c667p+0",nullptr),
       std::strtod("0x1.25f83837b4a6fp+2",nullptr),
       std::strtod("0x1.00249939cb82fp+0",nullptr)},
      {std::strtod("0x1.8746430746d5dp-2",nullptr),
       std::strtod("0x1.535185f261955p+2",nullptr),
       std::strtod("0x1.6582dd1c3eaf5p+0",nullptr)},
      {std::strtod("0x1.0422d7a7dab41p-1",nullptr),
       std::strtod("0x1.da6faf3caaea9p+1",nullptr),
       std::strtod("0x1.5ff61082f42fp+0",nullptr)}}};
  for(std::size_t i=0;i<expected.size();++i) {
    CHECK(recovered.inserted_points[i].x==expected[i].x);
    CHECK(recovered.inserted_points[i].y==expected[i].y);
    CHECK(recovered.inserted_points[i].z==expected[i].z);
  }
  CHECK(mesh.audit().accepted());
  CHECK(mesh.audit().active_cells==102U);
}

TEST_CASE("prototype-owned Wang line triangle arithmetic matches retained hits") {
  const auto plc=scheduler_plc();
  const auto hit3=compute_wang_segment_triangle_hit(
      plc.vertices[2].position,plc.vertices[3].position,
      plc.vertices[0].position,plc.vertices[7].position,
      plc.vertices[1].position);
  CHECK(hit3.x==std::strtod("0x1.7fdffab2c4418p-1",nullptr));
  CHECK(hit3.y==std::strtod("0x1.47dc5eecbfbfap+2",nullptr));
  CHECK(hit3.z==std::strtod("0x1.0fc9f4a325181p+0",nullptr));
  const auto hit5=compute_wang_segment_triangle_hit(
      plc.vertices[2].position,plc.vertices[3].position,
      plc.vertices[0].position,plc.vertices[9].position,
      plc.vertices[1].position);
  CHECK(hit5.x==std::strtod("0x1.8d10def203853p-6",nullptr));
  CHECK(hit5.y==std::strtod("0x1.105bc03310c23p+2",nullptr));
  CHECK(hit5.z==std::strtod("0x1.7b05026e3cce8p+0",nullptr));
}

TEST_CASE("pinned Wang oracle recovers the tetrahedron") {
  const auto result=recover_wang_constraints_from_pinned_author_code(
      tetrahedron_plc());
  REQUIRE(result.accepted());
  CHECK(result.seed_failure==CanonicalDelaunaySeedFailure::none);
  CHECK(result.edges_recovered_before_facet_stage);
  CHECK(result.inspection.accepted());
  CHECK(result.tetrahedra.size()==1U);
}

TEST_CASE("pinned Wang oracle recovers the cube without a prototype reverse pass") {
  const auto result=recover_wang_constraints_from_pinned_author_code(cube_plc());
  REQUIRE(result.accepted());
  CHECK(result.edges_recovered_before_facet_stage);
  CHECK(result.inspection.accepted());
  CHECK(result.tetrahedra.size()==6U);
}

TEST_CASE("Wang wrapper exposes the completed owned segment-stage handoff") {
  const auto result=tetrahedralize_wang_constrained_plc(scheduler_plc());

  // This fixture drains the owned AutorecoverEdges queue, then deliberately
  // stops at the next unimplemented paper stage.  It must not be reported as
  // a segment failure merely because facet recovery is still pending.
  CHECK(result.initial_tetrahedralization_complete);
  CHECK(result.segment_recovery_complete);
  CHECK_FALSE(result.facet_recovery_complete);
  CHECK(result.failure==WangConstrainedTetrahedralizationFailure::facet_recovery_failed);
  CHECK(result.recovery.failure==CanonicalPlcRecoveryFailure::facet_recovery_required);
  CHECK(result.recovery.segment_stage_constraints.vertices.size()<=
        result.recovery.constraints.vertices.size());
  CHECK(result.recovery.segment_stage_constraints.facets.size()==
        result.recovery.constraints.facets.size());
  CHECK(result.recovery.segment_stage_constraints.interior_steiner_vertices.size()<=
        result.recovery.constraints.interior_steiner_vertices.size());
  REQUIRE_FALSE(result.recovery.segment_stage_tetrahedra.empty());
  CHECK(inspect_canonical_plc_tetrahedra(
      result.recovery.segment_stage_constraints,
      result.recovery.segment_stage_tetrahedra).missing_edges.empty());
}

TEST_CASE("Wang wrapper does not publish a resource-limited segment stage") {
  WangConstrainedTetrahedralizationOptions options;
  options.recovery.maximum_edge_splits=0U;
  const auto result=tetrahedralize_wang_constrained_plc(scheduler_plc(),options);

  REQUIRE(result.initial_tetrahedralization_complete);
  CHECK_FALSE(result.segment_recovery_complete);
  CHECK_FALSE(result.facet_recovery_complete);
  CHECK(result.failure==WangConstrainedTetrahedralizationFailure::segment_recovery_failed);
  CHECK(result.recovery.failure==CanonicalPlcRecoveryFailure::resource_limit);
  CHECK(result.recovery.resource_limit==WangRecoveryResourceLimit::segment_boundary_splits);
}

TEST_CASE("Wang wrapper reports a seed-stage resource limit as initial failure") {
  WangConstrainedTetrahedralizationOptions options;
  options.recovery.maximum_vertices=scheduler_plc().vertices.size();
  const auto result=tetrahedralize_wang_constrained_plc(scheduler_plc(),options);

  CHECK_FALSE(result.initial_tetrahedralization_complete);
  CHECK_FALSE(result.segment_recovery_complete);
  CHECK(result.failure==
        WangConstrainedTetrahedralizationFailure::initial_tetrahedralization_failed);
  CHECK(result.recovery.failure==CanonicalPlcRecoveryFailure::resource_limit);
  CHECK(result.recovery.resource_limit==WangRecoveryResourceLimit::initial_vertices);
}

TEST_CASE("Wang local edge conformance retains the 72-cell author snapshot") {
  const auto trace=trace_wang_author_local_edge_sequence(scheduler_plc(),{{1U,7U}});
  REQUIRE(trace.target_found);
  CHECK(trace.forward_result==1);
  CHECK(trace.after_forward_cells.size()==72U);
  CHECK(trace.reverse_source_found);
  CHECK(trace.reverse_result==1);
  CHECK(trace.recovered_after_reverse);
}
