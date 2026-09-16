#include "tetra_probes/advancing_front_pocket.hpp"

#include "tetra_probes/exact_binary_predicates.hpp"
#include "tetra_probes/surface_core_contract.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <deque>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

namespace tetra::probes {
namespace {
using Face=std::array<std::uint32_t,3>;
using Tet=std::array<std::uint32_t,4>;
using Edge=std::array<std::uint32_t,2>;

double dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
double norm(Vec3 a){return std::sqrt(dot(a,a));}
Face key(Face face){std::sort(face.begin(),face.end());return face;}
Edge edge_key(std::uint32_t a,std::uint32_t b){if(b<a)std::swap(a,b);return {a,b};}

bool opposite_winding(Face first,Face second){
  if(key(first)!=key(second))return false;
  for(std::size_t shift=0;shift<3U;++shift)
    if(first[0]==second[shift]&&first[1]==second[(shift+2U)%3U]&&
       first[2]==second[(shift+1U)%3U])return true;
  return false;
}

std::array<Face,4> outward_faces(Tet tet,const std::vector<Vec3>& vertices){
  std::array<Face,4> result{};
  for(std::size_t opposite=0;opposite<4U;++opposite){
    std::size_t cursor{};
    for(std::size_t i=0;i<4U;++i)if(i!=opposite)result[opposite][cursor++]=tet[i];
    const auto& face=result[opposite];
    if(dot(cross(vertices[face[1]]-vertices[face[0]],vertices[face[2]]-vertices[face[0]]),
           vertices[tet[opposite]]-vertices[face[0]])>0.0)
      std::swap(result[opposite][1],result[opposite][2]);
  }
  return result;
}

std::array<Face,3> side_faces(Face base,std::uint32_t apex,
                              const std::vector<Vec3>& vertices){
  std::array<Face,3> result{{{{base[0],base[1],apex}},{{base[1],base[2],apex}},{{base[2],base[0],apex}}}};
  for(std::size_t i=0;i<3U;++i){const auto opposite=base[(i+2U)%3U];const auto face=result[i];
    if(dot(cross(vertices[face[1]]-vertices[face[0]],vertices[face[2]]-vertices[face[0]]),
           vertices[opposite]-vertices[face[0]])<0.0)std::swap(result[i][1],result[i][2]);}
  return result;
}

bool contains_strict(const std::array<Vec3,4>& tet,Vec3 point){
  const auto six=[](Vec3 a,Vec3 b,Vec3 c,Vec3 d){return dot(b-a,cross(c-a,d-a));};
  const auto total=six(tet[0],tet[1],tet[2],tet[3]);if(std::abs(total)<1e-14)return false;
  constexpr double epsilon=1e-10;
  const std::array<double,4> weights{{six(point,tet[1],tet[2],tet[3])/total,
      six(tet[0],point,tet[2],tet[3])/total,six(tet[0],tet[1],point,tet[3])/total,
      six(tet[0],tet[1],tet[2],point)/total}};
  return std::ranges::all_of(weights,[](double value){return value>epsilon&&value<1.0-epsilon;});
}

std::vector<std::pair<std::size_t,std::size_t>> residual_sector_pairs(
    const std::vector<std::size_t>& face_indices,Edge edge,
    const std::vector<Face>& faces,const std::vector<Vec3>& vertices){
  std::vector<std::pair<std::size_t,std::size_t>> result;
  if(face_indices.size()<2U)return result;
  auto axis=vertices[edge[1]]-vertices[edge[0]];
  const auto axis_length=norm(axis);if(axis_length<=1e-14)return result;
  axis=axis/axis_length;
  const auto radial=[&](std::size_t face_index){
    const auto face=faces[face_index];auto third=face[0];
    for(const auto vertex:face)if(vertex!=edge[0]&&vertex!=edge[1]){third=vertex;break;}
    auto value=vertices[third]-vertices[edge[0]];value=value-axis*dot(value,axis);
    const auto magnitude=norm(value);return magnitude>1e-14?value/magnitude:Vec3{};
  };
  const auto basis=radial(face_indices.front());if(norm(basis)<=1e-14)return result;
  const auto tangent=cross(axis,basis);
  struct AngularFace {double angle{};std::size_t index{};};
  std::vector<AngularFace> angular;angular.reserve(face_indices.size());
  for(const auto index:face_indices){const auto direction=radial(index);
    angular.push_back({std::atan2(dot(direction,tangent),dot(direction,basis)),index});}
  std::ranges::sort(angular,[](const auto& left,const auto& right){
    return left.angle!=right.angle?left.angle<right.angle:left.index<right.index;});
  for(std::size_t i=0;i<angular.size();++i){const auto j=(i+1U)%angular.size();
    auto end=angular[j].angle;if(j==0U)end+=2.0*std::numbers::pi;
    const auto middle=(angular[i].angle+end)*0.5;
    const auto direction=basis*std::cos(middle)+tangent*std::sin(middle);
    const auto first=faces[angular[i].index],second=faces[angular[j].index];
    const auto normal=[&](Face face){return cross(vertices[face[1]]-vertices[face[0]],
                                                 vertices[face[2]]-vertices[face[0]]);};
    if(dot(normal(first),direction)<-1e-14&&dot(normal(second),direction)<-1e-14)
      result.emplace_back(angular[i].index,angular[j].index);
  }
  return result;
}

struct BoundaryAudit {bool closed{true};bool oriented{true};bool intersection_free{true};};
BoundaryAudit audit_boundary(const std::vector<Face>& faces,const std::vector<Vec3>& vertices){
  BoundaryAudit result;std::map<Edge,std::vector<std::size_t>> edge_faces;
  for(std::size_t face_index=0;face_index<faces.size();++face_index)for(std::size_t i=0;i<3U;++i)
    edge_faces[edge_key(faces[face_index][i],faces[face_index][(i+1U)%3U])].push_back(face_index);
  std::map<std::pair<std::size_t,Edge>,std::size_t> paired_uses;
  const auto follows=[](Face face,std::uint32_t a,std::uint32_t b){
    for(std::size_t i=0;i<3U;++i)if(face[i]==a&&face[(i+1U)%3U]==b)return true;
    return false;
  };
  for(const auto& [edge,indices]:edge_faces){
    for(const auto& [first,second]:residual_sector_pairs(indices,edge,faces,vertices)){
      ++paired_uses[{first,edge}];++paired_uses[{second,edge}];
      if(follows(faces[first],edge[0],edge[1])==follows(faces[second],edge[0],edge[1]))
        result.oriented=false;
    }
  }
  for(std::size_t face_index=0;face_index<faces.size();++face_index)for(std::size_t i=0;i<3U;++i){
    const auto edge=edge_key(faces[face_index][i],faces[face_index][(i+1U)%3U]);
    if(paired_uses[{face_index,edge}]!=1U)result.closed=false;
  }
  for(std::size_t i=0;i<faces.size();++i)for(std::size_t j=i+1U;j<faces.size();++j){
    if(key(faces[i])==key(faces[j])){result.intersection_free=false;continue;}
    const std::array<Vec3,3> first{{vertices[faces[i][0]],vertices[faces[i][1]],vertices[faces[i][2]]}};
    const std::array<Vec3,3> second{{vertices[faces[j][0]],vertices[faces[j][1]],vertices[faces[j][2]]}};
    if(advancing_front_triangles_cross(first,second))result.intersection_free=false;
  }
  return result;
}

double boundary_volume(const std::vector<Face>& faces,const std::vector<Vec3>& vertices){
  double result{};for(const auto face:faces)
    result+=dot(vertices[face[0]],cross(vertices[face[1]],vertices[face[2]]))/6.0;
  return std::abs(result);
}

std::string_view array_field(std::string_view json,std::string_view name){
  const auto marker=std::string{"\""}+std::string{name}+"\":";
  auto begin=json.find(marker);if(begin==std::string_view::npos)throw std::invalid_argument("missing replay field: "+std::string{name});
  begin=json.find('[',begin+marker.size());if(begin==std::string_view::npos)throw std::invalid_argument("invalid replay array");
  std::size_t depth{};
  for(std::size_t end=begin;end<json.size();++end){if(json[end]=='[')++depth;else if(json[end]==']'&&--depth==0U)return json.substr(begin,end-begin+1U);}
  throw std::invalid_argument("unterminated replay array");
}

template<class Number>std::vector<Number> numbers(std::string_view text){
  std::vector<Number> result;
  for(std::size_t i=0;i<text.size();){
    while(i<text.size()&&text[i]!= '-'&&text[i]!='+'&&(text[i]<'0'||text[i]>'9'))++i;
    if(i==text.size())break;
    const auto begin=i;
    while(i<text.size()&&(text[i]=='-'||text[i]=='+'||text[i]=='.'||text[i]=='e'||text[i]=='E'||(text[i]>='0'&&text[i]<='9')))++i;
    Number value{};const auto parsed=std::from_chars(text.data()+begin,text.data()+i,value);
    if(parsed.ec!=std::errc{})throw std::invalid_argument("invalid replay number");
    result.push_back(value);
  }
  return result;
}

template<std::size_t N>std::vector<std::array<std::uint32_t,N>> index_arrays(std::string_view text){
  const auto flat=numbers<std::uint32_t>(text);if(flat.size()%N!=0U)throw std::invalid_argument("invalid replay index array");
  std::vector<std::array<std::uint32_t,N>> result(flat.size()/N);
  for(std::size_t i=0;i<flat.size();++i)result[i/N][i%N]=flat[i];
  return result;
}
} // namespace

AdvancingFrontFillResult parse_advancing_front_replay_data(std::string_view json){
  AdvancingFrontFillResult result;
  const auto coordinates=numbers<double>(array_field(json,"vertices"));
  if(coordinates.size()%3U!=0U)throw std::invalid_argument("invalid replay vertices");
  result.vertices.resize(coordinates.size()/3U);
  for(std::size_t i=0;i<coordinates.size();++i){auto& p=result.vertices[i/3U];
    if(i%3U==0U)p.x=coordinates[i];else if(i%3U==1U)p.y=coordinates[i];else p.z=coordinates[i];}
  result.stable_vertex_ids=numbers<std::uint64_t>(array_field(json,"stableVertexIds"));
  result.tetrahedra=index_arrays<4U>(array_field(json,"tetrahedra"));
  result.active_faces=index_arrays<3U>(array_field(json,"activeFaces"));
  if(result.stable_vertex_ids.size()!=result.vertices.size())throw std::invalid_argument("replay vertex/id count mismatch");
  for(const auto tet:result.tetrahedra)for(const auto vertex:tet)if(vertex>=result.vertices.size())throw std::invalid_argument("replay tet index out of range");
  for(const auto face:result.active_faces)for(const auto vertex:face)if(vertex>=result.vertices.size())throw std::invalid_argument("replay face index out of range");
  return result;
}

std::vector<AdvancingFrontPocket> extract_advancing_front_pockets(
    const AdvancingFrontFillResult& checkpoint){
  const auto& faces=checkpoint.active_faces;std::map<Edge,std::vector<std::size_t>> edge_faces;
  for(std::size_t i=0;i<faces.size();++i)for(std::size_t e=0;e<3U;++e)
    edge_faces[edge_key(faces[i][e],faces[i][(e+1U)%3U])].push_back(i);
  std::vector<std::vector<std::size_t>> neighbours(faces.size());
  for(const auto& [edge,indices]:edge_faces)
    for(const auto& [a,b]:residual_sector_pairs(indices,edge,faces,checkpoint.vertices)){
      neighbours[a].push_back(b);neighbours[b].push_back(a);}
  std::vector<bool> visited(faces.size());std::vector<AdvancingFrontPocket> result;
  for(std::size_t seed=0;seed<faces.size();++seed)if(!visited[seed]){
    AdvancingFrontPocket pocket;std::deque<std::size_t> pending{seed};visited[seed]=true;
    while(!pending.empty()){const auto current=pending.front();pending.pop_front();
      pocket.active_face_indices.push_back(current);pocket.pocket_faces.push_back(faces[current]);
      for(const auto other:neighbours[current])if(!visited[other]){visited[other]=true;pending.push_back(other);}}
    std::ranges::sort(pocket.active_face_indices);
    const auto boundary=audit_boundary(pocket.pocket_faces,checkpoint.vertices);
    pocket.audit.pocket_closed=boundary.closed;pocket.audit.pocket_oriented=boundary.oriented;
    pocket.audit.pocket_self_intersection_free=boundary.intersection_free;
    pocket.pocket_volume=boundary_volume(pocket.pocket_faces,checkpoint.vertices);
    std::set<Face> pocket_keys;for(const auto face:pocket.pocket_faces)pocket_keys.insert(key(face));
    for(std::size_t i=0;i<checkpoint.tetrahedra.size();++i){bool adjacent=false;
      for(const auto face:outward_faces(checkpoint.tetrahedra[i],checkpoint.vertices))
        adjacent=adjacent||pocket_keys.contains(key(face));
      if(adjacent)pocket.halo_tetrahedron_indices.push_back(i);}
    std::map<Face,Face> combined;bool exact_cancellation=true;
    for(const auto face:pocket.pocket_faces)combined.emplace(key(face),face);
    pocket.audit.halo_tetrahedra_exactly_positive=true;
    for(const auto index:pocket.halo_tetrahedron_indices){const auto tet=checkpoint.tetrahedra[index];
      pocket.audit.halo_tetrahedra_exactly_positive&=exact_orientation_3d(
          checkpoint.vertices[tet[0]],checkpoint.vertices[tet[1]],checkpoint.vertices[tet[2]],checkpoint.vertices[tet[3]])==ExactPredicateSign::positive;
      for(const auto face:outward_faces(tet,checkpoint.vertices)){const auto face_key=key(face);const auto found=combined.find(face_key);
        if(found==combined.end())combined.emplace(face_key,face);
        else{exact_cancellation&=opposite_winding(found->second,face);combined.erase(found);}}
    }
    pocket.audit.interface_cancels_exactly=exact_cancellation;
    for(const auto& [unused,face]:combined){static_cast<void>(unused);pocket.repair_boundary_faces.push_back(face);for(const auto vertex:face)pocket.repair_vertex_indices.push_back(vertex);}
    std::ranges::sort(pocket.repair_vertex_indices);pocket.repair_vertex_indices.erase(
        std::unique(pocket.repair_vertex_indices.begin(),pocket.repair_vertex_indices.end()),pocket.repair_vertex_indices.end());
    const auto repair=audit_boundary(pocket.repair_boundary_faces,checkpoint.vertices);
    pocket.audit.repair_boundary_closed=repair.closed;pocket.audit.repair_boundary_oriented=repair.oriented;
    pocket.audit.repair_boundary_self_intersection_free=repair.intersection_free;
    pocket.repair_volume=boundary_volume(pocket.repair_boundary_faces,checkpoint.vertices);
    result.push_back(std::move(pocket));
  }
  return result;
}

const AdvancingFrontPocket* smallest_advancing_front_pocket(
    const std::vector<AdvancingFrontPocket>& pockets){
  if(pockets.empty())return nullptr;
  return &*std::ranges::min_element(pockets,[](const auto& left,const auto& right){
    if(left.pocket_volume!=right.pocket_volume)return left.pocket_volume<right.pocket_volume;
    return left.active_face_indices.front()<right.active_face_indices.front();});
}

AdvancingFrontPocket expand_advancing_front_pocket_to_manifold(
    const AdvancingFrontFillResult& checkpoint,const AdvancingFrontPocket& input,
    std::size_t maximum_rounds){
  auto result=input;std::set<std::size_t> selected(
      result.halo_tetrahedron_indices.begin(),result.halo_tetrahedron_indices.end());
  const auto rebuild=[&](){
    std::map<Face,Face> combined;bool cancellation=true;
    for(const auto face:result.pocket_faces)combined.emplace(key(face),face);
    result.audit.halo_tetrahedra_exactly_positive=true;
    for(const auto index:selected){const auto tet=checkpoint.tetrahedra.at(index);
      result.audit.halo_tetrahedra_exactly_positive&=exact_orientation_3d(
          checkpoint.vertices[tet[0]],checkpoint.vertices[tet[1]],checkpoint.vertices[tet[2]],checkpoint.vertices[tet[3]])==ExactPredicateSign::positive;
      for(const auto face:outward_faces(tet,checkpoint.vertices)){const auto face_key=key(face);const auto found=combined.find(face_key);
        if(found==combined.end())combined.emplace(face_key,face);
        else{cancellation&=opposite_winding(found->second,face);combined.erase(found);}}
    }
    result.halo_tetrahedron_indices.assign(selected.begin(),selected.end());result.repair_boundary_faces.clear();result.repair_vertex_indices.clear();
    for(const auto& [unused,face]:combined){static_cast<void>(unused);result.repair_boundary_faces.push_back(face);for(const auto vertex:face)result.repair_vertex_indices.push_back(vertex);}
    std::ranges::sort(result.repair_vertex_indices);result.repair_vertex_indices.erase(
        std::unique(result.repair_vertex_indices.begin(),result.repair_vertex_indices.end()),result.repair_vertex_indices.end());
    result.audit.interface_cancels_exactly=cancellation;const auto audit=audit_boundary(result.repair_boundary_faces,checkpoint.vertices);
    result.audit.repair_boundary_closed=audit.closed;result.audit.repair_boundary_oriented=audit.oriented;
    result.audit.repair_boundary_self_intersection_free=audit.intersection_free;
    result.repair_volume=boundary_volume(result.repair_boundary_faces,checkpoint.vertices);
  };
  rebuild();
  for(std::size_t round=0;round<maximum_rounds;++round){
    std::map<Edge,std::size_t> uses;
    for(const auto face:result.repair_boundary_faces)for(std::size_t i=0;i<3U;++i)
      ++uses[edge_key(face[i],face[(i+1U)%3U])];
    std::set<Edge> pinched;for(const auto& [edge,count]:uses)if(count!=2U)pinched.insert(edge);
    if(pinched.empty())break;
    bool changed=false;
    for(std::size_t index=0;index<checkpoint.tetrahedra.size();++index)if(!selected.contains(index)){
      const auto tet=checkpoint.tetrahedra[index];bool incident=false;
      for(const auto edge:pinched)incident=incident||(
          std::ranges::find(tet,edge[0])!=tet.end()&&std::ranges::find(tet,edge[1])!=tet.end());
      if(incident){selected.insert(index);changed=true;}
    }
    if(!changed)break;
    rebuild();
  }
  return result;
}

AdvancingFrontPocket make_advancing_front_pocket_repair_region(
    const AdvancingFrontFillResult& checkpoint,const AdvancingFrontPocket& source,
    std::span<const std::size_t> mutable_indices){
  auto result=source;std::set<std::size_t> selected(mutable_indices.begin(),mutable_indices.end());
  std::map<Face,Face> combined;bool cancellation=true;
  for(const auto face:source.pocket_faces)combined.emplace(key(face),face);
  result.audit.halo_tetrahedra_exactly_positive=true;
  for(const auto index:selected){const auto tet=checkpoint.tetrahedra.at(index);
    result.audit.halo_tetrahedra_exactly_positive&=exact_orientation_3d(
        checkpoint.vertices[tet[0]],checkpoint.vertices[tet[1]],checkpoint.vertices[tet[2]],checkpoint.vertices[tet[3]])==ExactPredicateSign::positive;
    for(const auto face:outward_faces(tet,checkpoint.vertices)){const auto face_key=key(face);const auto found=combined.find(face_key);
      if(found==combined.end())combined.emplace(face_key,face);
      else{cancellation&=opposite_winding(found->second,face);combined.erase(found);}}
  }
  result.halo_tetrahedron_indices.assign(selected.begin(),selected.end());result.repair_boundary_faces.clear();result.repair_vertex_indices.clear();
  for(const auto& [unused,face]:combined){static_cast<void>(unused);result.repair_boundary_faces.push_back(face);for(const auto vertex:face)result.repair_vertex_indices.push_back(vertex);}
  std::ranges::sort(result.repair_vertex_indices);result.repair_vertex_indices.erase(
      std::unique(result.repair_vertex_indices.begin(),result.repair_vertex_indices.end()),result.repair_vertex_indices.end());
  result.audit.interface_cancels_exactly=cancellation;const auto audit=audit_boundary(result.repair_boundary_faces,checkpoint.vertices);
  result.audit.repair_boundary_closed=audit.closed;result.audit.repair_boundary_oriented=audit.oriented;
  result.audit.repair_boundary_self_intersection_free=audit.intersection_free;
  result.repair_volume=boundary_volume(result.repair_boundary_faces,checkpoint.vertices);return result;
}

AdvancingFrontPocketRepairResult search_advancing_front_pocket_existing_vertices(
    const AdvancingFrontFillResult& checkpoint,const AdvancingFrontPocket& pocket,
    const AdvancingFrontPocketRepairOptions& options){
  AdvancingFrontPocketRepairResult result;
  if(pocket.repair_boundary_faces.empty()||
     !pocket.audit.repair_boundary_closed||!pocket.audit.repair_boundary_oriented||
     !pocket.audit.repair_boundary_self_intersection_free||options.maximum_search_states==0U)return result;
  std::map<Face,Face> front;for(const auto face:pocket.repair_boundary_faces)
    if(!front.emplace(key(face),face).second)return result;
  auto working=checkpoint;const auto original_vertex_count=working.vertices.size();
  for(const auto point:options.deterministic_steiner_candidates){working.vertices.push_back(point);working.stable_vertex_ids.push_back(
      0xa000000000000000ULL+working.stable_vertex_ids.size()-original_vertex_count);}
  result.added_vertices=options.deterministic_steiner_candidates;
  const auto original_front=front;std::vector<std::uint32_t> mandatory_vertices;
  for(const auto face:pocket.repair_boundary_faces)mandatory_vertices.insert(mandatory_vertices.end(),face.begin(),face.end());
  for(const auto index:pocket.halo_tetrahedron_indices){const auto tet=working.tetrahedra.at(index);mandatory_vertices.insert(mandatory_vertices.end(),tet.begin(),tet.end());}
  std::ranges::sort(mandatory_vertices);mandatory_vertices.erase(std::unique(mandatory_vertices.begin(),mandatory_vertices.end()),mandatory_vertices.end());
  std::vector<std::uint32_t> vertices=mandatory_vertices;
  vertices.insert(vertices.end(),options.candidate_vertex_indices.begin(),options.candidate_vertex_indices.end());
  std::ranges::sort(vertices);vertices.erase(std::unique(vertices.begin(),vertices.end()),vertices.end());
  if(std::ranges::any_of(vertices,[&](auto index){return index>=original_vertex_count;}))return result;
  for(std::size_t i=original_vertex_count;i<working.vertices.size();++i)vertices.push_back(static_cast<std::uint32_t>(i));
  std::vector<Tet> selected;std::set<Tet> selected_keys;std::set<std::vector<Face>> failed_fronts;
  const std::set<std::size_t> mutable_tetrahedra(
      pocket.halo_tetrahedron_indices.begin(),pocket.halo_tetrahedron_indices.end());
  bool capped=false;
  const auto points=[&](Tet tet){return std::array<Vec3,4>{{working.vertices[tet[0]],working.vertices[tet[1]],working.vertices[tet[2]],working.vertices[tet[3]]}};};
  std::map<Face,std::vector<std::uint32_t>> candidate_cache;
  std::map<Face,std::vector<std::size_t>> obstacle_cache;
  const auto static_candidates=[&](Face base)->const std::vector<std::uint32_t>&{
    const auto found=candidate_cache.find(base);if(found!=candidate_cache.end())return found->second;
    std::vector<std::uint32_t> candidates;
    for(const auto apex:vertices){if(std::ranges::find(base,apex)!=base.end())continue;
      Tet tet{{base[0],base[2],base[1],apex}};const auto tet_points=points(tet);
      if(exact_orientation_3d(tet_points[0],tet_points[1],tet_points[2],tet_points[3])!=ExactPredicateSign::positive){++result.rejected_orientation;continue;}
      bool vertex_inside=false;for(const auto vertex:mandatory_vertices)if(std::ranges::find(tet,vertex)==tet.end()&&contains_strict(tet_points,working.vertices[vertex])){vertex_inside=true;break;}
      if(vertex_inside){++result.rejected_vertex_inside;continue;}
      std::vector<std::size_t> overlapping;for(std::size_t index=0;index<working.tetrahedra.size();++index)if(!mutable_tetrahedra.contains(index)&&
          strict_tetrahedra_overlap(tet_points,points(working.tetrahedra[index])))overlapping.push_back(index);
      if(!overlapping.empty()){++result.rejected_overlap;const auto found=obstacle_cache.find(base);
        if(found==obstacle_cache.end()||overlapping.size()<found->second.size())obstacle_cache[base]=std::move(overlapping);
        continue;
      }
      candidates.push_back(apex);
    }
    return candidate_cache.emplace(base,std::move(candidates)).first->second;
  };
  const auto viable=[&](Face base,std::uint32_t apex)->std::optional<std::map<Face,Face>>{
    ++result.candidate_tests;
    Tet tet{{base[0],base[2],base[1],apex}};const auto tet_points=points(tet);
    auto canonical=tet;std::ranges::sort(canonical);if(selected_keys.contains(canonical))return std::nullopt;
    for(const auto other:selected)if(strict_tetrahedra_overlap(tet_points,points(other))){++result.rejected_overlap;return std::nullopt;}
    auto updated=front;updated.erase(key(base));std::vector<Face> exposed;
    for(const auto side:side_faces(base,apex,working.vertices)){const auto side_key=key(side);const auto found=updated.find(side_key);
      if(found==updated.end()){updated.emplace(side_key,side);exposed.push_back(side);}
      else{if(!opposite_winding(side,found->second))return std::nullopt;updated.erase(found);}}
    for(const auto side:exposed)for(const auto& [other_key,other]:updated)if(other_key!=key(side)&&
        advancing_front_triangles_cross({{working.vertices[side[0]],working.vertices[side[1]],working.vertices[side[2]]}},
                                        {{working.vertices[other[0]],working.vertices[other[1]],working.vertices[other[2]]}})){
      ++result.rejected_front_crossing;return std::nullopt;}
    return updated;
  };
  const auto search=[&](auto&& self)->bool{
    if(++result.search_states>options.maximum_search_states){capped=true;return false;}
    if(front.empty())return true;
    std::optional<Face> chosen;std::vector<std::pair<std::uint32_t,std::map<Face,Face>>> chosen_candidates;
    std::size_t faces_considered{};
    for(const auto& [unused,base]:front){static_cast<void>(unused);std::vector<std::pair<std::uint32_t,std::map<Face,Face>>> candidates;
      for(const auto apex:static_candidates(base))if(const auto updated=viable(base,apex))candidates.emplace_back(apex,*updated);
      if(candidates.empty()){result.blocking_face=base;result.has_blocking_face=true;
        const auto blockers=obstacle_cache.find(base);if(blockers!=obstacle_cache.end())result.blocking_obstacle_tetrahedra=blockers->second;return false;}
      std::ranges::sort(candidates,[](const auto& left,const auto& right){
        return left.second.size()!=right.second.size()?left.second.size()<right.second.size():left.first<right.first;});
      if(!chosen||candidates.size()<chosen_candidates.size()){chosen=base;chosen_candidates=std::move(candidates);if(chosen_candidates.size()==1U)break;}
      if(++faces_considered==8U)break;
    }
    const auto old_front=front;
    for(auto& [apex,updated]:chosen_candidates){Tet tet{{(*chosen)[0],(*chosen)[2],(*chosen)[1],apex}};auto canonical=tet;std::ranges::sort(canonical);
      selected.push_back(tet);selected_keys.insert(canonical);front=std::move(updated);
      std::vector<Face> signature;signature.reserve(front.size());for(const auto& [face_key,unused]:front){static_cast<void>(unused);signature.push_back(face_key);}
      if(!failed_fronts.contains(signature)&&self(self))return true;
      failed_fronts.insert(std::move(signature));front=old_front;selected_keys.erase(canonical);selected.pop_back();if(capped)return false;
    }
    return false;
  };
  if(!search(search)){result.failure=capped?AdvancingFrontPocketRepairFailure::search_limit:
      AdvancingFrontPocketRepairFailure::no_existing_vertex_fill;return result;}
  result.tetrahedra=selected;result.exactly_positive=true;result.no_strict_overlap=true;
  std::map<Face,std::size_t> uses;double volume{};
  for(std::size_t i=0;i<selected.size();++i){const auto tet=selected[i];const auto p=points(tet);
    result.exactly_positive&=exact_orientation_3d(p[0],p[1],p[2],p[3])==ExactPredicateSign::positive;
    volume+=dot(p[1]-p[0],cross(p[2]-p[0],p[3]-p[0]))/6.0;
    for(const auto face:outward_faces(tet,working.vertices))++uses[key(face)];
    for(std::size_t j=0;j<i;++j)result.no_strict_overlap&=!strict_tetrahedra_overlap(p,points(selected[j]));
    for(std::size_t index=0;index<working.tetrahedra.size();++index)if(!mutable_tetrahedra.contains(index))
      result.no_strict_overlap&=!strict_tetrahedra_overlap(p,points(working.tetrahedra[index]));
  }
  result.exact_boundary=uses.size()>=original_front.size();
  for(const auto& [face,count]:uses)result.exact_boundary&=count==(original_front.contains(face)?1U:2U);
  for(const auto& [face,unused]:original_front){static_cast<void>(unused);result.exact_boundary&=uses.contains(face)&&uses.at(face)==1U;}
  result.exact_volume=std::abs(volume-pocket.repair_volume)<=1e-10*std::max(1.0,pocket.repair_volume);
  if(!result.exactly_positive||!result.exact_boundary||!result.no_strict_overlap||!result.exact_volume){
    result.failure=AdvancingFrontPocketRepairFailure::audit_failed;return result;}
  result.failure=AdvancingFrontPocketRepairFailure::none;return result;
}

} // namespace tetra::probes
