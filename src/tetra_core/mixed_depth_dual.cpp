#include "tetra_core/mixed_depth_dual.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

namespace tetra {
namespace {

struct RawIncident {
  VertexId vertex{};
  TetId conforming_cell{invalid_tet};
  TetId logical_owner{invalid_tet};
  bool transition{};
};

struct RawNeighbour {
  VertexId vertex{};
  VertexId neighbour{};
};

struct RawFace {
  std::array<VertexId,3> vertices{};
};

constexpr std::array<std::array<std::size_t,2>,6> tetrahedron_edges{{
    {{0U,1U}},{{0U,2U}},{{0U,3U}},{{1U,2U}},{{1U,3U}},{{2U,3U}}}};

Vec3 canonical_barycentre(
    const TetMesh& mesh,std::array<VertexId,4> vertices,std::size_t count) {
  std::sort(vertices.begin(),vertices.begin()+static_cast<std::ptrdiff_t>(count));
  Vec3 result{};
  for(std::size_t index=0;index<count;++index)
    result=result+mesh.vertices()[vertices[index]];
  return result/static_cast<double>(count);
}

void polygonize_flag_tetrahedron(
    const Sphere& surface,const std::array<Vec3,4>& points,
    TetId owner,std::vector<MixedDepthDualPatchTriangle>& output) {
  std::array<double,4> distances{};
  for(std::size_t index=0;index<points.size();++index)
    distances[index]=surface.signed_distance(points[index]);
  std::array<Vec3,4> crossings{};
  std::size_t crossing_count{};
  for(const auto edge:tetrahedron_edges){
    const auto first=edge[0],second=edge[1];
    if((distances[first]<0.0)==(distances[second]<0.0))continue;
    crossings[crossing_count++]=surface.edge_intersection(
        points[first],points[second]);
  }
  if(crossing_count<3U)return;
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto cross=[](Vec3 a,Vec3 b){
    return Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
  };
  Vec3 centre{};
  for(std::size_t index=0;index<crossing_count;++index)
    centre=centre+crossings[index];
  centre=centre/static_cast<double>(crossing_count);
  const auto normal=surface.normal(centre);
  const auto reference=std::abs(normal.z)<0.9?Vec3{0.0,0.0,1.0}:
                                               Vec3{0.0,1.0,0.0};
  const auto axis_u=cross(reference,normal);
  const auto axis_v=cross(normal,axis_u);
  std::sort(crossings.begin(),
            crossings.begin()+static_cast<std::ptrdiff_t>(crossing_count),
            [&](Vec3 left,Vec3 right){
    const auto left_offset=left-centre,right_offset=right-centre;
    return std::atan2(dot(left_offset,axis_v),dot(left_offset,axis_u))<
           std::atan2(dot(right_offset,axis_v),dot(right_offset,axis_u));
  });
  for(std::size_t index=1U;index+1U<crossing_count;++index){
    Triangle triangle{crossings[0],crossings[index],crossings[index+1U]};
    auto triangle_normal=cross(triangle.b-triangle.a,triangle.c-triangle.a);
    const auto triangle_centre=(triangle.a+triangle.b+triangle.c)/3.0;
    if(dot(triangle_normal,surface.normal(triangle_centre))<0.0){
      std::swap(triangle.b,triangle.c);
      triangle_normal=cross(triangle.b-triangle.a,triangle.c-triangle.a);
    }
    if(dot(triangle_normal,triangle_normal)>1e-24)
      output.push_back({owner,triangle});
  }
}

MixedDepthDualDecision topology_rejection(
    std::span<const TetId> owners,MixedDepthDualStarTopology topology) {
  if(owners.empty()||std::ranges::any_of(
         owners,[](TetId owner){return owner==invalid_tet;}))
    return MixedDepthDualDecision::malformed_incident;
  if(!topology.closed)return MixedDepthDualDecision::missing_incident_cell;
  if(!topology.manifold)return MixedDepthDualDecision::nonmanifold_star;
  if(topology.incident_cell_count<4U||topology.unique_neighbour_count<4U)
    return MixedDepthDualDecision::degenerate_star;
  return MixedDepthDualDecision::accepted;
}

} // namespace

MixedDepthDualResolution resolve_mixed_depth_dual_owner(
    std::span<const TetId> logical_owners,
    MixedDepthDualStarTopology topology) {
  const auto rejection=topology_rejection(logical_owners,topology);
  if(rejection!=MixedDepthDualDecision::accepted)return {invalid_tet,rejection};
  unsigned int finest_depth{};
  for(const TetId owner:logical_owners)
    finest_depth=std::max(finest_depth,tet_depth(owner));
  TetId winner=invalid_tet;
  for(const TetId owner:logical_owners)
    if(tet_depth(owner)==finest_depth&&(winner==invalid_tet||owner<winner))
      winner=owner;
  return {winner,MixedDepthDualDecision::accepted};
}

MixedDepthDualDecision evaluate_mixed_depth_dual_contender(
    std::span<const TetId> logical_owners,TetId self,
    MixedDepthDualStarTopology topology) {
  const auto rejection=topology_rejection(logical_owners,topology);
  if(rejection!=MixedDepthDualDecision::accepted)return rejection;
  if(self==invalid_tet||std::ranges::find(logical_owners,self)==logical_owners.end())
    return MixedDepthDualDecision::malformed_incident;
  const unsigned int self_depth=tet_depth(self);
  for(const TetId owner:logical_owners)
    if(tet_depth(owner)>self_depth)
      return MixedDepthDualDecision::finer_level_owner;
  for(const TetId owner:logical_owners)
    if(tet_depth(owner)==self_depth&&owner<self)
      return MixedDepthDualDecision::same_level_predecessor;
  return MixedDepthDualDecision::accepted;
}

MixedDepthDualIndex build_mixed_depth_dual_index(const TetMesh& mesh) {
  MixedDepthDualIndex result;
  const auto volume=mesh.conforming_volume();
  result.hierarchy_revision=volume.hierarchy_revision();

  std::vector<RawIncident> raw_incidents;
  std::vector<RawNeighbour> raw_neighbours;
  std::vector<RawFace> raw_faces;
  raw_incidents.reserve(volume.size()*4U);
  raw_neighbours.reserve(volume.size()*12U);
  raw_faces.reserve(volume.size()*4U);
  constexpr std::array<std::array<std::size_t,3>,4> faces{{
      {{1U,2U,3U}},{{0U,3U,2U}},{{0U,1U,3U}},{{0U,2U,1U}}}};

  for(std::size_t cell_index=0;cell_index<volume.size();++cell_index){
    const auto cell=volume.cell(cell_index);
    const auto& tet=mesh.tetrahedron(cell.address);
    for(std::size_t corner=0;corner<tet.vertices.size();++corner){
      raw_incidents.push_back(
          {tet.vertices[corner],cell.address,cell.logical_owner,cell.transition});
      for(std::size_t other=0;other<tet.vertices.size();++other)
        if(other!=corner)
          raw_neighbours.push_back({tet.vertices[corner],tet.vertices[other]});
    }
    for(const auto& face:faces){
      RawFace record{{tet.vertices[face[0]],tet.vertices[face[1]],
                      tet.vertices[face[2]]}};
      std::sort(record.vertices.begin(),record.vertices.end());
      raw_faces.push_back(record);
    }
  }
  std::sort(raw_incidents.begin(),raw_incidents.end(),[](const auto& a,const auto& b){
    return std::tie(a.vertex,a.conforming_cell,a.logical_owner,a.transition)<
           std::tie(b.vertex,b.conforming_cell,b.logical_owner,b.transition);
  });
  std::sort(raw_neighbours.begin(),raw_neighbours.end(),[](const auto& a,const auto& b){
    return std::tie(a.vertex,a.neighbour)<std::tie(b.vertex,b.neighbour);
  });
  raw_neighbours.erase(std::unique(raw_neighbours.begin(),raw_neighbours.end(),
      [](const auto& a,const auto& b){
        return a.vertex==b.vertex&&a.neighbour==b.neighbour;
      }),raw_neighbours.end());
  std::sort(raw_faces.begin(),raw_faces.end(),[](const auto& a,const auto& b){
    return a.vertices<b.vertices;
  });

  std::vector<bool> boundary_vertex(mesh.vertices().size());
  std::vector<bool> nonmanifold_vertex(mesh.vertices().size());
  for(std::size_t begin=0;begin<raw_faces.size();){
    std::size_t end=begin+1U;
    while(end<raw_faces.size()&&raw_faces[end].vertices==raw_faces[begin].vertices)
      ++end;
    if(end-begin!=2U){
      auto& flags=end-begin==1U?boundary_vertex:nonmanifold_vertex;
      for(const VertexId vertex:raw_faces[begin].vertices)flags[vertex]=true;
    }
    begin=end;
  }

  result.candidates.reserve(mesh.vertices().size());
  result.incidents.reserve(raw_incidents.size());
  result.contenders.reserve(raw_incidents.size());
  std::vector<TetId> owner_scratch;
  owner_scratch.reserve(raw_incidents.size());
  std::size_t neighbour_begin{};
  for(std::size_t begin=0;begin<raw_incidents.size();){
    std::size_t end=begin+1U;
    while(end<raw_incidents.size()&&
          raw_incidents[end].vertex==raw_incidents[begin].vertex)++end;
    const VertexId vertex=raw_incidents[begin].vertex;
    while(neighbour_begin<raw_neighbours.size()&&
          raw_neighbours[neighbour_begin].vertex<vertex)++neighbour_begin;
    std::size_t neighbour_end=neighbour_begin;
    while(neighbour_end<raw_neighbours.size()&&
          raw_neighbours[neighbour_end].vertex==vertex)++neighbour_end;

    MixedDepthDualCandidate candidate;
    candidate.primal_vertex=vertex;
    candidate.incident_begin=static_cast<std::uint32_t>(result.incidents.size());
    candidate.incident_count=static_cast<std::uint32_t>(end-begin);
    candidate.contender_begin=static_cast<std::uint32_t>(result.contenders.size());
    candidate.unique_neighbour_count=static_cast<std::uint32_t>(
        neighbour_end-neighbour_begin);
    owner_scratch.clear();
    for(std::size_t index=begin;index<end;++index){
      const auto& incident=raw_incidents[index];
      result.incidents.push_back({incident.conforming_cell,incident.logical_owner,
          static_cast<std::uint32_t>(tet_depth(incident.logical_owner)),
          incident.transition});
      owner_scratch.push_back(incident.logical_owner);
    }
    std::sort(owner_scratch.begin(),owner_scratch.end());
    owner_scratch.erase(
        std::unique(owner_scratch.begin(),owner_scratch.end()),owner_scratch.end());
    candidate.contender_count=static_cast<std::uint32_t>(owner_scratch.size());
    const MixedDepthDualStarTopology topology{
        !boundary_vertex[vertex],!nonmanifold_vertex[vertex],
        candidate.incident_count,candidate.unique_neighbour_count};
    const auto resolution=resolve_mixed_depth_dual_owner(owner_scratch,topology);
    candidate.owner=resolution.owner;
    candidate.decision=resolution.decision;
    for(const TetId owner:owner_scratch)
      result.contenders.push_back({owner,static_cast<std::uint32_t>(tet_depth(owner)),
          evaluate_mixed_depth_dual_contender(owner_scratch,owner,topology)});
    result.candidates.push_back(candidate);
    begin=end;
    neighbour_begin=neighbour_end;
  }
  return result;
}

void MixedDepthDualPatchBuilder::rebuild_index(const TetMesh& mesh) {
  index_=build_mixed_depth_dual_index(mesh);
  dependencies_.clear();
  dependencies_.reserve(index_.contenders.size());
  for(const auto& candidate:index_.candidates){
    if(candidate.decision!=MixedDepthDualDecision::accepted)continue;
    const auto contenders=std::span{index_.contenders}.subspan(
        candidate.contender_begin,candidate.contender_count);
    for(const auto& contender:contenders)
      dependencies_.push_back({candidate.owner,contender.logical_owner});
  }
  std::sort(dependencies_.begin(),dependencies_.end());
  dependencies_.erase(
      std::unique(dependencies_.begin(),dependencies_.end()),dependencies_.end());
}

void MixedDepthDualPatchBuilder::generate_patches(
    const TetMesh& mesh,const Sphere& surface,
    std::span<const TetId> selected_patch_owners,
    std::vector<MixedDepthDualPatchTriangle>& output) {
  if(index_.hierarchy_revision!=mesh.revision())
    throw std::logic_error("mixed-depth dual index is stale");
  const auto selected=[&](TetId owner){
    return selected_patch_owners.empty()||std::binary_search(
        selected_patch_owners.begin(),selected_patch_owners.end(),owner);
  };
  output.clear();
  output.reserve(index_.incidents.size()*3U);
  for(const auto& candidate:index_.candidates){
    if(candidate.decision!=MixedDepthDualDecision::accepted||
       !selected(candidate.owner))continue;
    const auto incidents=std::span{index_.incidents}.subspan(
        candidate.incident_begin,candidate.incident_count);
    for(const auto& incident:incidents){
      const auto& cell=mesh.tetrahedron(incident.conforming_cell);
      std::array<VertexId,3> others{};
      std::size_t other_count{};
      for(const VertexId vertex:cell.vertices)
        if(vertex!=candidate.primal_vertex)others[other_count++]=vertex;
      if(other_count!=3U)throw std::logic_error(
          "mixed-depth dual incident does not contain its candidate vertex");
      std::sort(others.begin(),others.end());
      do{
        const std::array<Vec3,4> flag{{
            mesh.vertices()[candidate.primal_vertex],
            canonical_barycentre(mesh,
                {{candidate.primal_vertex,others[0],0U,0U}},2U),
            canonical_barycentre(mesh,
                {{candidate.primal_vertex,others[0],others[1],0U}},3U),
            canonical_barycentre(mesh,cell.vertices,4U)}};
        polygonize_flag_tetrahedron(surface,flag,candidate.owner,output);
      }while(std::next_permutation(others.begin(),others.end()));
    }
  }
  std::stable_sort(output.begin(),output.end(),[](const auto& left,const auto& right){
    return left.patch_owner<right.patch_owner;
  });
}

std::size_t MixedDepthDualPatchBuilder::retained_bytes() const noexcept {
  return index_.candidates.capacity()*sizeof(MixedDepthDualCandidate)+
      index_.incidents.capacity()*sizeof(MixedDepthDualIncident)+
      index_.contenders.capacity()*sizeof(MixedDepthDualContender)+
      dependencies_.capacity()*sizeof(MixedDepthDualPatchDependency);
}

std::vector<Triangle> extract_mixed_depth_dual_isosurface(
    const TetMesh& mesh,const Sphere& surface) {
  MixedDepthDualPatchBuilder builder;
  builder.rebuild_index(mesh);
  std::vector<MixedDepthDualPatchTriangle> patches;
  builder.generate_patches(mesh,surface,{},patches);
  std::vector<Triangle> result;
  result.reserve(patches.size());
  for(const auto& patch:patches)result.push_back(patch.triangle);
  return result;
}

} // namespace tetra
