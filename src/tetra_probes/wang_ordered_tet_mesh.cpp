#include "tetra_probes/wang_ordered_tet_mesh.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <set>

namespace tetra::probes {
namespace {

using Face=std::array<std::uint32_t,3>;

Face face_opposite(const WangOrderedTetMesh::Tet& tet,unsigned opposite) {
  Face face{};
  unsigned cursor{};
  for(unsigned corner=0;corner<4U;++corner)
    if(corner!=opposite)face[cursor++]=tet[corner];
  return face;
}

Face face_key(Face face) {
  std::sort(face.begin(),face.end());
  return face;
}

std::array<std::uint32_t,4> cell_key(WangOrderedTetMesh::Tet cell) {
  std::sort(cell.begin(),cell.end());
  return cell;
}

std::pair<unsigned,unsigned> oriented_complement(unsigned first,
                                                  unsigned second) {
  std::array<unsigned,2> remaining{};
  unsigned cursor{};
  for(unsigned corner=0;corner<4U;++corner)
    if(corner!=first&&corner!=second)remaining[cursor++]=corner;
  const auto even=[&](unsigned third,unsigned fourth) {
    const std::array<unsigned,4> permutation{{first,second,third,fourth}};
    unsigned inversions{};
    for(unsigned i=0;i<4U;++i)for(unsigned j=i+1U;j<4U;++j)
      inversions+=permutation[i]>permutation[j];
    return inversions%2U==0U;
  };
  if(!even(remaining[0],remaining[1]))std::swap(remaining[0],remaining[1]);
  return {remaining[0],remaining[1]};
}

std::array<unsigned,3> decoded_face(unsigned opposite) {
  switch(opposite) {
    case 0U:return {{1U,3U,2U}};
    case 1U:return {{2U,3U,0U}};
    case 2U:return {{0U,3U,1U}};
    default:return {{0U,1U,2U}};
  }
}

bool contains(const WangOrderedTetMesh::Tet& cell,std::uint32_t vertex) {
  return std::find(cell.begin(),cell.end(),vertex)!=cell.end();
}

} // namespace

WangOrderedTetMesh::WangOrderedTetMesh(
    std::size_t vertex_count,const std::vector<Tet>& cells,
    std::int32_t ghost_vertex)
    : point_to_cell_(vertex_count,no_neighbour),
      deleted_vertices_(vertex_count,false),ghost_vertex_(ghost_vertex) {
  if(ghost_vertex_<0||static_cast<std::size_t>(ghost_vertex_)>=vertex_count)
    ghost_vertex_=no_neighbour;
  cells_.reserve(cells.size());
  for(const auto& vertices:cells)cells_.push_back({vertices,{},false});
  topology_failure_=rebuild_topology();
}

bool WangOrderedTetMesh::set_exact_affine_planes(
    std::vector<std::uint64_t> stable_vertex_ids,
    std::vector<ExactAffinePlaneProvenance> planes) {
  if(stable_vertex_ids.size()!=point_to_cell_.size())return false;
  std::set<std::uint64_t> known(stable_vertex_ids.begin(),stable_vertex_ids.end());
  if(known.size()!=stable_vertex_ids.size())return false;
  for(const auto& plane:planes)
    if(plane.construction.axis>=3U||plane.construction.denominator==0U||
       plane.vertex_ids.size()<4U||!std::is_sorted(plane.vertex_ids.begin(),
                                                    plane.vertex_ids.end())||
       std::adjacent_find(plane.vertex_ids.begin(),plane.vertex_ids.end())!=
           plane.vertex_ids.end()||
       !std::ranges::all_of(plane.vertex_ids,[&](std::uint64_t id) {
         return known.contains(id);
       })) return false;
  stable_vertex_ids_=std::move(stable_vertex_ids);
  exact_affine_planes_=std::move(planes);
  return true;
}

bool WangOrderedTetMesh::semantically_coplanar(const Tet& cell) const {
  if(stable_vertex_ids_.empty()||exact_affine_planes_.empty()||
     std::ranges::any_of(cell,[&](std::uint32_t vertex) {
       return vertex>=stable_vertex_ids_.size();
     }))return false;
  std::array<std::uint64_t,4> ids{};
  for(unsigned corner=0U;corner<4U;++corner)
    ids[corner]=stable_vertex_ids_[cell[corner]];
  return is_semantically_coplanar(ids,exact_affine_planes_);
}

bool WangOrderedTetMesh::set_source_slot_layout(
    const std::vector<std::pair<std::size_t,Tet>>& live_slots,
    std::size_t slot_count,const std::vector<std::size_t>& vacancies) {
  if(slot_count<live_slots.size())return false;
  std::vector<bool> live(slot_count),queued(slot_count);
  std::vector<Cell> replacement(slot_count);
  for(auto& cell:replacement)cell.deleted=true;
  for(const auto& [slot,vertices]:live_slots) {
    if(slot>=slot_count||live[slot])return false;
    live[slot]=true;
    replacement[slot]={vertices,{},false};
  }
  for(const auto slot:vacancies) {
    if(slot>=slot_count||live[slot]||queued[slot])return false;
    queued[slot]=true;
  }
  for(std::size_t slot=0;slot<slot_count;++slot)
    if(!live[slot]&&!queued[slot])return false;
  cells_=std::move(replacement);
  vacancies_.assign(vacancies.begin(),vacancies.end());
  return rebuild_topology()==TopologyFailure::none;
}

WangOrderedTetMesh::TopologyFailure WangOrderedTetMesh::rebuild_topology() {
  topology_failure_=TopologyFailure::none;
  hull_faces_.clear();
  const auto previous_incidence=point_to_cell_;
  std::fill(point_to_cell_.begin(),point_to_cell_.end(),no_neighbour);
  using Use=std::pair<std::uint32_t,std::uint8_t>;
  std::map<Face,std::vector<Use>> faces;
  std::set<std::array<std::uint32_t,4>> unique_cells;
  for(std::size_t cell_index=0;cell_index<cells_.size();++cell_index) {
    auto& cell=cells_[cell_index];
    cell.neighbours.fill(no_neighbour);
    if(cell.deleted)continue;
    auto sorted=cell.vertices;
    std::sort(sorted.begin(),sorted.end());
    if(std::adjacent_find(sorted.begin(),sorted.end())!=sorted.end())
      return topology_failure_=TopologyFailure::repeated_vertex;
    if(sorted.back()>=point_to_cell_.size()||
       std::any_of(cell.vertices.begin(),cell.vertices.end(),[&](const auto vertex) {
         return deleted_vertices_[vertex];
       }))
      return topology_failure_=TopologyFailure::invalid_vertex;
    if(!unique_cells.insert(cell_key(cell.vertices)).second)
      return topology_failure_=TopologyFailure::duplicate_cell;
    for(const auto vertex:cell.vertices) {
      const auto previous=previous_incidence[vertex];
      const bool previous_valid=previous>=0&&
          static_cast<std::size_t>(previous)<cells_.size()&&
          !cells_[static_cast<std::size_t>(previous)].deleted&&
          contains(cells_[static_cast<std::size_t>(previous)].vertices,vertex);
      point_to_cell_[vertex]=previous_valid?previous:
          static_cast<std::int32_t>(cell_index);
    }
    for(unsigned opposite=0;opposite<4U;++opposite)
      faces[face_key(face_opposite(cell.vertices,opposite))].push_back(
          {static_cast<std::uint32_t>(cell_index),
           static_cast<std::uint8_t>(opposite)});
  }
  for(const auto& [face,uses]:faces) {
    if(uses.size()>2U)
      return topology_failure_=TopologyFailure::nonmanifold_face;
    if(uses.size()==1U) {
      // `face` is the sorted map key used solely for incidence lookup.
      // The ghost cone must instead retain the source cell's local order:
      // finddirection and the DNC/DFC tables observe that permutation.
      const auto cell=uses.front().first;
      const auto opposite=uses.front().second;
      hull_faces_.push_back({face_opposite(cells_[cell].vertices,opposite),
                              cell,opposite});
      continue;
    }
    const auto [first_cell,first_face]=uses[0];
    const auto [second_cell,second_face]=uses[1];
    cells_[first_cell].neighbours[first_face]=static_cast<std::int32_t>(second_cell);
    cells_[second_cell].neighbours[second_face]=static_cast<std::int32_t>(first_cell);
  }
  return topology_failure_;
}

bool WangOrderedTetMesh::set_point_incidence(
    const std::vector<Tet>& incident_cells) {
  if(incident_cells.size()!=point_to_cell_.size())return false;
  std::map<std::array<std::uint32_t,4>,std::uint32_t> active;
  for(std::size_t cell=0;cell<cells_.size();++cell)
    if(!cells_[cell].deleted)
      active.emplace(cell_key(cells_[cell].vertices),
                     static_cast<std::uint32_t>(cell));
  std::vector<std::int32_t> selected(point_to_cell_.size(),no_neighbour);
  for(std::size_t vertex=0;vertex<incident_cells.size();++vertex) {
    const auto found=active.find(cell_key(incident_cells[vertex]));
    if(found==active.end()||
       std::find(cells_[found->second].vertices.begin(),
                 cells_[found->second].vertices.end(),vertex)==
           cells_[found->second].vertices.end())return false;
    selected[vertex]=static_cast<std::int32_t>(found->second);
  }
  point_to_cell_=std::move(selected);
  return true;
}

std::uint32_t WangOrderedTetMesh::add_cell(const Tet& vertices) {
  if(vacancies_.empty()) {
    const auto result=static_cast<std::uint32_t>(cells_.size());
    cells_.push_back({vertices,{},false});
    return result;
  }
  const auto result=vacancies_.front();
  vacancies_.pop_front();
  cells_[result]={vertices,{},false};
  return result;
}

bool WangOrderedTetMesh::erase_cell(std::uint32_t cell) {
  if(cell>=cells_.size()||cells_[cell].deleted)return false;
  cells_[cell].deleted=true;
  cells_[cell].neighbours.fill(no_neighbour);
  vacancies_.push_back(cell);
  return true;
}

bool WangOrderedTetMesh::rotate_hull_child_ghost_last(std::uint32_t cell) {
  if(ghost_vertex_<0||cell>=cells_.size()||cells_[cell].deleted)return false;
  auto& vertices=cells_[cell].vertices;
  const auto ghost=static_cast<std::uint32_t>(ghost_vertex_);
  const auto found=std::find(vertices.begin(),vertices.end(),ghost);
  if(found==vertices.end())return true;
  const auto position=static_cast<unsigned>(found-vertices.begin());
  // DT::flipnm applies matchtet({form[i+1], form[i+2], form[i+3], form[i]})
  // only for i in [0, 2].  A child whose ghost is already last is unchanged.
  if(position==3U)return true;
  const Tet original=vertices;
  for(unsigned corner=0U;corner<4U;++corner)
    vertices[corner]=original[(position+corner+1U)%4U];
  return rebuild_topology()==TopologyFailure::none;
}

WangOrderedTetMesh::CavityReplacementResult
WangOrderedTetMesh::replace_cavity_with_appended_vertex(
    const std::vector<Tet>& cavity,const std::vector<Tet>& replacement,
    std::optional<std::uint64_t> appended_stable_id,
    std::vector<ExactAffinePlaneProvenance> updated_planes) {
  CavityReplacementResult result;
  if(cavity.empty()||replacement.empty())return result;
  const auto appended=static_cast<std::uint32_t>(vertex_count());
  std::map<std::array<std::uint32_t,4>,std::uint32_t> active;
  for(std::size_t slot=0;slot<cells_.size();++slot)
    if(!cells_[slot].deleted)
      active.emplace(cell_key(cells_[slot].vertices),
                     static_cast<std::uint32_t>(slot));
  std::set<std::uint32_t> erased;
  for(const auto& cell:cavity) {
    const auto found=active.find(cell_key(cell));
    if(found==active.end()||!erased.insert(found->second).second)return result;
    result.erased_cells.push_back(found->second);
  }
  for(const auto& cell:replacement) {
    auto key=cell;std::sort(key.begin(),key.end());
    if(std::adjacent_find(key.begin(),key.end())!=key.end()||
       key.back()!=appended||key.front()>=appended||
       std::find(cell.begin(),cell.end(),appended)==cell.end())return result;
  }

  WangOrderedTetMesh trial=*this;
  trial.point_to_cell_.push_back(no_neighbour);
  trial.deleted_vertices_.push_back(false);
  if(!stable_vertex_ids_.empty()) {
    if(!appended_stable_id)return result;
    auto updated_ids=stable_vertex_ids_;
    updated_ids.push_back(*appended_stable_id);
    if(!trial.set_exact_affine_planes(std::move(updated_ids),
                                      std::move(updated_planes)))return result;
  }
  for(const auto& cell:replacement)
    if(trial.semantically_coplanar(cell)) {
      if(std::getenv("WANG_PLANE_PREDICATE_TRACE")!=nullptr) {
        std::cerr<<"plane_owned_commit_semantic cell=";
        for(const auto vertex:cell)std::cerr<<vertex<<',';
        std::cerr<<" ids=";
        for(const auto vertex:cell)
          std::cerr<<trial.stable_vertex_ids_[vertex]<<',';
        std::cerr<<'\n';
      }
      return result;
    }
  for(const auto& cell:replacement)
    result.created_cells.push_back(trial.add_cell(cell));
  for(const auto slot:result.erased_cells)
    if(!trial.erase_cell(slot))return {};
  for(std::size_t i=0;i<replacement.size();++i)
    for(const auto vertex:replacement[i])
      trial.point_to_cell_[vertex]=static_cast<std::int32_t>(result.created_cells[i]);
  if(trial.rebuild_topology()!=TopologyFailure::none||!trial.audit().accepted())
    return {};
  *this=std::move(trial);
  result.accepted=true;
  return result;
}

bool WangOrderedTetMesh::collapse_vertex_into(std::uint32_t vertex,
                                              std::uint32_t keep) {
  if(vertex==keep||vertex>=vertex_count()||keep>=vertex_count()||
     deleted_vertices_[vertex]||deleted_vertices_[keep])return false;
  WangOrderedTetMesh trial=*this;
  bool found=false;
  std::vector<std::uint32_t> shell;
  for(std::size_t index=0;index<trial.cells_.size();++index) {
    auto& cell=trial.cells_[index];
    if(cell.deleted||!contains(cell.vertices,vertex))continue;
    found=true;
    if(contains(cell.vertices,keep)) {
      shell.push_back(static_cast<std::uint32_t>(index));
      continue;
    }
    for(auto& corner:cell.vertices)if(corner==vertex)corner=keep;
  }
  if(!found)return false;
  for(const auto cell:shell)
    if(!trial.erase_cell(cell))return false;
  trial.deleted_vertices_[vertex]=true;
  trial.point_to_cell_[vertex]=no_neighbour;
  for(const auto& cell:trial.cells_)
    if(!cell.deleted&&trial.semantically_coplanar(cell.vertices))return false;
  if(trial.rebuild_topology()!=TopologyFailure::none||!trial.audit().accepted())
    return false;
  *this=std::move(trial);
  return true;
}

bool WangOrderedTetMesh::remove_vertex_four_to_one(std::uint32_t vertex) {
  if(vertex>=vertex_count()||deleted_vertices_[vertex])return false;
  WangOrderedTetMesh trial=*this;
  std::vector<std::uint32_t> incident;
  std::set<std::uint32_t> neighbours;
  for(std::size_t index=0;index<trial.cells_.size();++index) {
    const auto& cell=trial.cells_[index];
    if(cell.deleted||!contains(cell.vertices,vertex))continue;
    incident.push_back(static_cast<std::uint32_t>(index));
    for(const auto corner:cell.vertices)if(corner!=vertex)neighbours.insert(corner);
  }
  if(incident.size()!=4U||neighbours.size()!=4U)return false;
  Tet replacement{};
  std::copy(neighbours.begin(),neighbours.end(),replacement.begin());
  if(trial.semantically_coplanar(replacement))return false;
  for(const auto cell:incident)
    if(!trial.erase_cell(cell))return false;
  const auto replacement_cell=trial.add_cell(replacement);
  (void)replacement_cell;
  trial.deleted_vertices_[vertex]=true;
  trial.point_to_cell_[vertex]=no_neighbour;
  if(trial.rebuild_topology()!=TopologyFailure::none||!trial.audit().accepted())
    return false;
  *this=std::move(trial);
  return true;
}

bool WangOrderedTetMesh::is_vertex_deleted(std::uint32_t vertex) const noexcept {
  return vertex>=deleted_vertices_.size()||deleted_vertices_[vertex];
}

WangOrderedTetMesh::Flip32Result WangOrderedTetMesh::flip32(
    std::uint32_t first,std::uint32_t second,std::uint32_t anchor_cell) {
  std::vector<std::uint32_t> shell;
  if(anchor_cell!=static_cast<std::uint32_t>(no_neighbour)) {
    const auto ordered=find_shell(anchor_cell,first,second);
    if(!ordered.closed||ordered.cells.size()!=3U)return {};
    shell=ordered.cells;
  } else {
    for(std::size_t slot=0;slot<cells_.size();++slot) {
      const auto& cell=cells_[slot];
      if(!cell.deleted&&contains(cell.vertices,first)&&
         contains(cell.vertices,second))
        shell.push_back(static_cast<std::uint32_t>(slot));
    }
  }
  return flip32(shell,first,second);
}

WangOrderedTetMesh::Flip32Result WangOrderedTetMesh::flip32(
    const std::vector<std::uint32_t>& supplied_shell,
    std::uint32_t first,std::uint32_t second) {
  Flip32Result result;
  result.removed_edge={first,second};
  if(first==second||first>=vertex_count()||second>=vertex_count())return result;
  if(supplied_shell.size()!=3U)return result;
  auto shell=supplied_shell;
  for(const auto slot:shell)
    if(slot>=cells_.size()||cells_[slot].deleted||
       !contains(cells_[slot].vertices,first)||
       !contains(cells_[slot].vertices,second))return result;
  // Literal flipnm hull adjustment: while oldtet[0] is a hull tetrahedron,
  // swap it with slots one and two in that order.  It intentionally is not a
  // search for the first finite allocation slot.
  if(ghost_vertex_>=0&&first!=static_cast<std::uint32_t>(ghost_vertex_)&&
     second!=static_cast<std::uint32_t>(ghost_vertex_)) {
    const auto ghost=static_cast<std::uint32_t>(ghost_vertex_);
    for(unsigned rotations=0U;rotations<3U&&
        contains(cells_[shell[0]].vertices,ghost);++rotations) {
      std::swap(shell[0],shell[1]);
      std::swap(shell[0],shell[2]);
    }
    if(contains(cells_[shell[0]].vertices,ghost))return result;
  }

  const auto& anchor=cells_[shell[0]].vertices;
  const auto first_position=static_cast<unsigned>(
      std::find(anchor.begin(),anchor.end(),first)-anchor.begin());
  const auto second_position=static_cast<unsigned>(
      std::find(anchor.begin(),anchor.end(),second)-anchor.begin());
  if(first_position>=4U||second_position>=4U)return result;
  const auto [third_position,fourth_position]=
      oriented_complement(first_position,second_position);
  const auto third=anchor[third_position];
  const auto fourth=anchor[fourth_position];

  std::uint32_t across_third=std::numeric_limits<std::uint32_t>::max();
  std::uint32_t across_fourth=std::numeric_limits<std::uint32_t>::max();
  for(std::size_t i=1U;i<shell.size();++i) {
    const auto& cell=cells_[shell[i]].vertices;
    if(contains(cell,fourth)&&!contains(cell,third))across_third=shell[i];
    if(contains(cell,third)&&!contains(cell,fourth))across_fourth=shell[i];
  }
  if(across_third==std::numeric_limits<std::uint32_t>::max()||
     across_fourth==std::numeric_limits<std::uint32_t>::max()||
     across_third==across_fourth)return result;

  const auto& next=cells_[across_third].vertices;
  const auto apex=std::find_if(next.begin(),next.end(),[&](auto vertex) {
    return vertex!=first&&vertex!=second&&vertex!=fourth;
  });
  if(apex==next.end()||*apex==third||
     !contains(cells_[across_fourth].vertices,*apex))return result;

  const Tet first_replacement{{first,third,fourth,*apex}};
  const Tet second_replacement{{second,fourth,third,*apex}};
  const std::set<std::uint32_t> removed(shell.begin(),shell.end());
  const auto first_key=cell_key(first_replacement);
  const auto second_key=cell_key(second_replacement);
  if(first_key==second_key)return result;
  for(std::size_t slot=0;slot<cells_.size();++slot)
    if(!cells_[slot].deleted&&!removed.contains(static_cast<std::uint32_t>(slot))) {
      const auto key=cell_key(cells_[slot].vertices);
      if(key==first_key||key==second_key)return result;
    }

  result.created_cells[0]=add_cell(first_replacement);
  result.created_cells[1]=add_cell(second_replacement);
  result.erased_cells={shell[0],across_third,across_fourth};
  for(const auto slot:result.erased_cells)
    if(!erase_cell(slot))return result;
  if(rebuild_topology()!=TopologyFailure::none)return result;
  point_to_cell_[first]=static_cast<std::int32_t>(result.created_cells[0]);
  point_to_cell_[second]=static_cast<std::int32_t>(result.created_cells[1]);
  point_to_cell_[third]=static_cast<std::int32_t>(result.created_cells[1]);
  point_to_cell_[fourth]=static_cast<std::int32_t>(result.created_cells[1]);
  point_to_cell_[*apex]=static_cast<std::int32_t>(result.created_cells[1]);
  if(std::getenv("WANG_OWNED_PRIMITIVE_TRACE")!=nullptr)
    std::cerr<<"owned_flip32 edge "<<first<<' '<<second<<" cells "
             <<result.created_cells[0]<<' '<<result.created_cells[1]<<'\n';
  result.accepted=true;
  return result;
}

WangOrderedTetMesh::Flip23Result WangOrderedTetMesh::flip23(
    std::uint32_t cell,std::uint8_t opposite) {
  Flip23Result result;
  const auto reject=[&](const char* why) {
    if(std::getenv("WANG_OWNED_PRIMITIVE_DIAGNOSTICS")!=nullptr)
      std::cerr<<"owned_flip23_reject "<<cell<<' '
               <<static_cast<unsigned>(opposite)<<' '<<why<<'\n';
    return result;
  };
  if(cell>=cells_.size()||cells_[cell].deleted||opposite>=4U)return reject("input");
  const auto neighbour=cells_[cell].neighbours[opposite];
  if(neighbour<0||static_cast<std::size_t>(neighbour)>=cells_.size()||
     cells_[static_cast<std::size_t>(neighbour)].deleted)return reject("neighbour");
  const auto other=static_cast<std::uint32_t>(neighbour);
  const auto reciprocal=std::find(cells_[other].neighbours.begin(),
                                  cells_[other].neighbours.end(),
                                  static_cast<std::int32_t>(cell));
  if(reciprocal==cells_[other].neighbours.end())return reject("reciprocal");
  const auto other_opposite=static_cast<unsigned>(
      reciprocal-cells_[other].neighbours.begin());

  const auto first_apex=cells_[cell].vertices[opposite];
  const auto second_apex=cells_[other].vertices[other_opposite];
  if(first_apex==second_apex)return reject("same-apex");
  auto face_positions=decoded_face(other_opposite);
  // DT::flip23 applies these two DFC permutations before naming pc/pd/pe
  // whenever the neighbouring tetrahedron is a hull tetrahedron.  Besides
  // placing the ghost at `pe`, this selects the source's created-cell and
  // P2T assignment order; it is not merely a display rotation.
  if(ghost_vertex_>=0) {
    const auto ghost=static_cast<std::uint32_t>(ghost_vertex_);
    if(cells_[other].vertices[face_positions[0]]==ghost) {
      const auto original=face_positions;
      // Source: swap(c,e), then swap(d,c): (c,d,e) -> (d,e,c).
      face_positions={{original[1],original[2],original[0]}};
    } else if(cells_[other].vertices[face_positions[1]]==ghost) {
      const auto original=face_positions;
      // Source: swap(d,e), then swap(d,c): (c,d,e) -> (e,c,d).
      face_positions={{original[2],original[0],original[1]}};
    }
  }
  const auto third=cells_[other].vertices[face_positions[0]];
  const auto fourth=cells_[other].vertices[face_positions[1]];
  const auto fifth=cells_[other].vertices[face_positions[2]];
  if(!contains(cells_[cell].vertices,third)||
     !contains(cells_[cell].vertices,fourth)||
     !contains(cells_[cell].vertices,fifth))return reject("face");

  const std::array<Tet,3> replacements{{
      {{second_apex,fourth,first_apex,fifth}},
      {{second_apex,first_apex,third,fifth}},
      {{second_apex,first_apex,fourth,third}}}};
  std::set<std::array<std::uint32_t,4>> replacement_keys;
  for(const auto& replacement:replacements)
    if(!replacement_keys.insert(cell_key(replacement)).second)return reject("duplicate-new");
  for(std::size_t slot=0;slot<cells_.size();++slot)
    if(!cells_[slot].deleted&&slot!=cell&&slot!=other&&
       replacement_keys.contains(cell_key(cells_[slot].vertices)))return reject("duplicate-live");

  for(unsigned i=0;i<3U;++i)result.created_cells[i]=add_cell(replacements[i]);
  result.erased_cells={cell,other};
  if(!erase_cell(cell)||!erase_cell(other))return reject("erase");
  if(rebuild_topology()!=TopologyFailure::none)return reject("topology");
  point_to_cell_[first_apex]=static_cast<std::int32_t>(result.created_cells[2]);
  point_to_cell_[second_apex]=static_cast<std::int32_t>(result.created_cells[2]);
  point_to_cell_[third]=static_cast<std::int32_t>(result.created_cells[2]);
  point_to_cell_[fourth]=static_cast<std::int32_t>(result.created_cells[2]);
  point_to_cell_[fifth]=static_cast<std::int32_t>(result.created_cells[1]);
  if(std::getenv("WANG_OWNED_PRIMITIVE_TRACE")!=nullptr)
    std::cerr<<"owned_flip23 face "<<third<<' '<<fourth<<' '<<fifth
             <<" cells "<<result.created_cells[0]<<' '
             <<result.created_cells[1]<<' '<<result.created_cells[2]<<'\n';
  result.accepted=true;
  return result;
}

WangOrderedTetMesh::EdgeShell WangOrderedTetMesh::find_shell(
    std::uint32_t start_cell,std::uint32_t first,std::uint32_t second) const {
  EdgeShell result;
  result.edge={first,second};
  if(start_cell>=cells_.size()||cells_[start_cell].deleted||first==second)
    return result;
  const auto& start=cells_[start_cell].vertices;
  const auto first_it=std::find(start.begin(),start.end(),first);
  const auto second_it=std::find(start.begin(),start.end(),second);
  if(first_it==start.end()||second_it==start.end())return result;
  const auto first_position=static_cast<unsigned>(first_it-start.begin());
  const auto second_position=static_cast<unsigned>(second_it-start.begin());
  const auto [start_opposite,end_position]=
      oriented_complement(first_position,second_position);
  const auto ring_start=start[start_opposite];
  auto ring_end=start[end_position];
  result.cells.push_back(start_cell);
  result.ring_vertices={ring_start,ring_end};

  auto current=start_cell;
  auto crossing_opposite=start_opposite;
  for(std::size_t steps=0;steps<cells_.size();++steps) {
    const auto neighbour=cells_[current].neighbours[crossing_opposite];
    if(neighbour<0||static_cast<std::size_t>(neighbour)>=cells_.size()||
       cells_[static_cast<std::size_t>(neighbour)].deleted)return result;
    const auto next=static_cast<std::uint32_t>(neighbour);
    if(std::find(result.cells.begin(),result.cells.end(),next)!=result.cells.end())
      return result;
    const auto reciprocal=std::find(cells_[next].neighbours.begin(),
                                    cells_[next].neighbours.end(),
                                    static_cast<std::int32_t>(current));
    if(reciprocal==cells_[next].neighbours.end())return result;
    const auto new_ring_position=static_cast<unsigned>(
        reciprocal-cells_[next].neighbours.begin());
    const auto new_ring=cells_[next].vertices[new_ring_position];
    result.cells.push_back(next);
    if(new_ring==ring_start) {
      result.closed=result.cells.size()==result.ring_vertices.size();
      return result;
    }
    result.ring_vertices.push_back(new_ring);

    unsigned next_crossing=4U;
    for(unsigned corner=0;corner<4U;++corner)
      if(corner!=new_ring_position&&cells_[next].vertices[corner]!=first&&
         cells_[next].vertices[corner]!=second) {
        if(cells_[next].vertices[corner]!=ring_end)return result;
        next_crossing=corner;
        break;
      }
    if(next_crossing>=4U)return result;
    current=next;
    crossing_opposite=next_crossing;
    ring_end=new_ring;
  }
  return result;
}

std::vector<std::uint32_t> WangOrderedTetMesh::find_sphere(
    std::uint32_t vertex) const {
  std::vector<std::uint32_t> sphere;
  if(vertex>=point_to_cell_.size()||deleted_vertices_[vertex])return sphere;
  std::int32_t carrier=point_to_cell_[vertex];
  if(carrier<0||static_cast<std::size_t>(carrier)>=cells_.size()||
     cells_[static_cast<std::size_t>(carrier)].deleted||
     !contains(cells_[static_cast<std::size_t>(carrier)].vertices,vertex)) {
    carrier=no_neighbour;
    for(std::size_t cell=0;cell<cells_.size();++cell)
      if(!cells_[cell].deleted&&contains(cells_[cell].vertices,vertex)) {
        carrier=static_cast<std::int32_t>(cell);break;
      }
  }
  if(carrier<0)return sphere;
  std::set<std::uint32_t> visited;
  visited.insert(static_cast<std::uint32_t>(carrier));
  sphere.push_back(static_cast<std::uint32_t>(carrier));
  for(std::size_t cursor=0;cursor<sphere.size();++cursor) {
    const auto cell=sphere[cursor];
    const auto position=std::find(cells_[cell].vertices.begin(),
                                  cells_[cell].vertices.end(),vertex);
    if(position==cells_[cell].vertices.end())continue;
    const auto opposite=static_cast<unsigned>(position-cells_[cell].vertices.begin());
    for(unsigned face=0;face<4U;++face) {
      if(face==opposite)continue;
      const auto neighbour=cells_[cell].neighbours[face];
      if(neighbour<0||static_cast<std::size_t>(neighbour)>=cells_.size())continue;
      const auto next=static_cast<std::uint32_t>(neighbour);
      if(cells_[next].deleted||!contains(cells_[next].vertices,vertex))continue;
      if(visited.insert(next).second)sphere.push_back(next);
    }
  }
  return sphere;
}

WangOrderedTetMesh::Audit WangOrderedTetMesh::audit() const {
  Audit result;
  result.failure=topology_failure_;
  result.reciprocal_neighbours=true;
  result.point_incidence_complete=true;
  result.hull_complete=true;
  std::set<std::pair<std::uint32_t,std::uint8_t>> hull_uses;
  for(const auto& hull:hull_faces_)hull_uses.insert({hull.cell,hull.opposite});
  std::vector<bool> used(point_to_cell_.size());
  for(std::size_t cell_index=0;cell_index<cells_.size();++cell_index) {
    const auto& cell=cells_[cell_index];
    if(cell.deleted)continue;
    ++result.active_cells;
    for(const auto vertex:cell.vertices) {
      if(vertex>=used.size()) {result.point_incidence_complete=false;continue;}
      used[vertex]=true;
    }
    for(unsigned opposite=0;opposite<4U;++opposite) {
      const auto neighbour=cell.neighbours[opposite];
      if(neighbour==no_neighbour) {
        if(!hull_uses.contains({static_cast<std::uint32_t>(cell_index),
                               static_cast<std::uint8_t>(opposite)}))
          result.hull_complete=false;
        continue;
      }
      if(neighbour<0||static_cast<std::size_t>(neighbour)>=cells_.size()||
         cells_[static_cast<std::size_t>(neighbour)].deleted) {
        result.reciprocal_neighbours=false;
        continue;
      }
      const auto reciprocal=std::count(
          cells_[static_cast<std::size_t>(neighbour)].neighbours.begin(),
          cells_[static_cast<std::size_t>(neighbour)].neighbours.end(),
          static_cast<std::int32_t>(cell_index));
      if(reciprocal!=1)result.reciprocal_neighbours=false;
    }
  }
  for(std::size_t vertex=0;vertex<used.size();++vertex) {
    if(!used[vertex])continue;
    const auto cell=point_to_cell_[vertex];
    if(cell<0||static_cast<std::size_t>(cell)>=cells_.size()||
       cells_[static_cast<std::size_t>(cell)].deleted||
       std::find(cells_[static_cast<std::size_t>(cell)].vertices.begin(),
                 cells_[static_cast<std::size_t>(cell)].vertices.end(),vertex)==
           cells_[static_cast<std::size_t>(cell)].vertices.end())
      result.point_incidence_complete=false;
  }
  return result;
}

} // namespace tetra::probes
