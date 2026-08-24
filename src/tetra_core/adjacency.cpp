#include "tetra_core/adjacency.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <tuple>

namespace tetra {
namespace {

using Clock=std::chrono::steady_clock;
constexpr std::uint32_t no_half=std::numeric_limits<std::uint32_t>::max();
constexpr std::array<std::array<std::size_t,3>,4> face_corners{{
    {{1,2,3}},{{0,3,2}},{{0,1,3}},{{0,2,1}}}};

struct FaceRecord {
  std::array<VertexId,3> key{};
  std::array<VertexId,3> oriented{};
  std::uint32_t half{};
};

std::uint8_t orientation_code(const std::array<VertexId,3>& first,
                              const std::array<VertexId,3>& second){
  std::uint8_t code{};
  for(std::size_t corner=0;corner<3U;++corner){
    const auto found=std::find(second.begin(),second.end(),first[corner]);
    if(found==second.end())return 0xffU;
    code|=static_cast<std::uint8_t>(found-second.begin())<<(corner*2U);
  }
  return code;
}

std::uint64_t mix(std::uint64_t hash,std::uint64_t value){
  hash^=value;return hash*1099511628211ULL;
}

} // namespace

AdjacencyExperiment build_adjacency_experiment(
    const TetMesh& mesh,AdjacencyRepresentation representation,
    std::span<const TetId> selected_cells) {
  const auto build_start=Clock::now();
  AdjacencyExperiment result;
  result.representation=representation;
  result.sibling_child_template.fill(0xffU);
  result.sibling_face_template.fill(0xffU);
  if(selected_cells.empty()){
    const auto cells=mesh.conforming_volume().addresses();
    result.cells.assign(cells.begin(),cells.end());
  }else result.cells.assign(selected_cells.begin(),selected_cells.end());
  std::sort(result.cells.begin(),result.cells.end());
  result.cells.erase(std::unique(result.cells.begin(),result.cells.end()),result.cells.end());
  result.opposite_half_facets.assign(result.cells.size()*4U,no_half);
  result.orientations.assign(result.cells.size()*4U,0xffU);
  result.vertex_anchors.assign(mesh.vertices().size(),no_half);

  std::vector<FaceRecord> faces;
  faces.reserve(result.cells.size()*4U);
  for(std::size_t cell=0;cell<result.cells.size();++cell){
    const auto& tet=mesh.tetrahedron(result.cells[cell]);
    for(std::size_t face=0;face<4U;++face){
      FaceRecord record;
      for(std::size_t corner=0;corner<3U;++corner)
        record.oriented[corner]=tet.vertices[face_corners[face][corner]];
      record.key=record.oriented;
      std::sort(record.key.begin(),record.key.end());
      record.half=static_cast<std::uint32_t>(cell*4U+face);
      faces.push_back(record);
      for(const VertexId vertex:record.oriented)
        if(result.vertex_anchors[vertex]==no_half)
          result.vertex_anchors[vertex]=record.half;
    }
  }
  std::sort(faces.begin(),faces.end(),[](const auto& left,const auto& right){
    return left.key<right.key||(left.key==right.key&&left.half<right.half);
  });
  std::uint64_t multiplicity_hash=1469598103934665603ULL;
  for(std::size_t begin=0;begin<faces.size();){
    std::size_t end=begin+1U;
    while(end<faces.size()&&faces[end].key==faces[begin].key)++end;
    multiplicity_hash=mix(multiplicity_hash,faces[begin].key[0]);
    multiplicity_hash=mix(multiplicity_hash,faces[begin].key[1]);
    multiplicity_hash=mix(multiplicity_hash,faces[begin].key[2]);
    multiplicity_hash=mix(multiplicity_hash,end-begin);
    if(end-begin==1U)++result.metrics.boundary_faces;
    else if(end-begin==2U){
      ++result.metrics.manifold_pairs;
      const auto first=faces[begin],second=faces[begin+1U];
      result.opposite_half_facets[first.half]=second.half;
      result.opposite_half_facets[second.half]=first.half;
      result.orientations[first.half]=orientation_code(first.oriented,second.oriented);
      result.orientations[second.half]=orientation_code(second.oriented,first.oriented);
    }else ++result.metrics.nonmanifold_faces;
    begin=end;
  }
  result.metrics.owner_multiplicity_hash=multiplicity_hash;

  // Learn the regular sibling relation once per parent orientation. Queries
  // inside a red family then use only parent path, child bits, and this table.
  for(std::size_t half=0;half<result.opposite_half_facets.size();++half){
    const auto opposite=result.opposite_half_facets[half];
    if(opposite==no_half)continue;
    const TetId first=result.cells[half/4U],second=result.cells[opposite/4U];
    const bool siblings=tet_depth(first)>=3U&&tet_depth(first)==tet_depth(second)&&
        tet_root(first)==tet_root(second)&&(tet_path(first)>>3U)==(tet_path(second)>>3U);
    if(!siblings)continue;
    const TetId parent=make_tet_id(tet_root(first),tet_path(first)>>3U);
    const std::size_t type=tet_refinement_type(parent)%3U;
    const std::size_t child=static_cast<std::size_t>(tet_path(first)&7U);
    const std::size_t slot=(type*8U+child)*4U+(half%4U);
    result.sibling_child_template[slot]=static_cast<std::uint8_t>(tet_path(second)&7U);
    result.sibling_face_template[slot]=static_cast<std::uint8_t>(opposite%4U);
    ++result.metrics.template_wired_half_facets;
  }

  if(representation==AdjacencyRepresentation::path_arithmetic){
    for(std::size_t half=0;half<result.opposite_half_facets.size();++half){
      const auto opposite=result.opposite_half_facets[half];
      if(opposite==no_half)continue;
      const TetId address=result.cells[half/4U];
      bool represented=false;
      if(tet_depth(address)>=3U){
        const TetId parent=make_tet_id(tet_root(address),tet_path(address)>>3U);
        const std::size_t type=tet_refinement_type(parent)%3U;
        const std::size_t child=static_cast<std::size_t>(tet_path(address)&7U);
        const auto slot=(type*8U+child)*4U+(half%4U);
        const auto neighbour_child=result.sibling_child_template[slot];
        if(neighbour_child!=0xffU){
          const TetId expected=make_tet_id(
              tet_root(parent),(tet_path(parent)<<3U)|neighbour_child);
          represented=result.cells[opposite/4U]==expected;
        }
      }
      if(!represented)result.path_exceptions.push_back(
          {static_cast<std::uint32_t>(half),opposite});
    }
    result.metrics.path_exceptions=result.path_exceptions.size();
  }
  result.metrics.shared_parent_facets_connected=
      result.metrics.manifold_pairs*2U-result.metrics.template_wired_half_facets;
  const auto& dirty=mesh.last_dirty_logical_owners();
  for(std::size_t cell=0;cell<result.cells.size();++cell){
    const auto& record=mesh.tetrahedron(result.cells[cell]);
    const TetId owner=record.transition_parent==invalid_tet
        ?result.cells[cell]:record.transition_parent;
    if(std::binary_search(dirty.begin(),dirty.end(),owner))
      result.metrics.dirty_half_facets_updated+=4U;
  }
  result.metrics.build_ms=std::chrono::duration<double,std::milli>(
      Clock::now()-build_start).count();

  const auto query_start=Clock::now();
  std::uint64_t oriented_hash=1469598103934665603ULL;
  for(std::size_t half=0;half<result.opposite_half_facets.size();++half){
    oriented_hash=mix(oriented_hash,result.cells[half/4U]);
    oriented_hash=mix(oriented_hash,half%4U);
    const auto opposite=result.opposite_half_facets[half];
    oriented_hash=mix(oriented_hash,opposite==no_half?invalid_tet:
                      result.cells[opposite/4U]);
    oriented_hash=mix(oriented_hash,result.orientations[half]);
  }
  result.metrics.oriented_adjacency_hash=oriented_hash;
  result.metrics.neighbour_query_ms=std::chrono::duration<double,std::milli>(
      Clock::now()-query_start).count();

  const auto validation_start=Clock::now();
  for(std::size_t half=0;half<result.opposite_half_facets.size();++half){
    const auto opposite=result.opposite_half_facets[half];
    if(opposite!=no_half&&
       (opposite>=result.opposite_half_facets.size()||
        result.opposite_half_facets[opposite]!=half))
      ++result.metrics.nonmanifold_faces;
  }
  result.metrics.validation_ms=std::chrono::duration<double,std::milli>(
      Clock::now()-validation_start).count();
  result.metrics.retained_bytes=result.cells.capacity()*sizeof(TetId);
  if(representation==AdjacencyRepresentation::packed_half_facets||
     representation==AdjacencyRepresentation::logical_face_table){
    result.metrics.retained_bytes+=
        result.opposite_half_facets.capacity()*sizeof(std::uint32_t)+
        result.orientations.capacity()*sizeof(std::uint8_t)+
        result.vertex_anchors.capacity()*sizeof(std::uint32_t);
  }else if(representation==AdjacencyRepresentation::path_arithmetic){
    result.metrics.retained_bytes+=
        result.path_exceptions.capacity()*sizeof(PathAdjacencyException)+
        result.sibling_child_template.size()+result.sibling_face_template.size();
  }else result.metrics.retained_bytes+=faces.capacity()*sizeof(FaceRecord);
  return result;
}

} // namespace tetra
