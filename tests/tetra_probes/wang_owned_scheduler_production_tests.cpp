#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_probes/canonical_delaunay_seed.hpp"
#include "tetra_probes/wang_ordered_tet_mesh.hpp"
#include "tetra_probes/wang_local_segment_recovery.hpp"
#include "tetra_probes/wang_segment_scheduler.hpp"
#include "tetra_probes/wang_constrained_tetrahedralizer.hpp"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace {
using namespace tetra::probes;

CanonicalPlcConstraintFacet facet(std::array<std::uint64_t,3> vertices) {
  CanonicalPlcConstraintFacet value;
  value.parent={vertices};
  value.vertices=vertices;
  value.source_vertices=vertices;
  value.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  return value;
}

CanonicalPlcConstraintSet full_search_fixture() {
  CanonicalPlcConstraintSet plc;
  const std::array<tetra::Vec3,12> points{{
      {-1.294,10.0,4.83},{4.83,0.0,1.294},{4.83,10.0,-1.294},
      {-3.536,0.0,3.536},{4.253,6.532,-2.426},{-0.301,9.760,0.0},
      {3.117,2.999,-2.571},{-2.183,8.657,0.646},{1.874,1.002,-1.808},
      {-3.330,6.864,1.350},{0.163,-0.105,-0.366},{-4.051,3.184,2.242}}};
  for(std::size_t i=0;i<points.size();++i)
    plc.vertices.push_back({i+1U,points[i]});
  for(const auto vertices:std::array<std::array<std::uint64_t,3>,20>{{
      {{3,4,1}},{{3,5,1}},{{12,4,1}},{{5,6,1}},{{6,7,1}},{{7,8,1}},
      {{8,9,1}},{{9,10,1}},{{10,11,1}},{{11,12,1}},{{3,4,2}},{{3,5,2}},
      {{12,4,2}},{{5,6,2}},{{6,7,2}},{{7,8,2}},{{8,9,2}},{{9,10,2}},
      {{10,11,2}},{{11,12,2}}}})
    plc.facets.push_back(facet(vertices));
  return plc;
}

// A closed cuboid whose upper face opens into an offset, deep rectangular
// well.  This is a genuine concave PLC (every undirected surface edge has two
// oppositely directed uses), not the earlier open-edge contact control.
CanonicalPlcConstraintSet closed_well_info2_fixture() {
  CanonicalPlcConstraintSet plc;
  const std::array<tetra::Vec3,16> points{{
      {-1.9095753177654222,-1.2288082402260079,-2.3150262651494558},
      { 1.9095753177654222,-1.2288082402260079,-2.3150262651494558},
      { 1.9095753177654222, 1.2288082402260079,-2.3150262651494558},
      {-1.9095753177654222, 1.2288082402260079,-2.3150262651494558},
      {-1.9095753177654222,-1.2288082402260079, 2.3130626531205447},
      { 1.9095753177654222,-1.2288082402260079, 2.3130626531205447},
      { 1.9095753177654222, 1.2288082402260079, 2.3130626531205447},
      {-1.9095753177654222, 1.2288082402260079, 2.3130626531205447},
      {-.46354617909137036,-.63175209486573602, 2.3130626531205447},
      { .387144125054876,-.63175209486573602, 2.3130626531205447},
      { .387144125054876, .34650385483539947, 2.3130626531205447},
      {-.46354617909137036, .34650385483539947, 2.3130626531205447},
      {-.46354617909137036,-.63175209486573602,-2.0826769278508648},
      { .387144125054876,-.63175209486573602,-2.0826769278508648},
      { .387144125054876, .34650385483539947,-2.0826769278508648},
      {-.46354617909137036, .34650385483539947,-2.0826769278508648}}};
  for(std::size_t i=0;i<points.size();++i)plc.vertices.push_back({i+1U,points[i]});
  for(const auto face:std::array<std::array<std::uint64_t,3>,28>{{
      {{1,3,2}},{{1,4,3}},{{1,2,6}},{{1,6,5}},{{2,3,7}},{{2,7,6}},
      {{3,4,8}},{{3,8,7}},{{4,1,5}},{{4,5,8}},{{5,6,10}},{{5,10,9}},
      {{6,7,11}},{{6,11,10}},{{7,8,12}},{{7,12,11}},{{8,5,9}},{{8,9,12}},
      {{9,10,14}},{{9,14,13}},{{10,11,15}},{{10,15,14}},{{11,12,16}},
      {{11,16,15}},{{12,9,13}},{{12,13,16}},{{13,14,15}},{{13,15,16}}}})
    plc.facets.push_back(facet(face));
  return plc;
}

double author_orient(tetra::Vec3 a,tetra::Vec3 b,tetra::Vec3 c,tetra::Vec3 d) {
  const double adx=a.x-d.x,bdx=b.x-d.x,cdx=c.x-d.x;
  const double ady=a.y-d.y,bdy=b.y-d.y,cdy=c.y-d.y;
  const double adz=a.z-d.z,bdz=b.z-d.z,cdz=c.z-d.z;
  return adz*(bdx*cdy-cdx*bdy)+bdz*(cdx*ady-adx*cdy)+
         cdz*(adx*bdy-bdx*ady);
}

// Frozen finite state captured from the pinned author implementation directly
// before its mode-one FHC call.  This is deliberately test data, not a
// production import: it separates the FHC operation from earlier Delaunay and
// local-flip history while retaining source cell slots and P2T choices.
struct AuthorPreFhcFixture {
  CanonicalPlcConstraintSet constraints;
  std::vector<WangOrderedTetMesh::Tet> cells;
  std::vector<WangOrderedTetMesh::Tet> incidence;
};

AuthorPreFhcFixture author_pre_fhc_fixture() {
  auto constraints=full_search_fixture();
  for(const auto& point:std::array<tetra::Vec3,8>{{
      {-0x1.0fba5e353f7cfp+3,-0x1.4a147ae147ae2p+2,-0x1.91604189374bcp+2},
      { 0x1.28a7ef9db22d1p+3,-0x1.4a147ae147ae2p+2,-0x1.91604189374bcp+2},
      { 0x1.28a7ef9db22d1p+3, 0x1.e1ae147ae147bp+3,-0x1.91604189374bcp+2},
      {-0x1.0fba5e353f7cfp+3, 0x1.e1ae147ae147bp+3,-0x1.91604189374bcp+2},
      {-0x1.0fba5e353f7cfp+3,-0x1.4a147ae147ae2p+2, 0x1.10f9db22d0e56p+3},
      { 0x1.28a7ef9db22d1p+3,-0x1.4a147ae147ae2p+2, 0x1.10f9db22d0e56p+3},
      { 0x1.28a7ef9db22d1p+3, 0x1.e1ae147ae147bp+3, 0x1.10f9db22d0e56p+3},
      {-0x1.0fba5e353f7cfp+3, 0x1.e1ae147ae147bp+3, 0x1.10f9db22d0e56p+3}}})
    constraints.vertices.push_back({constraints.vertices.size()+1U,point});
  // Author node 12 is the ghost hull vertex. Its coordinate is never read by
  // a valid finite feature calculation; the owned mesh retains it solely for
  // the source traversal's neighbour graph.
  constraints.vertices.push_back({constraints.vertices.size()+1U,{0,0,0}});
  std::vector<WangOrderedTetMesh::Tet> cells{
      {{16U,3U,10U,12U}}, {{15U,5U,7U,6U}}, {{19U,7U,0U,9U}},
      {{15U,9U,11U,12U}}, {{13U,12U,8U,6U}}, {{13U,1U,4U,6U}},
      {{13U,8U,1U,6U}}, {{19U,15U,7U,9U}}, {{9U,7U,0U,8U}},
      {{19U,16U,0U,17U}}, {{4U,5U,6U,1U}}, {{1U,11U,10U,3U}},
      {{6U,0U,1U,5U}}, {{0U,9U,8U,1U}}, {{10U,1U,8U,9U}},
      {{1U,7U,8U,0U}}, {{8U,7U,12U,9U}}, {{12U,7U,6U,15U}},
      {{4U,0U,5U,1U}}, {{4U,2U,0U,1U}}, {{4U,5U,0U,2U}},
      {{8U,10U,9U,11U}}, {{19U,17U,0U,18U}}, {{12U,9U,11U,8U}},
      {{1U,7U,0U,6U}}, {{1U,8U,7U,6U}}, {{10U,0U,11U,1U}},
      {{10U,0U,1U,9U}}, {{5U,7U,6U,0U}}, {{12U,7U,15U,9U}},
      {{18U,13U,1U,14U}}, {{12U,11U,10U,8U}}, {{16U,11U,3U,12U}},
      {{12U,10U,11U,3U}}, {{10U,11U,0U,9U}}, {{3U,0U,1U,11U}},
      {{19U,0U,5U,18U}}, {{13U,10U,1U,8U}}, {{13U,12U,10U,8U}},
      {{14U,1U,2U,4U}}, {{14U,13U,1U,4U}}, {{14U,13U,6U,12U}},
      {{14U,4U,6U,13U}}, {{16U,15U,11U,12U}}, {{17U,3U,0U,1U}},
      {{17U,0U,3U,16U}}, {{15U,4U,5U,6U}}, {{15U,14U,6U,12U}},
      {{15U,4U,6U,14U}}, {{15U,2U,5U,4U}}, {{15U,14U,2U,4U}},
      {{15U,5U,2U,14U}}, {{8U,7U,6U,12U}}, {{17U,13U,10U,1U}},
      {{17U,10U,3U,1U}}, {{17U,3U,10U,16U}}, {{17U,12U,10U,13U}},
      {{17U,10U,12U,16U}}, {{18U,1U,13U,17U}}, {{18U,0U,5U,2U}},
      {{18U,2U,5U,14U}}, {{18U,14U,1U,2U}}, {{18U,1U,0U,2U}},
      {{18U,0U,1U,17U}}, {{19U,15U,9U,11U}}, {{19U,16U,15U,11U}},
      {{19U,9U,0U,11U}}, {{19U,15U,14U,5U}}, {{19U,5U,14U,18U}},
      {{19U,0U,7U,5U}}, {{19U,5U,7U,15U}}, {{19U,11U,0U,3U}},
      {{19U,3U,0U,16U}}, {{19U,16U,11U,3U}},
      {{19U,14U,15U,20U}}, {{18U,17U,13U,20U}},
      {{19U,15U,16U,20U}}, {{19U,18U,14U,20U}},
      {{15U,14U,12U,20U}}, {{17U,12U,13U,20U}},
      {{19U,16U,17U,20U}}, {{14U,13U,12U,20U}},
      {{18U,13U,14U,20U}}, {{17U,16U,12U,20U}},
      {{16U,15U,12U,20U}}, {{19U,17U,18U,20U}}};
  const std::vector<WangOrderedTetMesh::Tet> incidence{{
      cells[15],cells[15],cells[62],cells[35],cells[10],cells[28],
      cells[24],cells[15],cells[15],cells[8],cells[26],cells[26],
      cells[52],cells[58],cells[68],cells[17],cells[73],cells[63],
      cells[68],cells[73],cells[74]}};
  return {std::move(constraints),std::move(cells),incidence};
}

TEST_CASE("owned ordered mesh retains the closed Cascade-FHC edge shell") {
  constexpr double root3=1.7320508075688772935;
  const std::vector<tetra::Vec3> points{{
      {0,0,-1},{0,0,1},{2,0,0},{1,root3,0},{-1,root3,0},
      {-2,0,0},{-1,-root3,0},{1,-root3,0}}};
  std::vector<WangOrderedTetMesh::Tet> cells;
  for(std::uint32_t i=0U;i<6U;++i)
    cells.push_back({{0U,1U,2U+i,2U+(i+1U)%6U}});
  WangOrderedTetMesh mesh(points.size(),cells);
  REQUIRE(mesh.audit().accepted());
  const auto shell=mesh.find_shell(0U,0U,1U);
  CHECK(shell.closed);
  CHECK(shell.cells.size()==6U);
  CHECK(shell.ring_vertices==
        std::vector<std::uint32_t>{{2U,3U,4U,5U,6U,7U}});
}

TEST_CASE("owned flip32 local topology matches a complete rebuild") {
  WangOrderedTetMesh mesh(5U,{
      {{0U,1U,2U,3U}},{{0U,1U,3U,4U}},{{0U,1U,4U,2U}}});
  REQUIRE(mesh.audit().accepted());
  const auto flip=mesh.flip32(0U,1U);
  REQUIRE(flip.accepted);
  REQUIRE(mesh.audit().accepted());

  auto rebuilt=mesh;
  REQUIRE(rebuilt.rebuild_topology()==WangOrderedTetMesh::TopologyFailure::none);
  REQUIRE(rebuilt.audit().accepted());
  REQUIRE(mesh.cells().size()==rebuilt.cells().size());
  for(std::size_t cell=0;cell<mesh.cells().size();++cell) {
    CHECK(mesh.cells()[cell].deleted==rebuilt.cells()[cell].deleted);
    CHECK(mesh.cells()[cell].vertices==rebuilt.cells()[cell].vertices);
    CHECK(mesh.cells()[cell].neighbours==rebuilt.cells()[cell].neighbours);
  }
  CHECK(mesh.point_to_cell()==rebuilt.point_to_cell());
  const auto hull_uses=[](const WangOrderedTetMesh& candidate) {
    std::set<std::pair<std::uint32_t,std::uint8_t>> uses;
    for(const auto& face:candidate.hull_faces())
      uses.insert({face.cell,face.opposite});
    return uses;
  };
  CHECK(hull_uses(mesh)==hull_uses(rebuilt));
}

TEST_CASE("owned scheduler retains an interior-vertex split hand-off") {
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
  REQUIRE(local.failure==WangOwnedLocalRecoveryFailure::vertex_obstruction);
  CHECK(local.obstructing_vertex==50U);
  const auto scheduler=run_wang_segment_scheduler_local_prefix(constraints,mesh);
  REQUIRE(scheduler.stop==WangOwnedSegmentSchedulerStop::interior_vertex_obstruction);
  CHECK(scheduler.obstructing_vertex==50U);
  REQUIRE(scheduler.lost_edges.size()==1U);
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

TEST_CASE("owned disturbance keeps the first positive-volume random offset") {
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={
      {10U,{1.0,0.0,0.0}},{20U,{0.0,1.0,0.0}},
      {30U,{0.0,0.0,1.0}},{40U,{-1.0,-1.0,-1.0}},
      {50U,{0.0,0.0,0.0}}};
  constraints.interior_steiner_vertices.push_back(
      {50U,CanonicalInteriorSteinerKind::cascade_fhc});
  WangOrderedTetMesh mesh(constraints.vertices.size(),{
      {{4U,1U,2U,3U}},{{4U,0U,3U,2U}},
      {{4U,0U,1U,3U}},{{4U,0U,2U,1U}}});
  REQUIRE(mesh.audit().accepted());
  const auto original=constraints.vertices[4].position;
  const auto disturbed=disturb_wang_owned_interior_vertex(constraints,50U,mesh);
  REQUIRE(disturbed.attempted);
  REQUIRE(disturbed.moved);
  CHECK(disturbed.samples>=1U);
  CHECK(disturbed.samples<=10U);
  CHECK(constraints.vertices[4].position.x>=original.x);
  CHECK(constraints.vertices[4].position.y>=original.y);
  CHECK(constraints.vertices[4].position.z>=original.z);
  CHECK(constraints.vertices[4].position.x<original.x+1.0e-6);
  CHECK(constraints.vertices[4].position.y<original.y+1.0e-6);
  CHECK(constraints.vertices[4].position.z<original.z+1.0e-6);
}

TEST_CASE("ordered remove-point collapse tombstones rather than renumbering") {
  WangOrderedTetMesh mesh(5U,{
      {{4U,1U,2U,3U}},{{4U,0U,3U,2U}},
      {{4U,0U,1U,3U}},{{4U,0U,2U,1U}}});
  REQUIRE(mesh.audit().accepted());
  REQUIRE(mesh.collapse_vertex_into(4U,0U));
  CHECK(mesh.audit().accepted());
  CHECK(mesh.vertex_count()==5U);
  CHECK(mesh.is_vertex_deleted(4U));
  CHECK(mesh.point_to_cell()[4U]==WangOrderedTetMesh::no_neighbour);
  std::size_t active{};
  for(const auto& cell:mesh.cells()) {
    if(cell.deleted)continue;
    ++active;
    CHECK(std::find(cell.vertices.begin(),cell.vertices.end(),4U)==cell.vertices.end());
  }
  CHECK(active==1U);
}

TEST_CASE("owned ordered point star follows the DT P2T breadth-first order") {
  const std::vector<WangOrderedTetMesh::Tet> cells{{
      {{0U,2U,3U,4U}},{{1U,4U,3U,2U}},
      {{1U,2U,3U,5U}},{{0U,5U,3U,2U}}}};
  WangOrderedTetMesh mesh(6U,cells);
  REQUIRE(mesh.audit().accepted());
  // This is the source oracle's P2T state. With cell zero as P2T(2),
  // DT::findSphere crosses its incident local faces
  // in ascending local-face order: 0->1, 0->3, then 1->2.
  REQUIRE(mesh.set_point_incidence(
      std::vector<WangOrderedTetMesh::Tet>{{cells[0],cells[1],cells[0],
                                            cells[0],cells[0],cells[2]}}));
  const auto sphere=mesh.find_sphere(2U);
  REQUIRE(sphere.size()==4U);
  CHECK(sphere[0]==0U);
  CHECK(sphere[1]==1U);
  CHECK(sphere[2]==3U);
  CHECK(sphere[3]==2U);
}

TEST_CASE("owned Cascade-FHC uses the ordered shell rather than vector recovery") {
  constexpr double root3=1.7320508075688772935;
  CanonicalPlcConstraintSet plc;
  plc.vertices={
      {100,{0,0,-1}},{200,{0,0,1}},
      {10,{2,0,0}},{20,{1,root3,0}},{30,{-1,root3,0}},
      {40,{-2,0,0}},{50,{-1,-root3,0}},{60,{1,-root3,0}},
      {70,{-3,-.4,0}},{80,{3,.4,0}}};
  plc.facets={facet({100,10,20})};
  std::vector<WangOrderedTetMesh::Tet> cells;
  for(std::uint32_t i=0U;i<6U;++i)
    cells.push_back({{0U,1U,2U+i,2U+(i+1U)%6U}});
  WangOrderedTetMesh mesh(plc.vertices.size(),cells);
  const auto insertion=insert_wang_owned_cascade_fhc_point(
      plc,{70,80},{{0U,1U}},mesh);
  REQUIRE(insertion.inserted);
  CHECK(insertion.cavity.size()==6U);
  CHECK(insertion.replacement.size()==12U);
  CHECK(insertion.point.z==doctest::Approx(0.5));
  // The retained vector implementation is test-only here. It provides a
  // regression oracle while production takes the ordered-mesh path above.
  const auto vector_oracle=insert_cascade_fhc_vertex(
      plc,{70,80},{100,200},cells);
  REQUIRE(vector_oracle.accepted);
  REQUIRE(vector_oracle.inserted_steiner_vertex.has_value());
  CHECK(insertion.point.x==vector_oracle.inserted_steiner_vertex->x);
  CHECK(insertion.point.y==vector_oracle.inserted_steiner_vertex->y);
  CHECK(insertion.point.z==vector_oracle.inserted_steiner_vertex->z);
  plc.vertices.push_back({201,insertion.point});
  REQUIRE(mesh.replace_cavity_with_appended_vertex(
      insertion.cavity,insertion.replacement).accepted);
  CHECK(plc.vertices.size()==11U);
  CHECK(mesh.vertex_count()==11U);
  CHECK(mesh.audit().accepted());
  for(const auto& cell:mesh.cells())if(!cell.deleted) {
    const bool retains_blocking_edge=
        std::find(cell.vertices.begin(),cell.vertices.end(),0U)!=cell.vertices.end()&&
        std::find(cell.vertices.begin(),cell.vertices.end(),1U)!=cell.vertices.end();
    CHECK_FALSE(retains_blocking_edge);
  }
}

TEST_CASE("owned Cascade-FHC matches the author closed-star placement") {
  // This is the finite part of the author-oracle DT state.  The reference
  // additionally carries ghost hull cells for point location; those are a
  // representation detail and are intentionally absent from WangOrderedTetMesh.
  CanonicalPlcConstraintSet plc;
  plc.vertices={
      {100,{-1,0,0}},{200,{1,0,0}},{300,{0,-1,0}},
      {400,{0,1,0}},{500,{0,0,1}},{600,{0,0,-1}}};
  const std::vector<WangOrderedTetMesh::Tet> cells{{
      {{0U,2U,3U,4U}},{{1U,4U,3U,2U}},
      {{1U,2U,3U,5U}},{{0U,5U,3U,2U}}}};
  WangOrderedTetMesh mesh(plc.vertices.size(),cells);
  REQUIRE(mesh.audit().accepted());
  const auto insertion=insert_wang_owned_cascade_fhc_point(
      plc,{100U,200U},{{2U,3U}},mesh);
  REQUIRE(insertion.inserted);
  CHECK(insertion.cavity.size()==4U);
  CHECK(insertion.replacement.size()==8U);
  // Captured from DT::addinnerSteiner_Edge on the same finite star with its
  // required ghost hull: its first accepted 0.5*d smoothing candidate.
  CHECK(insertion.point.x==0.0);
  CHECK(insertion.point.y==0.5);
  CHECK(insertion.point.z==-0.25);
  plc.vertices.push_back({700U,insertion.point});
  REQUIRE(mesh.replace_cavity_with_appended_vertex(
      insertion.cavity,insertion.replacement).accepted);
  CHECK(mesh.audit().accepted());
  const auto retry=recover_wang_segment_by_local_flips(
      plc,{100U,200U},false,1000U,mesh);
  CHECK(retry.recovered);
  CHECK(std::count_if(mesh.cells().begin(),mesh.cells().end(),
      [](const auto& cell){return !cell.deleted;})==9U);
  std::vector<std::array<std::uint32_t,4>> actual;
  for(const auto& cell:mesh.cells())if(!cell.deleted) {
    auto vertices=cell.vertices;std::sort(vertices.begin(),vertices.end());
    actual.push_back(vertices);
  }
  std::sort(actual.begin(),actual.end());
  std::vector<std::array<std::uint32_t,4>> author_post{{
      {{0U,2U,5U,6U}},{{0U,1U,2U,4U}},{{0U,1U,2U,6U}},
      {{0U,3U,5U,6U}},{{0U,3U,4U,6U}},{{0U,1U,4U,6U}},
      {{1U,2U,5U,6U}},{{1U,3U,5U,6U}},{{1U,3U,4U,6U}}}};
  std::sort(author_post.begin(),author_post.end());
  CHECK(actual==author_post);
}

TEST_CASE("owned Locked-FHC begins at the author frozen ghost-hull face") {
  auto fixture=author_pre_fhc_fixture();
  WangOrderedTetMesh mesh(fixture.constraints.vertices.size(),fixture.cells,20);
  REQUIRE(mesh.audit().accepted());
  REQUIRE(mesh.set_point_incidence(fixture.incidence));

  const auto& points=fixture.constraints.vertices;
  CHECK(author_orient(points[2].position,points[0].position,points[1].position,
                      points[3].position)>0.0);
  CHECK(author_orient(points[2].position,points[1].position,points[11].position,
                      points[3].position)>0.0);
  CHECK(author_orient(points[2].position,points[11].position,points[0].position,
                      points[3].position)>0.0);

  // The source mode-one call directs the pending edge from source node 2 to
  // source node 3. IDs are source indices plus one.
  const auto trace=inspect_wang_full_search_features(
      fixture.constraints,{3U,4U},mesh);
  REQUIRE(trace.failure==WangOwnedFullSearchFailure::none);
  REQUIRE(trace.features.size()==8U);
  REQUIRE(trace.features.front().kind==WangOwnedFullSearchFeatureKind::face);
  CHECK(trace.features.front().vertices[0]==1U);
  CHECK(trace.features.front().vertices[1]==0U);
  CHECK(trace.features.front().vertices[2]==4U);
  INFO("initial feature kind="<<static_cast<unsigned>(trace.features.front().kind)
       <<" feature="<<trace.features.front().vertices[0]<<','
       <<trace.features.front().vertices[1]<<','<<trace.features.front().vertices[2]);
  const auto first=insert_first_wang_locked_fhc_point(
      fixture.constraints,{3U,4U},mesh);
  INFO("first failure="<<static_cast<unsigned>(first.failure)
       <<" face="<<first.intersecting_face[0]<<','<<first.intersecting_face[1]
       <<','<<first.intersecting_face[2]<<" edge="<<first.locking_edge[0]
       <<','<<first.locking_edge[1]);
  REQUIRE(first.inserted);
  const std::array<std::array<const char*,3>,1> author_points{{
      {{"0x1.d6a2fc60a59b8p+0","0x1.fc092f811c5c5p+2","0x1.9d79a55b5dac7p-1"}},
      }};
  {
    const auto& point=first.inserted_point;
    CHECK(point.x==std::strtod(author_points[0][0],nullptr));
    CHECK(point.y==std::strtod(author_points[0][1],nullptr));
    CHECK(point.z==std::strtod(author_points[0][2],nullptr));
  }
  CHECK(mesh.audit().accepted());

  auto complete_fixture=author_pre_fhc_fixture();
  WangOrderedTetMesh complete_mesh(complete_fixture.constraints.vertices.size(),
                                   complete_fixture.cells,20);
  REQUIRE(complete_mesh.set_point_incidence(complete_fixture.incidence));
  const auto complete=recover_wang_segment_with_interior_steiner_mode1(
      complete_fixture.constraints,{3U,4U},complete_mesh,5U);
  REQUIRE(complete.inserted_points.size()==5U);
  const std::array<std::array<const char*,3>,5> author_retry_points{{
      {{"0x1.d6a2fc60a59b8p+0","0x1.fc092f811c5c5p+2","0x1.9d79a55b5dac7p-1"}},
      {{"0x1.05213ae665309p+1","0x1.52f538c78e8b7p+2","0x1.3f1f7d28d253bp-1"}},
      {{"0x1.21db42b34c667p+0","0x1.25f83837b4a6fp+2","0x1.00249939cb82fp+0"}},
      {{"0x1.8746430746d5dp-2","0x1.535185f261955p+2","0x1.6582dd1c3eaf5p+0"}},
      {{"0x1.0422d7a7dab41p-1","0x1.da6faf3caaea9p+1","0x1.5ff61082f42fp+0"}}}};
  for(std::size_t index=0;index<author_retry_points.size();++index) {
    const auto& point=complete.inserted_points[index];
    CHECK(point.x==std::strtod(author_retry_points[index][0],nullptr));
    CHECK(point.y==std::strtod(author_retry_points[index][1],nullptr));
    CHECK(point.z==std::strtod(author_retry_points[index][2],nullptr));
  }
  CHECK(complete_mesh.audit().accepted());
  const auto finite_cells=std::count_if(
      complete_mesh.cells().begin(),complete_mesh.cells().end(),
      [&](const auto& cell) {
        return !cell.deleted&&
            !std::count(cell.vertices.begin(),cell.vertices.end(),20U);
      });
  CHECK(finite_cells==102U);

}
} // namespace

TEST_CASE("owned Wang scheduler continues through facet recovery after unsuccessful FHC") {
  const auto recovery=recover_wang_constraints(full_search_fixture(),{});

  CHECK(recovery.owned_segment_scheduler_invoked);
  CHECK(recovery.failure==CanonicalPlcRecoveryFailure::none);
  CHECK(recovery.owned_segment_scheduler_round>5U);
  CHECK(recovery.owned_segment_fhc_invoked);
  CHECK(recovery.owned_segment_fhc_insertions>=5U);
  CHECK(recovery.owned_segment_fhc_recovered);
  CHECK(recovery.edges_recovered_before_facet_stage);
  std::size_t fhc_records{},facet_interior_records{};
  std::set<std::uint64_t> interior_steiner_ids;
  for(const auto& steiner:recovery.constraints.interior_steiner_vertices) {
    CHECK(interior_steiner_ids.insert(steiner.id).second);
    CHECK(std::any_of(recovery.constraints.vertices.begin(),
                      recovery.constraints.vertices.end(),
                      [&](const auto& vertex) { return vertex.id==steiner.id; }));
    const auto is_fhc_provenance=
        steiner.kind==CanonicalInteriorSteinerKind::locked_fhc||
        steiner.kind==CanonicalInteriorSteinerKind::cascade_fhc;
    if(is_fhc_provenance)++fhc_records;
    else if(steiner.kind==CanonicalInteriorSteinerKind::facet_interior)
      ++facet_interior_records;
    else CHECK(false);
  }
  CHECK(fhc_records==recovery.owned_segment_fhc_insertions);
  CHECK(facet_interior_records==recovery.facet_interior_steiner_insertions);
  CHECK(recovery.constraints.interior_steiner_vertices.size()==
        fhc_records+facet_interior_records);
  CHECK(recovery.constraints.vertices.size()>25U);
  // First Locked-FHC point: reference's locked-edge/face barycenter.
  REQUIRE(recovery.constraints.vertices.size()>20U);
  const auto& first_fhc=recovery.constraints.vertices[20].position;
  CHECK(first_fhc.x==std::strtod("0x1.d6a2fc60a59b8p+0",nullptr));
  CHECK(first_fhc.y==std::strtod("0x1.fc092f811c5c5p+2",nullptr));
  CHECK(first_fhc.z==std::strtod("0x1.9d79a55b5dac7p-1",nullptr));
  // These are byte-for-byte records from the pinned
  // addinnerSteiner_Edge Locked-FHC run.
  const std::array<std::array<const char*,3>,5> author_locked{{
      {{"0x1.d6a2fc60a59b8p+0","0x1.fc092f811c5c5p+2","0x1.9d79a55b5dac7p-1"}},
      {{"0x1.05213ae665309p+1","0x1.52f538c78e8b7p+2","0x1.3f1f7d28d253bp-1"}},
      {{"0x1.21db42b34c667p+0","0x1.25f83837b4a6fp+2","0x1.00249939cb82fp+0"}},
      {{"0x1.8746430746d5dp-2","0x1.535185f261955p+2","0x1.6582dd1c3eaf5p+0"}},
      {{"0x1.0422d7a7dab41p-1","0x1.da6faf3caaea9p+1","0x1.5ff61082f42fp+0"}}}};
  for(std::size_t offset=0;offset<author_locked.size();++offset) {
    const auto& point=recovery.constraints.vertices[20U+offset].position;
    CHECK(point.x==std::strtod(author_locked[offset][0],nullptr));
    CHECK(point.y==std::strtod(author_locked[offset][1],nullptr));
    CHECK(point.z==std::strtod(author_locked[offset][2],nullptr));
  }
  CHECK(recovery.initial_tetrahedra.size()==73U);
  // The full-search/local-flip stage changes the owned topology before FHC.
  CHECK(recovery.tetrahedra.size()>102U);
  REQUIRE_FALSE(recovery.segment_stage_tetrahedra.empty());
  CHECK(inspect_canonical_plc_tetrahedra(
      recovery.segment_stage_constraints,recovery.segment_stage_tetrahedra)
          .missing_edges.empty());
  CHECK_FALSE(recovery.segment_scheduler_attempt_trace.empty());
  CHECK(recovery.segment_stage_constraints.vertices.size()<=
        recovery.constraints.vertices.size());
  for(const auto& tet:recovery.tetrahedra)
    for(const auto vertex:tet)
      CHECK(vertex<recovery.constraints.vertices.size());
  CHECK(recovery.inspection.missing_edges.empty());
  // splitBndEdge journals the boundary insertion, appends the two segment
  // children before radial children, and immediately marks unrecovered
  // children -3 before they return to the live scheduler queue.
  REQUIRE(!recovery.constraints.recovery_journal.empty());
  REQUIRE(!recovery.segment_post_split_child_edge_calls.empty());
  REQUIRE(recovery.segment_post_split_child_edge_calls.size()==
          recovery.segment_post_split_child_info.size());
  const auto& children=recovery.segment_post_split_child_edge_calls.front();
  const auto& child_info=recovery.segment_post_split_child_info.front();
  REQUIRE(children.size()>=2U);
  REQUIRE(children.size()==child_info.size());
  CHECK(children[0][1]==children[1][1]);
  CHECK(std::any_of(child_info.begin(),child_info.end(),
      [](const int info){return info==-3;}));
}

TEST_CASE("closed well recovers without stale info-two escalation") {
  const auto fixture=closed_well_info2_fixture();
  const auto forward=recover_wang_constraints(fixture,{});
  REQUIRE(forward.accepted());
  REQUIRE_FALSE(forward.facet_recovery_attempt_trace.empty());
  CHECK(std::all_of(forward.facet_recovery_attempt_trace.begin(),
                    forward.facet_recovery_attempt_trace.end(),
                    [](const auto& attempt) { return attempt.info==0U; }));
  CHECK(forward.facet_interior_steiner_attempts==0U);
  CHECK(forward.facet_interior_steiner_insertions==0U);
  CHECK(forward.facet_splits==0U);
  CHECK(forward.facet_post_split_child_calls.empty());
  CHECK(forward.inspection.accepted());

  const auto completed=tetrahedralize_wang_constrained_plc(fixture,{});
  REQUIRE(completed.accepted());
  CHECK(completed.boundary_removal_attempts.empty());
  CHECK(completed.boundary_points_restored==0U);
  CHECK(completed.reverse_boundary_restoration_complete);
  CHECK(completed.boundary_audit.accepted());

  // Disabling the unused split fallback must not perturb this flip-only path.
  WangConstrainedTetrahedralizationOptions restricted;
  restricted.restricted_viability_experiment=true;
  restricted.recovery.maximum_facet_splits=0U;
  const auto exhausted=tetrahedralize_wang_constrained_plc(fixture,restricted);
  CHECK(exhausted.accepted());
  CHECK(exhausted.boundary_audit.accepted());
}

TEST_CASE("owned public Wang pass removes recovered boundary Steiner points") {
  // This PLC takes the owned segment scheduler through splitBndEdge, so the
  // forward recovery records real boundary insertions rather than fabricating
  // a removal-only mesh.
  const auto forward=recover_wang_constraints(full_search_fixture(),{});
  REQUIRE(forward.accepted());
  REQUIRE_FALSE(forward.constraints.recovery_journal.empty());

  const auto completed=tetrahedralize_wang_constrained_plc(
      full_search_fixture(),{});
  INFO("failure="<<static_cast<unsigned>(completed.failure)
       <<" removal attempts="<<completed.boundary_removal_attempts.size()
       <<" remaining="<<completed.recovery.constraints.recovery_journal.size());
  REQUIRE(completed.accepted());
  CHECK(completed.pre_removal_volume_optimization_invoked);
  // The pre-removal sweep is over the disposable FHC points present at the
  // handoff, independently of whether reverse restoration later creates more.
  CHECK(completed.pre_removal_volume_optimization_attempts.size()==
        forward.constraints.interior_steiner_vertices.size());
  CHECK(std::all_of(completed.pre_removal_volume_optimization_attempts.begin(),
                    completed.pre_removal_volume_optimization_attempts.end(),
                    [](const auto& attempt) { return attempt.attempted; }));
  CHECK(completed.interior_removal_stage_invoked);
  CHECK_FALSE(completed.boundary_removal_attempts.empty());
  CHECK(completed.reverse_boundary_restoration_complete);
  CHECK(completed.boundary_audit.accepted());
  CHECK(completed.boundary_audit.no_boundary_steiner_points);
  CHECK(completed.recovery.constraints.recovery_journal.empty());
  CHECK(completed.recovery.inspection.accepted());
}

TEST_CASE("owned reverse removal runs line-23 volume optimization without a boundary journal") {
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{0,0,0}},{20,{2,0,0}},{30,{0,2,0}},
                        {40,{0,0,2}},{50,{.35,.45,.55}}};
  constraints.facets={facet({20,30,40}),facet({10,40,30}),
                      facet({10,20,40}),facet({10,30,20})};
  constraints.interior_steiner_vertices.push_back(
      {50,CanonicalInteriorSteinerKind::facet_interior});
  const std::vector<std::array<std::uint32_t,4>> star{
      {{4,1,2,3}},{{4,0,3,2}},{{4,0,1,3}},{{4,0,2,1}}};
  REQUIRE(inspect_canonical_plc_tetrahedra(constraints,star).accepted());

  const auto completed=run_wang_reverse_boundary_removal(constraints,star);

  CHECK(completed.attempts.empty());
  REQUIRE(completed.pre_removal_volume_optimization_invoked);
  REQUIRE(completed.pre_removal_volume_optimization_attempts.size()==1U);
  const auto& optimization=
      completed.pre_removal_volume_optimization_attempts.front();
  CHECK(optimization.vertex.id==50U);
  CHECK(optimization.attempted);
  CHECK(completed.interior_removal_stage_invoked);
  REQUIRE(completed.interior_attempts.size()==1U);
  CHECK(completed.interior_attempts.front().vertex.id==50U);
}

TEST_CASE("public Wang entry rejects an open-edge boundary contact") {
  // Vertex 50 is geometrically inside literal edge 10--20 but is not one of
  // that edge's endpoints.  This is the non-embedded PLC state for which the
  // reference's mutable AttachPnt2Seg cleanup exists; it is not admissible
  // Algorithm 2 input.
  CanonicalPlcConstraintSet plc;
  plc.vertices={{10,{0,0,0}},{20,{2,0,0}},{30,{0,1,0}},{40,{0,0,1}},
                {50,{1,0,0}}};
  plc.facets={facet({10,20,30}),facet({20,10,40}),facet({50,30,40}),
              facet({50,40,10})};
  const auto result=tetrahedralize_wang_constrained_plc(plc,{});
  CHECK(result.failure==WangConstrainedTetrahedralizationFailure::invalid_plc);
  CHECK_FALSE(result.initial_tetrahedralization_complete);
  CHECK(result.recovery.constraints.recovery_journal.empty());
  CHECK(result.recovery.constraints.vertices.empty());
}
