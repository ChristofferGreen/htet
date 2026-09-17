#include "dt.h"
#include "dt_bw_parallel.h"
#include "mesh_io.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using GeometryCell=std::array<std::string,4>;
using GeometryFace=std::array<std::string,3>;
using SeedStages=std::vector<std::vector<GeometryCell>>;
using SeedSlots=std::vector<std::vector<std::pair<int,GeometryCell>>>;
using SeedP2T=std::vector<std::vector<std::string>>;

struct SeedPlanTrace {
  std::vector<GeometryCell> carrier;
  std::vector<GeometryCell> reads;
  std::vector<GeometryCell> working;
  std::vector<GeometryFace> found_boundary;
  std::vector<GeometryCell> adjusted;
  std::vector<GeometryFace> adjusted_boundary;
  std::vector<GeometryCell> cavity;
  std::vector<GeometryFace> faces;
  std::vector<GeometryCell> fill;
  std::vector<std::string> predicates;
};
struct ReferenceSeedTrace {
  SeedStages stages;
  SeedSlots liveSlots;
  SeedSlots liveRawSlots;
  SeedSlots originalSlots;
  SeedSlots originalRawSlots;
  SeedP2T p2tStages;
  std::vector<std::string> originalPlanOrder;
  SeedStages boxCarriers;
  SeedPlanTrace eighth;
};

struct SchedulerEvent {
  std::array<std::string,2> edge{};
  int edge_index{};
  int round{};
  int info{};
  int depth{};
  int full{};
  int steiner{};
  int outcome{};
  int immediate{};
};

std::string point_key(const double* point) {
  std::ostringstream out;
  out<<std::hexfloat<<point[0]<<','<<point[1]<<','<<point[2];
  return out.str();
}

std::string node_key(dt::DT& reference,int node) {
  return node==reference.ghost?"ghost":point_key(reference.Nodes[node].pt);
}

std::vector<std::string> current_p2t(dt::DT& reference,
                                     const std::set<int>& active_nodes) {
  std::vector<std::string> snapshot;
  for(const int node:active_nodes) {
    if(node==reference.ghost||reference.isDelNod(node))continue;
    const int carrier=reference.getP2T(node);
    if(carrier<0||carrier>=static_cast<int>(reference.Elems.size()) ||
       reference.isDelEle(carrier))continue;
    std::ostringstream line;line<<node_key(reference,node);
    for(int corner=0;corner<4;++corner)
      line<<' '<<node_key(reference,reference.Elems[carrier].form[corner]);
    snapshot.push_back(line.str());
  }
  return snapshot;
}

GeometryCell cell_geometry(dt::DT& reference,int index) {
  GeometryCell cell{};
  for(unsigned corner=0;corner<4U;++corner)
    cell[corner]=node_key(reference,reference.Elems[index].form[corner]);
  std::sort(cell.begin(),cell.end());
  return cell;
}

GeometryFace face_geometry(dt::DT& reference,int tet,int omitted) {
  GeometryFace face{};unsigned cursor{};
  for(int corner=0;corner<4;++corner)if(corner!=omitted)
    face[cursor++]=node_key(reference,reference.Elems[tet].form[corner]);
  std::sort(face.begin(),face.end());
  return face;
}

template<class Geometry>
void canonicalize(std::vector<Geometry>& value) {
  std::sort(value.begin(),value.end());
}

std::vector<GeometryCell> plan_cells(
    dt::DT& reference,const std::vector<int>& indices) {
  std::vector<GeometryCell> result;
  for(const auto index:indices)if(index>=0)
    result.push_back(cell_geometry(reference,index));
  canonicalize(result);return result;
}

std::vector<GeometryFace> plan_boundary(
    dt::DT& reference,const dt::BWPlan& plan) {
  std::vector<GeometryFace> result;
  for(std::size_t i=0;i<plan.workingCavity.size();++i) {
    const auto tet=plan.workingCavity[i];if(tet<0)continue;
    for(int face=0;face<4;++face)
      if(plan.boundaryFaces[i]&(1<<face))
        result.push_back(face_geometry(reference,tet,face));
  }
  canonicalize(result);return result;
}

std::vector<GeometryCell> current_cells(dt::DT& reference) {
  std::vector<GeometryCell> cells;
  for(std::size_t index=0;index<reference.Elems.size();++index) {
    if(reference.isDelEle(static_cast<int>(index))||
       reference.ishulltet(static_cast<int>(index)))continue;
    GeometryCell cell{};
    bool valid=true;
    for(unsigned corner=0;corner<4U;++corner) {
      const auto vertex=reference.Elems[index].form[corner];
      if(vertex<0||vertex==reference.ghost||
         static_cast<std::size_t>(vertex)>=reference.Nodes.size()) {
        valid=false;break;
      }
      cell[corner]=point_key(reference.Nodes[vertex].pt);
    }
    if(!valid)continue;
    std::sort(cell.begin(),cell.end());
    cells.push_back(cell);
  }
  std::sort(cells.begin(),cells.end());
  return cells;
}

std::vector<std::pair<int,GeometryCell>> current_slots(dt::DT& reference) {
  std::vector<std::pair<int,GeometryCell>> result;
  for(int cell=0;cell<static_cast<int>(reference.Elems.size());++cell)
    if(!reference.isDelEle(cell))result.push_back({cell,cell_geometry(reference,cell)});
  return result;
}

std::vector<std::pair<int,GeometryCell>> current_raw_slots(dt::DT& reference) {
  std::vector<std::pair<int,GeometryCell>> result;
  for(int cell=0;cell<static_cast<int>(reference.Elems.size());++cell) {
    if(reference.isDelEle(cell))continue;
    GeometryCell geometry{};
    for(unsigned corner=0;corner<4U;++corner)
      geometry[corner]=node_key(reference,reference.Elems[cell].form[corner]);
    result.push_back({cell,geometry});
  }
  return result;
}

std::vector<GeometryCell> output_cells(const dt::Mesh& mesh) {
  std::vector<GeometryCell> cells;
  for(const auto& source:mesh.T) {
    GeometryCell cell{};
    for(unsigned corner=0;corner<4U;++corner)
      cell[corner]=point_key(mesh.V[static_cast<std::size_t>(source[corner])].data());
    std::sort(cell.begin(),cell.end());
    cells.push_back(cell);
  }
  std::sort(cells.begin(),cells.end());
  return cells;
}

std::size_t missing_edges(dt::DT& reference) {
  std::size_t count{};
  for(std::size_t edge=0;edge<reference.SurEdgs.size();++edge)
    if(!reference.isDelSurEdg(static_cast<int>(edge))&&
       !reference.isMeshEdge(reference.SurEdgs[edge].iStart,
                             reference.SurEdgs[edge].iEnd))++count;
  return count;
}

std::size_t missing_facets(dt::DT& reference) {
  std::size_t count{};
  for(std::size_t facet=0;facet<reference.SurTris.size();++facet)
    if(!reference.isDelSurTri(static_cast<int>(facet))&&
       !reference.isMeshFace(reference.SurTris[facet].form[0],
                             reference.SurTris[facet].form[1],
                             reference.SurTris[facet].form[2]))++count;
  return count;
}

void write_cells(std::ostream& out,const char* stage,
                 const std::vector<GeometryCell>& cells) {
  out<<stage<<"_count "<<cells.size()<<'\n';
  for(const auto& cell:cells) {
    out<<stage;
    for(const auto& point:cell)out<<' '<<point;
    out<<'\n';
  }
}

void write_ordered_cells(std::ostream& out,const char* stage,dt::DT& reference) {
  std::vector<std::string> records;
  for(int cell=0;cell<static_cast<int>(reference.Elems.size());++cell) {
    if(reference.isDelEle(cell)||reference.ishulltet(cell))continue;
    std::ostringstream record;
    record<<stage;
    for(int corner=0;corner<4;++corner)
      record<<' '<<node_key(reference,reference.Elems[cell].form[corner]);
    records.push_back(record.str());
  }
  std::sort(records.begin(),records.end());
  out<<stage<<"_count "<<records.size()<<'\n';
  for(const auto& record:records)out<<record<<'\n';
}

// Raw mutable-state record used only to isolate the owned Locked-FHC routine
// from earlier seed/recovery ordering.  Geometry records are insufficient:
// DT::isMeshFace begins from P2T, whose carrier choices are observable.
void write_indexed_state(std::ostream& out,const char* stage,dt::DT& reference) {
  out<<stage<<"_node_count "<<reference.Nodes.size()<<'\n';
  for(int node=0;node<static_cast<int>(reference.Nodes.size());++node)
    if(!reference.isDelNod(node))
      out<<stage<<"_node "<<node<<' '<<node_key(reference,node)<<'\n';
  for(int cell=0;cell<static_cast<int>(reference.Elems.size());++cell) {
    if(reference.isDelEle(cell)||reference.ishulltet(cell))continue;
    out<<stage<<"_cell "<<cell;
    for(int corner=0;corner<4;++corner)out<<' '<<reference.Elems[cell].form[corner];
    out<<'\n';
  }
  for(int node=0;node<static_cast<int>(reference.Nodes.size());++node)
    if(!reference.isDelNod(node))
      out<<stage<<"_p2t "<<node<<' '<<reference.getP2T(node)<<'\n';
}

void write_hull_cells(std::ostream& out,const char* stage,dt::DT& reference) {
  for(int cell=0;cell<static_cast<int>(reference.Elems.size());++cell) {
    if(reference.isDelEle(cell)||!reference.ishulltet(cell))continue;
    out<<stage<<"_cell "<<cell;
    for(int corner=0;corner<4;++corner)out<<' '<<reference.Elems[cell].form[corner];
    out<<'\n';
  }
}

void write_order(std::ofstream& out,dt::DT& reference,const dt::Mesh& mesh) {
  std::vector<int> order(mesh.V.size());
  for(std::size_t i=0;i<order.size();++i)order[i]=static_cast<int>(i);
  reference.Hilbert(mesh.V,order);
  out<<"insertion_order";
  for(const auto vertex:order)out<<' '<<point_key(mesh.V[vertex].data());
  out<<'\n';
}

void write_seed_stages(std::ofstream& out,const SeedStages& stages) {
  out<<"seed_stage_count "<<stages.size()<<'\n';
  for(std::size_t stage=0;stage<stages.size();++stage) {
    out<<"seed_stage "<<stage<<' '<<stages[stage].size()<<'\n';
    for(const auto& cell:stages[stage]) {
      out<<"seed_stage_cell "<<stage;
      for(const auto& point:cell)out<<' '<<point;
      out<<'\n';
    }
  }
}

void write_seed_slots(std::ofstream& out,const SeedSlots& stages) {
  for(std::size_t stage=0;stage<stages.size();++stage)
    for(const auto& entry:stages[stage]) {
      out<<"seed_slot "<<stage<<' '<<entry.first;
      for(const auto& point:entry.second)out<<' '<<point;
      out<<'\n';
    }
}

void write_box_carriers(std::ofstream& out,const SeedStages& carriers) {
  out<<"seed_box_carrier_count "<<carriers.size()<<'\n';
  for(std::size_t box=0;box<carriers.size();++box) {
    out<<"seed_box_carrier "<<box;
    if(!carriers[box].empty())
      for(const auto& point:carriers[box].front())out<<' '<<point;
    out<<'\n';
  }
}

void write_owned_state(std::ostream& out,dt::DT& reference) {
  std::vector<std::string> cells,neighbours,p2t;
  for(int cell=0;cell<static_cast<int>(reference.Elems.size());++cell) {
    if(reference.isDelEle(cell)||reference.ishulltet(cell))continue;
    std::ostringstream cell_line;
    cell_line<<"state_cell";
    for(int corner=0;corner<4;++corner)
      cell_line<<' '<<node_key(reference,reference.Elems[cell].form[corner]);
    cells.push_back(cell_line.str());
    const auto owner=cell_geometry(reference,cell);
    for(int face=0;face<4;++face) {
      const auto neighbour=reference.getNeig(cell,face);
      std::ostringstream line;
      line<<"state_neighbour";
      for(const auto& point:owner)line<<' '<<point;
      const auto shared=face_geometry(reference,cell,face);
      for(const auto& point:shared)line<<' '<<point;
      if(neighbour<0||reference.ishulltet(neighbour))line<<" hull";
      else {
        const auto other=cell_geometry(reference,neighbour);
        for(const auto& point:other)line<<' '<<point;
      }
      neighbours.push_back(line.str());
    }
  }
  for(int node=0;node<static_cast<int>(reference.Nodes.size());++node) {
    if(node==reference.ghost||reference.isDelNod(node))continue;
    const auto carrier=reference.getP2T(node);
    if(carrier<0||reference.isDelEle(carrier)||reference.ishulltet(carrier))continue;
    std::ostringstream line;
    line<<"state_p2t "<<node_key(reference,node);
    const auto geometry=cell_geometry(reference,carrier);
    for(const auto& point:geometry)line<<' '<<point;
    p2t.push_back(line.str());
  }
  std::sort(cells.begin(),cells.end());
  std::sort(neighbours.begin(),neighbours.end());
  std::sort(p2t.begin(),p2t.end());
  out<<"state_cell_count "<<cells.size()<<'\n';
  for(const auto& line:cells)out<<line<<'\n';
  out<<"state_neighbour_count "<<neighbours.size()<<'\n';
  for(const auto& line:neighbours)out<<line<<'\n';
  out<<"state_p2t_count "<<p2t.size()<<'\n';
  for(const auto& line:p2t)out<<line<<'\n';
}

// Diagnostic-only first AutorecoverEdges transaction for an arbitrary input
// surface.  This intentionally uses DT's native SurEdg order and the exact
// recoverEdge call, while retaining the author source strictly as an oracle.
void write_real_first_segment_transaction(std::ostream& out,dt::DT& reference) {
  for(int cell=0;cell<static_cast<int>(reference.Elems.size());++cell) {
    if(reference.isDelEle(cell))continue;
    for(int local_edge=0;local_edge<6;++local_edge) {
      int* boundary=reference.BndEdg.find(
          reference.Elems[cell].form[dt::Egid[local_edge][0]],
          reference.Elems[cell].form[dt::Egid[local_edge][1]]);
      if(boundary)reference.SurEdgs[*boundary].info=1;
    }
  }
  for(int facet=0;facet<static_cast<int>(reference.SurTris.size());++facet) {
    if(reference.isDelSurTri(facet))continue;
    out<<"boundary_facet "<<facet;
    for(int corner=0;corner<3;++corner)
      out<<' '<<node_key(reference,reference.SurTris[facet].form[corner]);
    out<<'\n';
  }
  int target=-1;
  for(int edge=0;edge<static_cast<int>(reference.SurEdgs.size());++edge) {
    if(reference.isDelSurEdg(edge))continue;
    const auto& surface=reference.SurEdgs[edge];
    out<<"boundary_edge "<<edge<<' '<<node_key(reference,surface.iStart)<<' '
       <<node_key(reference,surface.iEnd)<<" info "<<surface.info
       <<" recovered "<<(reference.isRecBndEdg(edge)?1:0)<<'\n';
    if(target<0&&!reference.isRecBndEdg(edge))target=edge;
  }
  if(target<0) {out<<"first_missing_segment none\n";return;}
  const auto start=reference.SurEdgs[target].iStart;
  const auto end=reference.SurEdgs[target].iEnd;
  out<<"first_missing_segment "<<target<<' '<<node_key(reference,start)<<' '
     <<node_key(reference,end)<<" info "<<reference.SurEdgs[target].info<<'\n';
  {
    // finddirection sets transient visit bits, so inspect a copy and leave
    // the subsequently executed recoverEdge transaction untouched.
    dt::DT direction_probe=reference;
    int source=-1;
    const int direction=direction_probe.finddirection(start,end,source);
    out<<"first_segment_direction "<<direction<<" source "<<source;
    if(source>=0&&!direction_probe.isDelEle(source))
      for(int corner=0;corner<4;++corner)
        out<<' '<<node_key(direction_probe,direction_probe.Elems[source].form[corner]);
    out<<'\n';
    if(direction>=-14&&direction<=-1&&source>=0) {
      const int first=((-direction)>>2)&3;
      const int second=(-direction)&3;
      std::vector<int> shell,ring;
      (void)direction_probe.findShell(source,first,second,shell,ring);
      out<<"first_segment_shell";
      for(const int cell:shell)out<<' '<<cell;
      out<<" ring";
      for(const int point:ring)out<<' '<<point;
      out<<'\n';
      std::vector<int> removal_shell{{source}};
      const int removed=direction_probe.removeEdge(removal_shell,first,second,1);
      out<<"first_segment_remove_edge "<<removed<<" local "<<first<<' '
         <<second<<'\n';
      write_indexed_state(out,"first_segment_remove_edge_after",direction_probe);
    }
  }
  write_indexed_state(out,"first_segment_before",reference);
  reference.fliplevel=1-reference.SurEdgs[target].info*10;
  const int outcome=reference.recoverEdge(target,false,0);
  out<<"first_segment_outcome "<<outcome<<" depth "<<reference.fliplevel<<'\n';
  write_indexed_state(out,"first_segment_after",reference);
}

// Oracle-only reproduction of recoverEdgebyFlip's non-boundary vertex arm.
// The surface is first tetrahedralized normally.  We then insert one free
// point, place it on the open midpoint of the first literal boundary edge,
// and record the author's remove/disturb/split sequence on independent
// copies.  This intentionally describes the implementation's robustness
// branch; it is not a valid embedded PLC input fixture.
void write_interior_vertex_obstruction(std::ostream& out,dt::DT& reference) {
  if(reference.SurEdgs.empty()) return;
  int edge=-1;
  for(int candidate=0;candidate<static_cast<int>(reference.SurEdgs.size());++candidate)
    if(!reference.isMeshEdge(reference.SurEdgs[candidate].iStart,
                             reference.SurEdgs[candidate].iEnd)) {
      edge=candidate;break;
    }
  if(edge<0) {out<<"interior_obstruction_no_missing_edge\n";return;}
  const int first=reference.SurEdgs[edge].iStart;
  const int second=reference.SurEdgs[edge].iEnd;
  const double midpoint[3]{
      (reference.Nodes[first].pt[0]+reference.Nodes[second].pt[0])*0.5,
      (reference.Nodes[first].pt[1]+reference.Nodes[second].pt[1])*0.5,
      (reference.Nodes[first].pt[2]+reference.Nodes[second].pt[2])*0.5};
  // One normally inserted free point already has a star too large for the
  // 4-to-1 removal path in this retained source fixture.
  double center[3]{};
  for(int node=0;node<reference.nSurNodes;++node)
    for(int axis=0;axis<3;++axis)center[axis]+=reference.Nodes[node].pt[axis];
  for(double& coordinate:center)coordinate/=static_cast<double>(reference.nSurNodes);
  const std::array<std::array<double,3>,1> offsets{{{{-.010,-.010,-.010}}}};
  double near_first[3]{};
  for(int axis=0;axis<3;++axis)
    near_first[axis]=.80*reference.Nodes[first].pt[axis]+.10*midpoint[axis]+
                     .10*center[axis];
  std::vector<int> inserted_nodes;
  for(std::size_t offset_index=0;offset_index<offsets.size();++offset_index) {
    const auto& offset=offsets[offset_index];
    const int inserted=reference.addNode(near_first[0]+offset[0],near_first[1]+offset[1],
                                         near_first[2]+offset[2],1.0);
    std::vector<int> seed{{reference.getP2T(first)}};
    if(reference.BW_insert_vertex(inserted,seed,3)<=0) {
      out<<"interior_obstruction_setup_failed\n";return;
    }
    inserted_nodes.push_back(inserted);
  }
  int inserted=-1;
  for(const int candidate:inserted_nodes) {
    std::vector<int> star;
    reference.findSphere(candidate,star);
    if(star.size()>4U) {inserted=candidate;break;}
  }
  if(inserted<0) {out<<"interior_obstruction_no_large_star\n";return;}
  for(int axis=0;axis<3;++axis)reference.Nodes[inserted].pt[axis]=midpoint[axis];
  std::vector<int> selected_star;
  reference.findSphere(inserted,selected_star);
  for(const int cell:selected_star)
    for(int corner=0;corner<4;++corner)
      if(reference.Elems[cell].form[corner]==first) {
        reference.setP2T(first,cell);goto selected_carrier;
      }
selected_carrier:
  // Locking is author-supported state (destroyShortEdge rejects lockV) and
  // makes this a minimal direct witness of the source's remove->disturb->
  // split fallback rather than relying on incidental cloud geometry.
  reference.lockV.insert(inserted);
  out<<"interior_obstruction edge "<<edge<<" endpoints "<<first<<' '<<second
     <<" vertex "<<inserted<<" midpoint "<<point_key(midpoint)<<'\n';
  {
    dt::DT removal=reference;
    out<<"interior_obstruction_remove "<<removal.removePnt(inserted)
       <<" nodes "<<removal.Nodes.size()<<" surface_edges "
       <<removal.SurEdgs.size()<<'\n';
  }
  {
    dt::DT disturbance=reference;
    const int disturbed=disturbance.disturbPnt(inserted);
    out<<"interior_obstruction_disturb "<<disturbed<<" position "
       <<node_key(disturbance,inserted)<<'\n';
  }
  {
    dt::DT direction_probe=reference;
    int source=-1;
    const int direction=direction_probe.finddirection(first,second,source);
    out<<"interior_obstruction_direction "<<direction<<" source "<<source<<'\n';
  }
  {
    dt::DT scheduled=reference;
    const auto first_child=scheduled.SurEdgs.size();
    std::queue<int> lost;
    lost.push(edge);
    const int recovered=scheduled.recoverEdges(lost,1,0);
    out<<"interior_obstruction_scheduler recovered "<<recovered
       <<" queued "<<lost.size()<<" child_begin "<<first_child
       <<" child_end "<<scheduled.SurEdgs.size()<<'\n';
    for(std::size_t child=first_child;child<scheduled.SurEdgs.size();++child)
      out<<"interior_obstruction_child "<<child<<" endpoints "
         <<scheduled.SurEdgs[child].iStart<<' '<<scheduled.SurEdgs[child].iEnd
         <<" info "<<scheduled.SurEdgs[child].info<<" recovered "
         <<scheduled.isRecBndEdg(static_cast<int>(child))<<'\n';
  }
  const int outcome=reference.recoverEdgebyFlip(edge,0,2);
  out<<"interior_obstruction_recover "<<outcome<<" boundary "
     <<reference.isbndpnt(inserted)<<" surface_edges "<<reference.SurEdgs.size()
     <<" surface_tris "<<reference.SurTris.size()<<'\n';
  write_indexed_state(out,"interior_obstruction_after",reference);
}

template<class Geometry>
void write_geometry(std::ofstream& out,const char* label,
                    const std::vector<Geometry>& values) {
  out<<label<<"_count "<<values.size()<<'\n';
  for(const auto& value:values) {
    out<<label;
    for(const auto& point:value)out<<' '<<point;
    out<<'\n';
  }
}

void write_seed_plan(std::ofstream& out,const SeedPlanTrace& trace) {
  write_geometry(out,"seed_plan_carrier",trace.carrier);
  write_geometry(out,"seed_plan_reads",trace.reads);
  write_geometry(out,"seed_plan_working",trace.working);
  write_geometry(out,"seed_plan_found_boundary",trace.found_boundary);
  write_geometry(out,"seed_plan_adjusted",trace.adjusted);
  write_geometry(out,"seed_plan_adjusted_boundary",trace.adjusted_boundary);
  write_geometry(out,"seed_plan_cavity",trace.cavity);
  write_geometry(out,"seed_plan_faces",trace.faces);
  write_geometry(out,"seed_plan_fill",trace.fill);
  out<<"seed_plan_predicate_count "<<trace.predicates.size()<<'\n';
  for(const auto& predicate:trace.predicates)
    out<<"seed_plan_predicate "<<predicate<<'\n';
}

ReferenceSeedTrace reference_seed_stages(const dt::Mesh& source,dt::Args args) {
  auto mesh=source;
  dt::DT staged;
  if(!staged.dt_init(mesh,args))return {};
  staged.buildPntInfo(mesh);
  staged.ghost=staged.addNode();
  std::vector<int> order(staged.nSurNodes);
  for(int i=0;i<staged.nSurNodes;++i)order[i]=i;
  staged.Hilbert(mesh.V,order);
  if(order.size()<4U)return {};

  const double epsilon=1e-30;
  const double boxsize=std::sqrt(staged.norm2(
      staged.maxW[0]-staged.minW[0],staged.maxW[0]-staged.minW[0],
      staged.maxW[2]-staged.minW[0]));
  std::size_t selected=1U;
  while(selected+1U<order.size()&&
        staged.distance(staged.Nodes[order[0]].pt,
                        staged.Nodes[order[selected]].pt)/boxsize<epsilon)
    ++selected;
  std::swap(order[1],order[selected]);
  selected=2U;
  double first[3],candidate[3],normal[3];
  for(unsigned axis=0;axis<3U;++axis)
    first[axis]=staged.Nodes[order[1]].pt[axis]-
                staged.Nodes[order[0]].pt[axis];
  for(;;) {
    for(unsigned axis=0;axis<3U;++axis)
      candidate[axis]=staged.Nodes[order[selected]].pt[axis]-
                      staged.Nodes[order[0]].pt[axis];
    staged.cross(first,candidate,normal);
    // Retain BndPntInst's expression verbatim: the source multiplies back
    // by boxsize after division (it does not normalize by squared scale).
    if(staged.lenvec(normal)/boxsize*boxsize>=epsilon||
       selected+1U==order.size())break;
    ++selected;
  }
  std::swap(order[2],order[selected]);
  selected=3U;
  double orientation=dt::GEOM_FUNC::orient3d(
      staged.Nodes[order[0]].pt,staged.Nodes[order[1]].pt,
      staged.Nodes[order[2]].pt,staged.Nodes[order[selected]].pt);
  while(std::fabs(orientation)<epsilon&&selected+1U<order.size()) {
    ++selected;
    orientation=dt::GEOM_FUNC::orient3d(
        staged.Nodes[order[0]].pt,staged.Nodes[order[1]].pt,
        staged.Nodes[order[2]].pt,staged.Nodes[order[selected]].pt);
  }
  if(std::fabs(orientation)<epsilon)return {};
  std::swap(order[3],order[selected]);
  if(orientation>0.0)std::swap(order[0],order[1]);

  const int first_tet=staged.addElem(order[0],order[1],order[2],order[3]);
  const int hull0=staged.addElem(order[1],order[2],order[3],staged.ghost);
  const int hull1=staged.addElem(order[2],order[0],order[3],staged.ghost);
  const int hull2=staged.addElem(order[0],order[1],order[3],staged.ghost);
  const int hull3=staged.addElem(order[0],order[2],order[1],staged.ghost);
  staged.bond(first_tet,0,hull0,3);staged.bond(first_tet,1,hull1,3);
  staged.bond(first_tet,2,hull2,3);staged.bond(first_tet,3,hull3,3);
  staged.bond(hull3,0,hull0,2);staged.bond(hull3,2,hull1,2);
  staged.bond(hull3,1,hull2,2);staged.bond(hull0,0,hull1,1);
  staged.bond(hull1,0,hull2,1);staged.bond(hull2,0,hull0,1);
  ReferenceSeedTrace trace;
  // The fixture is configured for one worker.  Spell out that serial path
  // here solely to record each commit's allocator state; each operation is
  // still performed by the pinned author's public BW entry point.
  int anchor=order[0];
  std::set<int> active_nodes{order[0],order[1],order[2],order[3]};
  for(std::size_t index=4U;index<order.size();++index) {
    std::vector<int> carrier{staged.getP2T(anchor)};
    {
      int located=carrier[0];
      const int location=staged.locate_pnt(order[index],located);
      std::ostringstream line;
      line<<"seed_original_location "<<(index-4U)<<' '<<location;
      for(const auto& point:cell_geometry(staged,located))line<<' '<<point;
      trace.originalPlanOrder.push_back(line.str());
    }
    // The real-fixture seed gate compares the exact plan before it is
    // committed. This is a read-only author-plan observation on the staged
    // mesh, retained separately from production code.
    {
      dt::BWPlan plan;
      plan.request=dt::makeBWRequest(staged,order[index],carrier,0);
      for(int tet=0;tet<static_cast<int>(staged.Elems.size());++tet) {
        if(staged.isDelEle(tet)||!staged.ishulltet(tet))continue;
        const int inner=staged.getNeig(tet,3);
        std::ostringstream line;line<<"seed_original_hull_predicate "<<(index-4U);
        for(const auto& point:cell_geometry(staged,tet))line<<' '<<point;
        const auto* form=staged.Elems[tet].form;
        line<<' '<<std::hexfloat<<dt::GEOM_FUNC::orient3d(
            staged.Nodes[form[0]].pt,staged.Nodes[form[1]].pt,
            staged.Nodes[form[2]].pt,staged.Nodes[order[index]].pt)
            <<std::defaultfloat<<' ';
        if(inner>=0&&!staged.ishulltet(inner)) {
          const auto* inner_form=staged.Elems[inner].form;
          line<<(staged.insphere_s(inner_form[0],inner_form[1],inner_form[2],
                                  inner_form[3],order[index])<0.0?-1:1);
        } else line<<0;
        trace.originalPlanOrder.push_back(line.str());
      }
      if(dt::findBWCavity(staged,plan)!=dt::BWStatus::Ready ||
         dt::adjustBWCavity(staged,plan)!=dt::BWStatus::Ready)return {};
      for(const auto tet:plan.workingCavity)if(tet>=0) {
        std::ostringstream line;line<<"seed_original_working "<<(index-4U);
        for(const auto& point:cell_geometry(staged,tet))line<<' '<<point;
        trace.originalPlanOrder.push_back(line.str());
      }
      if(dt::prepareBWFill(staged,plan)!=dt::BWStatus::Ready)return {};
      for(const auto tet:plan.cavity) {
        std::ostringstream line;line<<"seed_original_adjusted "<<(index-4U);
        for(const auto& point:cell_geometry(staged,tet))line<<' '<<point;
        trace.originalPlanOrder.push_back(line.str());
      }
    }
    {
      std::ostringstream line;
      line<<"seed_original_carrier "<<(index-4U)<<' '<<carrier[0];
      trace.originalPlanOrder.push_back(line.str());
    }
    {
      std::ostringstream line;
      line<<"seed_original_carrier_raw "<<(index-4U);
      for(unsigned corner=0;corner<4U;++corner)
        line<<' '<<node_key(staged,staged.Elems[carrier[0]].form[corner]);
      trace.originalPlanOrder.push_back(line.str());
    }
    if(index==8U) {
      dt::BWPlan plan;
      plan.request=dt::makeBWRequest(staged,order[index],carrier,0);
      if(dt::findBWCavity(staged,plan)!=dt::BWStatus::Ready ||
         dt::adjustBWCavity(staged,plan)!=dt::BWStatus::Ready ||
         dt::prepareBWFill(staged,plan)!=dt::BWStatus::Ready)return {};
      for(std::size_t queue=0;queue<plan.workingCavity.size();++queue) {
        const auto tet=plan.workingCavity[queue];if(tet<0)continue;
        std::ostringstream line;line<<"seed_original_working 4 "<<tet;
        for(const auto& point:cell_geometry(staged,tet))line<<' '<<point;
        trace.originalPlanOrder.push_back(line.str());
      }
      for(std::size_t face=0;face<plan.faces.size();++face) {
        std::ostringstream line;line<<"seed_original_face 4 "<<face;
        auto geometry=plan.faces[face].vertices;std::array<std::string,3> points{};
        for(unsigned corner=0;corner<3U;++corner)points[corner]=node_key(staged,geometry[corner]);
        std::sort(points.begin(),points.end());
        for(const auto& point:points)line<<' '<<point;
        trace.originalPlanOrder.push_back(line.str());
      }
    }
    if(staged.BW_insert_vertex(order[index],carrier,0)!=1)return {};
    anchor=order[index];
    active_nodes.insert(order[index]);
    trace.p2tStages.push_back(current_p2t(staged,active_nodes));
    trace.originalSlots.push_back(current_slots(staged));
    trace.originalRawSlots.push_back(current_raw_slots(staged));
  }

  trace.stages={current_cells(staged)};
  trace.liveSlots={current_slots(staged)};
  trace.liveRawSlots={current_raw_slots(staged)};
  double low[3],high[3];
  for(unsigned axis=0;axis<3U;++axis) {
    const auto centre=.5*(staged.minW[axis]+staged.maxW[axis]);
    const auto half=.5*(staged.maxW[axis]-staged.minW[axis]);
    low[axis]=centre-2.0*half;high[axis]=centre+2.0*half;
  }
  const double box[8][3]={{low[0],low[1],low[2]},
      {high[0],low[1],low[2]},{high[0],high[1],low[2]},
      {low[0],high[1],low[2]},{low[0],low[1],high[2]},
      {high[0],low[1],high[2]},{high[0],high[1],high[2]},
      {low[0],high[1],high[2]}};
  int searchtet=0;
  while(staged.isDelEle(searchtet)||staged.ishulltet(searchtet))++searchtet;
  for(unsigned box_index=0;box_index<8U;++box_index) {
    const auto& point=box[box_index];
    const int node=staged.addNode(point[0],point[1],point[2],0);
    std::vector<int> carrier{searchtet};
    trace.boxCarriers.push_back(plan_cells(staged,carrier));
    if(box_index==0U) {
      dt::BWPlan plan;
      plan.request=dt::makeBWRequest(staged,node,carrier,0);
      if(dt::findBWCavity(staged,plan)!=dt::BWStatus::Ready ||
         dt::adjustBWCavity(staged,plan)!=dt::BWStatus::Ready ||
         dt::prepareBWFill(staged,plan)!=dt::BWStatus::Ready)return {};
      for(const auto tet:plan.workingCavity)if(tet>=0) {
        std::ostringstream line;line<<"seed_box0_working "<<tet;
        for(unsigned corner=0;corner<4U;++corner)
          line<<' '<<node_key(staged,staged.Elems[tet].form[corner]);
        trace.originalPlanOrder.push_back(line.str());
      }
      for(std::size_t face=0;face<plan.faces.size();++face) {
        std::ostringstream line;line<<"seed_box0_face "<<face;
        for(unsigned corner=0;corner<3U;++corner)
          line<<' '<<node_key(staged,plan.faces[face].vertices[corner]);
        trace.originalPlanOrder.push_back(line.str());
      }
    }
    if(box_index==4U) {
      dt::BWPlan plan;
      plan.request=dt::makeBWRequest(staged,node,carrier,0);
      if(dt::findBWCavity(staged,plan)!=dt::BWStatus::Ready ||
         dt::adjustBWCavity(staged,plan)!=dt::BWStatus::Ready ||
         dt::prepareBWFill(staged,plan)!=dt::BWStatus::Ready)return {};
      for(const auto tet:plan.workingCavity)if(tet>=0) {
        std::ostringstream line;line<<"seed_box4_working "<<tet;
        for(unsigned corner=0;corner<4U;++corner)
          line<<' '<<node_key(staged,staged.Elems[tet].form[corner]);
        trace.originalPlanOrder.push_back(line.str());
      }
      for(std::size_t face=0;face<plan.faces.size();++face) {
        std::ostringstream line;line<<"seed_box4_face "<<face;
        for(unsigned corner=0;corner<3U;++corner)
          line<<' '<<node_key(staged,plan.faces[face].vertices[corner]);
        trace.originalPlanOrder.push_back(line.str());
      }
    }
    if(box_index==3U) {
      for(int tet=0;tet<static_cast<int>(staged.Elems.size());++tet) {
        if(staged.isDelEle(tet)||staged.ishulltet(tet))continue;
        const auto& element=staged.Elems[tet];
        const double raw=staged.insphere_s(
            element.form[0],element.form[1],element.form[2],element.form[3],node);
        std::ostringstream line;line<<"seed_box3_predicate";
        for(unsigned corner=0;corner<4U;++corner)
          line<<' '<<node_key(staged,element.form[corner]);
        const int orientation=dt::GEOM_FUNC::orient3d(
            staged.Nodes[element.form[0]].pt,staged.Nodes[element.form[1]].pt,
            staged.Nodes[element.form[2]].pt,staged.Nodes[element.form[3]].pt)<0.0?-1:1;
        line<<' '<<orientation<<' '<<(raw<0.0?-1:(raw>0.0?1:0))
            <<' '<<(raw<0.0?-1:(raw>0.0?1:0))<<' '<<(raw<=0.0?1:0);
        trace.originalPlanOrder.push_back(line.str());
      }
      dt::BWPlan plan;
      plan.request=dt::makeBWRequest(staged,node,carrier,0);
      if(dt::findBWCavity(staged,plan)!=dt::BWStatus::Ready ||
         dt::adjustBWCavity(staged,plan)!=dt::BWStatus::Ready ||
         dt::prepareBWFill(staged,plan)!=dt::BWStatus::Ready)return {};
      for(const auto tet:plan.workingCavity)if(tet>=0) {
        std::ostringstream line;line<<"seed_box3_working "<<tet;
        for(unsigned corner=0;corner<4U;++corner)
          line<<' '<<node_key(staged,staged.Elems[tet].form[corner]);
        trace.originalPlanOrder.push_back(line.str());
      }
      for(std::size_t face=0;face<plan.faces.size();++face) {
        std::ostringstream line;line<<"seed_box3_face "<<face;
        for(unsigned corner=0;corner<3U;++corner)
          line<<' '<<node_key(staged,plan.faces[face].vertices[corner]);
        trace.originalPlanOrder.push_back(line.str());
      }
    }
    if(box_index==6U) {
      dt::BWPlan plan;
      plan.request=dt::makeBWRequest(staged,node,carrier,0);
      if(dt::findBWCavity(staged,plan)!=dt::BWStatus::Ready ||
         dt::adjustBWCavity(staged,plan)!=dt::BWStatus::Ready ||
         dt::prepareBWFill(staged,plan)!=dt::BWStatus::Ready)return {};
      for(std::size_t queue=0;queue<plan.workingCavity.size();++queue) {
        const auto tet=plan.workingCavity[queue];if(tet<0)continue;
        std::ostringstream line;line<<"seed_box6_working "<<tet;
        for(unsigned corner=0;corner<4U;++corner)
          line<<' '<<node_key(staged,staged.Elems[tet].form[corner]);
        trace.originalPlanOrder.push_back(line.str());
      }
      for(std::size_t face=0;face<plan.faces.size();++face) {
        std::ostringstream line;line<<"seed_box6_face "<<face;
        for(unsigned corner=0;corner<3U;++corner)
          line<<' '<<node_key(staged,plan.faces[face].vertices[corner]);
        trace.originalPlanOrder.push_back(line.str());
      }
    }
    if(box_index==7U) {
      // Predicate diagnostics must not execute on the mesh that will be
      // committed below.  The pinned predicate implementation owns mutable
      // floating-point scratch state, and running its exact diagnostic path
      // before `BW_insert_vertex` changes a subsequent predicate result.
      // A value-copy keeps this oracle observation non-invasive.
      auto diagnostic_mesh=staged;
      auto& inspected=diagnostic_mesh;
      auto& diagnostic=trace.eighth;
      diagnostic.carrier=plan_cells(inspected,carrier);
      {
        std::ostringstream line;
        line<<"seed_plan_query "<<node_key(inspected,node);
        trace.originalPlanOrder.push_back(line.str());
      }
      for(int tet=0;tet<static_cast<int>(inspected.Elems.size());++tet) {
        if(inspected.isDelEle(tet)||inspected.ishulltet(tet))continue;
        const auto& element=inspected.Elems[tet];
        const auto raw=dt::GEOM_FUNC::insphere(
            inspected.Nodes[element.form[0]].pt,inspected.Nodes[element.form[1]].pt,
            inspected.Nodes[element.form[2]].pt,inspected.Nodes[element.form[3]].pt,
            inspected.Nodes[node].pt);
        const auto exact=dt::GEOM_FUNC::insphereexact(
            inspected.Nodes[element.form[0]].pt,inspected.Nodes[element.form[1]].pt,
            inspected.Nodes[element.form[2]].pt,inspected.Nodes[element.form[3]].pt,
            inspected.Nodes[node].pt);
        const auto resolved=inspected.insphere_s(
            element.form[0],element.form[1],element.form[2],element.form[3],node);
        std::ostringstream line;
        const auto geometry=cell_geometry(inspected,tet);
        for(const auto& vertex:geometry)line<<vertex<<' ';
        line<<std::hexfloat<<raw<<' '<<exact<<' '<<resolved<<' '
            <<(resolved<=0.0);
        diagnostic.predicates.push_back(line.str());
      }
      std::sort(diagnostic.predicates.begin(),diagnostic.predicates.end());
      auto request=dt::makeBWRequest(inspected,node,carrier,0);
      request.trackAccess=false;
      if(!carrier.empty()) {
        const auto& initial=inspected.Elems[carrier[0]];
        static constexpr int dnc[4][3]={{1,2,3},{3,2,0},{0,1,3},{2,1,0}};
        for(int face=0;face<4;++face) {
          const auto value=dt::GEOM_FUNC::orient3d(
              inspected.Nodes[initial.form[dnc[face][0]]].pt,
              inspected.Nodes[initial.form[dnc[face][1]]].pt,
              inspected.Nodes[initial.form[dnc[face][2]]].pt,
              inspected.Nodes[node].pt);
          std::ostringstream line;line<<"seed_plan_location_face "<<face<<' '
              <<std::hexfloat<<value;
          trace.originalPlanOrder.push_back(line.str());
        }
      }
      for(int cell=0;cell<static_cast<int>(inspected.Elems.size());++cell) {
        if(inspected.isDelEle(cell)||!inspected.ishulltet(cell))continue;
        const auto& hull=inspected.Elems[cell];
        const auto value=dt::GEOM_FUNC::orient3d(inspected.Nodes[hull.form[0]].pt,
            inspected.Nodes[hull.form[1]].pt,inspected.Nodes[hull.form[2]].pt,
            inspected.Nodes[node].pt);
        std::ostringstream line;line<<"seed_plan_hull_eval "<<cell<<' '
            <<std::hexfloat<<value<<' '<<(value<0.0?1:0);
        trace.originalPlanOrder.push_back(line.str());
      }
      {
        int location=carrier.front();
        const auto location_result=inspected.locate_pnt(node,location);
        std::ostringstream line;line<<"seed_plan_source_location "
            <<location_result<<' '<<location;
        trace.originalPlanOrder.push_back(line.str());
        const auto& located=inspected.Elems[location];
        static constexpr int dnc[4][3]={{1,2,3},{3,2,0},{0,1,3},{2,1,0}};
        for(int face=0;face<4;++face) {
          const auto value=dt::GEOM_FUNC::orient3d(
              inspected.Nodes[located.form[dnc[face][0]]].pt,
              inspected.Nodes[located.form[dnc[face][1]]].pt,
              inspected.Nodes[located.form[dnc[face][2]]].pt,
              inspected.Nodes[node].pt);
          std::ostringstream predicate;predicate<<"seed_plan_source_endpoint_face "
              <<face<<' '<<std::hexfloat<<value;
          trace.originalPlanOrder.push_back(predicate.str());
        }
      }
      dt::BWPlan plan;plan.request=request;
      if(dt::findBWCavity(inspected,plan)!=dt::BWStatus::Ready)return {};
      diagnostic.reads=plan_cells(inspected,plan.readTets);
      diagnostic.working=plan_cells(inspected,plan.workingCavity);
      diagnostic.found_boundary=plan_boundary(inspected,plan);
      if(dt::adjustBWCavity(inspected,plan)!=dt::BWStatus::Ready)return {};
      diagnostic.adjusted=plan_cells(inspected,plan.workingCavity);
      diagnostic.adjusted_boundary=plan_boundary(inspected,plan);
      if(dt::prepareBWFill(inspected,plan)!=dt::BWStatus::Ready)return {};
      diagnostic.cavity=plan_cells(inspected,plan.cavity);
      for(const auto tet:plan.workingCavity)if(tet>=0) {
        std::ostringstream line;line<<"seed_plan_raw_working "<<tet;
        for(unsigned corner=0;corner<4U;++corner)
          line<<' '<<node_key(inspected,inspected.Elems[tet].form[corner]);
        trace.originalPlanOrder.push_back(line.str());
        for(int face=0;face<4;++face) {
          std::ostringstream neighbour;
          neighbour<<"seed_plan_raw_neighbor "<<tet<<' '<<face<<' '
                   <<inspected.getNeig(tet,face);
          trace.originalPlanOrder.push_back(neighbour.str());
        }
      }
      for(const auto& face:plan.faces) {
        {
          std::ostringstream line;line<<"seed_plan_raw_face";
          for(unsigned i=0;i<3U;++i)
            line<<' '<<node_key(inspected,face.vertices[i]);
          trace.originalPlanOrder.push_back(line.str());
        }
        {
          std::ostringstream line;line<<"seed_plan_raw_fill "<<node_key(inspected,node);
          for(unsigned i=0;i<3U;++i)
            line<<' '<<node_key(inspected,face.vertices[i]);
          trace.originalPlanOrder.push_back(line.str());
        }
        GeometryFace geometry{};
        for(unsigned i=0;i<3U;++i)geometry[i]=node_key(inspected,face.vertices[i]);
        std::sort(geometry.begin(),geometry.end());
        diagnostic.faces.push_back(geometry);
        GeometryCell fill{{node_key(inspected,node),geometry[0],geometry[1],geometry[2]}};
        std::sort(fill.begin(),fill.end());diagnostic.fill.push_back(fill);
      }
      canonicalize(diagnostic.faces);canonicalize(diagnostic.fill);
    }
    if(staged.BW_insert_vertex(node,carrier,0)!=1)return {};
    searchtet=carrier[0];staged.setbndpnt(node);
    active_nodes.insert(node);
    trace.p2tStages.push_back(current_p2t(staged,active_nodes));
    trace.stages.push_back(current_cells(staged));
    trace.liveSlots.push_back(current_slots(staged));
    trace.liveRawSlots.push_back(current_raw_slots(staged));
  }
  return trace;
}

std::vector<SchedulerEvent> run_scheduler_trace(dt::DT& reference) {
  std::queue<int> lost;
  std::unordered_map<int,int> previous;
  for(int i=0;i<static_cast<int>(reference.Elems.size());++i) {
    if(reference.isDelEle(i))continue;
    for(int j=0;j<6;++j) {
      int* edge=reference.BndEdg.find(
          reference.Elems[i].form[dt::Egid[j][0]],
          reference.Elems[i].form[dt::Egid[j][1]]);
      if(edge)reference.SurEdgs[*edge].info=1;
    }
  }
  for(int i=0;i<static_cast<int>(reference.SurEdgs.size());++i)
    if(!reference.isRecBndEdg(i)) {
      lost.push(i);
      ++previous[reference.SurEdgs[i].iStart];
      ++previous[reference.SurEdgs[i].iEnd];
    }
  std::vector<SchedulerEvent> events;
  for(int round=1;!lost.empty()&&round<=1000;++round) {
    const auto count=lost.size();
    for(std::size_t i=0;i<count;++i) {
      const auto target=lost.front();lost.pop();
      if(reference.isDelSurEdg(target))continue;
      const auto info=reference.SurEdgs[target].info;
      reference.fliplevel=round-info*10;
      const auto full=info<=-3;
      if(full)reference.fliplevel=std::max(1000,reference.fliplevel);
      const int steiner=info<=-5?2:(info<=-4?1:0);
      auto key=std::array<std::string,2>{{
          point_key(reference.Nodes[reference.SurEdgs[target].iStart].pt),
          point_key(reference.Nodes[reference.SurEdgs[target].iEnd].pt)}};
      std::sort(key.begin(),key.end());
      const auto result=reference.recoverEdge(target,full,steiner);
      events.push_back({key,target,round,info,reference.fliplevel,
                        full,steiner,result==0?0:(result==1?1:2),0});
      if(result==0)lost.push(target);
      else if(result>1)for(int child=result;
          child<static_cast<int>(reference.SurEdgs.size());++child) {
        auto child_key=std::array<std::string,2>{{
            point_key(reference.Nodes[reference.SurEdgs[child].iStart].pt),
            point_key(reference.Nodes[reference.SurEdgs[child].iEnd].pt)}};
        std::sort(child_key.begin(),child_key.end());
        const auto child_result=reference.recoverEdge(child,1,0);
        events.push_back({child_key,child,round,-3,reference.fliplevel,1,0,
                          child_result==0?0:(child_result==1?1:2),1});
        if(child_result==0) {
          reference.SurEdgs[child].info=-3;
          lost.push(child);
        }
      }
    }
    reference.updateFliptype(previous,lost);
  }
  return events;
}

struct SchedulerLocalPrefix {
  std::size_t attempts{};
  int next_round{};
  int pending_edge{-1};
  std::vector<GeometryCell> cells;
  std::vector<std::vector<GeometryCell>> cells_after_attempt;
  std::vector<std::vector<std::array<int,4>>> raw_cells_after_attempt;
  std::vector<std::vector<std::array<int,4>>> p2t_after_attempt;
  struct PrimitiveStep {
    int direction{}, result{};
    std::array<int,4> source{{-1,-1,-1,-1}};
    std::vector<std::array<int,4>> p2t;
  };
  std::vector<std::vector<PrimitiveStep>> primitive_steps_after_attempt;
  std::vector<std::vector<std::vector<std::array<int,4>>>> raw_cells_after_local_pass;
  struct AttemptEvent { int edge{}, start{}, end{}, info{}, flip_level{}, result{}, direction{};
    std::array<int,4> source{{-1,-1,-1,-1}}; };
  std::vector<AttemptEvent> attempt_events;
};

SchedulerLocalPrefix run_scheduler_local_prefix(dt::DT& reference) {
  std::queue<int> lost;
  std::unordered_map<int,int> previous;
  for(int cell=0;cell<static_cast<int>(reference.Elems.size());++cell) {
    if(reference.isDelEle(cell))continue;
    for(int local_edge=0;local_edge<6;++local_edge) {
      int* boundary=reference.BndEdg.find(
          reference.Elems[cell].form[dt::Egid[local_edge][0]],
          reference.Elems[cell].form[dt::Egid[local_edge][1]]);
      if(boundary)reference.SurEdgs[*boundary].info=1;
    }
  }
  for(int edge=0;edge<static_cast<int>(reference.SurEdgs.size());++edge)
    if(!reference.isRecBndEdg(edge)) {
      lost.push(edge);
      ++previous[reference.SurEdgs[edge].iStart];
      ++previous[reference.SurEdgs[edge].iEnd];
    }
  if(std::getenv("WANG_REFERENCE_DIAGNOSTICS")!=nullptr) {
    for(int node=0;node<static_cast<int>(reference.Nodes.size());++node) {
      const int carrier=reference.getP2T(node);
      if(carrier<0||reference.isDelEle(carrier))continue;
      std::cerr<<"scheduler_initial_p2t "<<node<<' '<<carrier;
      for(int corner=0;corner<4;++corner)
        std::cerr<<' '<<reference.Elems[carrier].form[corner];
      std::cerr<<'\n';
    }
  }
  SchedulerLocalPrefix prefix;
  for(int round=1;!lost.empty();++round) {
    const auto count=lost.size();
    for(std::size_t queued=0;queued<count;++queued) {
      const int edge=lost.front();lost.pop();
      if(reference.isDelSurEdg(edge))continue;
      if(reference.SurEdgs[edge].info<=-3) {
        prefix.next_round=round;
        prefix.pending_edge=edge;
        prefix.cells=current_cells(reference);
        return prefix;
      }
      const int info=reference.SurEdgs[edge].info;
      reference.fliplevel=round-info*10;
      int direction_source=-1;
      int direction{};
      // finddirection intentionally leaves its low-bit visit marks set on a
      // face return.  Calling it here merely to decorate a diagnostic would
      // therefore alter the following recoverEdgebyFlip-equivalent walk.
      // The primitive trace below is the observation point: do not insert a
      // preliminary state-mutating traversal ahead of it.
      std::array<int,4> direction_tet{{-1,-1,-1,-1}};
      if(direction_source>=0)
        for(int corner=0;corner<4;++corner)
          direction_tet[corner]=reference.Elems[direction_source].form[corner];
      std::vector<std::vector<std::array<int,4>>> local_passes;
      std::vector<SchedulerLocalPrefix::PrimitiveStep> primitive_steps;
      int recovered{};
      if(std::getenv("WANG_REFERENCE_DIAGNOSTICS")!=nullptr) {
        // This is recoverEdge(edge, 0, 0)'s source-defined local-flip
        // prefix, split only to expose the state after each directed pass.
        auto snapshot_raw=[&]() {
          std::vector<std::array<int,4>> raw;
          for(int cell=0;cell<static_cast<int>(reference.Elems.size());++cell)
            if(!reference.isDelEle(cell)) {
              std::array<int,4> vertices{};
              for(int corner=0;corner<4;++corner)vertices[corner]=reference.Elems[cell].form[corner];
              raw.push_back(vertices);
            }
          return raw;
        };
        const auto run_face_path=[&](bool reverse) {
          const auto& constraint=reference.SurEdgs[edge];
          const int p1=reverse?constraint.iEnd:constraint.iStart;
          const int p2=reverse?constraint.iStart:constraint.iEnd;
          for(int iteration=0;iteration<=1000;++iteration) {
            int source=-1;
            const int local=reference.finddirection(p1,p2,source);
            if(local>=0&&local<=3)return 1;
            if(local<4||local>7)return 0;
            SchedulerLocalPrefix::PrimitiveStep step;
            step.direction=local;
            for(int corner=0;corner<4;++corner)step.source[corner]=reference.Elems[source].form[corner];
            if(std::getenv("WANG_REFERENCE_DIAGNOSTICS")!=nullptr) {
              std::cerr<<"scheduler_primitive_raw "<<edge<<' '
                       <<(reverse?1:0)<<' '<<local<<' '<<source;
              for(const auto node:step.source)std::cerr<<' '<<node;
              std::cerr<<'\n';
              if(local>=4&&local<=7) {
                static constexpr int dfc[4][4]={{0,1,3,2},{1,2,3,0},
                                                 {2,0,3,1},{3,0,1,2}};
                const int face=local-4;
                const int a=step.source[dfc[face][0]],b=step.source[dfc[face][1]];
                const int c=step.source[dfc[face][2]],d=step.source[dfc[face][3]];
                const int neighbour=reference.getNeig(source,face);
                if(neighbour>=0) {
                  const int e=reference.Elems[neighbour].form[
                      reference.getNeigOrd(source,face)];
                  std::cerr<<"scheduler_removeface_orientation "<<edge<<' '
                           <<(reverse?1:0)<<' '<<iteration<<' '<<source<<' '
                           <<face<<" 0 "<<dt::GEOM_FUNC::orient3d(reference.Nodes[a].pt,reference.Nodes[b].pt,reference.Nodes[d].pt,reference.Nodes[e].pt)<<'\n'
                           <<"scheduler_removeface_orientation "<<edge<<' '
                           <<(reverse?1:0)<<' '<<iteration<<' '<<source<<' '
                           <<face<<" 1 "<<dt::GEOM_FUNC::orient3d(reference.Nodes[a].pt,reference.Nodes[d].pt,reference.Nodes[c].pt,reference.Nodes[e].pt)<<'\n'
                           <<"scheduler_removeface_orientation "<<edge<<' '
                           <<(reverse?1:0)<<' '<<iteration<<' '<<source<<' '
                           <<face<<" 2 "<<dt::GEOM_FUNC::orient3d(reference.Nodes[a].pt,reference.Nodes[c].pt,reference.Nodes[b].pt,reference.Nodes[e].pt)<<'\n';
                }
              }
              // Read-only source traversal evidence: enumerate the exact
              // findShell order for each local edge of this source tet.
              // removeface chooses one of these via its DFC/orientation
              // branch, while recursive flipnm calls derive later stars.
              for(int left=0;left<4;++left)for(int right=left+1;right<4;++right) {
                std::vector<int> shell,ring;
                reference.findShell(source,left,right,shell,ring);
                std::cerr<<"scheduler_shell "<<edge<<' '<<(reverse?1:0)<<' '
                         <<iteration<<' '<<source<<' '<<left<<' '<<right;
                for(const auto slot:shell)std::cerr<<' '<<slot;
                std::cerr<<'\n';
              }
            }
            std::vector<int> cells{{source}};
            step.result=reference.removeface(cells,local-4,reference.fliplevel);
            if(std::getenv("WANG_REFERENCE_DIAGNOSTICS")!=nullptr) {
              int active{};
              for(int slot=0;slot<static_cast<int>(reference.Elems.size());++slot)
                if(!reference.isDelEle(slot))++active;
              std::cerr<<"scheduler_primitive_result "<<edge<<' '
                       <<(reverse?1:0)<<' '<<iteration<<' '
                       <<step.result<<' '<<active<<'\n';
            }
            // Oracle-only: preserve the physical element slots as well as the
            // forms.  `flip23` allocates before retiring its two inputs, so
            // slot identity is part of the later P2T/finddirection behavior.
            if(std::getenv("WANG_REFERENCE_DIAGNOSTICS")!=nullptr) {
              for(int slot=0;slot<static_cast<int>(reference.Elems.size());++slot) {
                if(reference.isDelEle(slot))continue;
                std::cerr<<"scheduler_primitive_live "<<edge<<' '
                         <<(reverse?1:0)<<' '<<iteration<<' '<<slot;
                for(int corner=0;corner<4;++corner)
                  std::cerr<<' '<<reference.Elems[slot].form[corner];
                std::cerr<<'\n';
              }
            }
            for(int node=0;node<static_cast<int>(reference.Nodes.size());++node) {
              const int carrier=reference.getP2T(node);
              std::array<int,4> vertices{{-1,-1,-1,-1}};
              if(carrier>=0&&!reference.isDelEle(carrier))
                for(int corner=0;corner<4;++corner)vertices[corner]=reference.Elems[carrier].form[corner];
              step.p2t.push_back(vertices);
            }
            primitive_steps.push_back(std::move(step));
            if(primitive_steps.back().result!=1)return 0;
          }
          return 0;
        };
        const int forward=run_face_path(false);
        local_passes.push_back(snapshot_raw());
        recovered=forward;
        if(forward==1)reference.SurEdgs[edge].info=1;
        else if(forward==0) {
          const int reverse=run_face_path(true);
          local_passes.push_back(snapshot_raw());
          recovered=reverse;
          if(reverse==1)reference.SurEdgs[edge].info=1;
        }
      } else {
        recovered=reference.recoverEdge(edge,0,0);
      }
      ++prefix.attempts;
      prefix.attempt_events.push_back({edge,reference.SurEdgs[edge].iStart,
          reference.SurEdgs[edge].iEnd,info,reference.fliplevel,recovered,
          direction,direction_tet});
      prefix.cells_after_attempt.push_back(current_cells(reference));
      std::vector<std::array<int,4>> raw_cells;
      for(int cell=0;cell<static_cast<int>(reference.Elems.size());++cell)
        if(!reference.isDelEle(cell)) {
          std::array<int,4> vertices{};
          for(int corner=0;corner<4;++corner)
            vertices[corner]=reference.Elems[cell].form[corner];
          raw_cells.push_back(vertices);
        }
      prefix.raw_cells_after_attempt.push_back(std::move(raw_cells));
      if(std::getenv("WANG_REFERENCE_DIAGNOSTICS")!=nullptr) {
        for(int cell=0;cell<static_cast<int>(reference.Elems.size());++cell) {
          if(reference.isDelEle(cell))continue;
          // Elem::info is source control state.  It contains both the
          // finddirection visit bit and active flipnm-star membership.
          // Record it by physical slot so a topology/P2T match cannot hide
          // a different next traversal.
          std::cerr<<"scheduler_info_attempt_"<<prefix.attempts-1U<<' '
                   <<cell<<' '<<reference.Elems[cell].info<<'\n';
        }
      }
      std::vector<std::array<int,4>> p2t;
      p2t.reserve(reference.Nodes.size());
      for(int node=0;node<static_cast<int>(reference.Nodes.size());++node) {
        const int carrier=reference.getP2T(node);
        std::array<int,4> vertices{{-1,-1,-1,-1}};
        if(carrier>=0&&!reference.isDelEle(carrier))
          for(int corner=0;corner<4;++corner)vertices[corner]=reference.Elems[carrier].form[corner];
        p2t.push_back(vertices);
      }
      prefix.p2t_after_attempt.push_back(std::move(p2t));
      prefix.primitive_steps_after_attempt.push_back(std::move(primitive_steps));
      prefix.raw_cells_after_local_pass.push_back(std::move(local_passes));
      if(recovered==0)lost.push(edge);
    }
    reference.updateFliptype(previous,lost);
  }
  prefix.next_round=1;
  prefix.cells=current_cells(reference);
  return prefix;
}

// Isolate the first local scheduler operation.  This is an oracle diagnostic
// only: it lets the owned port compare the exact finddirection/removeface
// transition that first changes topology, without editing the pinned source.
void write_first_scheduler_operation(std::ostream& out,dt::DT reference) {
  for(int cell=0;cell<static_cast<int>(reference.Elems.size());++cell) {
    if(reference.isDelEle(cell))continue;
    for(int local_edge=0;local_edge<6;++local_edge) {
      int* boundary=reference.BndEdg.find(
          reference.Elems[cell].form[dt::Egid[local_edge][0]],
          reference.Elems[cell].form[dt::Egid[local_edge][1]]);
      if(boundary)reference.SurEdgs[*boundary].info=1;
    }
  }
  int edge=-1;
  for(int candidate=0;candidate<static_cast<int>(reference.SurEdgs.size());++candidate)
    if(!reference.isRecBndEdg(candidate)) {edge=candidate;break;}
  if(edge<0)return;
  const int start=reference.SurEdgs[edge].iStart;
  const int end=reference.SurEdgs[edge].iEnd;
  int cell=-1;
  const int direction=reference.finddirection(start,end,cell);
  out<<"scheduler_first_operation edge "<<node_key(reference,start)<<' '
     <<node_key(reference,end)<<" direction "<<direction;
  if(cell>=0)out<<" cell";
  if(cell>=0)for(int corner=0;corner<4;++corner)
    out<<' '<<node_key(reference,reference.Elems[cell].form[corner]);
  out<<'\n';
  if(direction>=4&&direction<=7) {
    std::vector<int> oldtet{{cell}};
    const int removed=reference.removeface(oldtet,direction-4,0);
    out<<"scheduler_first_operation_removeface "<<removed<<'\n';
    write_cells(out,"scheduler_first_operation_cells",current_cells(reference));
  }
  // recoverEdge tries the directed walk and then its reverse on the same
  // mutable state.  Record both whole calls, not just the first face probe.
  dt::DT recovery=reference;
  recovery.fliplevel=1;
  const int forward=recovery.recoverEdgebyFlip(edge,0,2);
  out<<"scheduler_first_operation_forward "<<forward<<'\n';
  write_cells(out,"scheduler_first_operation_forward_cells",current_cells(recovery));
  if(forward!=1) {
    const int reverse=recovery.recoverEdgebyFlip(edge,1,2);
    out<<"scheduler_first_operation_reverse "<<reverse<<'\n';
    write_cells(out,"scheduler_first_operation_reverse_cells",current_cells(recovery));
  }
}

void write_scheduler(std::ofstream& out,
                     const std::vector<SchedulerEvent>& events) {
  out<<"scheduler_event_count "<<events.size()<<'\n';
  for(const auto& event:events) {
    out<<"scheduler_index "<<event.edge_index<<'\n';
    out<<"scheduler "<<event.edge[0]<<' '<<event.edge[1]<<' '
       <<event.round<<' '<<event.info<<' '<<event.depth<<' '
       <<event.full<<' '<<event.steiner<<' '<<event.outcome<<' '
       <<event.immediate<<'\n';
  }
}

void write_direction_probes(std::ostream& out,dt::DT& reference) {
  for(const auto edge:std::array<std::array<int,2>,3>{
          {{{0,6}},{{0,8}},{{7,8}}}}) {
    for(int reverse=0;reverse<2;++reverse) {
      const int start=edge[reverse?1:0],end=edge[reverse?0:1];
      const int p2t=reference.getP2T(start);
      int source=-1;
      const int direction=reference.finddirection(start,end,source);
      out<<"direction_probe "<<edge[0]+1<<' '<<edge[1]+1<<' '
         <<reverse<<' '<<direction;
      out<<" p2t";
      for(int corner=0;corner<4;++corner)
        out<<' '<<node_key(reference,reference.Elems[p2t].form[corner]);
      out<<" source";
      for(int corner=0;corner<4;++corner)
        out<<' '<<node_key(reference,reference.Elems[source].form[corner]);
      if(0<=direction&&direction<=3) {
        out<<" vertex "
           <<node_key(reference,reference.Elems[source].form[direction]);
      } else if(4<=direction&&direction<=7) {
        const auto face=face_geometry(reference,source,direction-4);
        out<<" face";
        for(const auto& point:face)out<<' '<<point;
      } else if(-14<=direction&&direction<=-1) {
        const int first=((-direction)>>2)&3;
        const int second=(-direction)&3;
        std::array<std::string,2> geometry{{
            node_key(reference,reference.Elems[source].form[first]),
            node_key(reference,reference.Elems[source].form[second])}};
        std::sort(geometry.begin(),geometry.end());
        out<<" edge "<<geometry[0]<<' '<<geometry[1];
      }
      out<<'\n';
      out<<"direction_semantic "<<edge[0]+1<<' '<<edge[1]+1<<' '
         <<reverse;
      const auto source_geometry=cell_geometry(reference,source);
      if(0<=direction&&direction<=3) {
        out<<" vertex "<<node_key(reference,reference.Elems[source].form[direction]);
      } else if(4<=direction&&direction<=7) {
        out<<" face";
        const auto feature=face_geometry(reference,source,direction-4);
        for(const auto& point:feature)out<<' '<<point;
      } else if(-14<=direction&&direction<=-1) {
        const int first=((-direction)>>2)&3;
        const int second=(-direction)&3;
        std::array<std::string,2> feature{{
            node_key(reference,reference.Elems[source].form[first]),
            node_key(reference,reference.Elems[source].form[second])}};
        std::sort(feature.begin(),feature.end());
        out<<" edge "<<feature[0]<<' '<<feature[1];
      } else out<<" unknown";
      out<<" source";
      for(const auto& point:source_geometry)out<<' '<<point;
      out<<'\n';
    }
  }
}

void write_local_sequence_probe(std::ostream& out,dt::DT& reference) {
  int* target=reference.BndEdg.find(0,6);
  if(!target) {out<<"local_sequence_missing 1 7\n";return;}
  reference.seg[0]=0;
  reference.seg[1]=6;
  const int forward=reference.recoverEdgebyFlip(*target,0,2);
  out<<"local_sequence 1 7 forward "<<forward<<'\n';
  const auto after_forward=current_cells(reference);
  out<<"local_after_forward_count "<<after_forward.size()<<'\n';
  for(const auto& cell:after_forward) {
    out<<"local_after_forward";
    for(const auto& point:cell)out<<' '<<point;
    out<<'\n';
  }
  int source=-1;
  const int direction=reference.finddirection(6,0,source);
  out<<"local_after_forward_reverse_direction "<<direction<<" source";
  for(int corner=0;corner<4;++corner)
    out<<' '<<node_key(reference,reference.Elems[source].form[corner]);
  out<<'\n';
  const int reverse=reference.recoverEdgebyFlip(*target,1,2);
  out<<"local_sequence 1 7 reverse "<<reverse<<" recovered "
     <<reference.isMeshEdge(0,6)<<'\n';
}

void write_edge_contact_candidates(std::ostream& out,dt::DT& reference) {
  for(int first=0;first<12;++first)for(int second=0;second<12;++second) {
    if(first==second||reference.isMeshEdge(first,second))continue;
    int source=-1;
    const int direction=reference.finddirection(first,second,source);
    if(direction> -1||direction< -14)continue;
    const int a=((-direction)>>2)&3;
    const int b=(-direction)&3;
    std::array<std::string,2> feature{{
        node_key(reference,reference.Elems[source].form[a]),
        node_key(reference,reference.Elems[source].form[b])}};
    std::sort(feature.begin(),feature.end());
    out<<"edge_contact_candidate "<<first+1<<' '<<second+1;
    for(const auto& point:feature)out<<' '<<point;
    out<<'\n';
  }
}

void write_synthetic_edge_contact(std::ostream& out) {
  dt::DT reference;
  const std::array<std::array<double,3>,6> points{{
      {{-1.0,0.0,0.0}},{{1.0,0.0,0.0}},{{0.0,-1.0,0.0}},
      {{0.0,1.0,0.0}},{{0.0,0.0,1.0}},{{0.0,0.0,-1.0}}}};
  for(const auto& point:points)
    reference.addNode(point[0],point[1],point[2],1.0,false);
  reference.ghost=reference.addNode(false);
  const std::array<std::array<int,4>,4> forms{{
      {{0,2,3,4}},{{1,4,3,2}},{{1,2,3,5}},{{0,5,3,2}}}};
  std::array<int,4> cells{};
  for(std::size_t i=0;i<forms.size();++i)
    cells[i]=reference.addElem(forms[i][0],forms[i][1],forms[i][2],forms[i][3],false);
  for(unsigned left=0;left<cells.size();++left)
    for(unsigned right=left+1U;right<cells.size();++right)
      for(unsigned a=0;a<4U;++a)for(unsigned b=0;b<4U;++b) {
        std::array<int,3> face_a{},face_b{};unsigned ca{},cb{};
        for(unsigned corner=0;corner<4U;++corner) {
          if(corner!=a)face_a[ca++]=forms[left][corner];
          if(corner!=b)face_b[cb++]=forms[right][corner];
        }
        std::sort(face_a.begin(),face_a.end());
        std::sort(face_b.begin(),face_b.end());
        if(face_a==face_b)reference.bond(cells[left],a,cells[right],b);
      }
  // Complete the finite octahedron with the source representation's ghost
  // hull.  addinnerSteiner_Edge locates its midpoint from a boundary endpoint,
  // so an unbonded hand-built finite complex is not a valid DT oracle state.
  for(unsigned cell=0;cell<cells.size();++cell)for(unsigned omitted=0;omitted<4U;
      ++omitted) {
    if(reference.getNeig(cells[cell],static_cast<int>(omitted))>=0)continue;
    std::array<int,3> face{};unsigned cursor{};
    for(unsigned corner=0;corner<4U;++corner)
      if(corner!=omitted)face[cursor++]=forms[cell][corner];
    const int hull=reference.addElem(face[0],face[1],face[2],reference.ghost,false);
    reference.bond(cells[cell],static_cast<int>(omitted),hull,3);
  }
  reference.setP2T(0,cells[0]);reference.setP2T(1,cells[1]);
  reference.setP2T(2,cells[0]);reference.setP2T(3,cells[0]);
  reference.setP2T(4,cells[0]);reference.setP2T(5,cells[2]);
  reference.SurEdgs.emplace_back(dt::SurEdg());
  reference.SurEdgs[0].iStart=0;reference.SurEdgs[0].iEnd=1;
  reference.BndEdg.add(0,1,0);
  int source=-1;
  const int direction=reference.finddirection(0,1,source);
  std::vector<std::array<int,3>> features;
  const int walk=reference.findIntersectwithEdgs(0,features);
  out<<"edge_contact_synthetic direction "<<direction<<" walk "<<walk
     <<" feature_count "<<features.size()<<'\n';
  for(const auto& feature:features) {
    out<<"edge_contact_synthetic_feature";
    const unsigned count=feature[2]<0?2U:3U;
    std::vector<std::string> geometry;
    for(unsigned i=0;i<count;++i)
      geometry.push_back(node_key(reference,feature[i]));
    std::sort(geometry.begin(),geometry.end());
    for(const auto& point:geometry)out<<' '<<point;
    out<<'\n';
  }
  std::vector<int> cascade_nodes;
  const int cascade_result=reference.addinnerSteiner_Edge(0,cascade_nodes,0);
  out<<"cascade_synthetic result "<<cascade_result<<" nodes "
     <<cascade_nodes.size();
  for(const auto node:cascade_nodes)out<<' '<<node_key(reference,node);
  out<<" finite_cells "<<current_cells(reference).size()<<'\n';
  write_cells(out,"cascade_synthetic_post",current_cells(reference));
}

void write_first_removeface_probe(std::ostream& out,dt::DT& reference) {
  int* target=reference.BndEdg.find(0,6);
  if(!target) {out<<"first_removeface_missing 1 7\n";return;}
  // recoverEdge follows the directed SurEdg record, which is not necessarily
  // the same orientation as the sorted lookup key used above.
  const int start=reference.SurEdgs[*target].iStart;
  const int end=reference.SurEdgs[*target].iEnd;
  reference.seg[0]=start;
  reference.seg[1]=end;
  int source=-1;
  const int direction=reference.finddirection(start,end,source);
  if(direction<4||direction>7) {
    out<<"first_removeface_nonface "<<direction<<'\n';return;
  }
  const auto before=current_cells(reference);
  const auto source_geometry=cell_geometry(reference,source);
  const auto crossed_face=face_geometry(reference,source,direction-4);
  std::vector<int> cavity{{source}};
  const int removed=reference.removeface(cavity,direction-4,1);
  out<<"first_removeface 1 7 stored "<<start+1<<' '<<end+1
     <<" direction "<<direction<<" result "<<removed
     <<" cavity_size "<<cavity.size()<<'\n';
  out<<"first_removeface_source";
  for(const auto& point:source_geometry)out<<' '<<point;
  out<<'\n';
  out<<"first_removeface_feature";
  for(const auto& point:crossed_face)out<<' '<<point;
  out<<'\n';
  const auto after=current_cells(reference);
  std::vector<GeometryCell> erased,created;
  std::set_difference(before.begin(),before.end(),after.begin(),after.end(),
                      std::back_inserter(erased));
  std::set_difference(after.begin(),after.end(),before.begin(),before.end(),
                      std::back_inserter(created));
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
}

void write_flip32_step(std::ostream& out,dt::DT& reference,int step,
                       int first,int second) {
  int source=-1;
  if(!reference.isMeshEdge(first,second,&source)) {
    out<<"first_flip32_step "<<step<<" missing "<<first+1<<' '<<second+1<<'\n';
    return;
  }
  int first_position=-1,second_position=-1;
  for(int corner=0;corner<4;++corner) {
    if(reference.Elems[source].form[corner]==first)first_position=corner;
    if(reference.Elems[source].form[corner]==second)second_position=corner;
  }
  std::vector<int> shell,shell_points;
  reference.findShell(source,first_position,second_position,shell,shell_points);
  const auto before=current_cells(reference);
  const int result=reference.flip32(shell,first_position,second_position,-1);
  const auto after=current_cells(reference);
  std::vector<GeometryCell> erased,created;
  std::set_difference(before.begin(),before.end(),after.begin(),after.end(),
                      std::back_inserter(erased));
  std::set_difference(after.begin(),after.end(),before.begin(),before.end(),
                      std::back_inserter(created));
  out<<"first_flip32_step "<<step<<" edge "<<first+1<<' '<<second+1
     <<" shell "<<shell.size()<<" result "<<result<<'\n';
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
}

void write_first_flip32_sequence(std::ostream& out,dt::DT& reference) {
  // These are the two 3-to-2 primitives selected by the depth-one flipnm
  // search for the corrected 7 -> 1 first crossed-face operation.
  write_flip32_step(out,reference,1,4,7);
  write_flip32_step(out,reference,2,9,4);

  int source=-1,opposite=-1;
  for(int cell=0;cell<static_cast<int>(reference.Elems.size());++cell) {
    if(reference.isDelEle(cell)||reference.ishulltet(cell))continue;
    int apex=-1,first=-1,second=-1,third=-1;
    for(int corner=0;corner<4;++corner) {
      const int vertex=reference.Elems[cell].form[corner];
      if(vertex==6)apex=corner;
      else if(vertex==9)first=corner;
      else if(vertex==5)second=corner;
      else if(vertex==1)third=corner;
    }
    if(apex>=0&&first>=0&&second>=0&&third>=0) {
      source=cell;opposite=apex;break;
    }
  }
  if(source<0) {out<<"first_flip23 missing\n";return;}
  const auto before=current_cells(reference);
  std::vector<int> pair{{source,reference.getNeig(source,opposite)}};
  const int result=reference.flip23(pair,opposite,-1);
  const auto after=current_cells(reference);
  std::vector<GeometryCell> erased,created;
  std::set_difference(before.begin(),before.end(),after.begin(),after.end(),
                      std::back_inserter(erased));
  std::set_difference(after.begin(),after.end(),before.begin(),before.end(),
                      std::back_inserter(created));
  out<<"first_flip23 face 10 6 2 apex 7 result "<<result<<'\n';
  out<<"first_flip23_erased_count "<<erased.size()<<'\n';
  for(const auto& cell:erased) {
    out<<"first_flip23_erased";
    for(const auto& point:cell)out<<' '<<point;
    out<<'\n';
  }
  out<<"first_flip23_created_count "<<created.size()<<'\n';
  for(const auto& cell:created) {
    out<<"first_flip23_created";
    for(const auto& point:cell)out<<' '<<point;
    out<<'\n';
  }
  out<<"first_flip23_cells "<<after.size()<<'\n';
}

} // namespace

int main(int argc,char** argv) {
  if(argc!=4) {
    std::cerr<<"usage: wang_author_reference_probe FIXTURE INPUT OUTPUT\n";
    return 2;
  }
  const std::string fixture=argv[1],output=argv[3];
  std::string input=argv[2];
  dt::Mesh mesh;
  if(!dt::readMesh(input,mesh))return 3;

  dt::Args args;
  args.constrain=1;
  args.ignoreIntersect=0;
  args.autoflip=1;
  args.refine=0;
  args.optlevel=0;
  args.nthread=1;
  args.infolevel=2;

  dt::DT reference;
  if(!reference.dt_init(mesh,args)||!reference.BndPntInst(mesh,args))return 4;
  if(fixture=="seed_file") {
    const auto trace=reference_seed_stages(mesh,args);
    if(trace.stages.empty())return 13;
    std::ofstream out(output);
    if(!out)return 11;
    write_order(out,reference,mesh);
    out<<"seed_result 0 0\n";
    for(std::size_t stage=0;stage<trace.originalSlots.size();++stage)
      for(const auto& entry:trace.originalSlots[stage]) {
        out<<"seed_original_slot "<<stage<<' '<<entry.first;
        for(const auto& point:entry.second)out<<' '<<point;
        out<<'\n';
      }
    for(std::size_t stage=0;stage<trace.originalRawSlots.size();++stage)
      for(const auto& entry:trace.originalRawSlots[stage]) {
        out<<"seed_original_slot_raw "<<stage<<' '<<entry.first;
        for(const auto& point:entry.second)out<<' '<<point;
        out<<'\n';
      }
    for(const auto& line:trace.originalPlanOrder)
      if(line.rfind("seed_original_working ",0U)==0U ||
         line.rfind("seed_original_adjusted ",0U)==0U ||
         line.rfind("seed_original_carrier ",0U)==0U ||
         line.rfind("seed_original_carrier_raw ",0U)==0U ||
         line.rfind("seed_original_location ",0U)==0U ||
         line.rfind("seed_original_hull_predicate ",0U)==0U)
        out<<line<<'\n';
    write_seed_stages(out,trace.stages);
    return out?0:12;
  }
  const auto pre_boundary_p2t=[&]() {
    std::set<int> active;
    for(int node=0;node<static_cast<int>(reference.Nodes.size());++node)
      if(!reference.isDelNod(node))active.insert(node);
    return current_p2t(reference,active);
  }();
  const auto seed=current_cells(reference);
  reference.buildBndInfo(mesh,args);
  if(fixture=="real_segment_file") {
    std::ofstream out(output);
    if(!out)return 11;
    out<<"fixture "<<fixture<<'\n';
    write_real_first_segment_transaction(out,reference);
    return out?0:12;
  }
  // Oracle-only trace for an arbitrary exported surface.  Unlike the
  // synthetic `scheduler` fixture below, this deliberately performs no
  // auxiliary direction probes: those probes mutate DT visit bits and would
  // change the scheduler transaction being compared.
  if(fixture=="scheduler_file") {
    const auto events=run_scheduler_trace(reference);
    std::ofstream out(output);
    if(!out)return 11;
    out<<"fixture "<<fixture<<'\n';
    write_scheduler(out,events);
    return out?0:12;
  }
  if(fixture=="interior_obstruction_file") {
    std::ofstream out(output);
    if(!out)return 11;
    out<<"fixture "<<fixture<<'\n';
    write_interior_vertex_obstruction(out,reference);
    return out?0:12;
  }
  if(fixture=="scheduler") {
    std::ostringstream state_text;
    write_owned_state(state_text,reference);
    std::ostringstream direction_text;
    write_direction_probes(direction_text,reference);
    std::ostringstream edge_contact_text;
    write_edge_contact_candidates(edge_contact_text,reference);
    dt::DT local_reference;
    if(!local_reference.dt_init(mesh,args)||
       !local_reference.BndPntInst(mesh,args))return 14;
    local_reference.buildBndInfo(mesh,args);
    std::ostringstream local_sequence_text;
    write_local_sequence_probe(local_sequence_text,local_reference);
    dt::DT direct_reference;
    if(!direct_reference.dt_init(mesh,args)||
       !direct_reference.BndPntInst(mesh,args))return 15;
    direct_reference.buildBndInfo(mesh,args);
    std::ostringstream direct_operation_text;
    write_first_removeface_probe(direct_operation_text,direct_reference);
    dt::DT primitive_reference;
    if(!primitive_reference.dt_init(mesh,args)||
       !primitive_reference.BndPntInst(mesh,args))return 16;
    primitive_reference.buildBndInfo(mesh,args);
    std::ostringstream primitive_operation_text;
    write_first_flip32_sequence(primitive_operation_text,primitive_reference);
    dt::DT prefix_reference;
    if(!prefix_reference.dt_init(mesh,args)||
       !prefix_reference.BndPntInst(mesh,args))return 17;
    prefix_reference.buildBndInfo(mesh,args);
    std::ostringstream first_scheduler_operation_text;
    write_first_scheduler_operation(first_scheduler_operation_text,prefix_reference);
    const auto local_prefix=run_scheduler_local_prefix(prefix_reference);
    {
      std::vector<std::string> scheduler_state;
      for(int edge=0;edge<static_cast<int>(prefix_reference.SurEdgs.size());++edge) {
        if(prefix_reference.isDelSurEdg(edge))continue;
        auto endpoints=std::array<std::string,2>{{
            node_key(prefix_reference,prefix_reference.SurEdgs[edge].iStart),
            node_key(prefix_reference,prefix_reference.SurEdgs[edge].iEnd)}};
        std::sort(endpoints.begin(),endpoints.end());
        std::ostringstream line;
        line<<"scheduler_local_prefix_edge "<<endpoints[0]<<' '<<endpoints[1]
            <<' '<<prefix_reference.SurEdgs[edge].info;
        scheduler_state.push_back(line.str());
      }
      std::sort(scheduler_state.begin(),scheduler_state.end());
      for(const auto& line:scheduler_state)state_text<<line<<'\n';
    }
    prefix_reference.fliplevel=1000;
    const int full_forward=prefix_reference.recoverEdgebyFlip(
        local_prefix.pending_edge,0,2000);
    const int full_reverse=prefix_reference.recoverEdgebyFlip(
        local_prefix.pending_edge,1,2000);
    std::vector<std::array<int,3>> full_features;
    const int full_walk=prefix_reference.findIntersectwithEdgs(
        local_prefix.pending_edge,full_features);
    const auto full_pre_removal=current_cells(prefix_reference);
    const int full_result=prefix_reference.recoverEdgebyFlip(
        local_prefix.pending_edge,0,2001);
    const auto full_post_removal=current_cells(prefix_reference);
    // Retain the exact mutable ordering immediately before mode-one FHC;
    // `prefix_reference` itself is intentionally mutated below to obtain the
    // author result.
    const dt::DT pre_steiner_reference=prefix_reference;
    std::swap(prefix_reference.SurEdgs[local_prefix.pending_edge].iStart,
              prefix_reference.SurEdgs[local_prefix.pending_edge].iEnd);
    prefix_reference.SurEdgs[local_prefix.pending_edge].info=-4;
    prefix_reference.fliplevel=1000;
    const auto nodes_before_steiner=prefix_reference.Nodes.size();
    const int steiner1_result=prefix_reference.recoverEdge(
        local_prefix.pending_edge,1,1);
    std::vector<std::string> steiner1_points;
    for(std::size_t node=nodes_before_steiner;
        node<prefix_reference.Nodes.size();++node)
      if(!prefix_reference.isDelNod(static_cast<int>(node)))
        steiner1_points.push_back(node_key(prefix_reference,static_cast<int>(node)));
    const auto steiner1_cells=current_cells(prefix_reference);
    dt::DT full5_probe_reference;
    if(!full5_probe_reference.dt_init(mesh,args)||
       !full5_probe_reference.BndPntInst(mesh,args))return 19;
    full5_probe_reference.buildBndInfo(mesh,args);
    const auto full5_prefix=run_scheduler_local_prefix(full5_probe_reference);
    full5_probe_reference.fliplevel=1000;
    (void)full5_probe_reference.recoverEdgebyFlip(full5_prefix.pending_edge,0,2000);
    (void)full5_probe_reference.recoverEdgebyFlip(full5_prefix.pending_edge,1,2000);
    (void)full5_probe_reference.recoverEdgebyFlip(full5_prefix.pending_edge,0,2001);
    std::swap(full5_probe_reference.SurEdgs[full5_prefix.pending_edge].iStart,
              full5_probe_reference.SurEdgs[full5_prefix.pending_edge].iEnd);
    (void)full5_probe_reference.recoverEdgebyFlip(full5_prefix.pending_edge,0,2000);
    (void)full5_probe_reference.recoverEdgebyFlip(full5_prefix.pending_edge,1,2000);
    (void)full5_probe_reference.recoverEdgebyFlip(full5_prefix.pending_edge,0,2000);
    std::vector<std::array<int,3>> full5_features;
    (void)full5_probe_reference.findIntersectwithEdgs(
        full5_prefix.pending_edge,full5_features);
    std::ostringstream full5_text;
    for(const auto& feature:full5_features) {
      if(feature[2]<0)continue;
      int source=-1;
      if(!full5_probe_reference.isMeshFace(
             feature[0],feature[1],feature[2],&source))continue;
      int opposite=-1;
      for(int corner=0;corner<4;++corner)
        if(full5_probe_reference.Elems[source].form[corner]!=feature[0]&&
           full5_probe_reference.Elems[source].form[corner]!=feature[1]&&
           full5_probe_reference.Elems[source].form[corner]!=feature[2])
          opposite=corner;
      std::vector<int> cavity{{source}};
      const int removal=full5_probe_reference.removeface(cavity,opposite,32,-1);
      std::vector<std::string> geometry;
      for(const int vertex:feature)
        geometry.push_back(node_key(full5_probe_reference,vertex));
      std::sort(geometry.begin(),geometry.end());
      full5_text<<"scheduler_round5_full_face";
      for(const auto& point:geometry)full5_text<<' '<<point;
      full5_text<<" result "<<removal<<" cells "
                <<current_cells(full5_probe_reference).size()<<'\n';
    }
    dt::DT fhc_probe_reference;
    if(!fhc_probe_reference.dt_init(mesh,args)||
       !fhc_probe_reference.BndPntInst(mesh,args))return 18;
    fhc_probe_reference.buildBndInfo(mesh,args);
    const auto fhc_prefix=run_scheduler_local_prefix(fhc_probe_reference);
    fhc_probe_reference.fliplevel=1000;
    (void)fhc_probe_reference.recoverEdgebyFlip(fhc_prefix.pending_edge,0,2000);
    (void)fhc_probe_reference.recoverEdgebyFlip(fhc_prefix.pending_edge,1,2000);
    (void)fhc_probe_reference.recoverEdgebyFlip(fhc_prefix.pending_edge,0,2001);
    std::swap(fhc_probe_reference.SurEdgs[fhc_prefix.pending_edge].iStart,
              fhc_probe_reference.SurEdgs[fhc_prefix.pending_edge].iEnd);
    fhc_probe_reference.SurEdgs[fhc_prefix.pending_edge].info=-4;
    std::vector<std::array<int,3>> fhc_features;
    (void)fhc_probe_reference.findIntersectwithEdgs(
        fhc_prefix.pending_edge,fhc_features);
    std::ostringstream first_fhc_text;
    bool recorded_first_fhc=false;
    for(const auto& feature:fhc_features) {
      if(feature[2]<0)continue;
      int source=-1;
      if(!fhc_probe_reference.isMeshFace(
             feature[0],feature[1],feature[2],&source))continue;
      int opposite=-1;
      for(int corner=0;corner<4;++corner)
        if(fhc_probe_reference.Elems[source].form[corner]!=feature[0]&&
           fhc_probe_reference.Elems[source].form[corner]!=feature[1]&&
           fhc_probe_reference.Elems[source].form[corner]!=feature[2])
          opposite=corner;
      int a=0,b=0,c=0,d=0;
      DFC(opposite,a,b,c,d);
      const std::array<int,3> ordered_face{{
          fhc_probe_reference.Elems[source].form[b],
          fhc_probe_reference.Elems[source].form[c],
          fhc_probe_reference.Elems[source].form[d]}};
      std::vector<int> cavity{{source}};
      const int removal=fhc_probe_reference.removeface(cavity,opposite,10,-1);
      std::vector<std::string> probed_face;
      for(const int vertex:feature)
        probed_face.push_back(node_key(fhc_probe_reference,vertex));
      std::sort(probed_face.begin(),probed_face.end());
      first_fhc_text<<"scheduler_steiner1_face_removal";
      for(const auto& point:probed_face)first_fhc_text<<' '<<point;
      first_fhc_text<<" result "<<removal<<" cells "
                    <<current_cells(fhc_probe_reference).size();
      if(removal>=0) {
        const int retry=fhc_probe_reference.recoverEdge(
            fhc_prefix.pending_edge,0,0);
        first_fhc_text<<" retry "<<retry<<'\n';
        continue;
      }
      first_fhc_text<<'\n';
      const int blocked=-removal-1;
      std::array<int,2> locking{{}};
      if(blocked==0)locking={{ordered_face[0],ordered_face[2]}};
      else if(blocked==1)locking={{ordered_face[2],ordered_face[1]}};
      else locking={{ordered_face[1],ordered_face[0]}};
      std::vector<std::string> face_geometry,edge_geometry;
      for(const int vertex:feature)
        face_geometry.push_back(node_key(fhc_probe_reference,vertex));
      for(const int vertex:locking)
        edge_geometry.push_back(node_key(fhc_probe_reference,vertex));
      std::sort(face_geometry.begin(),face_geometry.end());
      std::sort(edge_geometry.begin(),edge_geometry.end());
      first_fhc_text<<"scheduler_steiner1_first_locked_face";
      for(const auto& point:face_geometry)first_fhc_text<<' '<<point;
      first_fhc_text<<" edge";
      for(const auto& point:edge_geometry)first_fhc_text<<' '<<point;
      first_fhc_text<<" removal "<<removal<<'\n';
      double line[2][3]{},triangle[3][3]{},intersection[3]{};
      const int segment_start=fhc_probe_reference.SurEdgs[
          fhc_prefix.pending_edge].iStart;
      const int segment_end=fhc_probe_reference.SurEdgs[
          fhc_prefix.pending_edge].iEnd;
      for(int axis=0;axis<3;++axis) {
        line[0][axis]=fhc_probe_reference.Nodes[segment_start].pt[axis];
        line[1][axis]=fhc_probe_reference.Nodes[segment_end].pt[axis];
        for(int corner=0;corner<3;++corner)
          triangle[corner][axis]=fhc_probe_reference.Nodes[feature[corner]].pt[axis];
      }
      int intersection_type=0,intersection_code=0;
      dt::GEOM_FUNC::lin_tri_intersect3d(
          line,triangle,&intersection_type,&intersection_code,intersection);
      const double start_weight=dt::GEOM_FUNC::orient3d(
          triangle[0],triangle[1],triangle[2],line[0]);
      const double end_weight=dt::GEOM_FUNC::orient3d(
          triangle[0],triangle[1],triangle[2],line[1]);
      const int inserted=fhc_probe_reference.addNode(false);
      for(int axis=0;axis<3;++axis)
        fhc_probe_reference.Nodes[inserted].pt[axis]=(
            fhc_probe_reference.Nodes[locking[0]].pt[axis]+
            fhc_probe_reference.Nodes[locking[1]].pt[axis]+
            intersection[axis])/3.0;
      int insertion_source=-1;
      (void)fhc_probe_reference.isMeshFace(
          feature[0],feature[1],feature[2],&insertion_source);
      int insertion_opposite=-1;
      for(int corner=0;corner<4;++corner)
        if(fhc_probe_reference.Elems[insertion_source].form[corner]!=feature[0]&&
           fhc_probe_reference.Elems[insertion_source].form[corner]!=feature[1]&&
           fhc_probe_reference.Elems[insertion_source].form[corner]!=feature[2])
          insertion_opposite=corner;
      int location_tet=fhc_probe_reference.getP2T(segment_start);
      const int location=fhc_probe_reference.locate_pnt(inserted,location_tet);
      std::vector<int> insertion_cavity{{insertion_source,
          fhc_probe_reference.getNeig(insertion_source,insertion_opposite)}};
      const int insertion_result=location<1?0:
          fhc_probe_reference.BW_insert_vertex(inserted,insertion_cavity,3);
      first_fhc_text<<"scheduler_steiner1_reference_placement face";
      for(const int vertex:feature)
        first_fhc_text<<' '<<node_key(fhc_probe_reference,vertex);
      first_fhc_text<<" edge";
      for(const int vertex:locking)
        first_fhc_text<<' '<<node_key(fhc_probe_reference,vertex);
      first_fhc_text<<" hit "<<point_key(intersection)
                    <<" weights "<<std::hexfloat<<start_weight<<' '
                    <<end_weight
                    <<" point "<<node_key(fhc_probe_reference,inserted)<<'\n';
      if(!recorded_first_fhc) {
        first_fhc_text<<"scheduler_steiner1_first_insert result "
                      <<insertion_result<<" location "<<location<<" point "
                      <<node_key(fhc_probe_reference,inserted)<<'\n';
        write_cells(first_fhc_text,"scheduler_steiner1_first_insert",
                    current_cells(fhc_probe_reference));
        recorded_first_fhc=true;
      }
      if(insertion_result>0)
        (void)fhc_probe_reference.recoverEdge(fhc_prefix.pending_edge,0,0);
      else
        fhc_probe_reference.DelNod(inserted);
    }
    const auto events=run_scheduler_trace(reference);
    const auto trace=reference_seed_stages(mesh,args);
    if(trace.stages.size()!=9U||trace.stages.back()!=seed)return 13;
    std::ofstream out(output);
    if(!out)return 11;
    write_order(out,reference,mesh);
    write_seed_stages(out,trace.stages);
    if(std::getenv("WANG_REFERENCE_DIAGNOSTICS")!=nullptr)
      for(std::size_t stage=0;stage<trace.p2tStages.size();++stage)
        for(const auto& record:trace.p2tStages[stage])
          out<<"seed_p2t "<<stage<<' '<<record<<'\n';
    write_seed_slots(out,trace.liveSlots);
    for(std::size_t stage=0;stage<trace.liveRawSlots.size();++stage)
      for(const auto& entry:trace.liveRawSlots[stage]) {
        out<<"seed_slot_raw "<<stage<<' '<<entry.first;
        for(const auto& point:entry.second)out<<' '<<point;
        out<<'\n';
      }
    for(std::size_t stage=0;stage<trace.originalSlots.size();++stage)
      for(const auto& entry:trace.originalSlots[stage]) {
        out<<"seed_original_slot "<<stage<<' '<<entry.first;
        for(const auto& point:entry.second)out<<' '<<point;
        out<<'\n';
      }
    for(std::size_t stage=0;stage<trace.originalRawSlots.size();++stage)
      for(const auto& entry:trace.originalRawSlots[stage]) {
        out<<"seed_original_slot_raw "<<stage<<' '<<entry.first;
        for(const auto& point:entry.second)out<<' '<<point;
        out<<'\n';
      }
    for(const auto& line:trace.originalPlanOrder)out<<line<<'\n';
    write_box_carriers(out,trace.boxCarriers);
    write_seed_plan(out,trace.eighth);
    if(std::getenv("WANG_REFERENCE_DIAGNOSTICS")!=nullptr)
      for(const auto& record:pre_boundary_p2t)
        out<<"seed_pre_boundary_p2t "<<record<<'\n';
    write_cells(out,"seed",seed);
    out<<state_text.str();
    out<<direction_text.str();
    out<<edge_contact_text.str();
    write_synthetic_edge_contact(out);
    out<<local_sequence_text.str();
    out<<direct_operation_text.str();
    out<<primitive_operation_text.str();
    out<<first_scheduler_operation_text.str();
    out<<"scheduler_local_prefix_next "<<local_prefix.next_round
       <<" attempts "<<local_prefix.attempts<<'\n';
    for(const auto& event:local_prefix.attempt_events) {
      out<<"scheduler_local_prefix_attempt_event "
         <<node_key(prefix_reference,event.start)<<' '
         <<node_key(prefix_reference,event.end)<<" info "<<event.info
         <<" level "<<event.flip_level<<" result "<<event.result;
      if(std::getenv("WANG_REFERENCE_DIAGNOSTICS")!=nullptr) {
        out<<" direction "<<event.direction;
        for(const auto vertex:event.source)out<<' '<<node_key(prefix_reference,vertex);
      }
      out<<'\n';
    }
    write_cells(out,"scheduler_local_prefix",local_prefix.cells);
    for(std::size_t attempt=0;attempt<local_prefix.cells_after_attempt.size();++attempt) {
      const auto stage="scheduler_local_prefix_attempt_"+std::to_string(attempt);
      write_cells(out,stage.c_str(),local_prefix.cells_after_attempt[attempt]);
      std::vector<std::string> ordered;
      for(const auto& cell:local_prefix.raw_cells_after_attempt[attempt]) {
        std::ostringstream record;record<<"scheduler_raw_local_prefix_attempt_"
                                      <<attempt;
        for(const auto vertex:cell)record<<' '<<node_key(reference,vertex);
        ordered.push_back(record.str());
      }
      std::sort(ordered.begin(),ordered.end());
      for(const auto& record:ordered)out<<record<<'\n';
      // P2T is normal scheduler state, not a verbose-only observation:
      // subsequent finddirection calls begin from it.
      std::vector<std::string> p2t_records;
      for(std::size_t node=0;node<local_prefix.p2t_after_attempt[attempt].size();++node) {
        if(prefix_reference.isDelNod(static_cast<int>(node)))continue;
        std::ostringstream record;
        record<<"scheduler_p2t_attempt_"<<attempt<<' '<<node_key(prefix_reference,static_cast<int>(node));
        for(const auto vertex:local_prefix.p2t_after_attempt[attempt][node])
          if(vertex>=0)record<<' '<<node_key(prefix_reference,vertex);
        p2t_records.push_back(record.str());
      }
      std::sort(p2t_records.begin(),p2t_records.end());
      for(const auto& record:p2t_records)out<<record<<'\n';
      if(std::getenv("WANG_REFERENCE_DIAGNOSTICS")!=nullptr) {
      for(std::size_t step=0;step<local_prefix.primitive_steps_after_attempt[attempt].size();++step) {
        const auto& primitive=local_prefix.primitive_steps_after_attempt[attempt][step];
        out<<"scheduler_primitive_attempt_"<<attempt<<"_step_"<<step
           <<" direction "<<primitive.direction<<" result "<<primitive.result;
        for(const auto vertex:primitive.source)out<<' '<<node_key(prefix_reference,vertex);
        out<<'\n';
        for(std::size_t node=0;node<primitive.p2t.size();++node) {
          if(prefix_reference.isDelNod(static_cast<int>(node))||
             static_cast<int>(node)==prefix_reference.ghost)continue;
          const auto& carrier=primitive.p2t[node];
          if(carrier[0]<0)continue;
          out<<"scheduler_primitive_p2t_attempt_"<<attempt<<"_step_"<<step
             <<' '<<node_key(prefix_reference,static_cast<int>(node));
          for(const auto vertex:carrier)out<<' '<<node_key(prefix_reference,vertex);
          out<<'\n';
        }
      }
      for(std::size_t pass=0;pass<local_prefix.raw_cells_after_local_pass[attempt].size();++pass) {
        std::vector<std::string> pass_records;
        for(const auto& cell:local_prefix.raw_cells_after_local_pass[attempt][pass]) {
          std::ostringstream record;
          record<<"scheduler_raw_local_prefix_attempt_"<<attempt<<"_pass_"<<pass;
          for(const auto vertex:cell)record<<' '<<node_key(reference,vertex);
          pass_records.push_back(record.str());
        }
        std::sort(pass_records.begin(),pass_records.end());
        for(const auto& record:pass_records)out<<record<<'\n';
      }
      }
    }
    out<<"scheduler_full_search_walk "<<full_walk<<" forward "<<full_forward
       <<" reverse "<<full_reverse<<" feature_count "<<full_features.size()<<'\n';
    for(const auto& feature:full_features) {
      out<<"scheduler_full_search_feature";
      const unsigned count=feature[2]<0?2U:3U;
      std::vector<std::string> geometry;
      for(unsigned i=0;i<count;++i)
        geometry.push_back(node_key(prefix_reference,feature[i]));
      std::sort(geometry.begin(),geometry.end());
      for(const auto& point:geometry)out<<' '<<point;
      out<<'\n';
    }
    write_cells(out,"scheduler_full_search_pre_removal",
                full_pre_removal);
    out<<"scheduler_full_search_result "<<full_result<<'\n';
    write_cells(out,"scheduler_full_search_post_removal",
                full_post_removal);
    out<<"scheduler_pre_steiner_next 5 attempts "
       <<local_prefix.attempts+1U<<'\n';
    write_cells(out,"scheduler_pre_steiner",full_post_removal);
    auto ordered_pre_steiner=pre_steiner_reference;
    write_ordered_cells(out,"scheduler_pre_steiner_ordered",ordered_pre_steiner);
    auto indexed_pre_steiner=pre_steiner_reference;
    write_indexed_state(out,"scheduler_pre_steiner_indexed",indexed_pre_steiner);
    write_hull_cells(out,"author_pre_steiner_hull",indexed_pre_steiner);
    auto direction_probe=pre_steiner_reference;
    std::swap(direction_probe.SurEdgs[local_prefix.pending_edge].iStart,
              direction_probe.SurEdgs[local_prefix.pending_edge].iEnd);
    int direction_cell=-1;
    const int direction=direction_probe.finddirection(
        direction_probe.SurEdgs[local_prefix.pending_edge].iStart,
        direction_probe.SurEdgs[local_prefix.pending_edge].iEnd,direction_cell);
    out<<"author_pre_steiner_direction "<<direction<<' '<<direction_cell<<'\n';
    const int direction_start=direction_probe.SurEdgs[local_prefix.pending_edge].iStart;
    const int direction_end=direction_probe.SurEdgs[local_prefix.pending_edge].iEnd;
    out<<"author_pre_steiner_direction_endpoints "<<direction_start<<' '
       <<direction_end<<'\n';
    const auto& direction_tet=direction_probe.Elems[55].form;
    out<<"author_pre_steiner_carrier_signs "
       <<std::hexfloat
       <<dt::GEOM_FUNC::orient3d(direction_probe.Nodes[direction_start].pt,
                                 direction_probe.Nodes[direction_tet[1]].pt,
                                 direction_probe.Nodes[direction_tet[2]].pt,
                                 direction_probe.Nodes[direction_end].pt)<<' '
       <<dt::GEOM_FUNC::orient3d(direction_probe.Nodes[direction_start].pt,
                                 direction_probe.Nodes[direction_tet[2]].pt,
                                 direction_probe.Nodes[direction_tet[3]].pt,
                                 direction_probe.Nodes[direction_end].pt)<<' '
       <<dt::GEOM_FUNC::orient3d(direction_probe.Nodes[direction_start].pt,
                                 direction_probe.Nodes[direction_tet[3]].pt,
                                 direction_probe.Nodes[direction_tet[1]].pt,
                                 direction_probe.Nodes[direction_end].pt)<<'\n';
    out<<"author_pre_steiner_carrier_neighbor "
       <<direction_probe.getNeig(55,1)<<'\n';
    std::vector<std::array<int,3>> direction_features;
    (void)direction_probe.findIntersectwithEdgs(
        local_prefix.pending_edge,direction_features);
    for(const auto& feature:direction_features) {
      out<<"author_pre_steiner_feature";
      for(const int vertex:feature)out<<' '<<vertex;
      out<<'\n';
    }
    out<<"scheduler_steiner1_result "<<steiner1_result<<" point_count "
       <<steiner1_points.size()<<'\n';
    for(const auto& point:steiner1_points)
      out<<"scheduler_steiner1_point "<<point<<'\n';
    write_cells(out,"scheduler_steiner1",steiner1_cells);
    out<<first_fhc_text.str();
    out<<full5_text.str();
    write_scheduler(out,events);
    return out?0:12;
  }
  const auto initial_missing_edges=missing_edges(reference);
  const auto initial_missing_facets=missing_facets(reference);
  // Retain the native scheduler trace from an identical seed state.  The
  // production oracle run below remains untouched; this copy makes the first
  // segment-recovery divergence observable rather than inferred from cells.
  auto scheduler_trace_reference=reference;
  const auto scheduler_events=run_scheduler_trace(scheduler_trace_reference);
  if(reference.AutorecoverEdges(args)!=0)return 5;
  const auto segment=current_cells(reference);
  const auto segment_missing_edges=missing_edges(reference);
  const auto segment_missing_facets=missing_facets(reference);
  struct FacetStep {
    std::array<std::string,3> facet{};
    int result{};
    std::vector<GeometryCell> cells;
  };
  std::vector<FacetStep> facet_steps;
  auto facet_trace_reference=reference;
  // recoverFacesPass first marks every constraint already present in the
  // segment mesh. Reproduce that preamble before constructing its lost queue.
  for(auto& surface_facet:facet_trace_reference.SurTris)
    if(facet_trace_reference.isMeshFace(surface_facet.form[0],
                                       surface_facet.form[1],
                                       surface_facet.form[2]))
      surface_facet.info=1;
  for(int facet_index=0;
      facet_index<static_cast<int>(facet_trace_reference.SurTris.size());
      ++facet_index) {
    if(facet_trace_reference.isDelSurTri(facet_index)||
       facet_trace_reference.isRecBndTri(facet_index))continue;
    FacetStep step;
    for(unsigned corner=0;corner<3U;++corner)
      step.facet[corner]=node_key(
          facet_trace_reference,
          facet_trace_reference.SurTris[facet_index].form[corner]);
    std::sort(step.facet.begin(),step.facet.end());
    step.result=facet_trace_reference.recoverFace(facet_index,0);
    step.cells=current_cells(facet_trace_reference);
    facet_steps.push_back(std::move(step));
  }
  if(reference.recoverFacesPass(args)!=0)return 6;
  const auto facet=current_cells(reference);
  const auto facet_missing=missing_facets(reference);
  if(reference.removeStPass(args)!=0)return 7;
  const auto interior_insertions=reference.addst;
  const auto boundary_insertions=reference.addstbnd;
  if(reference.ColorVirtualTet(args)<0)return 8;
  if(reference.RemoveTet(args)!=0)return 9;
  if(reference.outMesh(mesh,args)!=1)return 10;
  const auto final_cells=output_cells(mesh);

  std::ofstream out(output);
  if(!out)return 11;
  out<<"fixture "<<fixture<<'\n';
  write_order(out,reference,mesh);
  out<<"initial_missing_edges "<<initial_missing_edges<<'\n';
  out<<"initial_missing_facets "<<initial_missing_facets<<'\n';
  out<<"segment_missing_edges "<<segment_missing_edges<<'\n';
  out<<"segment_missing_facets "<<segment_missing_facets<<'\n';
  out<<"facet_missing_facets "<<facet_missing<<'\n';
  out<<"interior_insertions "<<interior_insertions<<'\n';
  out<<"boundary_insertions "<<boundary_insertions<<'\n';
  out<<"segment_topology_changed "<<(segment!=seed?1:0)<<'\n';
  write_scheduler(out,scheduler_events);
  out<<"facet_step_count "<<facet_steps.size()<<'\n';
  for(std::size_t step=0;step<facet_steps.size();++step) {
    out<<"facet_step_target "<<step;
    for(const auto& point:facet_steps[step].facet)out<<' '<<point;
    out<<" result "<<facet_steps[step].result<<'\n';
    const auto label="facet_step_"+std::to_string(step);
    write_cells(out,label.c_str(),facet_steps[step].cells);
  }
  write_cells(out,"seed",seed);
  write_cells(out,"segment",segment);
  write_cells(out,"facet",facet);
  write_cells(out,"final",final_cells);
  return out?0:12;
}
