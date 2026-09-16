#include "tetra_probes/wang_constrained_tetrahedralizer.hpp"
#include "tetra_probes/wang_local_segment_recovery.hpp"
#include "tetra_probes/wang_ordered_tet_mesh.hpp"
#include "tetra_probes/wang_segment_scheduler.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace tetra::probes;
using GeometryCell=std::array<std::string,4>;

CanonicalPlcConstraintSet tetrahedron_plc() {
  CanonicalPlcConstraintSet plc;
  plc.vertices={{10,{0,0,0}},{20,{1,0,0}},{30,{0,1,0}},{40,{0,0,1}}};
  const std::array<std::array<std::uint64_t,3>,4> faces{{
      {{20,30,40}},{{10,40,30}},{{10,20,40}},{{10,30,20}}}};
  for(const auto vertices:faces) {
    CanonicalPlcConstraintFacet facet;
    facet.parent={vertices};facet.vertices=vertices;facet.source_vertices=vertices;
    facet.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
    plc.facets.push_back(facet);
  }
  return plc;
}

CanonicalPlcConstraintSet cube_plc() {
  CanonicalPlcConstraintSet plc;
  plc.vertices={{10,{0,0,0}},{20,{1,0,0}},{30,{1,1,0}},{40,{0,1,0}},
                {50,{0,0,1}},{60,{1,0,1}},{70,{1,1,1}},{80,{0,1,1}}};
  const std::array<std::array<std::uint64_t,3>,12> faces{{
      {{10,30,20}},{{10,40,30}},{{50,60,70}},{{50,70,80}},
      {{10,20,60}},{{10,60,50}},{{40,80,70}},{{40,70,30}},
      {{10,50,80}},{{10,80,40}},{{20,30,70}},{{20,70,60}}}};
  for(const auto vertices:faces) {
    CanonicalPlcConstraintFacet facet;
    facet.parent={vertices};facet.vertices=vertices;facet.source_vertices=vertices;
    facet.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
    plc.facets.push_back(facet);
  }
  return plc;
}

CanonicalPlcConstraintSet scheduler_plc() {
  CanonicalPlcConstraintSet plc;
  const std::array<std::array<double,3>,12> points{{
      {{-1.294,10.0,4.83}},{{4.83,0.0,1.294}},
      {{4.83,10.0,-1.294}},{{-3.536,0.0,3.536}},
      {{4.253,6.532,-2.426}},{{-0.301,9.760,0.0}},
      {{3.117,2.999,-2.571}},{{-2.183,8.657,0.646}},
      {{1.874,1.002,-1.808}},{{-3.330,6.864,1.350}},
      {{0.163,-0.105,-0.366}},{{-4.051,3.184,2.242}}}};
  for(std::size_t i=0;i<points.size();++i)
    plc.vertices.push_back({i+1U,{points[i][0],points[i][1],points[i][2]}});
  const std::array<std::array<std::uint64_t,3>,20> faces{{
      // Preserve POLYGONS order from reference_scheduler_surface.vtk.  DT's
      // SurEdg indices (and therefore AutorecoverEdges queue order) are
      // assigned while buildBndInfo consumes this stream.
      {{3,4,1}},{{3,5,1}},{{12,4,1}},{{5,6,1}},{{6,7,1}},
      {{7,8,1}},{{8,9,1}},{{9,10,1}},{{10,11,1}},{{11,12,1}},
      {{3,4,2}},{{3,5,2}},{{12,4,2}},{{5,6,2}},{{6,7,2}},
      {{7,8,2}},{{8,9,2}},{{9,10,2}},{{10,11,2}},{{11,12,2}}}};
  for(const auto vertices:faces) {
    CanonicalPlcConstraintFacet facet;
    facet.parent={vertices};facet.vertices=vertices;facet.source_vertices=vertices;
    facet.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
    plc.facets.push_back(facet);
  }
  return plc;
}

CanonicalPlcConstraintSet read_scheduler_plc(const std::string& path) {
  std::ifstream input(path);
  CanonicalPlcConstraintSet plc;
  std::string token;
  while(input>>token) {
    if(token=="POINTS") {
      std::size_t count{};std::string scalar_type;
      input>>count>>scalar_type;
      for(std::size_t i=0;i<count;++i) {
        tetra::Vec3 point{};input>>point.x>>point.y>>point.z;
        plc.vertices.push_back({i+1U,point});
      }
    } else if(token=="POLYGONS") {
      std::size_t count{},storage{};input>>count>>storage;
      for(std::size_t i=0;i<count;++i) {
        unsigned corners{};std::array<std::uint64_t,3> vertices{};
        input>>corners;
        if(corners!=3U)return {};
        for(auto& vertex:vertices){input>>vertex;++vertex;}
        CanonicalPlcConstraintFacet facet;
        facet.parent={vertices};facet.vertices=vertices;
        facet.source_vertices=vertices;
        facet.corners={{{{1,0,0},1U},{{0,1,0},1U},{{0,0,1},1U}}};
        plc.facets.push_back(facet);
      }
      break;
    }
  }
  return plc;
}

std::string point_key(const tetra::Vec3& point) {
  std::ostringstream out;
  out<<std::hexfloat<<point.x<<','<<point.y<<','<<point.z;
  return out.str();
}

template <typename Index>
std::vector<GeometryCell> cells(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<std::array<Index,4>>& tetrahedra) {
  std::vector<GeometryCell> result;
  for(const auto& source:tetrahedra) {
    GeometryCell cell{};
    for(unsigned corner=0;corner<4U;++corner)
      cell[corner]=point_key(constraints.vertices[source[corner]].position);
    std::sort(cell.begin(),cell.end());
    result.push_back(cell);
  }
  std::sort(result.begin(),result.end());
  return result;
}

std::vector<GeometryCell> cells_by_id(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<std::array<std::uint64_t,4>>& tetrahedra) {
  std::vector<GeometryCell> result;
  for(const auto& source:tetrahedra) {
    GeometryCell cell{};
    for(unsigned corner=0;corner<4U;++corner) {
      const auto vertex=std::find_if(constraints.vertices.begin(),
          constraints.vertices.end(),[&](const auto& candidate) {
            return candidate.id==source[corner];
          });
      if(vertex==constraints.vertices.end())return {};
      cell[corner]=point_key(vertex->position);
    }
    std::sort(cell.begin(),cell.end());
    result.push_back(cell);
  }
  std::sort(result.begin(),result.end());
  return result;
}

void write_cells(std::ofstream& out,const char* stage,
                 const std::vector<GeometryCell>& value) {
  out<<stage<<"_count "<<value.size()<<'\n';
  for(const auto& cell:value) {
    out<<stage;
    for(const auto& point:cell)out<<' '<<point;
    out<<'\n';
  }
}

void write_ordered_cells(std::ofstream& out,const char* stage,
                         const CanonicalPlcConstraintSet& constraints,
                         const WangOrderedTetMesh& mesh) {
  std::vector<std::string> records;
  for(const auto& cell:mesh.cells())if(!cell.deleted&&
      std::find(cell.vertices.begin(),cell.vertices.end(),
                static_cast<std::uint32_t>(mesh.ghost_vertex()))==cell.vertices.end()) {
    std::ostringstream record;
    record<<stage;
    for(const auto vertex:cell.vertices)
      record<<' '<<point_key(constraints.vertices[vertex].position);
    records.push_back(record.str());
  }
  std::sort(records.begin(),records.end());
  out<<stage<<"_count "<<records.size()<<'\n';
  for(const auto& record:records)out<<record<<'\n';
}

// Preserve the physical state that drives the next source operation.  The
// geometry-only records above intentionally sort tetrahedra for comparison;
// DT's `getP2T()` and recycled element slots are instead observable control
// state because `isMeshFace()`/`finddirection()` begin from those carriers.
// This writer is a probe-only view of the independently-owned mesh, using the
// same source node and element indices established by set_source_slot_layout.
void write_indexed_state(std::ofstream& out,const char* stage,
                         const CanonicalPlcConstraintSet& constraints,
                         const WangOrderedTetMesh& mesh) {
  const auto ghost=static_cast<std::uint32_t>(mesh.ghost_vertex());
  out<<stage<<"_node_count "<<mesh.vertex_count()<<'\n';
  for(std::size_t vertex=0;vertex<mesh.vertex_count();++vertex) {
    out<<stage<<"_node "<<vertex<<' ';
    if(vertex==ghost)out<<"ghost";
    else out<<point_key(constraints.vertices[vertex].position);
    out<<'\n';
  }
  for(std::size_t slot=0;slot<mesh.cells().size();++slot) {
    const auto& cell=mesh.cells()[slot];
    if(cell.deleted||std::find(cell.vertices.begin(),cell.vertices.end(),ghost)!=
        cell.vertices.end())continue;
    out<<stage<<"_cell "<<slot;
    for(const auto vertex:cell.vertices)out<<' '<<vertex;
    out<<'\n';
  }
  for(std::size_t vertex=0;vertex<mesh.vertex_count();++vertex)
    out<<stage<<"_p2t "<<vertex<<' '<<mesh.point_to_cell()[vertex]<<'\n';
}

void write_order(std::ofstream& out,const CanonicalPlcConstraintSet& plc) {
  std::vector<tetra::Vec3> points;
  points.reserve(plc.vertices.size());
  for(const auto& vertex:plc.vertices)points.push_back(vertex.position);
  out<<"insertion_order";
  for(const auto index:wang_reference_hilbert_order(points))
    out<<' '<<point_key(points[index]);
  out<<'\n';
}

std::pair<CanonicalPlcConstraintSet,WangReferenceSeedTrace> seed_trace(
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
  for(const auto point:std::array<tetra::Vec3,8>{{
          {low.x,low.y,low.z},{high.x,low.y,low.z},
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
  return {plc,trace_wang_reference_seed(input,original_count)};
}

void write_seed_stages(std::ofstream& out,
                       const CanonicalPlcConstraintSet& constraints,
                       const WangReferenceSeedTrace& trace) {
  out<<"seed_stage_count "<<trace.stages.size()<<'\n';
  for(std::size_t stage=0;stage<trace.stages.size();++stage) {
    const auto stage_cells=cells(constraints,trace.stages[stage]);
    out<<"seed_stage "<<stage<<' '<<stage_cells.size()<<'\n';
    for(const auto& cell:stage_cells) {
      out<<"seed_stage_cell "<<stage;
      for(const auto& point:cell)out<<' '<<point;
      out<<'\n';
    }
  }
  out<<"seed_box_carrier_count "<<trace.box_carriers.size()<<'\n';
  for(std::size_t box=0;box<trace.box_carriers.size();++box) {
    out<<"seed_box_carrier "<<box;
    std::array<std::string,4> geometry{};
    for(unsigned corner=0;corner<4U;++corner)
      geometry[corner]=point_key(
          constraints.vertices[trace.box_carriers[box][corner]].position);
    std::sort(geometry.begin(),geometry.end());
    for(const auto& point:geometry)out<<' '<<point;
    out<<'\n';
  }
  for(std::size_t stage=0;stage<trace.live_slot_stages.size();++stage)
    for(const auto& slot:trace.live_slot_stages[stage]) {
      out<<"seed_slot "<<stage<<' '<<slot.index;
      std::array<std::string,4> geometry{};
      for(std::size_t corner=0;corner<4U;++corner)
        geometry[corner]=slot.cell[corner]==trace.ghost_vertex?"ghost":
            point_key(constraints.vertices[slot.cell[corner]].position);
      std::sort(geometry.begin(),geometry.end());
      for(const auto& point:geometry)out<<' '<<point;
      out<<'\n';
    }
  for(std::size_t stage=0;stage<trace.live_slot_stages.size();++stage)
    for(const auto& slot:trace.live_slot_stages[stage]) {
      out<<"seed_slot_raw "<<stage<<' '<<slot.index;
      for(const auto vertex:slot.cell)
        out<<' '<<(vertex==trace.ghost_vertex?"ghost":
            point_key(constraints.vertices[vertex].position));
      out<<'\n';
    }
  for(std::size_t stage=0;stage<trace.original_slot_stages.size();++stage)
    for(const auto& slot:trace.original_slot_stages[stage]) {
      out<<"seed_original_slot "<<stage<<' '<<slot.index;
      std::array<std::string,4> geometry{};
      for(std::size_t corner=0;corner<4U;++corner)
        geometry[corner]=slot.cell[corner]==trace.ghost_vertex?"ghost":
            point_key(constraints.vertices[slot.cell[corner]].position);
      std::sort(geometry.begin(),geometry.end());
      for(const auto& point:geometry)out<<' '<<point;
      out<<'\n';
    }
  for(std::size_t stage=0;stage<trace.original_slot_stages.size();++stage)
    for(const auto& slot:trace.original_slot_stages[stage]) {
      out<<"seed_original_slot_raw "<<stage<<' '<<slot.index;
      for(const auto vertex:slot.cell)
        out<<' '<<(vertex==trace.ghost_vertex?"ghost":
            point_key(constraints.vertices[vertex].position));
      out<<'\n';
    }
  if(trace.original_working_cavities.size()>4U) {
    for(std::size_t stage=0;stage<trace.original_carrier_slots.size();++stage)
      out<<"seed_original_carrier "<<stage<<' '<<trace.original_carrier_slots[stage]<<'\n';
    for(std::size_t stage=0;stage<trace.original_carrier_cells.size();++stage) {
      out<<"seed_original_carrier_raw "<<stage;
      for(const auto vertex:trace.original_carrier_cells[stage])
        out<<' '<<(vertex==trace.ghost_vertex?"ghost":
            point_key(constraints.vertices[vertex].position));
      out<<'\n';
    }
    for(std::size_t stage=0;stage<trace.original_location_results.size();++stage) {
      out<<"seed_original_location "<<stage<<' '
         <<trace.original_location_results[stage];
      for(const auto vertex:trace.original_location_cells[stage])
        out<<' '<<(vertex==trace.ghost_vertex?"ghost":
            point_key(constraints.vertices[vertex].position));
      out<<'\n';
    }
    for(std::size_t stage=0;stage<trace.original_hull_predicates.size();++stage)
      for(const auto& predicate:trace.original_hull_predicates[stage]) {
        out<<"seed_original_hull_predicate "<<stage;
        std::array<std::string,4> geometry{};
        for(unsigned corner=0;corner<4U;++corner)
          geometry[corner]=predicate.cell[corner]==trace.ghost_vertex?"ghost":
              point_key(constraints.vertices[predicate.cell[corner]].position);
        std::sort(geometry.begin(),geometry.end());
        for(const auto& point:geometry)out<<' '<<point;
        out<<' '<<std::hexfloat<<predicate.orientation<<std::defaultfloat<<' '
           <<predicate.inner_sphere<<'\n';
      }
    for(std::size_t queue=0;queue<trace.original_working_cavities[4U].size();++queue) {
      out<<"seed_original_working 4 "<<trace.original_working_slots[4U][queue];
      auto cell=trace.original_working_cavities[4U][queue];
      std::array<std::string,4> geometry{};
      for(std::size_t corner=0;corner<4U;++corner)
        geometry[corner]=cell[corner]==trace.ghost_vertex?"ghost":
            point_key(constraints.vertices[cell[corner]].position);
      std::sort(geometry.begin(),geometry.end());
      for(const auto& point:geometry)out<<' '<<point;
      out<<'\n';
    }
    for(std::size_t index=0;index<trace.original_boundary_faces[4U].size();++index) {
      const auto& source=trace.original_boundary_faces[4U][index];
      std::array<std::string,3> face{};
      for(std::size_t corner=0;corner<3U;++corner)
        face[corner]=source[corner]==trace.ghost_vertex?"ghost":
            point_key(constraints.vertices[source[corner]].position);
      std::sort(face.begin(),face.end());
      out<<"seed_original_face 4 "<<index;
      for(const auto& point:face)out<<' '<<point;
      out<<'\n';
    }
  }
  for(std::size_t queue=0;queue<trace.first_box_working_cavity.size();++queue) {
    out<<"seed_box0_working "<<trace.first_box_working_slots[queue];
    for(const auto vertex:trace.first_box_working_cavity[queue])
      out<<' '<<(vertex==trace.ghost_vertex?"ghost":
          point_key(constraints.vertices[vertex].position));
    out<<'\n';
  }
  for(std::size_t face=0;face<trace.first_box_boundary_faces.size();++face) {
    out<<"seed_box0_face "<<face;
    for(const auto vertex:trace.first_box_boundary_faces[face])
      out<<' '<<(vertex==trace.ghost_vertex?"ghost":
          point_key(constraints.vertices[vertex].position));
    out<<'\n';
  }
  for(std::size_t queue=0;queue<trace.fourth_box_working_cavity.size();++queue) {
    out<<"seed_box4_working "<<trace.fourth_box_working_slots[queue];
    for(const auto vertex:trace.fourth_box_working_cavity[queue])
      out<<' '<<(vertex==trace.ghost_vertex?"ghost":
          point_key(constraints.vertices[vertex].position));
    out<<'\n';
  }
  for(std::size_t face=0;face<trace.fourth_box_boundary_faces.size();++face) {
    out<<"seed_box4_face "<<face;
    for(const auto vertex:trace.fourth_box_boundary_faces[face])
      out<<' '<<(vertex==trace.ghost_vertex?"ghost":
          point_key(constraints.vertices[vertex].position));
    out<<'\n';
  }
  for(std::size_t queue=0;queue<trace.third_box_working_cavity.size();++queue) {
    out<<"seed_box3_working "<<trace.third_box_working_slots[queue];
    for(const auto vertex:trace.third_box_working_cavity[queue])
      out<<' '<<(vertex==trace.ghost_vertex?"ghost":
          point_key(constraints.vertices[vertex].position));
    out<<'\n';
  }
  for(std::size_t face=0;face<trace.third_box_boundary_faces.size();++face) {
    out<<"seed_box3_face "<<face;
    for(const auto vertex:trace.third_box_boundary_faces[face])
      out<<' '<<(vertex==trace.ghost_vertex?"ghost":
          point_key(constraints.vertices[vertex].position));
    out<<'\n';
  }
  for(const auto& predicate:trace.third_box_predicates) {
    out<<"seed_box3_predicate";
    for(const auto vertex:predicate.cell)
      out<<' '<<(vertex==trace.ghost_vertex?"ghost":
          point_key(constraints.vertices[vertex].position));
    out<<' '<<predicate.orientation<<' '<<predicate.exact_in_sphere<<' '
       <<predicate.source_in_sphere<<' '<<(predicate.selected?1:0)<<'\n';
  }
  for(std::size_t queue=0;queue<trace.seventh_box_working_cavity.size();++queue) {
    out<<"seed_box6_working "<<trace.seventh_box_working_slots[queue];
    for(const auto vertex:trace.seventh_box_working_cavity[queue])
      out<<' '<<(vertex==trace.ghost_vertex?"ghost":point_key(constraints.vertices[vertex].position));
    out<<'\n';
  }
  for(std::size_t face=0;face<trace.seventh_box_boundary_faces.size();++face) {
    out<<"seed_box6_face "<<face;
    for(const auto vertex:trace.seventh_box_boundary_faces[face])
      out<<' '<<(vertex==trace.ghost_vertex?"ghost":point_key(constraints.vertices[vertex].position));
    out<<'\n';
  }
}

// Keep every successful original-point transaction, including sparse physical
// slot identity, in the real-fixture differential.  This is the earliest
// state that can distinguish source control-flow divergence from the later
// AddBox failure.
void write_original_seed_slots(std::ofstream& out,
                              const CanonicalPlcConstraintSet& constraints,
                              const WangReferenceSeedTrace& trace) {
  for(std::size_t stage=0;stage<trace.original_slot_stages.size();++stage)
    for(const auto& slot:trace.original_slot_stages[stage]) {
      out<<"seed_original_slot "<<stage<<' '<<slot.index;
      std::array<std::string,4> geometry{};
      for(unsigned corner=0;corner<4U;++corner)
        geometry[corner]=slot.cell[corner]==trace.ghost_vertex?"ghost":
            point_key(constraints.vertices[slot.cell[corner]].position);
      std::sort(geometry.begin(),geometry.end());
      for(const auto& point:geometry)out<<' '<<point;
      out<<'\n';
    }
}

void write_original_seed_plans(std::ofstream& out,
                               const CanonicalPlcConstraintSet& constraints,
                               const WangReferenceSeedTrace& trace) {
  const auto write=[&](const char* label,const auto& stages,std::size_t stage) {
      if(stage>=stages.size())return;
      for(const auto& cell:stages[stage]) {
        out<<label<<' '<<stage;
        std::array<std::string,4> geometry{};
        for(unsigned corner=0;corner<4U;++corner)
          geometry[corner]=cell[corner]==trace.ghost_vertex?"ghost":
              point_key(constraints.vertices[cell[corner]].position);
        std::sort(geometry.begin(),geometry.end());
        for(const auto& point:geometry)out<<' '<<point;
        out<<'\n';
      }
  };
  const auto count=std::max(trace.original_working_cavities.size(),
                            trace.original_adjusted_cavities.size());
  for(std::size_t stage=0;stage<count;++stage) {
    write("seed_original_working",trace.original_working_cavities,stage);
    write("seed_original_adjusted",trace.original_adjusted_cavities,stage);
  }
}

void write_seed_p2t(std::ofstream& out,
                    const CanonicalPlcConstraintSet& constraints,
                    const WangReferenceSeedTrace& trace) {
  constexpr auto absent=std::numeric_limits<std::uint32_t>::max();
  for(std::size_t stage=0;stage<trace.point_carrier_stages.size();++stage)
    for(std::size_t node=0;node<trace.point_carrier_stages[stage].size();++node) {
      const auto& cell=trace.point_carrier_stages[stage][node];
      if(cell[0]==absent)continue;
      out<<"seed_p2t "<<stage<<' '
         <<point_key(constraints.vertices[node].position);
      for(const auto vertex:cell)
        out<<' '<<(vertex==trace.ghost_vertex?"ghost":
            point_key(constraints.vertices[vertex].position));
      out<<'\n';
    }
}

template<std::size_t N>
void write_seed_plan_geometry(
    std::ofstream& out,const char* label,
    const CanonicalPlcConstraintSet& constraints,
    std::vector<std::array<std::uint32_t,N>> values) {
  std::vector<std::array<std::string,N>> geometry;
  for(const auto& value:values) {
    std::array<std::string,N> item{};
    for(std::size_t i=0;i<N;++i)
      item[i]=value[i]>=constraints.vertices.size()?"ghost":
          point_key(constraints.vertices[value[i]].position);
    std::sort(item.begin(),item.end());geometry.push_back(item);
  }
  std::sort(geometry.begin(),geometry.end());
  out<<label<<"_count "<<geometry.size()<<'\n';
  for(const auto& item:geometry) {
    out<<label;
    for(const auto& point:item)out<<' '<<point;
    out<<'\n';
  }
}

void write_seed_plan(std::ofstream& out,
                     const CanonicalPlcConstraintSet& constraints,
                     const WangReferenceSeedTrace& trace) {
  out<<"seed_plan_carrier_count 0\nseed_plan_reads_count 0\n";
  write_seed_plan_geometry(out,"seed_plan_working",constraints,
                           trace.eighth_working_cavity);
  write_seed_plan_geometry(out,"seed_plan_found_boundary",constraints,
                           trace.eighth_boundary_faces);
  write_seed_plan_geometry(out,"seed_plan_adjusted",constraints,
                           trace.eighth_working_cavity);
  write_seed_plan_geometry(out,"seed_plan_adjusted_boundary",constraints,
                           trace.eighth_boundary_faces);
  write_seed_plan_geometry(out,"seed_plan_cavity",constraints,
                           trace.eighth_working_cavity);
  write_seed_plan_geometry(out,"seed_plan_faces",constraints,
                           trace.eighth_boundary_faces);
  write_seed_plan_geometry(out,"seed_plan_fill",constraints,
                           trace.eighth_fill_cells);
  out<<"seed_plan_query "<<point_key(
      constraints.vertices[trace.eighth_query].position)<<'\n';
  out<<"seed_plan_predicate_count "<<trace.eighth_predicates.size()<<'\n';
  std::vector<std::string> predicates;
  for(const auto& predicate:trace.eighth_predicates) {
    std::array<std::string,4> geometry{};
    for(unsigned i=0;i<4U;++i)
      geometry[i]=point_key(constraints.vertices[predicate.cell[i]].position);
    std::sort(geometry.begin(),geometry.end());
    std::ostringstream line;
    for(const auto& point:geometry)line<<point<<' ';
    line<<predicate.orientation<<' '<<predicate.exact_in_sphere<<' '
        <<predicate.source_in_sphere<<' '<<predicate.selected;
    predicates.push_back(line.str());
  }
  std::sort(predicates.begin(),predicates.end());
  for(const auto& predicate:predicates)
    out<<"seed_plan_predicate "<<predicate<<'\n';
  const auto orient_value=[](const tetra::Vec3& a,const tetra::Vec3& b,
                               const tetra::Vec3& c,const tetra::Vec3& d) {
      const double adx=a.x-d.x,bdx=b.x-d.x,cdx=c.x-d.x;
      const double ady=a.y-d.y,bdy=b.y-d.y,cdy=c.y-d.y;
      const double adz=a.z-d.z,bdz=b.z-d.z,cdz=c.z-d.z;
      return adz*(bdx*cdy-cdx*bdy)+bdz*(cdx*ady-adx*cdy)+
          cdz*(adx*bdy-bdx*ady);
    };
  if(trace.box_carriers.size()>7U) {
    const auto& tet=trace.box_carriers[7U];
    static constexpr unsigned dnc[4][3]={{1U,2U,3U},{3U,2U,0U},
                                           {0U,1U,3U},{2U,1U,0U}};
    for(unsigned face=0;face<4U;++face) {
      const auto value=orient_value(
          constraints.vertices[tet[dnc[face][0]]].position,
          constraints.vertices[tet[dnc[face][1]]].position,
          constraints.vertices[tet[dnc[face][2]]].position,
          constraints.vertices[trace.eighth_query].position);
      out<<"seed_plan_location_face "<<face<<' '<<std::hexfloat<<value
         <<std::defaultfloat<<'\n';
    }
  }
  for(const auto& slot:trace.live_slot_stages[7U]) {
    const auto& tet=slot.cell;
    if(tet[3]!=trace.ghost_vertex)continue;
    const auto value=orient_value(constraints.vertices[tet[0]].position,
        constraints.vertices[tet[1]].position,constraints.vertices[tet[2]].position,
        constraints.vertices[trace.eighth_query].position);
    out<<"seed_plan_hull_eval "<<slot.index<<' '<<std::hexfloat<<value
       <<std::defaultfloat<<' '<<(value<0.0?1:0)<<'\n';
  }
  for(const auto& decision:trace.eighth_traversal)
    out<<"seed_plan_owned_traversal "<<decision.current_slot<<' '
       <<decision.face<<' '<<decision.neighbour_slot<<' '
       <<(decision.included?1:0)<<'\n';
  out<<"seed_plan_owned_location "<<trace.eighth_location_result;
  for(const auto slot:trace.eighth_location_path)out<<' '<<slot;
  out<<'\n';
  if(!trace.eighth_location_path.empty()) {
    const auto endpoint=trace.eighth_location_path.back();
    const auto located=std::find_if(trace.live_slot_stages[7U].begin(),
        trace.live_slot_stages[7U].end(),[&](const auto& slot) {
          return slot.index==endpoint;
        });
    if(located!=trace.live_slot_stages[7U].end()) {
      static constexpr unsigned dnc[4][3]={{1U,2U,3U},{3U,2U,0U},
                                             {0U,1U,3U},{2U,1U,0U}};
      for(unsigned face=0;face<4U;++face) {
        const auto value=orient_value(
            constraints.vertices[located->cell[dnc[face][0]]].position,
            constraints.vertices[located->cell[dnc[face][1]]].position,
            constraints.vertices[located->cell[dnc[face][2]]].position,
            constraints.vertices[trace.eighth_query].position);
        out<<"seed_plan_owned_endpoint_face "<<face<<' '<<std::hexfloat
           <<value<<std::defaultfloat<<'\n';
      }
    }
  }
  // The cavity search follows physical face neighbours, not the canonical
  // face map.  Emit the pre-insertion adjacency so a queue-order difference
  // is attributable to a concrete face link.
  std::map<std::array<std::uint32_t,3>,std::vector<std::pair<std::size_t,unsigned>>> links;
  for(std::size_t cell=0;cell<trace.live_slot_stages[7U].size();++cell) {
    const auto& tet=trace.live_slot_stages[7U][cell].cell;
    for(unsigned face=0;face<4U;++face) {
      std::array<std::uint32_t,3> key{};unsigned cursor{};
      for(unsigned corner=0;corner<4U;++corner)if(corner!=face)key[cursor++]=tet[corner];
      std::sort(key.begin(),key.end());links[key].push_back({cell,face});
    }
  }
  for(std::size_t i=0;i<trace.eighth_working_cavity.size();++i) {
    const auto slot=trace.eighth_working_slots[i];
    const auto cell=std::find_if(trace.live_slot_stages[7U].begin(),
        trace.live_slot_stages[7U].end(),[&](const auto& entry){return entry.index==slot;});
    if(cell==trace.live_slot_stages[7U].end())continue;
    const auto cell_index=static_cast<std::size_t>(cell-trace.live_slot_stages[7U].begin());
    out<<"seed_plan_raw_working "<<slot;
    for(const auto vertex:trace.eighth_working_cavity[i])
      out<<' '<<(vertex==trace.ghost_vertex?"ghost":
          point_key(constraints.vertices[vertex].position));
    out<<'\n';
    for(unsigned face=0;face<4U;++face) {
      std::array<std::uint32_t,3> key{};unsigned cursor{};
      for(unsigned corner=0;corner<4U;++corner)if(corner!=face)key[cursor++]=cell->cell[corner];
      std::sort(key.begin(),key.end());const auto& uses=links.at(key);
      const auto other=uses[0].first==cell_index?uses[1].first:uses[0].first;
      out<<"seed_plan_raw_neighbor "<<slot<<' '<<face<<' '
         <<trace.live_slot_stages[7U][other].index<<'\n';
    }
  }
  // prepareBWFill appends one boundary face and commitBW immediately maps
  // that same position to one new element.  Keep the diagnostic interleaved
  // as the pinned implementation exposes it: this makes a later difference
  // an actual face/child mismatch rather than a probe-only batching artifact.
  if(trace.eighth_boundary_faces.size()==trace.eighth_fill_cells.size())
    for(std::size_t i=0U;i<trace.eighth_boundary_faces.size();++i) {
      out<<"seed_plan_raw_face";
      for(const auto vertex:trace.eighth_boundary_faces[i])
        out<<' '<<(vertex==trace.ghost_vertex?"ghost":
            point_key(constraints.vertices[vertex].position));
      out<<'\n';
      out<<"seed_plan_raw_fill";
      for(const auto vertex:trace.eighth_fill_cells[i])
        out<<' '<<(vertex==trace.ghost_vertex?"ghost":
            point_key(constraints.vertices[vertex].position));
      out<<'\n';
    }
}

void write_owned_state(std::ostream& out,
                       const CanonicalPlcConstraintSet& constraints,
                       const std::vector<std::array<std::uint32_t,4>>& cells,
                       const std::vector<std::array<std::uint32_t,4>>& p2t) {
  WangOrderedTetMesh mesh(constraints.vertices.size(),cells);
  if(!mesh.set_point_incidence(p2t)||!mesh.audit().accepted())return;
  const auto geometry=[&](const WangOrderedTetMesh::Tet& cell) {
    GeometryCell result{};
    for(unsigned corner=0;corner<4U;++corner)
      result[corner]=point_key(constraints.vertices[cell[corner]].position);
    std::sort(result.begin(),result.end());return result;
  };
  std::vector<std::string> cell_lines,neighbour_lines,p2t_lines;
  for(std::size_t cell_index=0;cell_index<mesh.cells().size();++cell_index) {
    const auto& cell=mesh.cells()[cell_index];
    if(cell.deleted)continue;
    std::ostringstream cell_line;
    cell_line<<"state_cell";
    for(const auto vertex:cell.vertices)
      cell_line<<' '<<point_key(constraints.vertices[vertex].position);
    cell_lines.push_back(cell_line.str());
    const auto owner=geometry(cell.vertices);
    for(unsigned opposite=0;opposite<4U;++opposite) {
      std::array<std::string,3> face{};unsigned cursor{};
      for(unsigned corner=0;corner<4U;++corner)if(corner!=opposite)
        face[cursor++]=point_key(constraints.vertices[cell.vertices[corner]].position);
      std::sort(face.begin(),face.end());
      std::ostringstream line;line<<"state_neighbour";
      for(const auto& point:owner)line<<' '<<point;
      for(const auto& point:face)line<<' '<<point;
      const auto neighbour=cell.neighbours[opposite];
      if(neighbour==WangOrderedTetMesh::no_neighbour)line<<" hull";
      else {
        const auto other=geometry(mesh.cells()[static_cast<std::size_t>(neighbour)].vertices);
        for(const auto& point:other)line<<' '<<point;
      }
      neighbour_lines.push_back(line.str());
    }
  }
  for(std::size_t vertex=0;vertex<mesh.point_to_cell().size();++vertex) {
    const auto carrier=mesh.point_to_cell()[vertex];
    if(carrier<0)continue;
    std::ostringstream line;
    line<<"state_p2t "<<point_key(constraints.vertices[vertex].position);
    const auto cell=geometry(mesh.cells()[static_cast<std::size_t>(carrier)].vertices);
    for(const auto& point:cell)line<<' '<<point;
    p2t_lines.push_back(line.str());
  }
  std::sort(cell_lines.begin(),cell_lines.end());
  std::sort(neighbour_lines.begin(),neighbour_lines.end());
  std::sort(p2t_lines.begin(),p2t_lines.end());
  out<<"state_cell_count "<<cell_lines.size()<<'\n';
  for(const auto& line:cell_lines)out<<line<<'\n';
  out<<"state_neighbour_count "<<neighbour_lines.size()<<'\n';
  for(const auto& line:neighbour_lines)out<<line<<'\n';
  out<<"state_p2t_count "<<p2t_lines.size()<<'\n';
  for(const auto& line:p2t_lines)out<<line<<'\n';
}

void write_direction_semantics(
    std::ostream& out,const CanonicalPlcConstraintSet& constraints,
    const std::vector<std::array<std::uint32_t,4>>& cells) {
  for(const auto edge:std::array<std::array<std::uint64_t,2>,3>{
          {{{1,7}},{{1,9}},{{8,9}}}})
    for(int reverse=0;reverse<2;++reverse) {
      const auto selected=inspect_wang_endpoint_star_feature(
          constraints,edge,cells,reverse!=0);
      out<<"direction_semantic "<<edge[0]<<' '<<edge[1]<<' '<<reverse;
      if(!selected) {out<<" unknown source\n";continue;}
      const auto point_for_id=[&](std::uint64_t id) -> std::string {
        const auto found=std::find_if(constraints.vertices.begin(),
            constraints.vertices.end(),[&](const auto& vertex) {
              return vertex.id==id;
            });
        return found==constraints.vertices.end()?"missing":point_key(found->position);
      };
      if(selected->kind==WangEndpointStarFeatureKind::vertex)
        out<<" vertex "<<point_for_id(selected->feature[0]);
      else if(selected->kind==WangEndpointStarFeatureKind::edge) {
        std::array<std::string,2> feature{{point_for_id(selected->feature[0]),
                                           point_for_id(selected->feature[1])}};
        std::sort(feature.begin(),feature.end());
        out<<" edge "<<feature[0]<<' '<<feature[1];
      } else if(selected->kind==WangEndpointStarFeatureKind::face) {
        std::array<std::string,3> feature{{point_for_id(selected->feature[0]),
                                           point_for_id(selected->feature[1]),
                                           point_for_id(selected->feature[2])}};
        std::sort(feature.begin(),feature.end());
        out<<" face";
        for(const auto& point:feature)out<<' '<<point;
      } else out<<" unknown";
      std::array<std::string,4> source{};
      for(unsigned corner=0;corner<4U;++corner)
        source[corner]=point_for_id(selected->source_tetrahedron[corner]);
      std::sort(source.begin(),source.end());
      out<<" source";
      for(const auto& point:source)out<<' '<<point;
      out<<'\n';
    }
}

void write_synthetic_edge_contact(std::ostream& out) {
  CanonicalPlcConstraintSet constraints;
  constraints.vertices={
      {1U,{-1.0,0.0,0.0}},{2U,{1.0,0.0,0.0}},
      {3U,{0.0,-1.0,0.0}},{4U,{0.0,1.0,0.0}},
      {5U,{0.0,0.0,1.0}},{6U,{0.0,0.0,-1.0}}};
  const std::vector<WangOrderedTetMesh::Tet> cells{
      {{0U,2U,3U,4U}},{{1U,4U,3U,2U}},
      {{1U,2U,3U,5U}},{{0U,5U,3U,2U}}};
  WangOrderedTetMesh mesh(constraints.vertices.size(),cells);
  const auto trace=inspect_wang_full_search_features(
      constraints,{{1U,2U}},mesh);
  int direction=-20;
  if(!trace.features.empty()&&
     trace.features[0].kind==WangOwnedFullSearchFeatureKind::edge) {
    const auto& source=mesh.cells()[0].vertices;
    const auto first=static_cast<int>(std::find(
        source.begin(),source.end(),trace.features[0].vertices[0])-source.begin());
    const auto second=static_cast<int>(std::find(
        source.begin(),source.end(),trace.features[0].vertices[1])-source.begin());
    direction=-(first<<2|second);
  }
  out<<"edge_contact_synthetic direction "<<direction<<" walk "
     <<(trace.failure==WangOwnedFullSearchFailure::none?0:1)
     <<" feature_count "<<trace.features.size()<<'\n';
  for(const auto& feature:trace.features) {
    out<<"edge_contact_synthetic_feature";
    const unsigned count=feature.kind==WangOwnedFullSearchFeatureKind::edge?2U:3U;
    std::vector<std::string> geometry;
    for(unsigned i=0;i<count;++i)
      geometry.push_back(point_key(
          constraints.vertices[feature.vertices[i]].position));
    std::sort(geometry.begin(),geometry.end());
    for(const auto& point:geometry)out<<' '<<point;
    out<<'\n';
  }
  const auto cascade=insert_wang_owned_cascade_fhc_point(
      constraints,{{1U,2U}},{{2U,3U}},mesh);
  if(!cascade.inserted) {
    out<<"cascade_synthetic result 0 nodes 0 finite_cells "
       <<cells.size()<<'\n';
    return;
  }
  constraints.vertices.push_back({7U,cascade.point});
  const auto committed=mesh.replace_cavity_with_appended_vertex(
      cascade.cavity,cascade.replacement);
  const auto retry=committed.accepted?recover_wang_segment_by_local_flips(
      constraints,{{1U,2U}},false,1000U,mesh):WangOwnedLocalRecoveryResult{};
  std::vector<GeometryCell> post;
  for(const auto& cell:mesh.cells())if(!cell.deleted) {
    GeometryCell geometry{};
    for(unsigned corner=0;corner<4U;++corner)
      geometry[corner]=point_key(constraints.vertices[cell.vertices[corner]].position);
    std::sort(geometry.begin(),geometry.end());post.push_back(geometry);
  }
  std::sort(post.begin(),post.end());
  out<<"cascade_synthetic result "<<(retry.recovered?1:0)<<" nodes "
     <<(committed.accepted?1:0);
  if(committed.accepted)out<<' '<<point_key(cascade.point);
  out<<" finite_cells "<<post.size()<<'\n';
  out<<"cascade_synthetic_post_count "<<post.size()<<'\n';
  for(const auto& cell:post) {
    out<<"cascade_synthetic_post";
    for(const auto& point:cell)out<<' '<<point;
    out<<'\n';
  }
}

void write_first_face_flip(std::ostream& out,
                           const CanonicalPlcConstraintSet& constraints,
                           const std::vector<std::array<std::uint32_t,4>>& cells) {
  WangOrderedTetMesh recovered_mesh(constraints.vertices.size(),cells);
  const auto recovery=recover_wang_segment_by_local_flips(
      constraints,{{1U,7U}},true,1U,recovered_mesh);
  WangOrderedTetMesh mesh(constraints.vertices.size(),cells);
  const auto active=[](const WangOrderedTetMesh& state) {
    std::vector<WangOrderedTetMesh::Tet> result;
    for(const auto& cell:state.cells())if(!cell.deleted)result.push_back(cell.vertices);
    return result;
  };
  const auto delta=[&](const std::vector<GeometryCell>& before,
                       const std::vector<GeometryCell>& after) {
    std::pair<std::vector<GeometryCell>,std::vector<GeometryCell>> result;
    std::set_difference(before.begin(),before.end(),after.begin(),after.end(),
                        std::back_inserter(result.first));
    std::set_difference(after.begin(),after.end(),before.begin(),before.end(),
                        std::back_inserter(result.second));
    return result;
  };
  const auto write_step=[&](int step,std::uint32_t first,std::uint32_t second) {
    const auto before=::cells(constraints,active(mesh));
    const auto mutation=mesh.flip32(first,second);
    const auto after=::cells(constraints,active(mesh));
    const auto [erased,created]=delta(before,after);
    out<<"first_flip32_step "<<step<<" edge "<<first+1U<<' '<<second+1U
       <<" shell 3 result "<<(mutation.accepted?1:0)<<'\n';
    out<<"first_flip32_erased_count "<<step<<' '<<erased.size()<<'\n';
    for(const auto& cell:erased) {
      out<<"first_flip32_erased "<<step;
      for(const auto& point:cell)out<<' '<<point;
      out<<'\n';
    }
    out<<"first_flip32_created_count "<<step<<' '<<created.size()<<'\n';
    for(const auto& cell:created) {
      out<<"first_flip32_created "<<step;
      for(const auto& point:cell)out<<' '<<point;
      out<<'\n';
    }
    out<<"first_flip32_cells "<<step<<' '<<after.size()<<'\n';
    return mutation.accepted;
  };

  const auto before=::cells(constraints,active(mesh));
  std::ostringstream primitive;
  auto* destination=out.rdbuf(primitive.rdbuf());
  bool local_face_removed=recovery.recovered&&recovery.mutations.size()>=2U;
  for(std::size_t step=0;step<2U&&local_face_removed;++step) {
    const auto& mutation=recovery.mutations[step];
    local_face_removed=mutation.kind==WangOwnedLocalMutation::Kind::flip32&&
        write_step(static_cast<int>(step+1U),mutation.feature[0],
                   mutation.feature[1]);
  }
  out.rdbuf(destination);
  const auto after=::cells(constraints,active(mesh));
  const auto [erased,created]=delta(before,after);
  const auto selected=inspect_wang_endpoint_star_feature(
      constraints,{{1U,7U}},cells,true);
  out<<"first_removeface 1 7 stored 7 1 direction 4 result "
     <<(local_face_removed?1:0)<<" cavity_size "<<(local_face_removed?2:0)<<'\n';
  out<<"first_removeface_source";
  if(selected) {
    std::array<std::string,4> source{};
    for(unsigned i=0;i<4U;++i) {
      const auto id=selected->source_tetrahedron[i];
      const auto found=std::find_if(constraints.vertices.begin(),
          constraints.vertices.end(),[&](const auto& vertex){return vertex.id==id;});
      source[i]=point_key(found->position);
    }
    std::sort(source.begin(),source.end());
    for(const auto& point:source)out<<' '<<point;
  }
  out<<'\n';
  out<<"first_removeface_feature";
  if(selected) {
    std::array<std::string,3> feature{};
    for(unsigned i=0;i<3U;++i) {
      const auto id=selected->feature[i];
      const auto found=std::find_if(constraints.vertices.begin(),
          constraints.vertices.end(),[&](const auto& vertex){return vertex.id==id;});
      feature[i]=point_key(found->position);
    }
    std::sort(feature.begin(),feature.end());
    for(const auto& point:feature)out<<' '<<point;
  }
  out<<'\n';
  out<<"first_removeface_erased_count "<<erased.size()<<'\n';
  for(const auto& cell:erased) {
    out<<"first_removeface_erased";
    for(const auto& point:cell)out<<' '<<point;
    out<<'\n';
  }
  out<<"first_removeface_created_count "<<created.size()<<'\n';
  for(const auto& cell:created) {
    out<<"first_removeface_created";
    for(const auto& point:cell)out<<' '<<point;
    out<<'\n';
  }
  out<<"first_removeface_cells "<<after.size()<<'\n';
  for(const auto& cell:after) {
    out<<"first_removeface_cell";
    for(const auto& point:cell)out<<' '<<point;
    out<<'\n';
  }
  out<<primitive.str();

  std::uint32_t source=std::numeric_limits<std::uint32_t>::max();
  std::uint8_t opposite{};
  std::array<std::uint32_t,3> flip_face{};
  if(recovery.mutations.size()>=3U&&
     recovery.mutations[2].kind==WangOwnedLocalMutation::Kind::flip23)
    flip_face=recovery.mutations[2].feature;
  const auto expected_final=::cells(constraints,active(recovered_mesh));
  for(std::size_t slot=0;slot<mesh.cells().size()&&
      source==std::numeric_limits<std::uint32_t>::max();++slot) {
    const auto& cell=mesh.cells()[slot];
    if(cell.deleted)continue;
    for(unsigned candidate=0;candidate<4U;++candidate) {
      std::array<std::uint32_t,3> candidate_face{};
      unsigned cursor{};
      for(unsigned corner=0;corner<4U;++corner)
        if(corner!=candidate)candidate_face[cursor++]=cell.vertices[corner];
      auto sorted_candidate=candidate_face,sorted_expected=flip_face;
      std::sort(sorted_candidate.begin(),sorted_candidate.end());
      std::sort(sorted_expected.begin(),sorted_expected.end());
      if(sorted_candidate!=sorted_expected)continue;
      if(cell.vertices[candidate]!=recovery.mutations[2].apex)continue;
      WangOrderedTetMesh trial=mesh;
      if(!trial.flip23(static_cast<std::uint32_t>(slot),
                       static_cast<std::uint8_t>(candidate)).accepted)continue;
      if(::cells(constraints,active(trial))!=expected_final)continue;
      source=static_cast<std::uint32_t>(slot);
      opposite=static_cast<std::uint8_t>(candidate);
      break;
    }
  }
  if(source==std::numeric_limits<std::uint32_t>::max()) {
    out<<"first_flip23 missing\n";
    return;
  }
  const auto flip_apex=mesh.cells()[source].vertices[opposite];
  const auto before23=::cells(constraints,active(mesh));
  const auto flip23=mesh.flip23(source,opposite);
  const auto after23=::cells(constraints,active(mesh));
  const auto [erased23,created23]=delta(before23,after23);
  out<<"first_flip23 face";
  for(const auto vertex:flip_face)out<<' '<<vertex+1U;
  out<<" apex "<<flip_apex+1U
     <<" result "<<(flip23.accepted?1:0)<<'\n';
  out<<"first_flip23_erased_count "<<erased23.size()<<'\n';
  for(const auto& cell:erased23) {
    out<<"first_flip23_erased";
    for(const auto& point:cell)out<<' '<<point;
    out<<'\n';
  }
  out<<"first_flip23_created_count "<<created23.size()<<'\n';
  for(const auto& cell:created23) {
    out<<"first_flip23_created";
    for(const auto& point:cell)out<<' '<<point;
    out<<'\n';
  }
  out<<"first_flip23_cells "<<after23.size()<<'\n';
}

void write_scheduler(std::ofstream& out,
                     const CanonicalPlcRecoveryResult& recovery) {
  out<<"scheduler_event_count "
     <<recovery.segment_scheduler_attempt_trace.size()<<'\n';
  std::size_t split_index{};
  std::size_t immediate_remaining{};
  for(const auto& event:recovery.segment_scheduler_attempt_trace) {
    const auto find_vertex=[&](std::uint64_t id) -> const FrozenFacetVertex* {
      const auto found=std::find_if(recovery.constraints.vertices.begin(),
          recovery.constraints.vertices.end(),[&](const auto& vertex) {
            return vertex.id==id;
          });
      return found==recovery.constraints.vertices.end()?nullptr:&*found;
    };
    const auto* a=find_vertex(event.edge[0]);
    const auto* b=find_vertex(event.edge[1]);
    if(!a||!b)continue;
    std::array<std::string,2> edge{{point_key(a->position),point_key(b->position)}};
    std::sort(edge.begin(),edge.end());
    const auto immediate=immediate_remaining>0U;
    if(immediate_remaining>0U)--immediate_remaining;
    const auto outcome=static_cast<unsigned>(event.outcome);
    out<<"scheduler "<<edge[0]<<' '<<edge[1]<<' '
       <<event.round<<' '<<event.info_before<<' '<<event.search_depth<<' '
       <<event.full_search<<' '<<static_cast<unsigned>(event.steiner_mode)<<' '
       <<outcome<<' '<<immediate<<'\n';
    if(event.outcome==
       CanonicalPlcRecoveryResult::SegmentSchedulerOutcome::split) {
      immediate_remaining=
          recovery.segment_post_split_child_edge_calls[split_index++].size();
    }
  }
  out<<"facet_attempt_count "<<recovery.facet_recovery_attempt_trace.size()<<'\n';
  for(const auto& event:recovery.facet_recovery_attempt_trace) {
    auto face=event.facet;
    std::sort(face.begin(),face.end());
    out<<"facet_attempt";
    for(const auto id:face)out<<' '<<id;
    out<<' '<<static_cast<unsigned>(event.info)<<'\n';
  }
  out<<"facet_interior_attempts "<<recovery.facet_interior_steiner_attempts
     <<" insertions "<<recovery.facet_interior_steiner_insertions
     <<" boundary_splits "<<recovery.facet_splits<<'\n';
}

void write_local_sequence_probe(
    std::ofstream& out,const CanonicalPlcConstraintSet& constraints,
    const std::vector<std::array<std::uint32_t,4>>& seed_cells,
    const std::vector<std::array<std::uint32_t,4>>& point_to_tetrahedron) {
  const std::array<std::uint64_t,2> edge{{1U,7U}};
  WangOrderedTetMesh mesh(constraints.vertices.size(),seed_cells);
  if(!mesh.set_point_incidence(point_to_tetrahedron)) {
    out<<"local_sequence_missing 1 7\n";return;
  }
  const auto forward=recover_wang_segment_by_local_flips(
      constraints,edge,true,1U,mesh);
  out<<"local_sequence 1 7 forward "<<(forward.recovered?1:0)<<'\n';
  std::vector<GeometryCell> after_cells;
  std::vector<std::array<std::uint32_t,4>> active_cells;
  for(const auto& cell:mesh.cells())if(!cell.deleted)
    active_cells.push_back(cell.vertices);
  after_cells.reserve(active_cells.size());
  for(const auto& source:active_cells) {
    GeometryCell cell{};
    for(unsigned corner=0;corner<4U;++corner)
      cell[corner]=point_key(constraints.vertices[source[corner]].position);
    std::sort(cell.begin(),cell.end()); after_cells.push_back(cell);
  }
  std::sort(after_cells.begin(),after_cells.end());
  out<<"local_after_forward_count "<<after_cells.size()<<'\n';
  for(const auto& cell:after_cells) {
    out<<"local_after_forward";
    for(const auto& point:cell)out<<' '<<point;
    out<<'\n';
  }
  int reverse_direction=-20;
  const auto reverse_source=mesh.point_to_cell()[6];
  if(reverse_source>=0) {
    const auto& source=mesh.cells()[static_cast<std::size_t>(reverse_source)].vertices;
    const auto endpoint=std::find(source.begin(),source.end(),0U);
    if(endpoint!=source.end())
      reverse_direction=static_cast<int>(endpoint-source.begin());
  }
  out<<"local_after_forward_reverse_direction "<<reverse_direction<<" source";
  if(reverse_source>=0) {
    std::array<std::string,4> source{};
    for(unsigned corner=0;corner<4U;++corner) {
      const auto vertex=mesh.cells()[static_cast<std::size_t>(reverse_source)]
                            .vertices[corner];
      source[corner]=point_key(constraints.vertices[vertex].position);
    }
    for(const auto& point:source)out<<' '<<point;
  }
  out<<'\n';
  const auto reverse=recover_wang_segment_by_local_flips(
      constraints,edge,false,1U,mesh);
  const auto mesh_edge=[&]() {
    return std::any_of(mesh.cells().begin(),mesh.cells().end(),[](const auto& cell) {
      return !cell.deleted&&
          std::find(cell.vertices.begin(),cell.vertices.end(),0U)!=cell.vertices.end()&&
          std::find(cell.vertices.begin(),cell.vertices.end(),6U)!=cell.vertices.end();
    });
  };
  out<<"local_sequence 1 7 reverse "<<(reverse.recovered?1:0)<<" recovered "
     <<mesh_edge()<<'\n';
}

} // namespace

int main(int argc,char** argv) {
  if(argc!=3&&argc!=4) {
    std::cerr<<"usage: wang_prototype_reference_probe FIXTURE [INPUT] OUTPUT\n";
    return 2;
  }
  const std::string fixture=argv[1],output=argv[argc-1];
  const auto plc=fixture=="tetrahedron"?tetrahedron_plc():
                 fixture=="cube"?cube_plc():
                 fixture=="scheduler"?scheduler_plc():
                 (fixture=="scheduler_file"||fixture=="seed_file"||
                  fixture=="real_segment_file"||fixture=="full_file")&&argc==4?
                     read_scheduler_plc(argv[2]):CanonicalPlcConstraintSet{};
  if(plc.vertices.empty())return 3;

  if(fixture=="seed_file") {
    auto [seed_constraints,trace]=seed_trace(plc);
    std::ofstream out(output);
    if(!out)return 6;
    write_order(out,plc);
    out<<"seed_result "<<static_cast<unsigned>(trace.result.failure)<<' '
       <<static_cast<unsigned>(trace.result.invalid_reason)<<'\n';
    for(std::size_t stage=0;stage<trace.original_carrier_slots.size();++stage)
      out<<"seed_original_carrier "<<stage<<' '
         <<trace.original_carrier_slots[stage]<<'\n';
    for(std::size_t stage=0;stage<trace.original_carrier_cells.size();++stage) {
      out<<"seed_original_carrier_raw "<<stage;
      for(const auto vertex:trace.original_carrier_cells[stage])
        out<<' '<<(vertex==trace.ghost_vertex?"ghost":
            point_key(seed_constraints.vertices[vertex].position));
      out<<'\n';
    }
    for(std::size_t stage=0;stage<trace.original_location_results.size();++stage) {
      out<<"seed_original_location "<<stage<<' '
         <<trace.original_location_results[stage];
      for(const auto vertex:trace.original_location_cells[stage])
        out<<' '<<(vertex==trace.ghost_vertex?"ghost":
            point_key(seed_constraints.vertices[vertex].position));
      out<<'\n';
    }
    write_original_seed_slots(out,seed_constraints,trace);
    write_original_seed_plans(out,seed_constraints,trace);
    write_seed_stages(out,seed_constraints,trace);
    return out?0:7;
  }

  if(fixture=="scheduler"||fixture=="scheduler_file"||
     fixture=="real_segment_file") {
    const auto original_vertex_count=plc.vertices.size();
    auto [seed_constraints,trace]=seed_trace(plc);
    if(!trace.result.accepted()||trace.stages.size()!=9U)return 8;
    std::ofstream out(output);
    if(!out)return 6;
    write_order(out,plc);
    if(fixture!="real_segment_file") {
      write_seed_stages(out,seed_constraints,trace);
      if(std::getenv("WANG_REFERENCE_DIAGNOSTICS")!=nullptr)
        write_seed_p2t(out,seed_constraints,trace);
      write_seed_plan(out,seed_constraints,trace);
      write_owned_state(out,seed_constraints,trace.stages.back(),
                        trace.point_to_tetrahedron);
      write_direction_semantics(out,seed_constraints,trace.stages.back());
      write_synthetic_edge_contact(out);
      write_first_face_flip(out,seed_constraints,trace.stages.back());
      // The scheduler conformance record contains only source-backed stages.
      // The legacy geometry-only direction and local-flip diagnostics are not
      // part of the Wang implementation and must not enter this oracle stream.
      write_local_sequence_probe(out,seed_constraints,trace.stages.back(),
                                 trace.point_to_tetrahedron);
    }
    // The reference scheduler operates on DT's finite cells plus its retained
    // ghost hull.  Construct the same owned traversal state here; this probe
    // must not compare a finite-only shortcut against that source path.
    auto scheduler_constraints=seed_constraints;
    std::uint64_t ghost_id{};
    for(const auto& vertex:scheduler_constraints.vertices)
      ghost_id=std::max(ghost_id,vertex.id);
    // Source DT creates its ghost directly after the surface-node block,
    // before AddBox appends nodes.  Move from the compact seed trace's
    // [surface][box][ghost] numbering to its physical [surface][ghost][box]
    // numbering before observing the scheduler state.
    const auto scheduler_ghost=static_cast<std::uint32_t>(original_vertex_count);
    const auto compact_ghost=static_cast<std::uint32_t>(seed_constraints.vertices.size());
    scheduler_constraints.vertices.insert(
        scheduler_constraints.vertices.begin()+static_cast<std::ptrdiff_t>(scheduler_ghost),
        {ghost_id+1U,{0,0,0}});
    if(trace.ghost_vertex!=compact_ghost||trace.live_slot_stages.empty())return 9;
    const auto source_node=[&](std::uint32_t compact_node) {
      if(compact_node==compact_ghost)return scheduler_ghost;
      return compact_node>=scheduler_ghost?compact_node+1U:compact_node;
    };
    const auto source_cell=[&](WangOrderedTetMesh::Tet cell) {
      for(auto& node:cell)node=source_node(node);
      return cell;
    };
    // Do not synthesize a hull from an unordered face query.  The source
    // keeps the directed hull cells produced by the final AddBox transaction,
    // and their corner order is subsequently consumed by finddirection and
    // flip selection.  The trace records those cells independently of the
    // physical allocation slots; retain their encounter order here.
    std::vector<WangOrderedTetMesh::Tet> scheduler_cells;
    scheduler_cells.reserve(trace.live_slot_stages.back().size());
    for(const auto& slot:trace.live_slot_stages.back())
      scheduler_cells.push_back(source_cell(slot.cell));
    std::vector<WangOrderedTetMesh::Tet> scheduler_incidence(
        scheduler_constraints.vertices.size());
    for(std::uint32_t compact_node=0;compact_node<compact_ghost;++compact_node)
      scheduler_incidence[source_node(compact_node)]=
          source_cell(trace.point_to_tetrahedron[compact_node]);
    const auto ghost_cell=std::find_if(scheduler_cells.begin(),scheduler_cells.end(),
        [&](const auto& cell) {
          return std::find(cell.begin(),cell.end(),scheduler_ghost)!=cell.end();
        });
    if(ghost_cell==scheduler_cells.end())return 9;
    scheduler_incidence[scheduler_ghost]=*ghost_cell;
    WangOrderedTetMesh scheduler_mesh(scheduler_constraints.vertices.size(),scheduler_cells,
                                      static_cast<std::int32_t>(scheduler_ghost));
    std::vector<std::pair<std::size_t,WangOrderedTetMesh::Tet>> scheduler_slots;
    scheduler_slots.reserve(trace.final_live_slots.size());
    for(const auto& slot:trace.final_live_slots)
      scheduler_slots.push_back({slot.index,source_cell(slot.cell)});
    if(!scheduler_mesh.set_source_slot_layout(
           scheduler_slots,trace.final_slot_count,trace.final_vacancy_slots))return 9;
    if(!scheduler_mesh.set_point_incidence(scheduler_incidence))return 9;
    if(std::getenv("WANG_REFERENCE_DIAGNOSTICS")!=nullptr) {
      for(std::size_t vertex=0;vertex<scheduler_mesh.vertex_count();++vertex) {
        const auto carrier=scheduler_mesh.point_to_cell()[vertex];
        if(carrier<0)continue;
        std::cerr<<"scheduler_initial_p2t "<<vertex<<' '<<carrier;
        for(const auto corner:scheduler_mesh.cells()[static_cast<std::size_t>(carrier)].vertices)
          std::cerr<<' '<<corner;
        std::cerr<<'\n';
      }
    }
    const auto finite_cells=[&](const WangOrderedTetMesh& source) {
      std::vector<std::array<std::uint32_t,4>> result;
      for(const auto& cell:source.cells())
        if(!cell.deleted&&std::find(cell.vertices.begin(),cell.vertices.end(),scheduler_ghost)==cell.vertices.end())
          result.push_back(cell.vertices);
      return result;
    };
    if(fixture=="real_segment_file") {
      // Mirror the author probe's one native scheduler transaction.  The
      // scheduler owns the same buildBndInfo edge ordering; this output is a
      // diagnostic artifact only, never a production author dependency.
      const auto before_mesh=scheduler_mesh;
      for(std::size_t facet=0;facet<scheduler_constraints.facets.size();++facet) {
        out<<"boundary_facet "<<facet;
        for(const auto id:scheduler_constraints.facets[facet].vertices) {
          const auto vertex=std::find_if(scheduler_constraints.vertices.begin(),
              scheduler_constraints.vertices.end(),[&](const auto& candidate) {
                return candidate.id==id;
              });
          if(vertex==scheduler_constraints.vertices.end())return 9;
          out<<' '<<point_key(vertex->position);
        }
        out<<'\n';
      }
      // `prefix.surface_edges` has passed updateFliptype and may therefore
      // have had failed edges reversed. Reconstruct the immutable
      // buildBndInfo table for this one-transaction report.
      std::vector<WangOwnedSurfaceEdge> initial_edges;
      std::set<std::array<std::uint64_t,2>> seen_edges;
      for(const auto& facet:scheduler_constraints.facets)
        for(const auto corners:std::array<std::array<unsigned,2>,3>{
                {{{1U,2U}},{{2U,0U}},{{0U,1U}}}}) {
          const std::array<std::uint64_t,2> directed{{
              facet.vertices[corners[0]],facet.vertices[corners[1]]}};
          auto identity=directed;std::sort(identity.begin(),identity.end());
          if(!seen_edges.insert(identity).second)continue;
          const auto first=std::find_if(scheduler_constraints.vertices.begin(),
              scheduler_constraints.vertices.end(),[&](const auto& vertex) {
                return vertex.id==directed[0];
              });
          const auto second=std::find_if(scheduler_constraints.vertices.begin(),
              scheduler_constraints.vertices.end(),[&](const auto& vertex) {
                return vertex.id==directed[1];
              });
          if(first==scheduler_constraints.vertices.end()||
             second==scheduler_constraints.vertices.end())return 9;
          initial_edges.push_back({directed,{{static_cast<std::uint32_t>(
              first-scheduler_constraints.vertices.begin()),
              static_cast<std::uint32_t>(second-scheduler_constraints.vertices.begin())}},0});
        }
      for(std::size_t edge=0;edge<initial_edges.size();++edge) {
        const auto& surface=initial_edges[edge];
        const bool recovered=std::any_of(before_mesh.cells().begin(),
            before_mesh.cells().end(),[&](const auto& cell) {
              return !cell.deleted&&
                  std::find(cell.vertices.begin(),cell.vertices.end(),surface.indices[0])!=cell.vertices.end()&&
                  std::find(cell.vertices.begin(),cell.vertices.end(),surface.indices[1])!=cell.vertices.end();
            });
        out<<"boundary_edge "<<edge<<' '
           <<point_key(scheduler_constraints.vertices[surface.indices[0]].position)<<' '
           <<point_key(scheduler_constraints.vertices[surface.indices[1]].position)
           <<" info "<<(recovered?1:0)<<" recovered "<<(recovered?1:0)<<'\n';
      }
      std::optional<std::size_t> first_missing;
      for(std::size_t edge=0;edge<initial_edges.size();++edge)
        if(!std::any_of(before_mesh.cells().begin(),before_mesh.cells().end(),
                        [&](const auto& cell) {
                          return !cell.deleted&&
                              std::find(cell.vertices.begin(),cell.vertices.end(),
                                        initial_edges[edge].indices[0])!=cell.vertices.end()&&
                              std::find(cell.vertices.begin(),cell.vertices.end(),
                                        initial_edges[edge].indices[1])!=cell.vertices.end();
                        })) {first_missing=edge;break;}
      if(!first_missing) {out<<"first_missing_segment none\n";return out?0:7;}
      const auto& surface=initial_edges[*first_missing];
      out<<"first_missing_segment "<<*first_missing<<' '
         <<point_key(scheduler_constraints.vertices[initial_edges[
              *first_missing].indices[0]].position)<<' '
         <<point_key(scheduler_constraints.vertices[initial_edges[
              *first_missing].indices[1]].position)
         <<" info 0\n";
      write_indexed_state(out,"first_segment_before",scheduler_constraints,before_mesh);
      const auto recovered=recover_wang_segment_by_local_flips(
          scheduler_constraints,surface.vertices,false,1U,scheduler_mesh);
      out<<"first_segment_outcome "
         <<(recovered.recovered?1:0)<<" depth 1\n";
      write_indexed_state(out,"first_segment_after",scheduler_constraints,
                          scheduler_mesh);
      return out?0:7;
    }
    const auto local_prefix=run_wang_segment_scheduler_local_prefix(
        scheduler_constraints,scheduler_mesh);
    // The pinned scheduler checkpoint retains this exact mutable state when
    // it stops on the first full-search edge.  Its later geometry summary is
    // deliberately emitted from the full-search diagnostic copy, while its
    // ordered/indexed records remain this pre-full-search state.  Preserve
    // both states explicitly instead of restarting the scheduler and losing
    // the source queue history.
    const auto scheduler_prefix_mesh=scheduler_mesh;
    {
      std::vector<std::string> scheduler_state;
      for(const auto& edge:local_prefix.surface_edges) {
        auto endpoints=std::array<std::string,2>{{
            point_key(scheduler_constraints.vertices[edge.indices[0]].position),
            point_key(scheduler_constraints.vertices[edge.indices[1]].position)}};
        std::sort(endpoints.begin(),endpoints.end());
        std::ostringstream line;
        line<<"scheduler_local_prefix_edge "<<endpoints[0]<<' '<<endpoints[1]
            <<' '<<edge.info;
        scheduler_state.push_back(line.str());
      }
      std::sort(scheduler_state.begin(),scheduler_state.end());
      for(const auto& line:scheduler_state)out<<line<<'\n';
    }
    out<<"scheduler_local_prefix_next "<<local_prefix.next_round
       <<" attempts "<<local_prefix.attempts.size()<<'\n';
    for(const auto& attempt:local_prefix.attempts) {
      out<<"scheduler_local_prefix_attempt_event ";
      for(const auto vertex:attempt.edge)
        out<<point_key(scheduler_constraints.vertices[
            static_cast<std::size_t>(vertex-1U)].position)<<' ';
      out<<"info "<<attempt.info_before<<" level "<<attempt.search_depth
         <<" result "<<(attempt.outcome==WangOwnedSchedulerAttemptOutcome::recovered?1:0)
         <<'\n';
    }
    const auto prefix_cells=finite_cells(scheduler_mesh);
    write_cells(out,"scheduler_local_prefix",
                // `prefix_cells` uses source physical node indices
                // ([surface][ghost][box]), not the compact seed numbering.
                // Decoding through seed_constraints shifts every box vertex
                // after the inserted ghost and makes a valid scheduler state
                // appear to contain arbitrary coordinates.
                cells(scheduler_constraints,prefix_cells));
    for(std::size_t attempt=0;attempt<local_prefix.cells_after_attempt.size();++attempt) {
      const auto stage="scheduler_local_prefix_attempt_"+std::to_string(attempt);
      std::vector<std::array<std::uint32_t,4>> finite_attempt;
      for(const auto& cell:local_prefix.cells_after_attempt[attempt])
        if(std::find(cell.begin(),cell.end(),scheduler_ghost)==cell.end())
          finite_attempt.push_back(cell);
      write_cells(out,stage.c_str(),cells(scheduler_constraints,finite_attempt));
      std::vector<std::string> ordered;
      for(const auto& cell:local_prefix.cells_after_attempt[attempt]) {
        std::ostringstream record;record<<"scheduler_raw_local_prefix_attempt_"
                                      <<attempt;
        for(const auto vertex:cell)
          record<<' '<<(vertex==scheduler_ghost?"ghost":
              point_key(scheduler_constraints.vertices[vertex].position));
        ordered.push_back(record.str());
      }
      std::sort(ordered.begin(),ordered.end());
      for(const auto& record:ordered)out<<record<<'\n';
      std::vector<std::string> p2t_records;
      for(std::size_t vertex=0;vertex<scheduler_constraints.vertices.size();++vertex) {
        std::ostringstream record;
        record<<"scheduler_p2t_attempt_"<<attempt<<' '
           <<(vertex==scheduler_ghost?"ghost":
               point_key(scheduler_constraints.vertices[vertex].position));
        for(const auto carrier_vertex:local_prefix.p2t_after_attempt[attempt][vertex])
          record<<' '<<(carrier_vertex==scheduler_ghost?"ghost":
              point_key(scheduler_constraints.vertices[carrier_vertex].position));
        p2t_records.push_back(record.str());
      }
      std::sort(p2t_records.begin(),p2t_records.end());
      for(const auto& record:p2t_records)out<<record<<'\n';
      if(std::getenv("WANG_REFERENCE_DIAGNOSTICS")!=nullptr) {
      for(std::size_t pass=0;pass<local_prefix.cells_after_local_pass[attempt].size();++pass) {
        out<<"scheduler_local_prefix_attempt_"<<attempt<<"_pass_"<<pass
           <<"_mutations";
        for(const auto& mutation:local_prefix.mutations_after_local_pass[attempt][pass])
          out<<' '<<(mutation.kind==WangOwnedLocalMutation::Kind::flip23?"23":"32");
        out<<'\n';
        for(const auto& feature:local_prefix.features_after_local_pass[attempt][pass]) {
          out<<"scheduler_local_prefix_attempt_"<<attempt<<"_pass_"<<pass<<"_feature"
             <<' '<<static_cast<unsigned>(feature.kind);
          for(const auto vertex:feature.source_tetrahedron)
            out<<' '<<point_key(scheduler_constraints.vertices[vertex].position);
          out<<'\n';
        }
        for(std::size_t mutation=0;
            mutation<local_prefix.p2t_after_local_mutation[attempt][pass].size();++mutation)
          for(std::size_t vertex=0;vertex<scheduler_constraints.vertices.size();++vertex) {
            out<<"scheduler_primitive_p2t_attempt_"<<attempt<<"_pass_"<<pass
               <<"_mutation_"<<mutation<<' '
               <<(vertex==scheduler_ghost?"ghost":
                   point_key(scheduler_constraints.vertices[vertex].position));
            for(const auto carrier_vertex:
                local_prefix.p2t_after_local_mutation[attempt][pass][mutation][vertex])
              out<<' '<<(carrier_vertex==scheduler_ghost?"ghost":
                  point_key(scheduler_constraints.vertices[carrier_vertex].position));
            out<<'\n';
          }
        std::vector<std::string> pass_records;
        for(const auto& cell:local_prefix.cells_after_local_pass[attempt][pass]) {
          std::ostringstream record;
          record<<"scheduler_raw_local_prefix_attempt_"<<attempt<<"_pass_"<<pass;
          for(const auto vertex:cell)
            record<<' '<<(vertex==scheduler_ghost?"ghost":
                point_key(scheduler_constraints.vertices[vertex].position));
          pass_records.push_back(record.str());
        }
        std::sort(pass_records.begin(),pass_records.end());
        for(const auto& record:pass_records)out<<record<<'\n';
      }
      }
    }
    // Match the author probe's first directed/reverse recoverEdgebyFlip
    // decomposition.  This is diagnostic-only and operates on a fresh copy,
    // so it cannot alter the scheduler under comparison.
    if(!local_prefix.attempts.empty()) {
      WangOrderedTetMesh diagnostic_mesh(scheduler_constraints.vertices.size(),scheduler_cells,
                                         static_cast<std::int32_t>(scheduler_ghost));
      if(!diagnostic_mesh.set_point_incidence(scheduler_incidence))return 13;
      const auto first_edge=local_prefix.attempts.front().edge;
      const auto forward=recover_wang_segment_by_local_flips(
          scheduler_constraints,first_edge,false,1U,diagnostic_mesh);
      out<<"scheduler_first_operation_forward "<<(forward.recovered?1:0)<<'\n';
      write_cells(out,"scheduler_first_operation_forward_cells",
                  cells(scheduler_constraints,finite_cells(diagnostic_mesh)));
      const auto reverse=recover_wang_segment_by_local_flips(
          scheduler_constraints,first_edge,true,1U,diagnostic_mesh);
      out<<"scheduler_first_operation_reverse "<<(reverse.recovered?1:0)<<'\n';
      write_cells(out,"scheduler_first_operation_reverse_cells",
                  cells(scheduler_constraints,finite_cells(diagnostic_mesh)));
    }
    if(local_prefix.lost_edges.size()!=1U)return 10;
    const auto pending=local_prefix.surface_edges[
        local_prefix.lost_edges.front()].vertices;
    const auto full_forward=recover_wang_segment_by_local_flips(
        scheduler_constraints,pending,false,1000U,scheduler_mesh);
    const auto full_reverse=recover_wang_segment_by_local_flips(
        scheduler_constraints,pending,true,1000U,scheduler_mesh);
    const auto pre_full_cells=finite_cells(scheduler_mesh);
    const auto full=recover_wang_segment_by_full_search(
        scheduler_constraints,pending,1000U,scheduler_mesh);
    out<<"scheduler_full_search_walk "
       <<(full.failure==WangOwnedFullSearchFailure::none?0:1)
       <<" forward "<<(full_forward.recovered?1:0)
       <<" reverse "<<(full_reverse.recovered?1:0)
       <<" feature_count "<<full.features.size()<<'\n';
    for(const auto& feature:full.features) {
      out<<"scheduler_full_search_feature";
      std::vector<std::string> geometry;
      const unsigned count=feature.kind==WangOwnedFullSearchFeatureKind::edge?2U:3U;
      for(unsigned i=0;i<count;++i)
        geometry.push_back(point_key(
            scheduler_constraints.vertices[feature.vertices[i]].position));
      std::sort(geometry.begin(),geometry.end());
      for(const auto& point:geometry)out<<' '<<point;
      out<<'\n';
    }
    write_cells(out,"scheduler_full_search_pre_removal",
                cells(scheduler_constraints,pre_full_cells));
    out<<"scheduler_full_search_result "<<(full.recovered?1:0)<<'\n';
    const auto post_full_cells=finite_cells(scheduler_mesh);
    write_cells(out,"scheduler_full_search_post_removal",
                cells(scheduler_constraints,post_full_cells));
    // The author checkpoint is the mesh immediately after the explicit
    // forward/reverse/full recovery replay above.  Do not restart the whole
    // scheduler here: that would execute subsequent queue entries before
    // observing the state at which the author starts mode-one FHC.
    // Continue from the mesh which has just received the source-equivalent
    // forward, reverse, and full-search calls above.  The author snapshots
    // this same mutable instance immediately after the full-search call;
    // reverting to scheduler_prefix_mesh would discard the removeface slot
    // mutations that determine the following locate_pnt walk.
    auto pre_steiner_mesh=scheduler_mesh;
    // Source has performed the prefix's eight local attempts and attempted
    // the pending full-search edge once before the mode-one FHC checkpoint.
    // `run_wang_segment_scheduler_pre_steiner` starts a new queue, so using
    // its standalone count here was not source-equivalent continuation.
    out<<"scheduler_pre_steiner_next "<<(local_prefix.next_round+1U)
       <<" attempts "<<(local_prefix.attempts.size()+1U)<<'\n';
    const auto pre_steiner_cells=finite_cells(pre_steiner_mesh);
    write_cells(out,"scheduler_pre_steiner",
                cells(scheduler_constraints,pre_steiner_cells));
    write_ordered_cells(out,"scheduler_pre_steiner_ordered",
                        scheduler_constraints,pre_steiner_mesh);
    write_indexed_state(out,"scheduler_pre_steiner_indexed",
                        scheduler_constraints,pre_steiner_mesh);
    // The source reverses this queued surface edge before invoking
    // recoverEdge(..., fullsearch=1, info=1).  Locked-FHC's segment/face
    // intersection and hence its one-third placement are directed, so this
    // is control state rather than an interchangeable edge representation.
    auto steiner_edge=pending;
    std::swap(steiner_edge[0],steiner_edge[1]);
    auto mode_constraints=scheduler_constraints;
    auto mode_mesh=pre_steiner_mesh;
    const auto first_locked=insert_first_wang_locked_fhc_point(
        scheduler_constraints,steiner_edge,pre_steiner_mesh);
    out<<"scheduler_steiner1_first_insert result "
       <<(first_locked.inserted?1:0)<<" location "
       <<(first_locked.inserted?1:0)<<" point "
       <<point_key(first_locked.inserted_point)<<'\n';
    write_cells(out,"scheduler_steiner1_first_insert",
                cells(scheduler_constraints,first_locked.tetrahedra));
    const auto mode_one=recover_wang_segment_with_interior_steiner_mode1(
        mode_constraints,steiner_edge,mode_mesh);
    out<<"scheduler_steiner1_result "<<(mode_one.recovered?1:0)
       <<" point_count "<<mode_one.inserted_points.size()<<'\n';
    for(const auto& point:mode_one.inserted_points)
      out<<"scheduler_steiner1_point "<<point_key(point)<<'\n';
    std::vector<std::array<std::uint32_t,4>> mode_cells;
    for(const auto& cell:mode_mesh.cells())if(!cell.deleted)
      // `current_cells()` in the author probe reports finite elements only.
      // Keep hull traversal cells in the owned mesh, but do not reinterpret
      // the ghost placeholder as a finite coordinate in this comparison.
      if(std::find(cell.vertices.begin(),cell.vertices.end(),scheduler_ghost)==
         cell.vertices.end())
        mode_cells.push_back(cell.vertices);
    write_cells(out,"scheduler_steiner1",cells(mode_constraints,mode_cells));
    for(const auto& placement:mode_one.placements) {
      out<<"scheduler_steiner1_owned_placement face";
      for(const auto vertex:placement.face)
        out<<' '<<point_key(mode_constraints.vertices[vertex].position);
      out<<" edge";
      for(const auto vertex:placement.locking_edge)
        out<<' '<<point_key(mode_constraints.vertices[vertex].position);
      out<<" hit "<<point_key(placement.segment_face_hit)
         <<" weights "<<std::hexfloat<<placement.segment_face_weights[0]<<' '
         <<placement.segment_face_weights[1]<<std::defaultfloat
         <<" point "<<point_key(placement.point)<<'\n';
    }
    // Seed geometry is emitted by the independently retained AddBox trace.
    // The author DT subsequently recycles node slots, so using its later
    // slot table to decode the pre-recovery cell snapshot is invalid.
    write_cells(out,"seed",cells(seed_constraints,trace.result.tetrahedra));
    CanonicalPlcRecoveryOptions recovery_options;
    const auto recovery=recover_wang_constraints(plc,recovery_options);
    write_scheduler(out,recovery);
    return out?0:7;
  }
  CanonicalPlcRecoveryOptions recovery_options;
  if(fixture=="full_file") {
    recovery_options.maximum_vertices=1U<<18U;
    recovery_options.maximum_facets=1U<<19U;
    recovery_options.maximum_tetrahedra=1U<<22U;
  }
  const auto recovery=recover_wang_constraints(plc,recovery_options);
  if(!recovery.accepted()) {
    // A failed candidate can still be the precise fixture needed to cover a
    // residual Algorithm-2 branch.  Persist the owned trace before returning
    // so discovery never mistakes an incomplete final recovery for an
    // unobservable run.
    std::ofstream trace_out(output);
    if(trace_out) {
      trace_out<<"fixture "<<fixture<<'\n';
      trace_out<<"recovery_failure "<<static_cast<unsigned>(recovery.failure)<<'\n';
      trace_out<<"missing_edges "<<recovery.inspection.missing_edges.size()<<'\n';
      trace_out<<"missing_facets "<<recovery.inspection.missing_facets.size()<<'\n';
      write_scheduler(trace_out,recovery);
    }
    std::cerr<<"owned_recovery_failure "<<static_cast<unsigned>(recovery.failure)
             <<" missing_edges="<<recovery.inspection.missing_edges.size()
             <<" missing_facets="<<recovery.inspection.missing_facets.size()
             <<" vertices="<<recovery.constraints.vertices.size()
             <<" tetrahedra="<<recovery.tetrahedra.size()<<'\n';
    return 4;
  }
  const auto full=tetrahedralize_wang_constrained_plc(plc);
  if(!full.accepted())return 5;

  const auto initial=inspect_canonical_plc_tetrahedra(
      recovery.segment_stage_constraints,recovery.initial_tetrahedra);
  const auto segment=inspect_canonical_plc_tetrahedra(
      recovery.segment_stage_constraints,recovery.segment_stage_tetrahedra);
  const auto seed_cells=cells(recovery.segment_stage_constraints,
                              recovery.initial_tetrahedra);
  const auto segment_cells=cells(recovery.segment_stage_constraints,
                                 recovery.segment_stage_tetrahedra);
  const auto final_cells=cells_by_id(full.recovery.constraints,full.tetrahedra);

  std::ofstream out(output);
  if(!out)return 6;
  out<<"fixture "<<fixture<<'\n';
  write_order(out,plc);
  out<<"initial_missing_edges "<<initial.missing_edges.size()<<'\n';
  out<<"initial_missing_facets "<<initial.missing_facets.size()<<'\n';
  out<<"segment_missing_edges "<<segment.missing_edges.size()<<'\n';
  out<<"facet_missing_facets "<<recovery.inspection.missing_facets.size()<<'\n';
  out<<"interior_insertions "
     <<recovery.fhc_steiner_insertions+recovery.facet_interior_steiner_insertions
     <<'\n';
  out<<"boundary_insertions "<<recovery.edge_splits+recovery.facet_splits<<'\n';
  // Keep full-file experiments inspectable: the compact summary above says
  // whether recovery finished, while this trace retains every recoverFace
  // mode (notably the Algorithm-2 info=2 residual-facet pass) and boundary
  // insertion/restoration event needed to qualify a candidate fixture.
  write_scheduler(out,recovery);
  out<<"segment_topology_changed "<<(segment_cells!=seed_cells?1:0)<<'\n';
  write_cells(out,"seed",seed_cells);
  write_cells(out,"segment",segment_cells);
  write_cells(out,"final",final_cells);
  return out?0:7;
}
