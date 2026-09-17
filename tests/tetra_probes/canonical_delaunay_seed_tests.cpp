#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_probes/canonical_delaunay_seed.hpp"
#include "tetra_probes/wang_ordered_tet_mesh.hpp"

#include <algorithm>
#include <bit>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
double signed_volume(tetra::Vec3 a,tetra::Vec3 b,tetra::Vec3 c,tetra::Vec3 d) {
  b=b-a;c=c-a;d=d-a;
  return (b.x*(c.y*d.z-c.z*d.y)-b.y*(c.x*d.z-c.z*d.x)+b.z*(c.x*d.y-c.y*d.x))/6.0;
}
std::set<std::array<std::uint64_t,4>> topology(const tetra::probes::CanonicalDelaunaySeedInput& input,
                                                const tetra::probes::CanonicalDelaunaySeedResult& result) {
  std::set<std::array<std::uint64_t,4>> keys;
  for(const auto& tet:result.tetrahedra){std::array<std::uint64_t,4> key{};for(unsigned i=0;i<4;++i)key[i]=input.stable_vertex_ids[tet[i]];std::sort(key.begin(),key.end());keys.insert(key);}
  return keys;
}

using RegionFace=std::array<std::uint32_t,3>;
RegionFace region_face_key(RegionFace face) {
  std::sort(face.begin(),face.end());
  return face;
}
struct StructuredRegionGrid {
  tetra::probes::CanonicalPlcRegionInput input;
  std::vector<std::array<unsigned,3>> cell_cubes;
};
StructuredRegionGrid structured_region_grid(unsigned cubes_per_axis) {
  StructuredRegionGrid grid;
  const auto stride=cubes_per_axis+1U;
  const auto vertex=[&](unsigned x,unsigned y,unsigned z) {
    return static_cast<std::uint32_t>((z*stride+y)*stride+x);
  };
  for(unsigned z=0;z<=cubes_per_axis;++z)
    for(unsigned y=0;y<=cubes_per_axis;++y)
      for(unsigned x=0;x<=cubes_per_axis;++x)
        grid.input.vertices.push_back({static_cast<double>(x),static_cast<double>(y),static_cast<double>(z)});
  constexpr std::array<std::array<unsigned,3>,6> permutations{{
      {{0,1,2}},{{0,2,1}},{{1,0,2}},{{1,2,0}},{{2,0,1}},{{2,1,0}}}};
  for(unsigned z=0;z<cubes_per_axis;++z)
    for(unsigned y=0;y<cubes_per_axis;++y)
      for(unsigned x=0;x<cubes_per_axis;++x) {
        const auto corner=[&](std::array<unsigned,3> offset) {
          return vertex(x+offset[0],y+offset[1],z+offset[2]);
        };
        const auto origin=corner({{0,0,0}}),opposite=corner({{1,1,1}});
        for(const auto permutation:permutations) {
          std::array<unsigned,3> a{};a[permutation[0]]=1U;
          auto b=a;b[permutation[1]]=1U;
          std::array<std::uint32_t,4> tet{{origin,corner(a),corner(b),opposite}};
          if(signed_volume(grid.input.vertices[tet[0]],grid.input.vertices[tet[1]],
                           grid.input.vertices[tet[2]],grid.input.vertices[tet[3]])<0.0)
            std::swap(tet[1],tet[2]);
          grid.input.tetrahedra.push_back(tet);
          grid.cell_cubes.push_back({{x,y,z}});
        }
      }
  grid.input.coordinate_scale=static_cast<double>(cubes_per_axis);
  return grid;
}
std::vector<RegionFace> selected_region_boundary(
    const StructuredRegionGrid& grid,const std::vector<bool>& selected) {
  std::map<RegionFace,std::vector<std::uint32_t>> uses;
  for(std::uint32_t cell=0;cell<grid.input.tetrahedra.size();++cell)
    for(unsigned omit=0;omit<4U;++omit) {
      RegionFace face{};unsigned cursor{};
      for(unsigned i=0;i<4U;++i)if(i!=omit)face[cursor++]=grid.input.tetrahedra[cell][i];
      uses[region_face_key(face)].push_back(cell);
    }
  std::vector<RegionFace> boundary;
  for(const auto& [face,cells]:uses) {
    const bool first=selected[cells[0]];
    const bool second=cells.size()==2U&&selected[cells[1]];
    if((cells.size()==1U&&first)||(cells.size()==2U&&first!=second))boundary.push_back(face);
  }
  return boundary;
}
bool is_closed_region_boundary(const std::vector<RegionFace>& faces) {
  std::map<std::array<std::uint32_t,2>,unsigned> edge_uses;
  for(const auto& face:faces)for(const auto pair:std::array<std::array<unsigned,2>,3>{{{{0,1}},{{1,2}},{{0,2}}}}) {
    std::array<std::uint32_t,2> edge{{face[pair[0]],face[pair[1]]}};
    if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
    ++edge_uses[edge];
  }
  return !faces.empty()&&std::all_of(edge_uses.begin(),edge_uses.end(),
      [](const auto& use){return use.second==2U;});
}
struct InteriorStar {
  tetra::probes::CanonicalPlcConstraintSet constraints;
  std::vector<std::array<std::uint32_t,4>> tetrahedra;
};
InteriorStar octahedral_interior_star(tetra::Vec3 position) {
  using namespace tetra::probes;
  InteriorStar result;
  result.constraints.vertices={{10,{1,0,0}},{20,{-1,0,0}},
      {30,{0,1,0}},{40,{0,-1,0}},{50,{0,0,1}},{60,{0,0,-1}},{70,position}};
  const std::array<std::array<std::uint32_t,3>,8> faces{{
      {{0,2,4}},{{0,2,5}},{{0,3,4}},{{0,3,5}},
      {{1,2,4}},{{1,2,5}},{{1,3,4}},{{1,3,5}}}};
  for(const auto face:faces) {
    CanonicalPlcConstraintFacet facet;
    for(unsigned i=0U;i<3U;++i)facet.vertices[i]=result.constraints.vertices[face[i]].id;
    facet.source_vertices=facet.vertices;facet.parent.vertex_ids=facet.vertices;
    std::sort(facet.parent.vertex_ids.begin(),facet.parent.vertex_ids.end());
    facet.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
    result.constraints.facets.push_back(facet);
    std::array<std::uint32_t,4> cell{{6,face[0],face[1],face[2]}};
    if(signed_volume(result.constraints.vertices[cell[0]].position,
                     result.constraints.vertices[cell[1]].position,
                     result.constraints.vertices[cell[2]].position,
                     result.constraints.vertices[cell[3]].position)<0.0)
      std::swap(cell[0],cell[1]);
    result.tetrahedra.push_back(cell);
  }
  result.constraints.interior_steiner_vertices.push_back(
      {70,CanonicalInteriorSteinerKind::topology_repair});
  return result;
}
}

TEST_CASE("canonical Delaunay seed is bounded and derives cells only from input points") {
  const std::vector<tetra::Vec3> points{{0,0,0},{2,0,0},{0,2,0},{0,0,2},{.3,.4,.5}};
  const auto seed=tetra::probes::build_canonical_delaunay_seed(points,16U,64U);
  REQUIRE(seed.accepted());
  CHECK_FALSE(seed.tetrahedra.empty());
  for(const auto& tet:seed.tetrahedra) for(const auto vertex:tet) CHECK(vertex<points.size());
  CHECK(tetra::probes::build_canonical_delaunay_seed(points,4U,64U).failure==
        tetra::probes::CanonicalDelaunaySeedFailure::resource_limit);
}

TEST_CASE("seed is scale independent and covers the convex control volume") {
  const std::vector<tetra::Vec3> base{{0,0,0},{2,0,0},{0,2,0},{0,0,2},{.3,.4,.5}};
  for(const double scale:std::array{1e-8,1.0,1e8}) {
    auto points=base;for(auto& point:points)point=point*scale;
    const auto seed=tetra::probes::build_canonical_delaunay_seed(points,16U,64U);
    REQUIRE(seed.accepted());
    double total=0;for(const auto& tet:seed.tetrahedra){const auto cell=signed_volume(points[tet[0]],points[tet[1]],points[tet[2]],points[tet[3]]);CHECK(cell>0.0);total+=cell;}
    CHECK(total==doctest::Approx(4.0/3.0*scale*scale*scale).epsilon(1e-10));
  }
}

TEST_CASE("stable ids make input reordering topology invariant") {
  tetra::probes::CanonicalDelaunaySeedInput a;
  a.vertices={{0,0,0},{2,0,0},{0,2,0},{0,0,2},{.3,.4,.5}};a.stable_vertex_ids={50,10,40,20,30};a.maximum_tetrahedra=64;
  auto b=a;std::reverse(b.vertices.begin(),b.vertices.end());std::reverse(b.stable_vertex_ids.begin(),b.stable_vertex_ids.end());
  const auto left=tetra::probes::build_canonical_delaunay_seed(a),right=tetra::probes::build_canonical_delaunay_seed(b);
  REQUIRE(left.accepted());REQUIRE(right.accepted());CHECK(topology(a,left)==topology(b,right));
}

TEST_CASE("co-spherical insertion has a deterministic in-project tie rule") {
  using namespace tetra::probes;
  CanonicalDelaunaySeedInput forward;
  forward.vertices={{-1,-1,-1},{1,-1,-1},{-1,1,-1},{1,1,-1},
                    {-1,-1,1},{1,-1,1},{-1,1,1},{1,1,1}};
  forward.stable_vertex_ids={80,10,70,20,60,30,50,40};
  forward.maximum_tetrahedra=128U;
  auto reverse=forward;
  std::reverse(reverse.vertices.begin(),reverse.vertices.end());
  std::reverse(reverse.stable_vertex_ids.begin(),reverse.stable_vertex_ids.end());
  const auto first=build_canonical_delaunay_seed(forward);
  const auto second=build_canonical_delaunay_seed(reverse);
  REQUIRE(first.accepted());
  REQUIRE(second.accepted());
  CHECK(topology(forward,first)==topology(reverse,second));
  double volume{};
  for(const auto& tet:first.tetrahedra) {
    const auto cell=signed_volume(forward.vertices[tet[0]],forward.vertices[tet[1]],
                                  forward.vertices[tet[2]],forward.vertices[tet[3]]);
    CHECK(cell>0.0);
    volume+=cell;
  }
  CHECK(volume==doctest::Approx(8.0).epsilon(1e-12));
}

TEST_CASE("canonical stellar fallback covers coplanar hull and interior points") {
  using namespace tetra::probes;
  CanonicalDelaunaySeedInput forward;
  forward.vertices={{-1,-1,-1},{1,-1,-1},{-1,1,-1},{1,1,-1},
                    {-1,-1,1},{1,-1,1},{-1,1,1},{1,1,1},
                    {0,0,-1},{0,0,0},{0.2,-0.1,0.3}};
  forward.stable_vertex_ids={80,10,70,20,60,30,50,40,90,100,110};
  forward.maximum_tetrahedra=256U;
  auto reverse=forward;std::reverse(reverse.vertices.begin(),reverse.vertices.end());std::reverse(reverse.stable_vertex_ids.begin(),reverse.stable_vertex_ids.end());
  const auto first=build_canonical_stellar_seed(forward),second=build_canonical_stellar_seed(reverse);
  INFO("first="<<static_cast<unsigned>(first.failure)<<':'<<static_cast<unsigned>(first.invalid_reason)
       <<" second="<<static_cast<unsigned>(second.failure)<<':'<<static_cast<unsigned>(second.invalid_reason));
  REQUIRE(first.accepted());REQUIRE(second.accepted());
  CHECK(topology(forward,first)==topology(reverse,second));
  double volume{};for(const auto& tet:first.tetrahedra){const auto cell=signed_volume(forward.vertices[tet[0]],forward.vertices[tet[1]],forward.vertices[tet[2]],forward.vertices[tet[3]]);CHECK(cell>0.0);volume+=cell;}
  CHECK(volume==doctest::Approx(8.0).epsilon(1e-12));
}

TEST_CASE("ambiguous and lower-dimensional inputs refuse without cells") {
  using namespace tetra::probes;
  CanonicalDelaunaySeedInput duplicate{{{0,0,0},{1,0,0},{0,1,0},{0,0,1},{0,0,1}},{1,2,3,4,5},16,64};
  auto result=build_canonical_delaunay_seed(duplicate);CHECK(result.failure==CanonicalDelaunaySeedFailure::duplicate_position);CHECK(result.tetrahedra.empty());
  duplicate.vertices.pop_back();duplicate.stable_vertex_ids={1,2,3,3};result=build_canonical_delaunay_seed(duplicate);CHECK(result.failure==CanonicalDelaunaySeedFailure::duplicate_stable_id);CHECK(result.tetrahedra.empty());
  result=build_canonical_delaunay_seed({{{0,0,0},{1,0,0},{0,1,0},{1,1,0}},{1,2,3,4},16,64});CHECK(result.failure==CanonicalDelaunaySeedFailure::insufficient_dimension);CHECK(result.tetrahedra.empty());
  result=build_canonical_delaunay_seed({{{0,0,0},{1,0,0},{0,1,0},{0,0,1}},{1,2,3,4},16,1});CHECK(result.failure==CanonicalDelaunaySeedFailure::resource_limit);CHECK(result.tetrahedra.empty());
}

TEST_CASE("region flood retains a concave constrained solid without convex halfspace classification") {
  using namespace tetra;
  using namespace tetra::probes;
  CanonicalPlcRegionInput input;
  const auto vertex=[&](unsigned x,unsigned y,unsigned z) {
    return static_cast<std::uint32_t>((z*3U+y)*3U+x);
  };
  for(unsigned z=0;z<=1U;++z)for(unsigned y=0;y<=2U;++y)for(unsigned x=0;x<=2U;++x)
    input.vertices.push_back({static_cast<double>(x),static_cast<double>(y),static_cast<double>(z)});
  constexpr std::array<std::array<unsigned,3>,6> permutations{{{{0,1,2}},{{0,2,1}},{{1,0,2}},{{1,2,0}},{{2,0,1}},{{2,1,0}}}};
  std::vector<bool> material;
  for(unsigned y=0;y<2U;++y)for(unsigned x=0;x<2U;++x) {
    const std::array<std::array<unsigned,3>,8> offsets{{{{0,0,0}},{{1,0,0}},{{0,1,0}},{{0,0,1}},{{1,1,0}},{{1,0,1}},{{0,1,1}},{{1,1,1}}}};
    const auto corner=[&](std::array<unsigned,3> offset){return vertex(x+offset[0],y+offset[1],offset[2]);};
    const auto origin=corner(offsets[0]);
    for(const auto permutation:permutations) {
      std::array<unsigned,3> a{};a[permutation[0]]=1U;
      auto b=a;b[permutation[1]]=1U;
      input.tetrahedra.push_back({{origin,corner(a),corner(b),corner(offsets[7])}});
      material.push_back(!(x==1U&&y==1U));
    }
  }
  using Face=std::array<std::uint32_t,3>;
  std::map<Face,std::vector<std::uint32_t>> uses;
  for(std::uint32_t cell=0;cell<input.tetrahedra.size();++cell)for(unsigned omit=0;omit<4U;++omit) {
    Face face{};unsigned cursor{};for(unsigned i=0;i<4U;++i)if(i!=omit)face[cursor++]=input.tetrahedra[cell][i];std::sort(face.begin(),face.end());uses[face].push_back(cell);
  }
  for(const auto& [face,cells]:uses) {
    const bool first=material[cells[0]];
    const bool second=cells.size()==2U&&material[cells[1]];
    if((cells.size()==1U&&first)||(cells.size()==2U&&first!=second))input.outer_faces.push_back(face);
  }
  const auto result=classify_canonical_plc_regions(input);
  REQUIRE(result.accepted());
  CHECK(result.shell_cells==18U);
  CHECK(result.outside_cells==6U);
  CHECK(result.core_cells==0U);
  for(std::size_t i=0;i<material.size();++i)
    CHECK(result.regions[i]==(material[i]?CanonicalPlcCellRegion::shell:CanonicalPlcCellRegion::outside));
}

TEST_CASE("region flood maps the reference ghost component to finite outside cells") {
  using namespace tetra::probes;
  auto grid=structured_region_grid(1U);
  const auto result=classify_canonical_plc_regions(grid.input);
  REQUIRE(result.accepted());
  CHECK(result.outside_cells==6U);
  CHECK(result.shell_cells==0U);
  CHECK(result.core_cells==0U);
  CHECK(std::all_of(result.regions.begin(),result.regions.end(),[](const auto region) {
    return region==CanonicalPlcCellRegion::outside;
  }));
}

TEST_CASE("region flood stops at a closed recovered outer shell") {
  using namespace tetra::probes;
  auto grid=structured_region_grid(3U);
  std::vector<bool> material(grid.input.tetrahedra.size());
  for(std::size_t cell=0;cell<material.size();++cell)
    material[cell]=grid.cell_cubes[cell]==std::array<unsigned,3>{{1,1,1}};
  grid.input.outer_faces=selected_region_boundary(grid,material);
  REQUIRE(is_closed_region_boundary(grid.input.outer_faces));
  const auto result=classify_canonical_plc_regions(grid.input);
  REQUIRE(result.accepted());
  CHECK(result.outside_cells==156U);
  CHECK(result.shell_cells==6U);
  CHECK(result.core_cells==0U);
  for(std::size_t cell=0;cell<material.size();++cell)
    CHECK(result.regions[cell]==(material[cell]?CanonicalPlcCellRegion::shell
                                               :CanonicalPlcCellRegion::outside));
  std::vector<bool> retained(result.regions.size());
  for(std::size_t cell=0;cell<retained.size();++cell)
    retained[cell]=result.regions[cell]==CanonicalPlcCellRegion::shell;
  CHECK(selected_region_boundary(grid,retained)==grid.input.outer_faces);
  for(const auto& tet:grid.input.tetrahedra)
    CHECK(signed_volume(grid.input.vertices[tet[0]],grid.input.vertices[tet[1]],
                        grid.input.vertices[tet[2]],grid.input.vertices[tet[3]])>0.0);
}

TEST_CASE("region flood isolates a closed core interface for composition outside Wang recovery") {
  using namespace tetra::probes;
  auto grid=structured_region_grid(3U);
  std::vector<bool> all(grid.input.tetrahedra.size(),true),core(grid.input.tetrahedra.size());
  for(std::size_t cell=0;cell<core.size();++cell)
    core[cell]=grid.cell_cubes[cell]==std::array<unsigned,3>{{1,1,1}};
  grid.input.outer_faces=selected_region_boundary(grid,all);
  grid.input.core_faces=selected_region_boundary(grid,core);
  REQUIRE(is_closed_region_boundary(grid.input.outer_faces));
  REQUIRE(is_closed_region_boundary(grid.input.core_faces));
  for(std::size_t cell=0;cell<core.size();++cell)if(core[cell]) {
    const auto& tet=grid.input.tetrahedra[cell];
    const auto witness=(grid.input.vertices[tet[0]]+grid.input.vertices[tet[1]]+
                        grid.input.vertices[tet[2]]+grid.input.vertices[tet[3]])*.25;
    grid.input.core_witnesses.push_back(witness);
  }
  const auto result=classify_canonical_plc_regions(grid.input);
  REQUIRE(result.accepted());
  CHECK(result.outside_cells==0U);
  CHECK(result.shell_cells==156U);
  CHECK(result.core_cells==6U);
  for(std::size_t cell=0;cell<core.size();++cell)
    CHECK(result.regions[cell]==(core[cell]?CanonicalPlcCellRegion::core
                                          :CanonicalPlcCellRegion::shell));
  std::vector<bool> shell(result.regions.size());
  for(std::size_t cell=0;cell<shell.size();++cell)
    shell[cell]=result.regions[cell]==CanonicalPlcCellRegion::shell;
  auto expected=grid.input.outer_faces;
  expected.insert(expected.end(),grid.input.core_faces.begin(),grid.input.core_faces.end());
  std::sort(expected.begin(),expected.end());
  CHECK(selected_region_boundary(grid,shell)==expected);
}

TEST_CASE("constraint-edge midpoint identity is endpoint-derived and reorder stable") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet input;
  input.vertices={{10,{0,0,0}},{20,{2,0,0}},{30,{0,1,0}},{40,{0,0,1}},{50,{0,2,0}}};
  CanonicalPlcConstraintFacet face;
  face.parent={{10,20,30}}; face.vertices={{10,20,30}}; face.source_vertices=face.vertices;
  face.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  input.facets={face};
  const auto forward=split_canonical_plc_constraint_edge(input,{10,20},16U,16U);
  auto reordered=input;std::reverse(reordered.vertices.begin(),reordered.vertices.end());
  const auto reverse=split_canonical_plc_constraint_edge(reordered,{20,10},16U,16U);
  REQUIRE(forward.accepted()); REQUIRE(reverse.accepted());
  CHECK(forward.constraints.vertices.back().id==reverse.constraints.vertices.back().id);
  CHECK(forward.constraints.vertices.back().id!=(std::uint64_t{51}));
  CHECK(forward.constraints.split_vertices==reverse.constraints.split_vertices);
  REQUIRE(forward.constraints.split_vertices.size()==1U);
  CHECK(forward.constraints.split_vertices.front().edge==std::array<std::uint64_t,2>{{10,20}});
  CHECK(forward.constraints.split_vertices.front().numerator==1U);
  CHECK(forward.constraints.split_vertices.front().denominator==2U);
  REQUIRE(forward.constraints.recovery_journal.size()==1U);
  CHECK(forward.constraints.recovery_journal.front().kind==
        CanonicalPlcRecoveryInsertionKind::edge_split);
  CHECK(forward.constraints.recovery_journal.front().vertex_id==
        forward.constraints.split_vertices.front().id);
  for(const auto& child:forward.constraints.facets)
    CHECK(child.source_vertices==std::array<std::uint64_t,3>{{10,20,30}});
}

TEST_CASE("constraint-edge rational split is canonical under endpoint reversal") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet input;
  input.vertices={{10,{0,0,0}},{20,{2,0,0}},{30,{0,2,0}}};
  CanonicalPlcConstraintFacet face;face.parent={{10,20,30}};
  face.vertices={{10,20,30}};face.source_vertices=face.vertices;
  face.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  input.facets={face};
  const auto forward=split_canonical_plc_constraint_edge_at_ratio(input,{10,20},1U,4U,8U,8U);
  const auto reversed=split_canonical_plc_constraint_edge_at_ratio(input,{20,10},1U,4U,8U,8U);
  REQUIRE(forward.accepted());REQUIRE(reversed.accepted());
  CHECK(forward.constraints.vertices.back().id==reversed.constraints.vertices.back().id);
  CHECK(forward.constraints.vertices.back().position.x==doctest::Approx(0.5));
  CHECK(forward.constraints.vertices.back().position.x==reversed.constraints.vertices.back().position.x);
  REQUIRE(forward.constraints.split_vertices.size()==1U);
  CHECK(forward.constraints.split_vertices.front().numerator==1U);
  CHECK(forward.constraints.split_vertices.front().denominator==4U);
  REQUIRE(forward.constraints.facets.size()==reversed.constraints.facets.size());
  for(std::size_t i=0U;i<forward.constraints.facets.size();++i){
    CHECK(forward.constraints.facets[i].vertices==reversed.constraints.facets[i].vertices);
    CHECK(forward.constraints.facets[i].corners==reversed.constraints.facets[i].corners);
  }
}

TEST_CASE("reverse restoration undoes a journaled stellar edge split") {
  using namespace tetra::probes;
  const std::array<FrozenFacetVertex,4> vertices{{
      {10,{0,0,0}},{20,{2,0,0}},{30,{0,1,0}},{40,{0,0,1}}}};
  const std::array<std::array<std::uint64_t,3>,4> faces{{
      {{10,30,20}},{{10,20,40}},{{10,40,30}},{{20,30,40}}}};
  const auto original=materialize_canonical_plc_constraints(
      vertices,faces,false);
  REQUIRE(original.accepted());
  const auto split=split_canonical_plc_constraint_edge(
      original.constraints,{10,20},16U,32U);
  REQUIRE(split.accepted());
  REQUIRE(split.constraints.vertices.size()==5U);
  const std::vector<std::array<std::uint32_t,4>> split_mesh{
      {{0,4,2,3}},{{4,1,2,3}}};
  const auto restored=restore_last_canonical_boundary_steiner_point(
      split.constraints,split_mesh);
  INFO("restoration failure="<<static_cast<unsigned>(restored.failure));
  REQUIRE(restored.accepted());
  CHECK(restored.restored_points==1U);
  CHECK(restored.constraints.recovery_journal.empty());
  CHECK(restored.constraints.split_vertices.empty());
  REQUIRE(restored.constraints.vertices.size()==original.constraints.vertices.size());
  for(std::size_t i=0U;i<original.constraints.vertices.size();++i) {
    CHECK(restored.constraints.vertices[i].id==original.constraints.vertices[i].id);
    CHECK(restored.constraints.vertices[i].position.x==
          original.constraints.vertices[i].position.x);
    CHECK(restored.constraints.vertices[i].position.y==
          original.constraints.vertices[i].position.y);
    CHECK(restored.constraints.vertices[i].position.z==
          original.constraints.vertices[i].position.z);
  }
  REQUIRE(restored.constraints.facets.size()==original.constraints.facets.size());
  for(std::size_t i=0U;i<original.constraints.facets.size();++i) {
    CHECK(restored.constraints.facets[i].vertices==
          original.constraints.facets[i].vertices);
    CHECK(restored.constraints.facets[i].corners==
          original.constraints.facets[i].corners);
    CHECK(restored.constraints.facets[i].parent==
          original.constraints.facets[i].parent);
  }
  REQUIRE(restored.tetrahedra.size()==1U);
  auto cell=restored.tetrahedra.front();std::sort(cell.begin(),cell.end());
  CHECK(cell==std::array<std::uint32_t,4>{{0,1,2,3}});
}

TEST_CASE("Algorithm 1 relocates an edge Steiner point after its pairs are flipped") {
  using namespace tetra::probes;
  const std::array<FrozenFacetVertex,6> vertices{{
      {10,{0,0,-1}},{20,{0,0,1}},{30,{1,.5,-.25}},
      {40,{0,1,0}},{50,{-1,.5,-.25}},{60,{0,-1,0}}}};
  const std::array<std::array<std::uint64_t,3>,2> faces{{
      {{10,20,30}},{{10,50,20}}}};
  const auto original=materialize_canonical_plc_constraints(
      vertices,faces,false);
  REQUIRE(original.accepted());
  const auto split=split_canonical_plc_constraint_edge(
      original.constraints,{10,20},16U,32U);
  REQUIRE(split.accepted());
  const std::vector<std::array<std::uint32_t,4>> paired_mesh{
      {{0,6,2,3}},{{6,1,2,3}},{{0,6,3,4}},{{6,1,3,4}},
      {{0,6,4,5}},{{6,1,4,5}},{{0,6,5,2}},{{6,1,5,2}}};
  const auto flipped=try_recover_literal_edge_by_face_flip(
      split.constraints,{30,50},paired_mesh);
  INFO("face flip failure="<<static_cast<unsigned>(flipped.failure));
  REQUIRE(flipped.accepted);
  const auto restored=restore_last_canonical_boundary_steiner_point(
      split.constraints,flipped.tetrahedra);
  INFO("restoration failure="<<static_cast<unsigned>(restored.failure));
  REQUIRE(restored.accepted());
  CHECK(restored.restored_points==1U);
  CHECK(restored.interior_points_inserted==2U);
  CHECK(restored.relocation_regions==2U);
  CHECK(restored.bridge_tetrahedra==4U);
  REQUIRE(restored.constraints.interior_steiner_vertices.size()==2U);
  for(const auto& vertex:restored.constraints.interior_steiner_vertices)
    CHECK(vertex.kind==CanonicalInteriorSteinerKind::boundary_relocation);
  REQUIRE(restored.relocated_positions.size()==2U);
  std::array<double,2> transverse{};
  for(std::size_t i=0U;i<restored.relocated_positions.size();++i) {
    const auto& position=restored.relocated_positions[i];
    CHECK(position.x==doctest::Approx(0.0));
    CHECK(position.z==doctest::Approx(0.0));
    transverse[i]=position.y;
  }
  std::sort(transverse.begin(),transverse.end());
  const auto reference_offset=0.4/std::sqrt(5.0);
  CHECK(transverse[0]==doctest::Approx(-reference_offset));
  CHECK(transverse[1]==doctest::Approx(reference_offset));
  CHECK(restored.constraints.recovery_journal.empty());
  CHECK(restored.constraints.split_vertices.empty());
  REQUIRE(restored.constraints.facets.size()==original.constraints.facets.size());
  for(const auto& facet:restored.constraints.facets)
    CHECK(std::find(facet.vertices.begin(),facet.vertices.end(),
                    split.constraints.vertices.back().id)==facet.vertices.end());
  CHECK(inspect_canonical_plc_tetrahedra(
            restored.constraints,restored.tetrahedra).accepted());
}

TEST_CASE("edge relocation retries once after reference-ordered tiny-tet flips") {
  using namespace tetra::probes;
  const std::array<FrozenFacetVertex,6> vertices{{
      {10,{0,0,-1}},{20,{0,0,1}},{30,{1,.5,-.25}},
      {40,{0,1,0}},{50,{-1,.5,-.25}},{60,{0,-1,0}}}};
  const std::array<std::array<std::uint64_t,3>,2> faces{{
      {{10,20,30}},{{10,50,20}}}};
  const auto original=materialize_canonical_plc_constraints(vertices,faces,false);
  REQUIRE(original.accepted());
  const auto split=split_canonical_plc_constraint_edge(
      original.constraints,{10,20},16U,32U);
  REQUIRE(split.accepted());
  const std::vector<std::array<std::uint32_t,4>> paired_mesh{
      {{0,6,2,3}},{{6,1,2,3}},{{0,6,3,4}},{{6,1,3,4}},
      {{0,6,4,5}},{{6,1,4,5}},{{0,6,5,2}},{{6,1,5,2}}};
  const auto flipped=try_recover_literal_edge_by_face_flip(
      split.constraints,{30,50},paired_mesh);
  REQUIRE(flipped.accepted);
  auto modified=split.constraints;
  modified.vertices[3].position={0,1e-16,0};
  const auto add_surface_face=[&](std::array<std::uint64_t,3> vertices) {
    CanonicalPlcConstraintFacet facet;
    facet.vertices=vertices;facet.source_vertices=vertices;
    facet.parent.vertex_ids=vertices;
    std::sort(facet.parent.vertex_ids.begin(),facet.parent.vertex_ids.end());
    facet.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
    modified.facets.push_back(facet);
  };
  add_surface_face({50,30,40});
  add_surface_face({10,50,60});
  REQUIRE(inspect_canonical_plc_tetrahedra(
              modified,flipped.tetrahedra).accepted());

  const auto restored=restore_last_canonical_boundary_steiner_point(
      modified,flipped.tetrahedra);
  REQUIRE(restored.accepted());
  CHECK(restored.restored_points==1U);
  CHECK(restored.interior_points_inserted==2U);
  CHECK(restored.relocation_regions==2U);
  CHECK(restored.bridge_tetrahedra==4U);
  REQUIRE(restored.retry_attempts.size()==1U);
  const auto& retry=restored.retry_attempts.front();
  CHECK(retry.level==0U);
  CHECK(retry.tiny_tetrahedra==3U);
  CHECK(retry.edge_attempts==12U);
  CHECK(retry.face_attempts==7U);
  CHECK(retry.accepted_topology_mutations==1U);
  CHECK(retry.repair_point_attempts==1U);
  CHECK(retry.repair_point_candidate_attempts==2U);
  CHECK(retry.repair_point_insertions==2U);
  CHECK(retry.repair_point_location_refusals==0U);
  CHECK(retry.repair_point_insertion_refusals==
        std::array<std::size_t,8>{});
  CHECK(retry.repair_point_postcheck_refusals==0U);
  CHECK(retry.repair_point_smoothing_attempts==2U);
  CHECK(retry.repair_point_smoothing_moves==0U);
  REQUIRE(retry.repair_point_candidates.size()==2U);
  CHECK(retry.repair_point_candidates[0].kind==
        CanonicalBoundaryRestorationResult::RepairPointCandidateKind::
            neighbourhood_edge_midpoint);
  CHECK(retry.repair_point_candidates[1].kind==
        CanonicalBoundaryRestorationResult::RepairPointCandidateKind::
            bad_cell_edge_midpoint);
  CHECK(retry.repair_point_candidates[0].position.x==doctest::Approx(0.5));
  CHECK(retry.repair_point_candidates[0].position.y==doctest::Approx(-0.25));
  CHECK(retry.repair_point_candidates[0].position.z==doctest::Approx(-0.125));
  CHECK(retry.repair_point_candidates[1].position.x==doctest::Approx(0.0));
  CHECK(retry.repair_point_candidates[1].position.y==doctest::Approx(5e-17));
  CHECK(retry.repair_point_candidates[1].position.z==doctest::Approx(0.5));
  CHECK(retry.recursive_retry_invoked);
  REQUIRE(restored.constraints.vertices.size()==modified.vertices.size()+1U);
  REQUIRE(restored.constraints.interior_steiner_vertices.size()==2U);
  for(const auto& point:restored.constraints.interior_steiner_vertices)
    CHECK(point.kind==CanonicalInteriorSteinerKind::topology_repair);
  CHECK(restored.constraints.vertices[6].position.x==doctest::Approx(0.5));
  CHECK(restored.constraints.vertices[6].position.y==doctest::Approx(-0.25));
  CHECK(restored.constraints.vertices[6].position.z==doctest::Approx(-0.125));
  CHECK(restored.constraints.vertices[7].position.x==doctest::Approx(0.0));
  CHECK(restored.constraints.vertices[7].position.y==doctest::Approx(5e-17));
  CHECK(restored.constraints.vertices[7].position.z==doctest::Approx(0.5));
  CHECK(restored.constraints.recovery_journal.empty());
  CHECK(restored.constraints.split_vertices.empty());
  std::multiset<std::array<std::uint32_t,4>> actual;
  for(auto cell:restored.tetrahedra) {
    std::sort(cell.begin(),cell.end());actual.insert(cell);
  }
  std::multiset<std::array<std::uint32_t,4>> expected;
  for(auto cell:std::vector<std::array<std::uint32_t,4>>{
          {{0,1,4,7}},{{0,1,4,5}},{{0,2,3,4}},{{0,1,2,7}},
          {{0,2,3,7}},{{0,3,4,7}},{{0,2,3,4}},{{0,1,2,6}},
          {{0,1,5,6}}}) {
    std::sort(cell.begin(),cell.end());expected.insert(cell);
  }
  CHECK(actual==expected);
  CHECK(inspect_canonical_plc_tetrahedra(
            restored.constraints,restored.tetrahedra).accepted());
}

TEST_CASE("edge relocation partitions a three-facet nonmanifold one-ring") {
  using namespace tetra::probes;
  const std::array<FrozenFacetVertex,8> vertices{{
      {10,{0,0,-1}},{20,{0,0,1}},
      {30,{1,.5,-.25}},{40,{0,1,0}},{50,{-1,.5,-.25}},
      {60,{-.75,-.25,0}},{70,{0,-1,.25}},{80,{.75,-.25,0}}}};
  const std::array<std::array<std::uint64_t,3>,3> faces{{
      {{10,20,30}},{{10,50,20}},{{10,20,70}}}};
  const auto original=materialize_canonical_plc_constraints(vertices,faces,false);
  REQUIRE(original.accepted());
  const auto split=split_canonical_plc_constraint_edge(
      original.constraints,{10,20},32U,64U);
  REQUIRE(split.accepted());
  const std::array<std::uint32_t,6> ring{{2,3,4,5,6,7}};
  std::vector<std::array<std::uint32_t,4>> paired_mesh;
  for(std::size_t i=0U;i<ring.size();++i) {
    const auto j=(i+1U)%ring.size();
    paired_mesh.push_back({{0,8,ring[i],ring[j]}});
    paired_mesh.push_back({{8,1,ring[i],ring[j]}});
  }
  REQUIRE(inspect_canonical_plc_tetrahedra(
              split.constraints,paired_mesh).accepted());
  const auto flipped=try_recover_literal_edge_by_face_flip(
      split.constraints,{30,50},paired_mesh);
  REQUIRE(flipped.accepted);
  const auto restored=restore_last_canonical_boundary_steiner_point(
      split.constraints,flipped.tetrahedra);

  INFO("restoration failure="<<static_cast<unsigned>(restored.failure));
  REQUIRE(restored.accepted());
  CHECK(restored.restored_points==1U);
  CHECK(restored.relocation_regions==3U);
  CHECK(restored.interior_points_inserted==3U);
  CHECK(restored.bridge_tetrahedra==6U);
  CHECK(restored.constraints.recovery_journal.empty());
  CHECK(restored.constraints.split_vertices.empty());
  CHECK(inspect_canonical_plc_tetrahedra(
            restored.constraints,restored.tetrahedra).accepted());
}

TEST_CASE("removeInteriorSteiner deletes only a registered four-tet interior point") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{0,0,0}},{20,{1,0,0}},{30,{0,1,0}},
                        {40,{0,0,1}},{50,{.2,.2,.2}}};
  const auto facet=[&](std::array<std::uint64_t,3> vertices) {
    CanonicalPlcConstraintFacet result;
    result.vertices=vertices;
    result.source_vertices=vertices;
    result.parent.vertex_ids=vertices;
    std::sort(result.parent.vertex_ids.begin(),result.parent.vertex_ids.end());
    for(unsigned corner=0U;corner<3U;++corner) {
      const auto position=std::find(result.parent.vertex_ids.begin(),
                                    result.parent.vertex_ids.end(),vertices[corner]);
      result.corners[corner].denominator=1U;
      result.corners[corner].numerator[static_cast<std::size_t>(
          position-result.parent.vertex_ids.begin())]=1U;
    }
    return result;
  };
  constraints.facets={facet({20,30,40}),facet({10,40,30}),
                      facet({10,20,40}),facet({10,30,20})};
  constraints.interior_steiner_vertices.push_back(
      {50,CanonicalInteriorSteinerKind::facet_interior});
  const std::vector<std::array<std::uint32_t,4>> star{
      {{4,1,2,3}},{{4,0,3,2}},{{4,0,1,3}},{{4,0,2,1}}};
  REQUIRE(inspect_canonical_plc_tetrahedra(constraints,star).accepted());

  const auto refused=remove_canonical_interior_steiner_point(
      constraints,star,10U);
  CHECK_FALSE(refused.removed());
  CHECK(refused.failure==
        CanonicalInteriorPointRemovalFailure::unregistered_vertex);

  const auto removed=remove_canonical_interior_steiner_point(
      constraints,star,50U);
  INFO("removal failure="<<static_cast<unsigned>(removed.failure));
  REQUIRE(removed.removed());
  CHECK_FALSE(removed.used_four_to_one);
  CHECK(removed.edge_attempts==1U);
  CHECK(removed.edge_removals==1U);
  CHECK(removed.constraints.interior_steiner_vertices.empty());
  REQUIRE(removed.constraints.vertices.size()==4U);
  REQUIRE(removed.tetrahedra.size()==1U);
  auto cell=removed.tetrahedra.front();
  std::sort(cell.begin(),cell.end());
  CHECK(cell==std::array<std::uint32_t,4>{{0,1,2,3}});
  CHECK(inspect_canonical_plc_tetrahedra(
            removed.constraints,removed.tetrahedra).accepted());
}

TEST_CASE("smooth_volume moves a registered interior point toward equal volume") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{1,0,0}},{20,{-1,0,0}},
                        {30,{0,1,0}},{40,{0,-1,0}},
                        {50,{0,0,1}},{60,{0,0,-1}},
                        {70,{.12,.06,.03}}};
  const std::array<std::array<std::uint32_t,3>,8> faces{{
      {{0,2,4}},{{0,2,5}},{{0,3,4}},{{0,3,5}},
      {{1,2,4}},{{1,2,5}},{{1,3,4}},{{1,3,5}}}};
  std::vector<std::array<std::uint32_t,4>> star;
  for(const auto face:faces) {
    CanonicalPlcConstraintFacet facet;
    for(unsigned i=0U;i<3U;++i)
      facet.vertices[i]=constraints.vertices[face[i]].id;
    facet.source_vertices=facet.vertices;
    facet.parent.vertex_ids=facet.vertices;
    std::sort(facet.parent.vertex_ids.begin(),facet.parent.vertex_ids.end());
    facet.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
    constraints.facets.push_back(facet);
    std::array<std::uint32_t,4> cell{{6,face[0],face[1],face[2]}};
    const auto& a=constraints.vertices[cell[0]].position;
    const auto& b=constraints.vertices[cell[1]].position;
    const auto& c=constraints.vertices[cell[2]].position;
    const auto& d=constraints.vertices[cell[3]].position;
    if(signed_volume(a,b,c,d)<0.0)std::swap(cell[0],cell[1]);
    star.push_back(cell);
  }
  constraints.interior_steiner_vertices.push_back(
      {70,CanonicalInteriorSteinerKind::cascade_fhc});
  REQUIRE(inspect_canonical_plc_tetrahedra(constraints,star).accepted());

  const auto smoothed=smooth_canonical_interior_steiner_volume(
      constraints,star,70U);

  REQUIRE(smoothed.attempted);
  REQUIRE(smoothed.moved);
  CHECK(smoothed.descent_steps>=1U);
  const auto before=constraints.vertices.back().position;
  const auto after=smoothed.constraints.vertices.back().position;
  CHECK(after.x*after.x+after.y*after.y+after.z*after.z<
        before.x*before.x+before.y*before.y+before.z*before.z);
  for(std::size_t i=0U;i+1U<constraints.vertices.size();++i) {
    CHECK(smoothed.constraints.vertices[i].position.x==
          constraints.vertices[i].position.x);
    CHECK(smoothed.constraints.vertices[i].position.y==
          constraints.vertices[i].position.y);
    CHECK(smoothed.constraints.vertices[i].position.z==
          constraints.vertices[i].position.z);
  }
  CHECK(inspect_canonical_plc_tetrahedra(
            smoothed.constraints,smoothed.tetrahedra).accepted());
}

TEST_CASE("smooth_sus improves a registered repair point without moving its neighbours") {
  using namespace tetra::probes;
  const auto star=octahedral_interior_star({.32,.08,.04});
  REQUIRE(inspect_canonical_plc_tetrahedra(star.constraints,star.tetrahedra).accepted());

  const auto smoothed=smooth_canonical_interior_steiner_sus(
      star.constraints,star.tetrahedra,70U);

  REQUIRE(smoothed.attempted);
  REQUIRE(smoothed.moved);
  CHECK(smoothed.descent_steps>=1U);
  const auto before=star.constraints.vertices.back().position;
  const auto after=smoothed.constraints.vertices.back().position;
  CHECK(after.x*after.x+after.y*after.y+after.z*after.z<
        before.x*before.x+before.y*before.y+before.z*before.z);
  for(std::size_t i=0U;i+1U<star.constraints.vertices.size();++i) {
    CHECK(smoothed.constraints.vertices[i].position.x==star.constraints.vertices[i].position.x);
    CHECK(smoothed.constraints.vertices[i].position.y==star.constraints.vertices[i].position.y);
    CHECK(smoothed.constraints.vertices[i].position.z==star.constraints.vertices[i].position.z);
  }
  CHECK(smoothed.tetrahedra==star.tetrahedra);
  CHECK(inspect_canonical_plc_tetrahedra(
            smoothed.constraints,smoothed.tetrahedra).accepted());
}

TEST_CASE("nested constraint intersections retain barycentrics beyond 32 bits") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet input;
  input.vertices={{10,{0,0,0}},{20,{2,0,0}},{30,{0,2,0}}};
  CanonicalPlcConstraintFacet face;face.parent={{10,20,30}};
  face.vertices={{10,20,30}};face.source_vertices=face.vertices;
  face.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  input.facets={face};
  auto current=input;
  for(unsigned iteration=0U;iteration<3U;++iteration) {
    const auto split=split_canonical_plc_constraint_edge_at_ratio(
        current,{20,current.vertices.back().id},1U,4096U,16U,32U);
    REQUIRE(split.accepted());
    current=split.constraints;
  }
  std::uint64_t largest_denominator{};
  for(const auto& facet:current.facets)for(const auto& corner:facet.corners)
    largest_denominator=std::max(largest_denominator,corner.denominator);
  CHECK(largest_denominator>std::numeric_limits<std::uint32_t>::max());
}

TEST_CASE("constraint-facet interior split preserves immutable parent provenance") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet input;
  input.vertices={{10,{0,0,0}},{20,{3,0,0}},{30,{0,3,0}}};
  CanonicalPlcConstraintFacet face;face.parent={{10,20,30}};
  face.vertices={{10,20,30}};face.source_vertices=face.vertices;
  face.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  input.facets={face};
  const auto split=split_canonical_plc_constraint_facet(
      input,{10,20,30},{1,1,1},3U,8U,8U);
  REQUIRE(split.accepted());
  REQUIRE(split.constraints.vertices.size()==4U);
  CHECK(split.constraints.vertices.back().position.x==doctest::Approx(1.0));
  CHECK(split.constraints.vertices.back().position.y==doctest::Approx(1.0));
  CHECK(split.constraints.vertices.back().position.z==doctest::Approx(0.0));
  CHECK(split.constraints.facets.size()==3U);
  REQUIRE(split.constraints.facet_split_vertices.size()==1U);
  const auto split_id=split.constraints.facet_split_vertices.front().id;
  CHECK(split.constraints.facets[0].vertices==
        std::array<std::uint64_t,3>{{10U,20U,split_id}});
  CHECK(split.constraints.facets[1].vertices==
        std::array<std::uint64_t,3>{{20U,30U,split_id}});
  CHECK(split.constraints.facets[2].vertices==
        std::array<std::uint64_t,3>{{30U,10U,split_id}});
  CHECK(split.constraints.facet_split_vertices.front().parent==face.parent);
  CHECK(split.constraints.facet_split_vertices.front().barycentric==
        FacetBarycentricPoint{{1,1,1},3U});
  REQUIRE(split.constraints.recovery_journal.size()==1U);
  CHECK(split.constraints.recovery_journal.front().kind==
        CanonicalPlcRecoveryInsertionKind::facet_split);
  CHECK(split.constraints.recovery_journal.front().vertex_id==
        split.constraints.facet_split_vertices.front().id);
  for(const auto& child:split.constraints.facets) {
    CHECK(child.parent==face.parent);
    CHECK(child.source_vertices==face.source_vertices);
  }
}

TEST_CASE("reverse restoration undoes a journaled stellar facet split") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet original;
  original.vertices={{10,{0,0,0}},{20,{2,0,0}},{30,{0,2,0}},
                     {40,{0,0,1}},{50,{0,0,-1}}};
  CanonicalPlcConstraintFacet face;face.parent={{10,20,30}};
  face.vertices={{10,20,30}};face.source_vertices=face.vertices;
  face.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  original.facets={face};
  const auto split=split_canonical_plc_constraint_facet(
      original,{10,20,30},{1,1,1},3U,16U,16U);
  REQUIRE(split.accepted());
  const std::vector<std::array<std::uint32_t,4>> split_mesh{
      {{5,0,1,3}},{{5,1,2,3}},{{5,2,0,3}},
      {{5,1,0,4}},{{5,2,1,4}},{{5,0,2,4}}};
  const auto restored=restore_last_canonical_boundary_steiner_point(
      split.constraints,split_mesh);
  INFO("restoration failure="<<static_cast<unsigned>(restored.failure));
  REQUIRE(restored.accepted());
  CHECK(restored.constraints.recovery_journal.empty());
  CHECK(restored.constraints.facet_split_vertices.empty());
  REQUIRE(restored.constraints.facets.size()==1U);
  CHECK(restored.constraints.facets.front().vertices==face.vertices);
  CHECK(restored.constraints.facets.front().corners==face.corners);
  REQUIRE(restored.tetrahedra.size()==2U);
  std::multiset<std::array<std::uint32_t,4>> topology;
  for(auto cell:restored.tetrahedra) {
    std::sort(cell.begin(),cell.end());topology.insert(cell);
  }
  CHECK(topology==std::multiset<std::array<std::uint32_t,4>>{
      {{0,1,2,3}},{{0,1,2,4}}});
  CHECK(restored.relocation_regions==0U);
  CHECK(restored.bridge_tetrahedra==0U);
  CHECK(restored.relocated_positions.empty());
}

TEST_CASE("Algorithm 1 relocates a facet Steiner point with a refined half-ball") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet original;
  original.vertices={{10,{0,0,0}},{20,{2,0,0}},{30,{0,2,0}},
                     {40,{0,0,1}},{50,{0,0,-4}}};
  CanonicalPlcConstraintFacet face;face.parent={{10,20,30}};
  face.vertices={{10,20,30}};face.source_vertices=face.vertices;
  face.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  original.facets={face};
  auto split=split_canonical_plc_constraint_facet(
      original,{10,20,30},{1,1,1},3U,16U,16U);
  REQUIRE(split.accepted());
  const auto s=split.constraints.vertices.back().position;
  const auto x=(s+original.vertices[0].position+original.vertices[1].position+
                original.vertices[3].position)/4.0;
  std::uint64_t next_id{};
  for(const auto& vertex:split.constraints.vertices)
    next_id=std::max(next_id,vertex.id);
  split.constraints.vertices.push_back({next_id+1U,x});
  const std::vector<std::array<std::uint32_t,4>> refined_mesh{
      {{5,6,0,1}},{{5,6,1,3}},{{5,6,3,0}},{{6,0,1,3}},
      {{5,1,2,3}},{{5,2,0,3}},
      {{5,1,0,4}},{{5,2,1,4}},{{5,0,2,4}}};
  const auto restored=restore_last_canonical_boundary_steiner_point(
      split.constraints,refined_mesh);
  INFO("restoration failure="<<static_cast<unsigned>(restored.failure));
  REQUIRE(restored.accepted());
  CHECK(restored.restored_points==1U);
  CHECK(restored.interior_points_inserted==2U);
  CHECK(restored.relocation_regions==2U);
  CHECK(restored.bridge_tetrahedra==2U);
  REQUIRE(restored.constraints.interior_steiner_vertices.size()==2U);
  for(const auto& vertex:restored.constraints.interior_steiner_vertices)
    CHECK(vertex.kind==CanonicalInteriorSteinerKind::boundary_relocation);
  REQUIRE(restored.relocated_positions.size()==2U);
  const auto reference_step=std::sqrt(8.0/9.0);
  std::array<double,2> distances{};
  for(std::size_t i=0U;i<restored.relocated_positions.size();++i) {
    const auto& position=restored.relocated_positions[i];
    CHECK(position.x==doctest::Approx(2.0/3.0));
    CHECK(position.y==doctest::Approx(2.0/3.0));
    distances[i]=std::abs(position.z);
  }
  std::sort(distances.begin(),distances.end());
  CHECK(distances[0]==doctest::Approx(reference_step/4.0));
  CHECK(distances[1]==doctest::Approx(reference_step));
  CHECK(restored.relocated_positions[0].z*
        restored.relocated_positions[1].z<0.0);
  CHECK(restored.constraints.recovery_journal.empty());
  CHECK(restored.constraints.facet_split_vertices.empty());
  REQUIRE(restored.constraints.facets.size()==1U);
  CHECK(restored.constraints.facets.front().vertices==face.vertices);
  CHECK(restored.tetrahedra.size()==11U);
  CHECK(inspect_canonical_plc_tetrahedra(
            restored.constraints,restored.tetrahedra).accepted());
}

TEST_CASE("two-sided cavity recovery replaces a piercing bipyramid edge") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{-1,-1,0}},{20,{1,-1,0}},{30,{0,1,0}},
                        {40,{0,0,1}},{50,{0,0,-1}}};
  CanonicalPlcConstraintFacet face;face.parent={{10,20,30}};
  face.vertices={{10,20,30}};face.source_vertices=face.vertices;
  face.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  constraints.facets={face};
  const std::vector<std::array<std::uint32_t,4>> around_piercing_edge{
      {{3,4,0,1}},{{3,4,1,2}},{{3,4,2,0}}};

  const auto recovered=recover_literal_facet_by_two_sided_cavity(
      constraints,{10,20,30},around_piercing_edge);
  INFO("failure="<<static_cast<unsigned int>(recovered.failure)
       <<" intersected="<<recovered.intersected_tetrahedra
       <<" top="<<recovered.top_tetrahedra<<" bottom="<<recovered.bottom_tetrahedra
       <<" trials="<<recovered.retriangulation_trials
       <<" expansions="<<recovered.cavity_expansions
       <<" old6="<<recovered.original_cavity_six_volume
       <<" new6="<<recovered.replacement_six_volume);
  REQUIRE(recovered.accepted);
  CHECK_FALSE(recovered.already_recovered);
  CHECK(recovered.intersected_tetrahedra==3U);
  CHECK(recovered.top_tetrahedra==1U);
  CHECK(recovered.bottom_tetrahedra==1U);
  CHECK(recovered.used_local_delaunay);
  CHECK(recovered.tetrahedra.size()==2U);
  const auto inspection=inspect_canonical_plc_tetrahedra(
      constraints,recovered.tetrahedra);
  CHECK(inspection.accepted());

  const auto repeated=recover_literal_facet_by_two_sided_cavity(
      constraints,{30,10,20},recovered.tetrahedra);
  CHECK(repeated.accepted);
  CHECK(repeated.already_recovered);
  CHECK(repeated.tetrahedra==recovered.tetrahedra);

  auto reversed_input=around_piercing_edge;
  std::reverse(reversed_input.begin(),reversed_input.end());
  const auto reordered=recover_literal_facet_by_two_sided_cavity(
      constraints,{20,30,10},reversed_input);
  REQUIRE(reordered.accepted);
  CHECK(reordered.tetrahedra==recovered.tetrahedra);

  auto transformed=constraints;
  for(auto& vertex:transformed.vertices)vertex.position={
      2.0*vertex.position.x+1.0,2.0*vertex.position.y-3.0,
      2.0*vertex.position.z+4.0};
  const auto moved=recover_literal_facet_by_two_sided_cavity(
      transformed,{10,20,30},around_piercing_edge);
  REQUIRE(moved.accepted);
  CHECK(moved.tetrahedra==recovered.tetrahedra);
}

TEST_CASE("two-sided cavity recovery resolves a split-to-unsplit parent interface") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{-1,-1,0}},{20,{1,-1,0}},{30,{0,1,0}},
                        {40,{0,-1,1}},{50,{0,-3,0}},{60,{0,-1,-1}},
                        {70,{0,-1,0}}};
  const FrozenFacetIdentity parent{{10,20,30}};
  const FacetBarycentricPoint a{{1,0,0},1U},b{{0,1,0},1U},
      c{{0,0,1},1U},midpoint{{1,1,0},2U};
  CanonicalPlcConstraintFacet left;left.parent=parent;
  left.vertices={{10,70,30}};left.source_vertices={{10,20,30}};
  left.corners={{a,midpoint,c}};
  CanonicalPlcConstraintFacet right;right.parent=parent;
  right.vertices={{70,20,30}};right.source_vertices={{10,20,30}};
  right.corners={{midpoint,b,c}};
  constraints.facets={left,right};
  constraints.split_vertices.push_back({70,{10,20},1U,2U});
  // The lower half already uses the split patch, while the upper half still
  // exposes the coarse parent face. Literal-face presence alone must not
  // classify this T-junction as recovered.
  const std::vector<std::array<std::uint32_t,4>> nonconforming{
      {{0,1,2,3}},{{0,1,3,4}},{{0,1,4,5}},
      {{0,6,5,2}},{{6,1,5,2}}};
  CHECK_FALSE(inspect_canonical_plc_tetrahedra(
      constraints,nonconforming).accepted());

  const auto recovered=recover_literal_facet_by_two_sided_cavity(
      constraints,left.vertices,nonconforming);
  INFO("failure="<<static_cast<unsigned int>(recovered.failure)
       <<" intersected="<<recovered.intersected_tetrahedra
       <<" top="<<recovered.top_tetrahedra
       <<" bottom="<<recovered.bottom_tetrahedra
       <<" trials="<<recovered.retriangulation_trials);
  REQUIRE(recovered.accepted);
  CHECK_FALSE(recovered.already_recovered);
  CHECK(inspect_canonical_plc_tetrahedra(
      constraints,recovered.tetrahedra).accepted());
}

TEST_CASE("segment recovery removes a crossed face with a two-to-three flip") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{-1,-1,0}},{20,{1,-1,0}},{30,{0,1,0}},
                        {40,{0,0,-1}},{50,{0,0,1}}};
  const std::vector<std::array<std::uint32_t,4>> mesh{
      {{3,0,1,2}},{{4,0,2,1}}};
  const auto recovered=try_recover_literal_edge_by_face_flip(
      constraints,{40,50},mesh);
  INFO("failure="<<static_cast<unsigned>(recovered.failure));
  REQUIRE(recovered.accepted);
  CHECK(recovered.cavity_tetrahedra.size()==2U);
  CHECK(recovered.tetrahedra.size()==3U);
  for(const auto& cell:recovered.tetrahedra) {
    CHECK(std::find(cell.begin(),cell.end(),3U)!=cell.end());
    CHECK(std::find(cell.begin(),cell.end(),4U)!=cell.end());
  }
  auto reversed=mesh;std::reverse(reversed.begin(),reversed.end());
  const auto reordered=try_recover_literal_edge_by_face_flip(
      constraints,{50,40},reversed);
  REQUIRE(reordered.accepted);
  CHECK(reordered.tetrahedra==recovered.tetrahedra);

  const auto wang=try_recover_wang_segment_by_local_flips(
      constraints,{40,50},mesh);
  REQUIRE(wang.flip.accepted);
  CHECK(wang.segment_recovered);
  CHECK(wang.preserved_recovered_constraints);
  CHECK(wang.edge_removal_attempts==0U);
  CHECK(wang.flip.tetrahedra==recovered.tetrahedra);
}

TEST_CASE("Wang local segment flips freeze an already recovered facet") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{-1,-1,0}},{20,{1,-1,0}},{30,{0,1,0}},
                        {40,{0,0,-1}},{50,{0,0,1}}};
  CanonicalPlcConstraintFacet frozen;
  frozen.parent={{10,20,30}};
  frozen.vertices={{10,20,30}};
  frozen.source_vertices=frozen.vertices;
  frozen.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  constraints.facets={frozen};
  const std::vector<std::array<std::uint32_t,4>> mesh{
      {{3,0,1,2}},{{4,0,2,1}}};
  REQUIRE(inspect_canonical_plc_tetrahedra(constraints,mesh).accepted());

  const auto wang=try_recover_wang_segment_by_local_flips(
      constraints,{40,50},mesh);

  CHECK_FALSE(wang.flip.accepted);
  CHECK(wang.flip.failure==
        CanonicalLiteralEdgeFlipFailure::frozen_cavity_boundary);
  CHECK(wang.preserved_recovered_constraints);
  CHECK_FALSE(wang.segment_recovered);
}

TEST_CASE("a recovered constraint edge classifies and resolves a Locked-FHC") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{0,0,0}},{20,{2,0,0}},{30,{0,2,0}},
                        {40,{.2,.2,-1}},{50,{2,2,1}},
                        {60,{.1,.2,-1}},{70,{.7,.8,2}},
                        {80,{-1,.5,0}}};
  CanonicalPlcConstraintFacet protected_face;
  protected_face.vertices={20,30,40};
  protected_face.parent={protected_face.vertices};
  protected_face.source_vertices=protected_face.vertices;
  protected_face.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  CanonicalPlcConstraintFacet target_face;
  target_face.vertices={60,70,80};
  target_face.parent={target_face.vertices};
  target_face.source_vertices=target_face.vertices;
  target_face.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  constraints.facets={protected_face,target_face};
  const std::vector<std::array<std::uint32_t,4>> mesh{
      {{3,0,1,2}},{{4,0,2,1}},{{3,0,2,7}},{{4,0,7,2}}};
  const auto refused=try_recover_literal_edge_by_face_flip(
      constraints,{60,70},mesh);
  CHECK_FALSE(refused.accepted);
  CHECK(refused.failure==CanonicalLiteralEdgeFlipFailure::nonpositive_replacement);
  REQUIRE(refused.blocking_mesh_edge.has_value());

  const auto classification=classify_wang_locked_fhc(
      constraints,{60,70},mesh);
  REQUIRE(classification.classified());
  CHECK(classification.intersecting_face==
        std::array<std::uint64_t,3>{{10,20,30}});
  CHECK(classification.locking_constraint_edge==
        std::array<std::uint64_t,2>{{20,30}});
  CHECK(classification.locking_mesh_edge==
        std::array<std::uint32_t,2>{{1,2}});
  auto incident=classification.incident_tetrahedra;
  auto expected_incident=std::vector<std::array<std::uint32_t,4>>{
      {{3,0,1,2}},{{4,0,2,1}}};
  std::sort(incident.begin(),incident.end());
  std::sort(expected_incident.begin(),expected_incident.end());
  CHECK(incident==expected_incident);
  CHECK(classification.exact_s0.x==doctest::Approx(0.3).epsilon(1e-14));
  CHECK(classification.exact_s0.y==doctest::Approx(0.4).epsilon(1e-14));
  CHECK(classification.exact_s0.z==doctest::Approx(0.0));
  CHECK(std::bit_cast<std::uint64_t>(classification.exact_s0.z)==
        13587360075776786432ULL);
  CHECK(classification.barycenter.x==doctest::Approx(2.3/3.0));
  CHECK(classification.barycenter.y==doctest::Approx(2.4/3.0));
  CHECK(classification.barycenter.z==doctest::Approx(0.0));
  CHECK(std::bit_cast<std::uint64_t>(classification.barycenter.z)==
        13579854076397835605ULL);
  const auto locked_removal=try_remove_mesh_edge_by_flip(
      constraints,classification.locking_mesh_edge,mesh);
  CHECK_FALSE(locked_removal.accepted);
  CHECK(locked_removal.failure==
        CanonicalLiteralEdgeFlipFailure::frozen_cavity_boundary);

  const auto inserted=insert_locked_fhc_vertex(
      constraints,{60,70},classification.locking_mesh_edge,mesh);
  REQUIRE(inserted.accepted);
  REQUIRE(inserted.inserted_steiner_vertex.has_value());
  CHECK(inserted.inserted_steiner_vertex->x==doctest::Approx(2.3/3.0));
  CHECK(inserted.inserted_steiner_vertex->y==doctest::Approx(2.4/3.0));
  CHECK(inserted.inserted_steiner_vertex->z==doctest::Approx(0.0));
  CHECK(inserted.tetrahedra.size()==8U);
  CHECK(std::count_if(inserted.tetrahedra.begin(),inserted.tetrahedra.end(),
      [](const auto& cell) {
        return std::find(cell.begin(),cell.end(),8U)!=cell.end();
      })==6);
  auto augmented=constraints;
  augmented.vertices.push_back({81,*inserted.inserted_steiner_vertex});
  const auto updated=classify_wang_locked_fhc(
      augmented,{60,70},inserted.tetrahedra);
  const bool same_lock=updated.classified()&&
      updated.locking_constraint_edge==classification.locking_constraint_edge;
  CHECK_FALSE(same_lock);
  const auto before=inspect_canonical_plc_tetrahedra(constraints,mesh);
  const auto after=inspect_canonical_plc_tetrahedra(augmented,inserted.tetrahedra);
  CHECK(before.missing_edges==after.missing_edges);
  CHECK(before.missing_facets==after.missing_facets);

  const auto direct_after=try_recover_literal_edge_by_face_flip(
      augmented,{60,70},inserted.tetrahedra);
  REQUIRE(direct_after.accepted);
  const auto after_retry=inspect_canonical_plc_tetrahedra(
      augmented,direct_after.tetrahedra);
  CHECK(after.missing_edges==after_retry.missing_edges);
  CHECK(after.missing_facets==after_retry.missing_facets);
  const auto continued=try_recover_wang_segment_by_local_flips(
      augmented,{60,70},inserted.tetrahedra);
  CHECK(continued.preserved_recovered_constraints);

  const auto wrong_edge=insert_locked_fhc_vertex(
      constraints,{60,70},{0,1},mesh);
  CHECK_FALSE(wrong_edge.accepted);
  auto reversed=mesh;std::reverse(reversed.begin(),reversed.end());
  const auto reordered=classify_wang_locked_fhc(
      constraints,{70,60},reversed);
  REQUIRE(reordered.classified());
  CHECK(reordered.intersecting_face==classification.intersecting_face);
  CHECK(reordered.locking_constraint_edge==classification.locking_constraint_edge);
  CHECK(reordered.exact_s0.x==classification.exact_s0.x);
  CHECK(reordered.exact_s0.y==classification.exact_s0.y);
  CHECK(reordered.exact_s0.z==classification.exact_s0.z);
  const auto reordered_insertion=insert_locked_fhc_vertex(
      constraints,{70,60},reordered.locking_mesh_edge,reversed);
  REQUIRE(reordered_insertion.accepted);
  REQUIRE(reordered_insertion.inserted_steiner_vertex.has_value());
  REQUIRE(inserted.inserted_steiner_vertex.has_value());
  CHECK(reordered_insertion.inserted_steiner_vertex->x==
        inserted.inserted_steiner_vertex->x);
  CHECK(reordered_insertion.inserted_steiner_vertex->y==
        inserted.inserted_steiner_vertex->y);
  CHECK(reordered_insertion.inserted_steiner_vertex->z==
        inserted.inserted_steiner_vertex->z);
  CHECK(reordered_insertion.tetrahedra==inserted.tetrahedra);
}

TEST_CASE("a geometric face-flip failure is not a Locked-FHC") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{0,0,0}},{20,{2,0,0}},{30,{0,2,0}},
                        {40,{.2,.2,-1}},{50,{2,2,1}},
                        {60,{.1,.2,-1}},{70,{.7,.8,2}},
                        {80,{-1,.5,0}}};
  CanonicalPlcConstraintFacet target_face;
  target_face.vertices={60,70,80};
  target_face.parent={target_face.vertices};
  target_face.source_vertices=target_face.vertices;
  target_face.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  constraints.facets={target_face};
  const std::vector<std::array<std::uint32_t,4>> mesh{
      {{3,0,1,2}},{{4,0,2,1}}};
  const auto geometric_failure=try_recover_literal_edge_by_face_flip(
      constraints,{60,70},mesh);
  REQUIRE(geometric_failure.blocking_mesh_edge.has_value());
  const auto classification=classify_wang_locked_fhc(
      constraints,{60,70},mesh);
  CHECK_FALSE(classification.classified());
  CHECK(classification.failure==WangLockedFhcFailure::locking_edge_not_constraint);
  const auto insertion=insert_locked_fhc_vertex(
      constraints,{60,70},*geometric_failure.blocking_mesh_edge,mesh);
  CHECK_FALSE(insertion.accepted);
  auto unregistered_target=constraints;
  unregistered_target.facets.clear();
  const auto not_a_constraint=classify_wang_locked_fhc(
      unregistered_target,{60,70},mesh);
  CHECK(not_a_constraint.failure==WangLockedFhcFailure::invalid_segment);
}

TEST_CASE("Wang boundary insertion splits every incident PLC facet through its Bowyer-Watson cavity") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{0,0,-1}},{20,{0,0,1}},
                        {30,{-1,-1,.25}},{40,{1,-1,.25}},{50,{0,1,.25}}};
  const auto facet=[&](std::array<std::uint64_t,3> vertices) {
    CanonicalPlcConstraintFacet result;
    result.vertices=vertices;
    result.source_vertices=vertices;
    result.parent.vertex_ids=vertices;
    std::sort(result.parent.vertex_ids.begin(),result.parent.vertex_ids.end());
    for(unsigned corner=0U;corner<3U;++corner) {
      const auto position=std::find(result.parent.vertex_ids.begin(),
                                    result.parent.vertex_ids.end(),vertices[corner]);
      result.corners[corner].denominator=1U;
      result.corners[corner].numerator[static_cast<std::size_t>(
          position-result.parent.vertex_ids.begin())]=1U;
    }
    return result;
  };
  constraints.facets={facet({10,20,30}),facet({20,10,50})};
  const std::vector<std::array<std::uint32_t,4>> mesh{
      {{0,2,3,4}},{{1,2,4,3}}};
  const auto before=inspect_canonical_plc_tetrahedra(constraints,mesh);
  REQUIRE(before.failure==CanonicalPlcSeedFailure::unrecovered_constraint);
  CHECK(before.missing_edges==
        std::vector<std::array<std::uint64_t,2>>{{{10,20}}});
  REQUIRE(before.missing_facets.size()==2U);

  const auto inserted=insert_wang_segment_boundary_steiner_point(
      constraints,{10,20},mesh,16U,32U);
  INFO("failure="<<static_cast<unsigned>(inserted.failure)
       <<" insertion="<<static_cast<unsigned>(inserted.insertion_failure));
  REQUIRE(inserted.accepted());
  CHECK_FALSE(inserted.used_midpoint_fallback);
  CHECK(inserted.intersected_mesh_face==
        std::array<std::uint64_t,3>{{30,40,50}});
  CHECK(inserted.intersected_mesh_edge==std::array<std::uint64_t,2>{});
  CHECK(inserted.split_numerator==5U);
  CHECK(inserted.split_denominator==8U);
  REQUIRE(inserted.bowyer_watson_seed_tetrahedra.size()==2U);
  for(const auto& seed:inserted.bowyer_watson_seed_tetrahedra)
    CHECK(std::find(inserted.bowyer_watson_cavity.begin(),
                    inserted.bowyer_watson_cavity.end(),seed)!=
          inserted.bowyer_watson_cavity.end());
  CHECK(inserted.unrecovered_measure_after<
        inserted.unrecovered_measure_before);
  REQUIRE(inserted.constraints.vertices.size()==6U);
  const auto split_id=inserted.constraints.vertices.back().id;
  const auto split_position=inserted.constraints.vertices.back().position;
  CHECK(split_position.x==doctest::Approx(0.0));
  CHECK(split_position.y==doctest::Approx(0.0));
  CHECK(split_position.z==doctest::Approx(0.25));
  for(const auto& seed:inserted.bowyer_watson_seed_tetrahedra) {
    bool lies_on_seed_boundary=false;
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      std::array<tetra::Vec3,3> face_points{};
      unsigned cursor{};
      for(unsigned i=0U;i<4U;++i)if(i!=omitted)
        face_points[cursor++]=constraints.vertices[seed[i]].position;
      const auto opposite=constraints.vertices[seed[omitted]].position;
      const auto side_product=
          signed_volume(face_points[0],face_points[1],face_points[2],opposite)*
          signed_volume(face_points[0],face_points[1],face_points[2],
                        split_position);
      CHECK(side_product>=0.0);
      lies_on_seed_boundary=lies_on_seed_boundary||side_product==0.0;
    }
    CHECK(lies_on_seed_boundary);
  }
  CHECK(inserted.recovered_child_segment==
        std::array<std::uint64_t,2>{{10,split_id}});
  const auto recovered_length=constraints.vertices[0].position-split_position;
  CHECK(recovered_length.x*recovered_length.x+
        recovered_length.y*recovered_length.y+
        recovered_length.z*recovered_length.z>0.0);
  REQUIRE(inserted.constraints.split_vertices.size()==1U);
  CHECK(inserted.constraints.split_vertices.front().edge==
        std::array<std::uint64_t,2>{{10,20}});
  CHECK(inserted.constraints.split_vertices.front().numerator==5U);
  CHECK(inserted.constraints.split_vertices.front().denominator==8U);
  REQUIRE(inserted.constraints.recovery_journal.size()==1U);
  CHECK(inserted.constraints.recovery_journal.front()==
        CanonicalPlcRecoveryInsertion{
            CanonicalPlcRecoveryInsertionKind::edge_split,split_id,
            constraints.facets,constraints.facets});
  REQUIRE(inserted.constraints.facets.size()==4U);
  for(const auto& parent:constraints.facets) {
    const auto child_count=std::count_if(
        inserted.constraints.facets.begin(),inserted.constraints.facets.end(),
        [&](const auto& child) {
          return child.parent==parent.parent&&
                 child.source_vertices==parent.source_vertices&&
                 std::find(child.vertices.begin(),child.vertices.end(),split_id)!=
                     child.vertices.end();
        });
    CHECK(child_count==2);
  }
  CHECK(inspect_canonical_plc_tetrahedra(
            inserted.constraints,inserted.tetrahedra).accepted());

  const auto restored=restore_last_canonical_boundary_steiner_point(
      inserted.constraints,inserted.tetrahedra);
  INFO("restoration failure="<<static_cast<unsigned>(restored.failure));
  REQUIRE(restored.accepted());
  CHECK(restored.restored_points==1U);
  REQUIRE(restored.constraints.vertices.size()==constraints.vertices.size());
  for(std::size_t i=0U;i<constraints.vertices.size();++i) {
    CHECK(restored.constraints.vertices[i].id==constraints.vertices[i].id);
    CHECK(restored.constraints.vertices[i].position.x==
          constraints.vertices[i].position.x);
    CHECK(restored.constraints.vertices[i].position.y==
          constraints.vertices[i].position.y);
    CHECK(restored.constraints.vertices[i].position.z==
          constraints.vertices[i].position.z);
  }
  REQUIRE(restored.constraints.facets.size()==constraints.facets.size());
  for(std::size_t i=0U;i<constraints.facets.size();++i) {
    CHECK(restored.constraints.facets[i].vertices==constraints.facets[i].vertices);
    CHECK(restored.constraints.facets[i].source_vertices==
          constraints.facets[i].source_vertices);
    CHECK(restored.constraints.facets[i].parent==constraints.facets[i].parent);
    CHECK(restored.constraints.facets[i].corners==constraints.facets[i].corners);
  }
  CHECK(restored.constraints.split_vertices.empty());
  CHECK(restored.constraints.recovery_journal.empty());
  CHECK(inspect_canonical_plc_tetrahedra(
            restored.constraints,restored.tetrahedra).accepted());

  auto reversed_mesh=mesh;
  std::reverse(reversed_mesh.begin(),reversed_mesh.end());
  const auto reordered=insert_wang_segment_boundary_steiner_point(
      constraints,{20,10},reversed_mesh,16U,32U);
  REQUIRE(reordered.accepted());
  CHECK(reordered.split_numerator==inserted.split_numerator);
  CHECK(reordered.split_denominator==inserted.split_denominator);
  CHECK(reordered.seed_tetrahedron==inserted.seed_tetrahedron);
  CHECK(reordered.bowyer_watson_cavity==inserted.bowyer_watson_cavity);
  CHECK(reordered.constraints.split_vertices==inserted.constraints.split_vertices);
  REQUIRE(reordered.constraints.facets.size()==inserted.constraints.facets.size());
  for(std::size_t i=0U;i<inserted.constraints.facets.size();++i) {
    CHECK(reordered.constraints.facets[i].parent==
          inserted.constraints.facets[i].parent);
    CHECK(reordered.constraints.facets[i].corners==
          inserted.constraints.facets[i].corners);
    CHECK(reordered.constraints.facets[i].vertices==
          inserted.constraints.facets[i].vertices);
    CHECK(reordered.constraints.facets[i].source_vertices==
          inserted.constraints.facets[i].source_vertices);
  }
  CHECK(reordered.tetrahedra==inserted.tetrahedra);

  CHECK(insert_wang_segment_boundary_steiner_point(
            constraints,{10,30},mesh,16U,32U).failure==
        WangSegmentBoundaryInsertionFailure::segment_already_recovered);
  CHECK(insert_wang_segment_boundary_steiner_point(
            constraints,{30,40},mesh,16U,32U).failure==
        WangSegmentBoundaryInsertionFailure::invalid_segment);

  const auto capacity_refusal=insert_wang_segment_boundary_steiner_point(
      constraints,{10,20},mesh,constraints.vertices.size(),32U);
  CHECK(capacity_refusal.failure==
        WangSegmentBoundaryInsertionFailure::constraint_split_failed);
  CHECK_FALSE(capacity_refusal.used_midpoint_fallback);
  CHECK(capacity_refusal.intersected_mesh_face==
        std::array<std::uint64_t,3>{{30,40,50}});
}

TEST_CASE("Wang boundary restoration reverses nested edge splits through immediate facet snapshots") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet original;
  original.vertices={{10,{0,0,0}},{20,{2,0,0}},
                     {30,{0,2,0}},{40,{0,0,2}}};
  CanonicalPlcConstraintFacet facet;
  facet.vertices={{10,20,30}};
  facet.source_vertices=facet.vertices;
  facet.parent.vertex_ids=facet.vertices;
  for(unsigned corner=0U;corner<3U;++corner) {
    facet.corners[corner].denominator=1U;
    facet.corners[corner].numerator[corner]=1U;
  }
  original.facets={facet};

  const auto first=split_canonical_plc_constraint_edge_at_ratio(
      original,{10,20},1U,2U,16U,32U);
  REQUIRE(first.accepted());
  REQUIRE(first.constraints.recovery_journal.size()==1U);
  REQUIRE(first.constraints.recovery_journal.back().replaced_facets==
          original.facets);
  const auto first_id=first.constraints.recovery_journal.back().vertex_id;

  const auto second=split_canonical_plc_constraint_edge_at_ratio(
      first.constraints,{first_id,30},1U,2U,16U,32U);
  REQUIRE(second.accepted());
  REQUIRE(second.constraints.recovery_journal.size()==2U);
  REQUIRE(second.constraints.recovery_journal.back().replaced_facets==
          first.constraints.facets);
  const auto second_id=second.constraints.recovery_journal.back().vertex_id;
  CHECK(first_id!=second_id);

  // The two successive edge subdivisions of tetrahedron (10,20,30,40).
  // Vertex indices 4 and 5 are the first and second inserted points.
  const std::vector<std::array<std::uint32_t,4>> twice_split{{
      {{0,4,5,3}},{{0,5,2,3}},{{4,1,5,3}},{{5,1,2,3}}}};
  REQUIRE(inspect_canonical_plc_tetrahedra(
              second.constraints,twice_split).accepted());

  const auto undo_second=restore_last_canonical_boundary_steiner_point(
      second.constraints,twice_split);
  INFO("second restoration failure="
       <<static_cast<unsigned>(undo_second.failure));
  REQUIRE(undo_second.accepted());
  CHECK(undo_second.constraints.facets==first.constraints.facets);
  REQUIRE(undo_second.constraints.recovery_journal.size()==1U);
  REQUIRE(inspect_canonical_plc_tetrahedra(
              undo_second.constraints,undo_second.tetrahedra).accepted());

  const auto undo_first=restore_last_canonical_boundary_steiner_point(
      undo_second.constraints,undo_second.tetrahedra);
  INFO("first restoration failure="
       <<static_cast<unsigned>(undo_first.failure));
  REQUIRE(undo_first.accepted());
  CHECK(undo_first.constraints.facets==original.facets);
  REQUIRE(undo_first.constraints.vertices.size()==original.vertices.size());
  for(std::size_t i=0U;i<original.vertices.size();++i) {
    CHECK(undo_first.constraints.vertices[i].id==original.vertices[i].id);
    CHECK(undo_first.constraints.vertices[i].position.x==
          original.vertices[i].position.x);
    CHECK(undo_first.constraints.vertices[i].position.y==
          original.vertices[i].position.y);
    CHECK(undo_first.constraints.vertices[i].position.z==
          original.vertices[i].position.z);
  }
  CHECK(undo_first.constraints.recovery_journal.empty());
  REQUIRE(undo_first.tetrahedra.size()==1U);
  REQUIRE(inspect_canonical_plc_tetrahedra(
              undo_first.constraints,undo_first.tetrahedra).accepted());
}

TEST_CASE("Wang boundary insertion seeds a crossed mesh edge with its complete shell") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{0,0,-1}},{20,{0,0,1}},
                        {30,{-1,0,0}},{40,{1,0,0}},
                        {50,{0,1,0}},{60,{0,-1,0}}};
  const auto facet=[&](std::array<std::uint64_t,3> vertices) {
    CanonicalPlcConstraintFacet result;
    result.vertices=vertices;
    result.source_vertices=vertices;
    result.parent.vertex_ids=vertices;
    std::sort(result.parent.vertex_ids.begin(),result.parent.vertex_ids.end());
    for(unsigned corner=0U;corner<3U;++corner) {
      const auto position=std::find(result.parent.vertex_ids.begin(),
                                    result.parent.vertex_ids.end(),vertices[corner]);
      result.corners[corner].denominator=1U;
      result.corners[corner].numerator[static_cast<std::size_t>(
          position-result.parent.vertex_ids.begin())]=1U;
    }
    return result;
  };
  constraints.facets={facet({10,20,30}),facet({20,10,40})};
  const std::vector<std::array<std::uint32_t,4>> mesh{
      {{2,3,0,4}},{{2,3,4,1}},{{2,3,1,5}},{{2,3,5,0}}};

  const auto inserted=insert_wang_segment_boundary_steiner_point(
      constraints,{10,20},mesh,16U,32U);
  INFO("failure="<<static_cast<unsigned>(inserted.failure)
       <<" insertion="<<static_cast<unsigned>(inserted.insertion_failure));
  REQUIRE(inserted.accepted());
  CHECK_FALSE(inserted.used_midpoint_fallback);
  CHECK(inserted.intersected_mesh_edge==
        std::array<std::uint64_t,2>{{30,40}});
  CHECK(inserted.intersected_mesh_face==std::array<std::uint64_t,3>{});
  CHECK(inserted.split_numerator==1U);
  CHECK(inserted.split_denominator==2U);
  REQUIRE(inserted.bowyer_watson_seed_tetrahedra.size()==mesh.size());
  for(const auto& seed:mesh) {
    CHECK(std::find(inserted.bowyer_watson_seed_tetrahedra.begin(),
                    inserted.bowyer_watson_seed_tetrahedra.end(),seed)!=
          inserted.bowyer_watson_seed_tetrahedra.end());
    CHECK(std::find(inserted.bowyer_watson_cavity.begin(),
                    inserted.bowyer_watson_cavity.end(),seed)!=
          inserted.bowyer_watson_cavity.end());
  }
}

TEST_CASE("Wang boundary insertion uses the pinned midpoint retry when intersection provenance overflows") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{0,0,-1}},{20,{0,0,1}},
                        {30,{-1,-1,.25}},{40,{1,-1,.25}},{50,{0,1,.25}}};
  constexpr std::uint64_t denominator=(std::uint64_t{1}<<62U)-1U;
  const auto append_facet=[&](std::array<std::uint64_t,3> vertices,
                              std::array<std::uint64_t,3> parent_ids) {
    const auto a=constraints.vertices[vertices[0]==10?0U:1U].position;
    const auto b=constraints.vertices[vertices[0]==10?1U:0U].position;
    const auto c=constraints.vertices[vertices[2]==30?2U:4U].position;
    const auto scale=static_cast<double>(denominator);
    const auto p1=(b*scale-c)/static_cast<double>(denominator-1U);
    const auto p0=(a*scale-p1)/static_cast<double>(denominator-1U);
    constraints.vertices.push_back({parent_ids[0],p0});
    constraints.vertices.push_back({parent_ids[1],p1});
    constraints.vertices.push_back({parent_ids[2],c});
    CanonicalPlcConstraintFacet facet;
    facet.vertices=vertices;
    facet.source_vertices=vertices;
    facet.parent.vertex_ids=parent_ids;
    const FacetBarycentricPoint near_p0{{denominator-1U,1U,0U},denominator};
    const FacetBarycentricPoint near_p1{{0U,denominator-1U,1U},denominator};
    const FacetBarycentricPoint p2{{0U,0U,1U},1U};
    facet.corners=vertices[0]==10?
        std::array<FacetBarycentricPoint,3>{{near_p0,near_p1,p2}}:
        std::array<FacetBarycentricPoint,3>{{near_p1,near_p0,p2}};
    constraints.facets.push_back(facet);
  };
  append_facet({10,20,30},{100,101,102});
  append_facet({20,10,50},{110,111,112});
  const std::vector<std::array<std::uint32_t,4>> mesh{
      {{0,2,3,4}},{{1,2,4,3}}};

  const auto direct=split_canonical_plc_constraint_edge_at_ratio(
      constraints,{10,20},5U,8U,32U,32U);
  REQUIRE_FALSE(direct.accepted());
  CHECK(direct.failure==CanonicalPlcConstraintFailure::rational_overflow);

  const auto inserted=insert_wang_segment_boundary_steiner_point(
      constraints,{10,20},mesh,32U,32U);
  INFO("failure="<<static_cast<unsigned>(inserted.failure)
       <<" constraint="<<static_cast<unsigned>(inserted.constraint_failure)
       <<" insertion="<<static_cast<unsigned>(inserted.insertion_failure));
  REQUIRE(inserted.accepted());
  CHECK(inserted.used_midpoint_fallback);
  CHECK(inserted.split_numerator==1U);
  CHECK(inserted.split_denominator==2U);
  CHECK(inserted.constraints.split_vertices.back().numerator==1U);
  CHECK(inserted.constraints.split_vertices.back().denominator==2U);
}

TEST_CASE("Wang Bowyer-Watson growth stops at a recovered constraint facet") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{0,0,-1}},{20,{0,0,1}},
                        {30,{-1,-1,.25}},{40,{1,-1,.25}},
                        {50,{0,1,.25}},{60,{0,-1,-1}}};
  const auto facet=[&](std::array<std::uint64_t,3> vertices) {
    CanonicalPlcConstraintFacet result;
    result.vertices=vertices;
    result.source_vertices=vertices;
    result.parent.vertex_ids=vertices;
    std::sort(result.parent.vertex_ids.begin(),result.parent.vertex_ids.end());
    for(unsigned corner=0U;corner<3U;++corner) {
      const auto position=std::find(result.parent.vertex_ids.begin(),
                                    result.parent.vertex_ids.end(),vertices[corner]);
      result.corners[corner].denominator=1U;
      result.corners[corner].numerator[static_cast<std::size_t>(
          position-result.parent.vertex_ids.begin())]=1U;
    }
    return result;
  };
  constraints.facets={facet({10,20,50}),facet({20,10,40}),
                      facet({10,30,40})};
  const std::vector<std::array<std::uint32_t,4>> mesh{
      {{0,2,3,4}},{{1,2,4,3}},{{5,0,3,2}}};

  auto unprotected=constraints;
  unprotected.facets.pop_back();
  const auto unrestricted=insert_wang_segment_boundary_steiner_point(
      unprotected,{10,20},mesh,16U,32U);
  REQUIRE(unrestricted.accepted());
  CHECK(std::find(unrestricted.bowyer_watson_cavity.begin(),
                  unrestricted.bowyer_watson_cavity.end(),mesh[2])!=
        unrestricted.bowyer_watson_cavity.end());

  const auto inserted=insert_wang_segment_boundary_steiner_point(
      constraints,{10,20},mesh,16U,32U);
  INFO("failure="<<static_cast<unsigned>(inserted.failure)
       <<" insertion="<<static_cast<unsigned>(inserted.insertion_failure));
  REQUIRE(inserted.accepted());
  CHECK(std::find(inserted.bowyer_watson_cavity.begin(),
                  inserted.bowyer_watson_cavity.end(),mesh[2])==
        inserted.bowyer_watson_cavity.end());
  CHECK(inspect_canonical_plc_tetrahedra(
            inserted.constraints,inserted.tetrahedra).accepted());
}

TEST_CASE("generalized face recovery removes an unconstrained reflex edge") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{0,0,-1}},{20,{0,0,1}},
                        {30,{1,0,0}},{40,{0,1,0}},{50,{-1,-1,0}}};
  const std::vector<std::array<std::uint32_t,4>> shell{
      {{0,1,2,3}},{{0,1,3,4}},{{0,1,4,2}}};
  const auto removed=try_remove_mesh_edge_by_flip(
      constraints,{0,1},shell);
  REQUIRE(removed.accepted);
  CHECK(removed.cavity_tetrahedra==shell);
  CHECK(removed.tetrahedra.size()==2U);
  for(const auto& cell:removed.tetrahedra) {
    const auto retains_edge=std::find(cell.begin(),cell.end(),0U)!=cell.end()&&
        std::find(cell.begin(),cell.end(),1U)!=cell.end();
    CHECK_FALSE(retains_edge);
  }

  auto reversed=shell;std::reverse(reversed.begin(),reversed.end());
  const auto reordered=try_remove_mesh_edge_by_flip(
      constraints,{1,0},reversed);
  REQUIRE(reordered.accepted);
  CHECK(reordered.tetrahedra==removed.tetrahedra);

  CanonicalPlcConstraintFacet protected_face;
  protected_face.vertices={10,20,30};
  constraints.facets.push_back(protected_face);
  const auto protected_result=try_remove_mesh_edge_by_flip(
      constraints,{0,1},shell);
  CHECK_FALSE(protected_result.accepted);
  CHECK(protected_result.failure==
        CanonicalLiteralEdgeFlipFailure::frozen_cavity_boundary);
}

TEST_CASE("Wang facet local flips remove a transverse free edge deterministically") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{-1,-.6,0}},{20,{1,-.6,0}},
                        {30,{0,1.2,0}},{40,{0,0,.2}},{50,{0,0,-.2}}};
  CanonicalPlcConstraintFacet target;
  target.parent={{10,20,30}};
  target.vertices={{10,20,30}};
  target.source_vertices=target.vertices;
  target.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  constraints.facets={target};
  const std::vector<std::array<std::uint32_t,4>> mesh{
      {{3,4,0,1}},{{3,4,1,2}},{{3,4,2,0}}};

  const auto recovered=try_recover_wang_facet_by_local_flips(
      constraints,{30,10,20},mesh);
  REQUIRE(recovered.facet_recovered);
  CHECK(recovered.preserved_recovered_constraints);
  CHECK(recovered.intersecting_edges==1U);
  CHECK(recovered.edge_removal_attempts==1U);
  CHECK(recovered.edge_removals==1U);
  CHECK(recovered.edge_retriangulation_trials>=1U);
  CHECK(inspect_canonical_plc_tetrahedra(
            constraints,recovered.tetrahedra).accepted());

  auto reversed=mesh;
  std::reverse(reversed.begin(),reversed.end());
  const auto reordered=try_recover_wang_facet_by_local_flips(
      constraints,{20,30,10},reversed);
  REQUIRE(reordered.facet_recovered);
  CHECK(reordered.tetrahedra==recovered.tetrahedra);
  CHECK(reordered.edge_removal_attempts==recovered.edge_removal_attempts);
  CHECK(reordered.edge_retriangulation_trials==
        recovered.edge_retriangulation_trials);
}

TEST_CASE("Wang facet interior insertion uses the reference residual crossing placement") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{-1,-.6,0}},{20,{1,-.6,0}},
                        {30,{0,1.2,0}},{40,{0,0,.2}},{50,{0,0,-.2}}};
  CanonicalPlcConstraintFacet target;
  target.parent={{10,20,30}};
  target.vertices={{10,20,30}};
  target.source_vertices=target.vertices;
  target.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  constraints.facets={target};
  const std::vector<std::array<std::uint32_t,4>> mesh{
      {{3,4,0,1}},{{3,4,1,2}},{{3,4,2,0}}};

  const auto inserted=insert_wang_facet_interior_points(
      constraints,{30,10,20},mesh);
  INFO("failure="<<static_cast<unsigned>(inserted.failure));
  REQUIRE(inserted.changed());
  CHECK(inserted.failure==WangFacetInteriorInsertionFailure::none);
  CHECK(inserted.residual_mesh_edge==
        std::array<std::uint64_t,2>{{40,50}});
  CHECK(inserted.residual_intersection.x==doctest::Approx(0.0));
  CHECK(inserted.residual_intersection.y==doctest::Approx(0.0));
  CHECK(inserted.residual_intersection.z==doctest::Approx(0.0));
  CHECK(inserted.blended_base.x==doctest::Approx(0.0));
  CHECK(inserted.blended_base.y==doctest::Approx(0.0));
  CHECK(inserted.blended_base.z==doctest::Approx(0.0));
  CHECK(inserted.signed_heights[0]==doctest::Approx(.2L));
  CHECK(inserted.signed_heights[1]==doctest::Approx(-.2L));
  CHECK(inserted.inserted_points.front().z==doctest::Approx(.1));
  CHECK(inserted.constraints.vertices.size()==
        constraints.vertices.size()+inserted.inserted_points.size());
  REQUIRE(inserted.constraints.interior_steiner_vertices.size()==
          inserted.inserted_points.size());
  for(const auto& vertex:inserted.constraints.interior_steiner_vertices)
    CHECK(vertex.kind==CanonicalInteriorSteinerKind::facet_interior);
  CHECK(inserted.constraints.recovery_journal.empty());
  CHECK(inserted.bowyer_watson_attempts>=inserted.inserted_points.size());
  if(inserted.facet_recovered)
    CHECK(inspect_canonical_plc_tetrahedra(
              inserted.constraints,inserted.tetrahedra).accepted());
}

TEST_CASE("Wang facet interior insertion commits in the live ordered mesh") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{-1,-.6,0}},{20,{1,-.6,0}},
                        {30,{0,1.2,0}},{40,{0,0,.2}},{50,{0,0,-.2}}};
  CanonicalPlcConstraintFacet target;
  target.parent={{10,20,30}};
  target.vertices={{10,20,30}};
  target.source_vertices=target.vertices;
  target.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  constraints.facets={target};
  const std::vector<std::array<std::uint32_t,4>> cells{
      {{3,4,0,1}},{{3,4,1,2}},{{3,4,2,0}}};
  WangOrderedTetMesh ordered(constraints.vertices.size(),cells);
  REQUIRE(ordered.audit().accepted());

  const auto inserted=insert_wang_facet_interior_points(
      constraints,{10,20,30},cells,1000U,&ordered);
  INFO("failure="<<static_cast<unsigned>(inserted.failure));
  REQUIRE(inserted.changed());
  CHECK(inserted.constraints.recovery_journal.empty());
  CHECK(ordered.vertex_count()==constraints.vertices.size()+
        inserted.inserted_points.size());
  CHECK(ordered.audit().accepted());
  std::size_t live{};
  for(const auto& cell:ordered.cells())if(!cell.deleted)++live;
  CHECK(live==inserted.tetrahedra.size());
}

TEST_CASE("Wang facet insertion accepts the captured real author fallback patch") {
  using namespace tetra::probes;
  std::ifstream input("tests/fixtures/wang/a320_first_facet_patch.txt");
  if(!input)input.open("../tests/fixtures/wang/a320_first_facet_patch.txt");
  REQUIRE(input.good());
  std::array<std::uint64_t,3> target{};
  std::map<std::uint64_t,tetra::Vec3> positions;
  std::vector<std::array<std::uint64_t,4>> source_cells;
  for(std::string line;std::getline(input,line);) {
    std::istringstream fields(line);std::string tag;fields>>tag;
    if(tag=="facet_patch_target") {
      std::uint64_t ignored{};fields>>ignored>>target[0]>>target[1]>>target[2];
    } else if(tag=="facet_patch_node") {
      std::uint64_t id{};tetra::Vec3 point{};std::string ignored;
      fields>>id>>point.x>>point.y>>point.z>>ignored>>ignored;
      positions.emplace(id,point);
    } else if(tag=="facet_patch_cell") {
      std::uint64_t ignored{};std::array<std::uint64_t,4> cell{};
      fields>>ignored>>cell[0]>>cell[1]>>cell[2]>>cell[3];
      source_cells.push_back(cell);
    }
  }
  REQUIRE(positions.size()==65U);
  REQUIRE(source_cells.size()==155U);
  CanonicalPlcConstraintSet constraints;
  std::map<std::uint64_t,std::uint32_t> index;
  for(const auto& [id,position]:positions) {
    index.emplace(id,static_cast<std::uint32_t>(constraints.vertices.size()));
    constraints.vertices.push_back(FrozenFacetVertex{id,position});
  }
  CanonicalPlcConstraintFacet facet;
  facet.parent={{target[0],target[1],target[2]}};
  facet.vertices=target;facet.source_vertices=target;
  facet.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  constraints.facets.push_back(facet);
  std::vector<std::array<std::uint32_t,4>> cells;
  for(const auto& source:source_cells) {
    std::array<std::uint32_t,4> cell{};
    for(unsigned corner=0;corner<4U;++corner)cell[corner]=index.at(source[corner]);
    cells.push_back(cell);
  }
  WangOrderedTetMesh ordered(constraints.vertices.size(),cells);
  REQUIRE(ordered.audit().accepted());
  const auto before=inspect_canonical_plc_tetrahedra(constraints,cells);
  REQUIRE(before.missing_facets.size()==1U);
  auto expected_target=target,reported_target=before.missing_facets.front();
  std::sort(expected_target.begin(),expected_target.end());
  std::sort(reported_target.begin(),reported_target.end());
  CHECK(reported_target==expected_target);
  const auto inserted=insert_wang_facet_interior_points(
      constraints,target,cells,1000U,&ordered);
  INFO("failure="<<static_cast<unsigned>(inserted.failure));
  REQUIRE(inserted.changed());
  CHECK(inserted.signed_heights[0]==doctest::Approx(20.310625681727828));
  CHECK(inserted.signed_heights[1]==doctest::Approx(-16.913627306761228));
  CHECK(inserted.blended_base.x==doctest::Approx(-18788.70724492698));
  CHECK(inserted.blended_base.y==doctest::Approx(-2004.9016543414623));
  CHECK(inserted.blended_base.z==doctest::Approx(1526.8686831702696));
  CHECK(inserted.facet_recovered);
  CHECK(inserted.constraints.recovery_journal.empty());
  CHECK(inspect_canonical_plc_tetrahedra(
      inserted.constraints,inserted.tetrahedra).accepted());
  REQUIRE(inserted.constraints.interior_steiner_vertices.size()==
          inserted.inserted_points.size());
  for(const auto& point:inserted.constraints.interior_steiner_vertices)
    CHECK(point.kind==CanonicalInteriorSteinerKind::facet_interior);
  CHECK(ordered.audit().accepted());
  CHECK(ordered.vertex_count()==constraints.vertices.size()+
        inserted.inserted_points.size());
}

TEST_CASE("Wang facet boundary insertion splits at the crossed mesh edge shell") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{-1,-.6,0}},{20,{1,-.6,0}},
                        {30,{0,1.2,0}},{40,{0,0,.2}},{50,{0,0,-.2}}};
  CanonicalPlcConstraintFacet target;
  target.parent={{10,20,30}};
  target.vertices={{10,20,30}};
  target.source_vertices=target.vertices;
  target.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  constraints.facets={target};
  const std::vector<std::array<std::uint32_t,4>> mesh{
      {{3,4,0,1}},{{3,4,1,2}},{{3,4,2,0}}};

  const auto inserted=insert_wang_facet_boundary_steiner_point(
      constraints,{30,10,20},mesh,16U,32U);
  INFO("failure="<<static_cast<unsigned>(inserted.failure)
       <<" insertion="<<static_cast<unsigned>(inserted.insertion_failure));
  REQUIRE(inserted.accepted());
  CHECK(inserted.intersecting_mesh_edge==
        std::array<std::uint64_t,2>{{40,50}});
  CHECK(inserted.barycentric[0]+inserted.barycentric[1]+
        inserted.barycentric[2]==inserted.denominator);
  CHECK(inserted.bowyer_watson_seeds.size()==3U);
  for(const auto& seed:mesh)
    CHECK(std::find(inserted.bowyer_watson_cavity.begin(),
                    inserted.bowyer_watson_cavity.end(),seed)!=
          inserted.bowyer_watson_cavity.end());
  CHECK(inserted.constraints.vertices.size()==constraints.vertices.size()+1U);
  CHECK(inserted.constraints.facets.size()==3U);
  CHECK(inserted.constraints.facet_split_vertices.size()==1U);
  REQUIRE(inserted.constraints.recovery_journal.size()==1U);
  CHECK(inserted.constraints.recovery_journal.back().kind==
        CanonicalPlcRecoveryInsertionKind::facet_split);
  CHECK(inspect_canonical_plc_tetrahedra(
            inserted.constraints,inserted.tetrahedra).accepted());
  const auto restored=restore_last_canonical_boundary_steiner_point(
      inserted.constraints,inserted.tetrahedra);
  INFO("restoration failure="<<static_cast<unsigned>(restored.failure));
  REQUIRE(restored.accepted());
  CHECK(restored.restored_points==1U);
  CHECK(restored.constraints.recovery_journal.empty());
  CHECK(restored.constraints.facet_split_vertices.empty());
  REQUIRE(restored.constraints.facets.size()==1U);
  CHECK(restored.constraints.facets.front().vertices==target.vertices);
  CHECK(restored.constraints.facets.front().corners==target.corners);
  CHECK(inspect_canonical_plc_tetrahedra(
            restored.constraints,restored.tetrahedra).accepted());
}

TEST_CASE("generalized edge removal retriangulates a degree-four shell") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{0,0,-1}},{20,{0,0,1}},
                        {30,{1,0,0}},{40,{0,1,0}},
                        {50,{-1,0,0}},{60,{0,-1,0}}};
  const std::vector<std::array<std::uint32_t,4>> shell{
      {{0,1,2,3}},{{0,1,3,4}},{{0,1,4,5}},{{0,1,5,2}}};
  const auto removed=try_remove_mesh_edge_by_flip(constraints,{0,1},shell);
  REQUIRE(removed.accepted);
  CHECK(removed.cavity_tetrahedra==shell);
  CHECK(removed.tetrahedra.size()==4U);
  CHECK(removed.retriangulation_trials>=1U);
  for(const auto& cell:removed.tetrahedra) {
    const auto retains_edge=std::find(cell.begin(),cell.end(),0U)!=cell.end()&&
        std::find(cell.begin(),cell.end(),1U)!=cell.end();
    CHECK_FALSE(retains_edge);
  }
}

TEST_CASE("Wang easy walk removes the exact crossed endpoint-star edge") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{0,0,-1}},{20,{0,0,1}},
                        {30,{1,0,0}},{40,{0,1,0}},
                        {50,{-1,0,0}},{60,{0,-1,0}}};
  const std::vector<std::array<std::uint32_t,4>> shell{
      {{0,1,2,3}},{{0,1,3,4}},{{0,1,4,5}},{{0,1,5,2}}};

  const auto forward=try_recover_wang_segment_by_local_flips(
      constraints,{30,50},shell,WangSegmentFlipSearchMode::easy,false,2U);
  REQUIRE(forward.flip.accepted);
  CHECK(forward.segment_recovered);
  CHECK(forward.edge_removal_attempts==2U);
  CHECK(forward.edge_removals==2U);
  CHECK(forward.maximum_edge_degree==4U);

  const auto reverse=try_recover_wang_segment_by_local_flips(
      constraints,{30,50},shell,WangSegmentFlipSearchMode::easy,true,2U);
  REQUIRE(reverse.flip.accepted);
  CHECK(reverse.segment_recovered);
  CHECK(reverse.edge_removal_attempts==2U);
  CHECK(reverse.edge_removals==2U);
  CHECK(reverse.flip.tetrahedra==forward.flip.tetrahedra);
}

TEST_CASE("Wang easy walk applies removePnt to an intervening interior vertex") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{0,0,0}},{20,{-1,0,0}},{30,{1,0,0}},
                        {40,{0,1,0}},{50,{0,-1,0}},
                        {60,{0,0,1}},{70,{0,0,-1}}};
  const std::vector<std::array<std::uint32_t,4>> mesh{
      {{0,1,3,5}},{{0,1,5,4}},{{0,1,4,6}},{{0,1,6,3}},
      {{0,2,5,3}},{{0,2,4,5}},{{0,2,6,4}},{{0,2,3,6}}};

  const auto forward=try_recover_wang_segment_by_local_flips(
      constraints,{20,30},mesh,WangSegmentFlipSearchMode::easy,false,1U);
  REQUIRE(forward.flip.accepted);
  CHECK(forward.segment_recovered);
  CHECK(forward.edge_removal_attempts==0U);
  CHECK(std::none_of(forward.flip.tetrahedra.begin(),
                     forward.flip.tetrahedra.end(),[](const auto& cell) {
        return std::find(cell.begin(),cell.end(),0U)!=cell.end();
      }));

  const auto reverse=try_recover_wang_segment_by_local_flips(
      constraints,{20,30},mesh,WangSegmentFlipSearchMode::easy,true,1U);
  REQUIRE(reverse.flip.accepted);
  CHECK(reverse.segment_recovered);
  CHECK(reverse.flip.tetrahedra==forward.flip.tetrahedra);
}

TEST_CASE("Wang segment transaction commits a valid generalized edge removal") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={
      {10,{0,0,-1}},{20,{0,0,1}},
      {30,{1.6932440365170365,0.39788512496864009,0.13598397344799062}},
      {40,{0.17991280687483299,0.76190489876133471,-0.00082821534642887884}},
      {50,{-0.79037265333992235,0.088216648811087248,0.14418082384165853}},
      {60,{0.28745660125382844,-1.1899235556914092,-0.080632317727999031}},
      {70,{-0.88183703334763064,-0.77895362633847043,1.2985664506475771}},
      {80,{0.66809071446188373,0.081087101349609725,0.35769529632524488}}};
  const std::vector<std::array<std::uint32_t,4>> mesh{
      {{0,1,2,3}},{{0,1,3,4}},{{0,1,4,5}},{{0,1,5,2}}};

  const auto recovered=try_recover_wang_segment_by_local_flips(
      constraints,{70,80},mesh);
  REQUIRE(recovered.flip.accepted);
  CHECK(recovered.edge_removal_attempts==1U);
  CHECK(recovered.edge_removals==1U);
  CHECK(recovered.edge_retriangulation_trials>=1U);
  CHECK(recovered.maximum_edge_degree==4U);
  CHECK(recovered.preserved_recovered_constraints);
  CHECK_FALSE(recovered.segment_recovered);
  CHECK(recovered.flip.tetrahedra.size()==5U);

  auto reversed=mesh;
  std::reverse(reversed.begin(),reversed.end());
  const auto reordered=try_recover_wang_segment_by_local_flips(
      constraints,{80,70},reversed);
  REQUIRE(reordered.flip.accepted);
  CHECK(reordered.edge_removals==1U);
  CHECK(reordered.flip.tetrahedra==recovered.flip.tetrahedra);
}

TEST_CASE("forced cavity insertion preserves the complete cavity boundary") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{0,0,0}},{20,{2,0,0}},{30,{0,2,0}},
                        {40,{0,0,2}},{50,{.25,.25,.25}}};
  const std::vector<std::array<std::uint32_t,4>> mesh{{{0,1,2,3}}};
  const auto inserted=insert_forced_cavity_vertex(
      constraints,4U,mesh,mesh);
  REQUIRE(inserted.accepted);
  CHECK(inserted.cavity_tetrahedra==mesh);
  CHECK(inserted.tetrahedra.size()==4U);
  for(const auto& cell:inserted.tetrahedra)
    CHECK(std::find(cell.begin(),cell.end(),4U)!=cell.end());
}

TEST_CASE("forced cavity insertion cannot destroy a recovered constrained segment") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{0,0,1}},{20,{0,0,-1}},{30,{-1,-1,0}},
                        {40,{1,-1,0}},{50,{0,1,0}},{60,{.1,0,0}}};
  CanonicalPlcConstraintFacet facet;facet.vertices={10,20,60};
  facet.parent={facet.vertices};facet.source_vertices=facet.vertices;
  facet.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  constraints.facets={facet};
  const std::vector<std::array<std::uint32_t,4>> mesh{
      {{0,1,2,3}},{{0,1,3,4}},{{0,1,4,2}}};
  const auto inserted=insert_forced_cavity_vertex(
      constraints,5U,mesh,mesh);
  CHECK_FALSE(inserted.accepted);
  CHECK(inserted.failure==CanonicalLiteralEdgeFlipFailure::changed_boundary);
}

TEST_CASE("Wang info-3 Bowyer-Watson adjustment preserves an enclosed recovered edge") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10,{0,0,1}},{20,{0,0,-1}},{30,{-1,-1,0}},
                        {40,{1,-1,0}},{50,{0,1,0}},{60,{.1,-.25,.1}}};
  CanonicalPlcConstraintFacet facet;facet.vertices={10,20,60};
  facet.parent={facet.vertices};facet.source_vertices=facet.vertices;
  facet.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  constraints.facets={facet};
  const std::vector<std::array<std::uint32_t,4>> mesh{
      {{0,1,2,3}},{{0,1,3,4}},{{0,1,4,2}}};

  const auto inserted=insert_wang_constrained_bowyer_watson_vertex(
      constraints,5U,mesh,mesh);

  INFO("failure="<<static_cast<unsigned>(inserted.failure)
       <<" cavity="<<inserted.cavity_tetrahedra.size());
  REQUIRE(inserted.accepted);
  CHECK(inserted.cavity_tetrahedra.size()==2U);
  CHECK(std::any_of(inserted.tetrahedra.begin(),inserted.tetrahedra.end(),
      [](const auto& cell) {
        return std::find(cell.begin(),cell.end(),0U)!=cell.end()&&
               std::find(cell.begin(),cell.end(),1U)!=cell.end();
      }));
  CHECK(inspect_canonical_plc_tetrahedra(
            constraints,inserted.tetrahedra).missing_edges.empty());
}

TEST_CASE("Cascade-FHC insertion removes a blocking edge without refining the PLC") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constexpr double root3=1.7320508075688772935;
  constraints.vertices={
      {100,{0,0,-1}},{200,{0,0,1}},
      {10,{2,0,0}},{20,{1,root3,0}},{30,{-1,root3,0}},
      {40,{-2,0,0}},{50,{-1,-root3,0}},{60,{1,-root3,0}},
      {70,{-3,-.4,0}},{80,{3,.4,0}}};
  CanonicalPlcConstraintFacet protected_face;
  protected_face.vertices={100,10,20};
  protected_face.parent={protected_face.vertices};
  protected_face.source_vertices=protected_face.vertices;
  protected_face.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  constraints.facets={protected_face};
  std::vector<std::array<std::uint32_t,4>> mesh;
  for(std::uint32_t i=0U;i<6U;++i)
    mesh.push_back({0U,1U,2U+i,2U+(i+1U)%6U});

  const auto classification=classify_wang_cascade_fhc(
      constraints,{70,80},mesh);
  INFO("classification="<<static_cast<unsigned>(classification.failure)
       <<" valid="<<classification.valid_removal_flips
       <<" cascading="<<classification.cascading_removal_flips);
  CHECK(classification.classified());
  CHECK(classification.main_intersecting_edge==
        std::array<std::uint64_t,2>{{100,200}});
  CHECK(classification.valid_removal_flips==14U);
  CHECK(classification.cascading_removal_flips==14U);
  CHECK(classification.exact_s0.x==doctest::Approx(0.0));
  CHECK(classification.exact_s0.y==doctest::Approx(0.0));
  CHECK(classification.exact_s0.z==doctest::Approx(0.0));
  CHECK(classification.endpoint_b==200U);
  CHECK(classification.midpoint.x==doctest::Approx(0.0));
  CHECK(classification.midpoint.y==doctest::Approx(0.0));
  CHECK(classification.midpoint.z==doctest::Approx(0.5));
  const auto h=std::find_if(constraints.vertices.begin(),constraints.vertices.end(),
      [&](const auto& vertex){return vertex.id==classification.associated_h;});
  REQUIRE(h!=constraints.vertices.end());
  const auto segment_start=constraints.vertices[8].position;
  const auto h_offset=h->position-segment_start;
  CHECK(classification.oriented_normal.x*h_offset.x+
        classification.oriented_normal.y*h_offset.y+
        classification.oriented_normal.z*h_offset.z<0.0);

  const auto inserted=insert_cascade_fhc_vertex(
      constraints,{70,80},{100,200},mesh);
  INFO("failure="<<static_cast<unsigned>(inserted.failure)
       <<" cavity="<<inserted.cavity_tetrahedra.size());
  REQUIRE(inserted.accepted);
  REQUIRE(inserted.inserted_steiner_vertex.has_value());
  CHECK(inserted.cavity_tetrahedra.size()==6U);
  CHECK(inserted.tetrahedra.size()==12U);
  CHECK(constraints.vertices.size()==10U);
  const auto displacement=*inserted.inserted_steiner_vertex-classification.midpoint;
  CHECK(displacement.x*classification.oriented_normal.x+
        displacement.y*classification.oriented_normal.y+
        displacement.z*classification.oriented_normal.z>0.0);
  CHECK(inserted.retriangulation_trials<=16U);
  for(const auto& cell:inserted.tetrahedra) {
    CHECK(std::find(cell.begin(),cell.end(),10U)!=cell.end());
    const bool retains_blocking_edge=
        std::find(cell.begin(),cell.end(),0U)!=cell.end()&&
        std::find(cell.begin(),cell.end(),1U)!=cell.end();
    CHECK_FALSE(retains_blocking_edge);
  }
  auto augmented=constraints;
  augmented.vertices.push_back({201,*inserted.inserted_steiner_vertex});
  const auto easy_forward=try_recover_wang_segment_by_local_flips(
      augmented,{70,80},inserted.tetrahedra,WangSegmentFlipSearchMode::easy,
      false,1000U);
  const auto easy_reverse=try_recover_wang_segment_by_local_flips(
      augmented,{70,80},inserted.tetrahedra,WangSegmentFlipSearchMode::easy,
      true,1000U);
  CHECK(easy_forward.flip.accepted);
  CHECK(easy_reverse.flip.accepted);
  const auto enabled=try_recover_wang_segment_by_local_flips(
      augmented,{70,80},inserted.tetrahedra);
  CHECK(enabled.flip.accepted);
  CHECK(enabled.preserved_recovered_constraints);
  const auto after_one_flip=classify_wang_cascade_fhc(
      augmented,{70,80},enabled.flip.tetrahedra);
  const bool same_cascade=after_one_flip.classified()&&
      after_one_flip.main_intersecting_edge==classification.main_intersecting_edge;
  CHECK_FALSE(same_cascade);
  const auto before=inspect_canonical_plc_tetrahedra(constraints,mesh);
  const auto after=inspect_canonical_plc_tetrahedra(augmented,inserted.tetrahedra);
  CHECK(before.missing_edges==after.missing_edges);
  CHECK(before.missing_facets==after.missing_facets);

  auto reversed=mesh;
  std::reverse(reversed.begin(),reversed.end());
  const auto reordered_classification=classify_wang_cascade_fhc(
      constraints,{80,70},reversed);
  REQUIRE(reordered_classification.classified());
  CHECK(reordered_classification.main_intersecting_edge==
        classification.main_intersecting_edge);
  CHECK(reordered_classification.associated_h==classification.associated_h);
  CHECK(reordered_classification.exact_s0.x==classification.exact_s0.x);
  CHECK(reordered_classification.exact_s0.y==classification.exact_s0.y);
  CHECK(reordered_classification.exact_s0.z==classification.exact_s0.z);
  CHECK(reordered_classification.oriented_normal.x==
        classification.oriented_normal.x);
  CHECK(reordered_classification.oriented_normal.y==
        classification.oriented_normal.y);
  CHECK(reordered_classification.oriented_normal.z==
        classification.oriented_normal.z);
  const auto reordered=insert_cascade_fhc_vertex(
      constraints,{80,70},{200,100},reversed);
  REQUIRE(reordered.accepted);
  REQUIRE(reordered.inserted_steiner_vertex.has_value());
  CHECK(reordered.inserted_steiner_vertex->x==inserted.inserted_steiner_vertex->x);
  CHECK(reordered.inserted_steiner_vertex->y==inserted.inserted_steiner_vertex->y);
  CHECK(reordered.inserted_steiner_vertex->z==inserted.inserted_steiner_vertex->z);
  CHECK(reordered.tetrahedra==inserted.tetrahedra);
}

TEST_CASE("Cascade-FHC classification rejects a crossed edge with a noncascading flip") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constexpr double root3=1.7320508075688772935;
  constraints.vertices={
      {100,{0,0,-1}},{200,{0,0,1}},
      {10,{2,0,0}},{20,{1,root3,0}},{30,{-1,root3,0}},
      {40,{-2,0,0}},{50,{-1,-root3,0}},{60,{1,-root3,0}}};
  std::vector<std::array<std::uint32_t,4>> mesh;
  for(std::uint32_t i=0U;i<6U;++i)
    mesh.push_back({0U,1U,2U+i,2U+(i+1U)%6U});

  const auto classification=classify_wang_cascade_fhc(
      constraints,{10,40},mesh);
  CHECK_FALSE(classification.classified());
  CHECK(classification.failure==WangCascadeFhcFailure::flip_does_not_cascade);
  CHECK(classification.valid_removal_flips==14U);
  CHECK(classification.cascading_removal_flips<
        classification.valid_removal_flips);
  const auto insertion=insert_cascade_fhc_vertex(
      constraints,{10,40},{100,200},mesh);
  CHECK_FALSE(insertion.accepted);
}

TEST_CASE("Cascade-FHC S0 is not taken from the rounded post-stall key") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constexpr double root3=1.7320508075688772935;
  constraints.vertices={
      {100,{0,0,-1}},{200,{0,0,1}},
      {10,{2,0,0}},{20,{1,root3,0}},{30,{-1,root3,0}},
      {40,{-2,0,0}},{50,{-1,-root3,0}},{60,{1,-root3,0}},
      {70,{-4,-.4,-.1}},{80,{2,.2,.35}}};
  std::vector<std::array<std::uint32_t,4>> mesh;
  for(std::uint32_t i=0U;i<6U;++i)
    mesh.push_back({0U,1U,2U+i,2U+(i+1U)%6U});

  const auto classification=classify_wang_cascade_fhc(
      constraints,{70,80},mesh);
  INFO("failure="<<static_cast<unsigned>(classification.failure)
       <<" edge="<<classification.main_intersecting_edge[0]<<","<<classification.main_intersecting_edge[1]
       <<" valid="<<classification.valid_removal_flips
       <<" cascade="<<classification.cascading_removal_flips);
  REQUIRE(classification.classified());
  CHECK(classification.exact_s0.x==doctest::Approx(0.0).epsilon(1e-14));
  CHECK(classification.exact_s0.y==doctest::Approx(0.0).epsilon(1e-14));
  CHECK(classification.exact_s0.z==doctest::Approx(0.2).epsilon(1e-14));
  CHECK(classification.endpoint_b==100U);
  CHECK(classification.midpoint.z==doctest::Approx(-0.4).epsilon(1e-14));
}

TEST_CASE("production FHC scheduler selects the earlier mixed candidate") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constexpr double root3=1.7320508075688772935;
  constraints.vertices={
      {100,{0,0,-1}},{200,{0,0,1}},
      {10,{2,0,0}},{20,{1,root3,0}},{30,{-1,root3,0}},
      {40,{-2,0,0}},{50,{-1,-root3,0}},{60,{1,-root3,0}},
      {70,{-5,-.625,0}},{80,{3,.375,0}},
      {110,{-2.3395617283950618,-.29801234567901236,-.0074814814814814822}},
      {120,{-2.3246234567901234,-.25702469135802469,-.0089629629629629642}},
      {130,{-2.3196234567901235,-.29702469135802467,.03103703703703704}},
      {140,{-2.3859197530864198,-.29628395061728396,-.00007407407407407418}},
      {150,{-2.2548395061728397,-.25356790123456791,.025851851851851855}},
      {180,{-2.3420462962962962,-.31825925925925924,.0028888888888888888}}};
  const auto facet=[](std::array<std::uint64_t,3> vertices) {
    CanonicalPlcConstraintFacet result;
    result.vertices=vertices;result.parent={vertices};result.source_vertices=vertices;
    result.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
    return result;
  };
  constraints.facets={facet({100,10,20}),facet({120,130,140}),
                      facet({70,80,180})};
  std::vector<std::array<std::uint32_t,4>> mesh;
  for(std::uint32_t i=0U;i<6U;++i)
    mesh.push_back({0U,1U,2U+i,2U+(i+1U)%6U});
  mesh.insert(mesh.end(),{{{13,10,11,12}},{{14,10,12,11}},
                          {{13,10,12,15}},{{14,10,15,12}}});

  const auto schedule=schedule_wang_fhc_candidates(constraints,{70,80},mesh);
  const auto& cascade=schedule.cascade;
  const auto& locked=schedule.locked;
  INFO("cascade="<<static_cast<unsigned>(cascade.failure)
       <<" edge="<<cascade.main_intersecting_edge[0]<<','
       <<cascade.main_intersecting_edge[1]
       <<" valid="<<cascade.valid_removal_flips
       <<" cascading="<<cascade.cascading_removal_flips
       <<" locked="<<static_cast<unsigned>(locked.failure));
  REQUIRE(cascade.classified());
  REQUIRE(locked.classified());
  CHECK(cascade.main_intersecting_edge==
        std::array<std::uint64_t,2>{{100,200}});
  CHECK(locked.locking_constraint_edge==
        std::array<std::uint64_t,2>{{120,130}});
  CHECK(locked.exact_s0.x<cascade.exact_s0.x);
  CHECK(schedule.first==WangFhcCandidateKind::locked);
  const auto inserted=insert_locked_fhc_vertex(
      constraints,{70,80},locked.locking_mesh_edge,mesh);
  REQUIRE(inserted.accepted);

  std::reverse(mesh.begin(),mesh.end());
  const auto reversed=schedule_wang_fhc_candidates(constraints,{80,70},mesh);
  REQUIRE(reversed.cascade.classified());
  REQUIRE(reversed.locked.classified());
  CHECK(reversed.first==WangFhcCandidateKind::locked);
  CHECK(reversed.cascade.main_intersecting_edge==
        cascade.main_intersecting_edge);
  CHECK(reversed.locked.intersecting_face==locked.intersecting_face);
}

TEST_CASE("two-sided Delaunay recovery expands a concave half cavity") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={
      {100,{-0.45487372843982854,-0.42420532728476723,-0.05369288521069171}},
      {101,{-0.62044504499221831,-0.5510834800407256,0.10441419277319164}},
      {102,{-0.3730777744049607,0.64467120559114877,0.073920638477805722}},
      {103,{0.50454934101294091,0.53644941691779602,-0.6605219323657725}},
      {104,{-0.11718542584294789,0.25517462301745275,-0.013150108483797496}},
      {105,{-0.095751731652848227,0.99872614440910823,0.83094447276654826}},
      {106,{0.42043985937454509,-0.34532582601055273,0.19037663408732941}},
      {107,{-0.3832842867420605,-0.52664346915336435,0.05637657095306059}},
      {108,{0.5065557973963648,-0.091896384145863763,-0.30949563424578286}}};
  CanonicalPlcConstraintFacet face;face.parent={{105,107,108}};
  face.vertices={{105,107,108}};face.source_vertices=face.vertices;
  face.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
  constraints.facets={face};
  const std::vector<std::array<std::uint32_t,4>> mesh{
      {{1,0,2,4}},{{1,0,4,7}},{{2,0,3,4}},{{0,3,4,8}},{{4,0,6,7}},
      {{0,4,6,8}},{{0,6,7,8}},{{2,1,4,5}},{{1,4,5,7}},{{1,5,6,7}},
      {{3,2,4,5}},{{4,3,5,8}},{{5,3,6,8}},{{5,4,6,7}},{{4,5,6,8}}};

  const auto recovered=recover_literal_facet_by_two_sided_cavity(
      constraints,face.vertices,mesh,32U,200000U,32U);
  INFO("failure="<<static_cast<unsigned int>(recovered.failure)
       <<" intersected="<<recovered.intersected_tetrahedra
       <<" top="<<recovered.top_tetrahedra<<" bottom="<<recovered.bottom_tetrahedra
       <<" trials="<<recovered.retriangulation_trials
       <<" expansions="<<recovered.cavity_expansions
       <<" old6="<<recovered.original_cavity_six_volume
       <<" new6="<<recovered.replacement_six_volume);
  REQUIRE(recovered.accepted);
  CHECK(recovered.used_local_delaunay);
  CHECK(recovered.cavity_expansions==2U);
  CHECK(inspect_canonical_plc_tetrahedra(
            constraints,recovered.tetrahedra).accepted());

  const auto capped=recover_literal_facet_by_two_sided_cavity(
      constraints,face.vertices,mesh,32U,200000U,0U);
  CHECK_FALSE(capped.accepted);
  CHECK(capped.failure==CanonicalFacetCavityFailure::expansion_limit);

  auto reversed=mesh;std::reverse(reversed.begin(),reversed.end());
  const auto reordered=recover_literal_facet_by_two_sided_cavity(
      constraints,{108,105,107},reversed,32U,200000U,32U);
  REQUIRE(reordered.accepted);
  CHECK(reordered.cavity_expansions==recovered.cavity_expansions);
  CHECK(reordered.tetrahedra==recovered.tetrahedra);
}

TEST_CASE("Wang AttachPnt2Seg reuses a collinear boundary vertex") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  // The ray from 10 to 20 meets existing boundary vertex 50 before its
  // endpoint.  The two tetrahedra deliberately have the two child segments,
  // but not their coarse parent edge.
  constraints.vertices={
      {10,{0,0,0}},{20,{2,0,0}},{30,{0,1,0}},{40,{0,0,1}},
      {50,{1,0,0}}};
  const auto facet=[](std::array<std::uint64_t,3> vertices) {
    CanonicalPlcConstraintFacet value;
    value.vertices=vertices;
    value.source_vertices=vertices;
    value.parent.vertex_ids=vertices;
    std::sort(value.parent.vertex_ids.begin(),value.parent.vertex_ids.end());
    for(unsigned corner=0U;corner<3U;++corner) {
      const auto position=std::find(value.parent.vertex_ids.begin(),
          value.parent.vertex_ids.end(),vertices[corner]);
      value.corners[corner].denominator=1U;
      value.corners[corner].numerator[static_cast<std::size_t>(
          position-value.parent.vertex_ids.begin())]=1U;
    }
    return value;
  };
  constraints.facets={facet({10,20,30}),facet({20,10,40}),
                      facet({50,30,40})};
  const std::vector<std::array<std::uint32_t,4>> mesh{{
      {{0,4,2,3}},{{4,1,2,3}}}};

  const auto attached=attach_canonical_plc_boundary_vertex_to_segment(
      constraints,{10,20},50U);
  REQUIRE(attached.accepted());
  REQUIRE(attached.constraints.vertices.size()==constraints.vertices.size());
  for(std::size_t index=0U;index<constraints.vertices.size();++index) {
    CHECK(attached.constraints.vertices[index].id==constraints.vertices[index].id);
    CHECK(attached.constraints.vertices[index].position.x==
          constraints.vertices[index].position.x);
    CHECK(attached.constraints.vertices[index].position.y==
          constraints.vertices[index].position.y);
    CHECK(attached.constraints.vertices[index].position.z==
          constraints.vertices[index].position.z);
  }
  CHECK(attached.constraints.recovery_journal.empty());
  CHECK(attached.constraints.split_vertices.empty());
  CHECK(attached.constraints.facets.size()==5U);
  CHECK(std::none_of(attached.constraints.facets.begin(),
                     attached.constraints.facets.end(),[](const auto& value) {
    auto key=value.vertices;std::sort(key.begin(),key.end());
    return key==std::array<std::uint64_t,3>{{10,20,30}}||
           key==std::array<std::uint64_t,3>{{10,20,40}};
  }));

  const auto recovered=try_recover_wang_segment_by_local_flips(
      constraints,{10,20},mesh,WangSegmentFlipSearchMode::easy,false,1U);
  REQUIRE(recovered.boundary_vertex_attached);
  REQUIRE(recovered.segment_recovered);
  CHECK(recovered.attached_boundary_vertex==50U);
  CHECK(recovered.constraints.facets==attached.constraints.facets);
  CHECK(recovered.constraints.recovery_journal.empty());
  CHECK(recovered.constraints.split_vertices.empty());
  CHECK(recovered.flip.accepted);
  CHECK(recovered.flip.tetrahedra==mesh);
}

TEST_CASE("Wang negative-point edge split promotes only a disposable interior vertex") {
  using namespace tetra::probes;
  const auto materialized=materialize_canonical_plc_constraints(
      std::array<FrozenFacetVertex,4>{{
          {10,{0,0,0}},{20,{2,0,0}},{30,{0,1,0}},{40,{0,0,1}}}},
      std::array<std::array<std::uint64_t,3>,4>{{
          {{10,20,30}},{{20,10,40}},{{10,30,40}},{{20,40,30}}}});
  REQUIRE(materialized.accepted());
  auto constraints=materialized.constraints;
  constraints.vertices.push_back({50,{1,0,0}});
  constraints.interior_steiner_vertices.push_back(
      {50,CanonicalInteriorSteinerKind::cascade_fhc});

  const auto promoted=promote_canonical_interior_steiner_point_to_segment(
      constraints,{10,20},50U,1U,2U);
  REQUIRE(promoted.accepted());
  CHECK(promoted.constraints.vertices.size()==constraints.vertices.size());
  CHECK(promoted.constraints.vertices.back().id==50U);
  CHECK(promoted.constraints.vertices.back().position.x==1.0);
  CHECK(promoted.constraints.vertices.back().position.y==0.0);
  CHECK(promoted.constraints.vertices.back().position.z==0.0);
  CHECK(promoted.constraints.interior_steiner_vertices.empty());
  REQUIRE(promoted.constraints.split_vertices.size()==1U);
  CHECK(promoted.constraints.split_vertices.front().id==50U);
  CHECK(promoted.constraints.split_vertices.front().edge==
        std::array<std::uint64_t,2>{{10,20}});
  REQUIRE(promoted.constraints.recovery_journal.size()==1U);
  CHECK(promoted.constraints.recovery_journal.front().kind==
        CanonicalPlcRecoveryInsertionKind::edge_split);
  CHECK(promoted.constraints.recovery_journal.front().vertex_id==50U);
  CHECK(std::none_of(promoted.constraints.facets.begin(),
                     promoted.constraints.facets.end(),[](const auto& facet) {
    auto vertices=facet.vertices;std::sort(vertices.begin(),vertices.end());
    return vertices==std::array<std::uint64_t,3>{{10,20,30}}||
           vertices==std::array<std::uint64_t,3>{{10,20,40}};
  }));

  const auto original=promote_canonical_interior_steiner_point_to_segment(
      materialized.constraints,{10,20},10U,1U,2U);
  CHECK_FALSE(original.accepted());
}

TEST_CASE("exact affine plane construction survives PLC boundary splits and enters Wang mesh") {
  using namespace tetra::probes;
  const auto materialized=materialize_canonical_plc_constraints(
      std::array<FrozenFacetVertex,4>{{
          {10,{0,0,0}},{20,{2,0,0}},{30,{0,2,0}},{40,{0,0,1}}}},
      std::array<std::array<std::uint64_t,3>,1>{{{{10,20,30}}}});
  REQUIRE(materialized.accepted());
  auto constraints=materialized.constraints;
  constraints.exact_affine_planes.push_back({
      {ExactAffinePlaneConstructionKind::world_axis_rational,2U,0,1U},
      {10,20,30,40}});

  const auto edge=split_canonical_plc_constraint_edge_at_ratio(
      constraints,{10,20},1U,2U,16U,16U);
  REQUIRE(edge.accepted());
  const auto edge_id=edge.constraints.vertices.back().id;
  CHECK(std::binary_search(edge.constraints.exact_affine_planes.front().vertex_ids.begin(),
                           edge.constraints.exact_affine_planes.front().vertex_ids.end(),edge_id));
  const auto facet=split_canonical_plc_constraint_facet(
      constraints,{10,20,30},{1,1,1},3U,16U,16U);
  REQUIRE(facet.accepted());
  const auto facet_id=facet.constraints.vertices.back().id;
  CHECK(std::binary_search(facet.constraints.exact_affine_planes.front().vertex_ids.begin(),
                           facet.constraints.exact_affine_planes.front().vertex_ids.end(),facet_id));

  WangOrderedTetMesh mesh(5U,{{{{0,1,2,4}}}});
  CHECK(mesh.set_exact_affine_planes({10,20,30,40,facet_id},
                                     facet.constraints.exact_affine_planes));
  CHECK(mesh.exact_affine_planes().front().construction.axis==2U);
  CHECK_FALSE(mesh.set_exact_affine_planes({10,20,30,40},
                                           facet.constraints.exact_affine_planes));
}

TEST_CASE("plane-aware orientation separates semantic zero from traversal tie") {
  using namespace tetra::probes;
  const std::array<tetra::Vec3,4> positions{{
      {0.0,0.0,0.0},{1.0,0.0,0.0},{0.0,1.0,0.0},{0.25,0.25,1.0e-16}}};
  const std::array<std::uint64_t,4> ids{{10U,20U,30U,40U}};
  const ExactAffinePlaneProvenance plane{
      {ExactAffinePlaneConstructionKind::world_axis_rational,2U,0,1U},
      {10U,20U,30U,40U}};
  const auto forward=evaluate_plane_aware_orientation(
      positions,ids,std::span<const ExactAffinePlaneProvenance>(&plane,1U));
  auto reversed_positions=positions;
  auto reversed_ids=ids;
  std::swap(reversed_positions[0],reversed_positions[1]);
  std::swap(reversed_ids[0],reversed_ids[1]);
  const auto reversed=evaluate_plane_aware_orientation(
      reversed_positions,reversed_ids,
      std::span<const ExactAffinePlaneProvenance>(&plane,1U));
  CHECK(forward.geometric_sign!=0);
  CHECK(forward.semantically_coplanar);
  CHECK(forward.combinatorial_sign==-reversed.combinatorial_sign);
  CHECK(forward.geometric_sign==-reversed.geometric_sign);
}

TEST_CASE("publication repair admits an exact coplanar target without hiding it") {
  using namespace tetra::probes;
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={{10U,{0.0,0.0,0.0}},{20U,{1.0,0.0,0.0}},
                        {30U,{0.0,1.0,0.0}},{40U,{1.0,1.0,0.0}}};
  constraints.exact_affine_planes.push_back({
      {ExactAffinePlaneConstructionKind::world_axis_rational,2U,0,1U},
      {10U,20U,30U,40U}});
  const std::vector<std::array<std::uint32_t,4>> mesh{{{{0U,1U,2U,3U}}}};

  const auto repair=repair_canonical_plc_publication_degeneracies(
      constraints,mesh,2.0,1U);
  CHECK(repair.initial_degenerate_tetrahedra==1U);
  CHECK(repair.remaining_degenerate_tetrahedra==1U);
  CHECK(repair.has_first_unrepaired_tetrahedron);
  CHECK(repair.first_unrepaired_vertex_ids==
        std::array<std::uint64_t,4>{{10U,20U,30U,40U}});
}

TEST_CASE("canonical seed inserts a declared-planar boundary point without a flat cell") {
  using namespace tetra::probes;
  CanonicalDelaunaySeedInput input;
  input.vertices={{0.0,0.0,0.0},{1.0,0.0,0.0},{0.0,1.0,0.0},
                  {0.0,0.0,1.0},{0.25,0.25,0.0}};
  input.stable_vertex_ids={10U,20U,30U,40U,50U};
  input.exact_affine_planes.push_back({
      {ExactAffinePlaneConstructionKind::world_axis_rational,2U,0,1U},
      {10U,20U,30U,50U}});

  const auto seed=build_canonical_delaunay_seed(input);
  REQUIRE(seed.accepted());
  std::set<std::uint32_t> used;
  for(const auto& tet:seed.tetrahedra) {
    used.insert(tet.begin(),tet.end());
    std::array<std::uint64_t,4> ids{};
    for(std::size_t corner=0U;corner<4U;++corner)
      ids[corner]=input.stable_vertex_ids[tet[corner]];
    CHECK_FALSE(is_semantically_coplanar(ids,input.exact_affine_planes));
  }
  CHECK(used.size()==input.vertices.size());
}
