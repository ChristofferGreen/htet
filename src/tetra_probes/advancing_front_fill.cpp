#include "tetra_probes/advancing_front_fill.hpp"

#include "tetra_probes/surface_core_contract.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace tetra::probes {
namespace {
using Face=std::array<std::uint32_t,3>;
using Tet=std::array<std::uint32_t,4>;
using Edge=std::array<std::uint32_t,2>;
struct Bounds {Vec3 minimum{},maximum{};};
double dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
double norm(Vec3 a){return std::sqrt(dot(a,a));}
bool same_point(Vec3 a,Vec3 b){return a.x==b.x&&a.y==b.y&&a.z==b.z;}
double six(Vec3 a,Vec3 b,Vec3 c,Vec3 d){return dot(b-a,cross(c-a,d-a));}
Face key(Face f){std::sort(f.begin(),f.end());return f;}
Tet key(Tet t){std::sort(t.begin(),t.end());return t;}

bool segment_triangle_contact(Vec3 p,Vec3 q,Vec3 a,Vec3 b,Vec3 c){
  const auto d=q-p,e1=b-a,e2=c-a,h=cross(d,e2);const auto determinant=dot(e1,h);
  if(std::abs(determinant)<1e-12)return false;
  const auto inverse=1.0/determinant;const auto s=p-a;
  const auto u=inverse*dot(s,h);if(u<-1e-10||u>1.0+1e-10)return false;
  const auto r=cross(s,e1);const auto v=inverse*dot(d,r);
  if(v<-1e-10||u+v>1.0+1e-10)return false;
  const auto t=inverse*dot(e2,r);return t>1e-10&&t<1.0-1e-10;
}
Bounds triangle_bounds(const std::vector<Vec3>& vertices,Face face){
  Bounds result{vertices[face[0]],vertices[face[0]]};
  for(std::size_t i=1U;i<3U;++i){const auto p=vertices[face[i]];
    result.minimum.x=std::min(result.minimum.x,p.x);result.minimum.y=std::min(result.minimum.y,p.y);result.minimum.z=std::min(result.minimum.z,p.z);
    result.maximum.x=std::max(result.maximum.x,p.x);result.maximum.y=std::max(result.maximum.y,p.y);result.maximum.z=std::max(result.maximum.z,p.z);}
  return result;
}
bool overlaps(Bounds a,Bounds b){constexpr double epsilon=1e-12;return
    a.minimum.x<b.maximum.x-epsilon&&b.minimum.x<a.maximum.x-epsilon&&
    a.minimum.y<b.maximum.y-epsilon&&b.minimum.y<a.maximum.y-epsilon&&
    a.minimum.z<b.maximum.z-epsilon&&b.minimum.z<a.maximum.z-epsilon;}
bool touches(Bounds a,Bounds b){constexpr double epsilon=1e-12;return
    a.minimum.x<=b.maximum.x+epsilon&&b.minimum.x<=a.maximum.x+epsilon&&
    a.minimum.y<=b.maximum.y+epsilon&&b.minimum.y<=a.maximum.y+epsilon&&
    a.minimum.z<=b.maximum.z+epsilon&&b.minimum.z<=a.maximum.z+epsilon;}
double orient2(const std::array<double,2>& a,const std::array<double,2>& b,
               const std::array<double,2>& c){
  return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
}
bool coplanar_triangles_cross(const std::array<Vec3,3>& a,
                              const std::array<Vec3,3>& b,Vec3 normal){
  const Vec3 magnitude{std::abs(normal.x),std::abs(normal.y),std::abs(normal.z)};
  const unsigned dropped=magnitude.x>=magnitude.y&&magnitude.x>=magnitude.z?0U:
      (magnitude.y>=magnitude.z?1U:2U);
  const auto project=[dropped](Vec3 p){return dropped==0U?std::array<double,2>{{p.y,p.z}}:
      (dropped==1U?std::array<double,2>{{p.x,p.z}}:std::array<double,2>{{p.x,p.y}});};
  constexpr double epsilon=1e-12;
  const auto proper=[&](Vec3 p,Vec3 q,Vec3 r,Vec3 s){
    const auto P=project(p),Q=project(q),R=project(r),S=project(s);
    const auto pqr=orient2(P,Q,R),pqs=orient2(P,Q,S);
    const auto rsp=orient2(R,S,P),rsq=orient2(R,S,Q);
    return ((pqr>epsilon&&pqs<-epsilon)||(pqr<-epsilon&&pqs>epsilon))&&
           ((rsp>epsilon&&rsq<-epsilon)||(rsp<-epsilon&&rsq>epsilon));};
  const auto same2=[&](Vec3 p,Vec3 q){const auto P=project(p),Q=project(q);
    return std::abs(P[0]-Q[0])<=epsilon&&std::abs(P[1]-Q[1])<=epsilon;};
  const auto strictly_on_segment=[&](Vec3 p,Vec3 a,Vec3 b){
    const auto P=project(p),A=project(a),B=project(b);
    if(std::abs(orient2(A,B,P))>epsilon)return false;
    const auto ab0=B[0]-A[0],ab1=B[1]-A[1],ap0=P[0]-A[0],ap1=P[1]-A[1];
    const auto length2=ab0*ab0+ab1*ab1;if(length2<=epsilon*epsilon)return false;
    const auto t=(ap0*ab0+ap1*ab1)/length2;return t>epsilon&&t<1.0-epsilon;};
  const auto inside=[&](Vec3 p,const std::array<Vec3,3>& t){
    const auto P=project(p),A=project(t[0]),B=project(t[1]),C=project(t[2]);
    const auto ab=orient2(A,B,P),bc=orient2(B,C,P),ca=orient2(C,A,P);
    return (ab>epsilon&&bc>epsilon&&ca>epsilon)||
           (ab<-epsilon&&bc<-epsilon&&ca<-epsilon);};
  for(std::size_t i=0;i<3U;++i)for(std::size_t j=0;j<3U;++j){
    const auto ai=a[i],aj=a[(i+1U)%3U],bi=b[j],bj=b[(j+1U)%3U];
    if(proper(ai,aj,bi,bj))return true;
    const bool same_edge=(same2(ai,bi)&&same2(aj,bj))||(same2(ai,bj)&&same2(aj,bi));
    if(!same_edge&&(strictly_on_segment(ai,bi,bj)||strictly_on_segment(aj,bi,bj)||
                    strictly_on_segment(bi,ai,aj)||strictly_on_segment(bj,ai,aj)))return true;
  }
  for(const auto p:a)if(inside(p,b))return true;
  for(const auto p:b)if(inside(p,a))return true;
  return false;
}
bool opposite_winding(Face first,Face second){
  if(key(first)!=key(second))return false;
  for(std::size_t shift=0;shift<3U;++shift)
    if(first[0]==second[shift]&&first[1]==second[(shift+2U)%3U]&&
       first[2]==second[(shift+1U)%3U])return true;
  return false;
}
bool same_winding(Face first,Face second){
  if(key(first)!=key(second))return false;
  for(std::size_t shift=0;shift<3U;++shift)
    if(first[0]==second[shift]&&first[1]==second[(shift+1U)%3U]&&
       first[2]==second[(shift+2U)%3U])return true;
  return false;
}
std::vector<std::pair<Face,Face>> residual_sector_pairs(
    std::vector<Face> faces,Edge edge,const std::vector<Vec3>& vertices){
  std::vector<std::pair<Face,Face>> result;if(faces.size()<2U)return result;
  const auto third=[&](Face face){for(const auto vertex:face)if(vertex!=edge[0]&&vertex!=edge[1])return vertex;return face[0];};
  auto axis=vertices[edge[1]]-vertices[edge[0]];const auto axis_length=norm(axis);if(axis_length<=1e-14)return result;axis=axis/axis_length;
  const auto radial=[&](Face face){auto value=vertices[third(face)]-vertices[edge[0]];value=value-axis*dot(value,axis);const auto magnitude=norm(value);return magnitude>1e-14?value/magnitude:Vec3{};};
  const auto basis=radial(faces.front());if(norm(basis)<=1e-14)return result;const auto tangent=cross(axis,basis);
  struct AngularFace {double angle{};Face face{};};std::vector<AngularFace> angular;angular.reserve(faces.size());
  for(const auto face:faces){const auto direction=radial(face);angular.push_back({std::atan2(dot(direction,tangent),dot(direction,basis)),face});}
  std::ranges::sort(angular,[](const auto& a,const auto& b){return a.angle!=b.angle?a.angle<b.angle:key(a.face)<key(b.face);});
  std::set<std::pair<Face,Face>> unique;
  for(std::size_t i=0;i<angular.size();++i){const auto j=(i+1U)%angular.size();auto end=angular[j].angle;if(j==0U)end+=2.0*std::numbers::pi;const auto middle=(angular[i].angle+end)*0.5;
    const auto direction=basis*std::cos(middle)+tangent*std::sin(middle);const auto first=angular[i].face,second=angular[j].face;
    const auto normal=[&](Face face){return cross(vertices[face[1]]-vertices[face[0]],vertices[face[2]]-vertices[face[0]]);};
    if(dot(normal(first),direction)<-1e-14&&dot(normal(second),direction)<-1e-14){auto pair=std::pair<Face,Face>{key(first),key(second)};if(pair.second<pair.first)std::swap(pair.first,pair.second);unique.insert(pair);}}
  result.assign(unique.begin(),unique.end());return result;
}
bool triangle_crosses(const std::vector<Vec3>& vertices,Face a,Face b){
  if(!touches(triangle_bounds(vertices,a),triangle_bounds(vertices,b)))return false;
  return advancing_front_triangles_cross(
      {{vertices[a[0]],vertices[a[1]],vertices[a[2]]}},
      {{vertices[b[0]],vertices[b[1]],vertices[b[2]]}});
}
std::array<Face,3> sides(Face base,std::uint32_t apex,const std::vector<Vec3>& vertices){
  std::array<Face,3> result{{{{base[0],base[1],apex}},{{base[1],base[2],apex}},{{base[2],base[0],apex}}}};
  for(std::size_t i=0;i<3U;++i){const auto opposite=base[(i+2U)%3U];
    if(dot(cross(vertices[result[i][1]]-vertices[result[i][0]],vertices[result[i][2]]-vertices[result[i][0]]),vertices[opposite]-vertices[result[i][0]])<0.0)
      std::swap(result[i][1],result[i][2]);}
  return result;
}
std::array<Face,4> outward_faces(Tet tet,const std::vector<Vec3>& vertices){
  std::array<Face,4> result{};
  for(std::size_t opposite=0;opposite<4U;++opposite){std::size_t cursor{};
    for(std::size_t i=0;i<4U;++i)if(i!=opposite)result[opposite][cursor++]=tet[i];
    const auto& f=result[opposite];
    if(dot(cross(vertices[f[1]]-vertices[f[0]],vertices[f[2]]-vertices[f[0]]),
           vertices[tet[opposite]]-vertices[f[0]])>0.0)
      std::swap(result[opposite][1],result[opposite][2]);
  }
  return result;
}
bool contains_strict(const std::array<Vec3,4>& tet,Vec3 p){
  const auto total=six(tet[0],tet[1],tet[2],tet[3]);if(std::abs(total)<1e-14)return false;
  constexpr double epsilon=1e-9;
  const std::array<double,4> weights{{six(p,tet[1],tet[2],tet[3])/total,
      six(tet[0],p,tet[2],tet[3])/total,six(tet[0],tet[1],p,tet[3])/total,
      six(tet[0],tet[1],tet[2],p)/total}};
  return std::ranges::all_of(weights,[](double value){return value>epsilon&&value<1.0-epsilon;});
}
Bounds bounds(const std::array<Vec3,4>& tet){
  Bounds result{tet[0],tet[0]};
  for(std::size_t i=1U;i<4U;++i){result.minimum.x=std::min(result.minimum.x,tet[i].x);result.minimum.y=std::min(result.minimum.y,tet[i].y);result.minimum.z=std::min(result.minimum.z,tet[i].z);
    result.maximum.x=std::max(result.maximum.x,tet[i].x);result.maximum.y=std::max(result.maximum.y,tet[i].y);result.maximum.z=std::max(result.maximum.z,tet[i].z);}
  return result;
}
} // namespace

bool advancing_front_triangles_cross(const std::array<Vec3,3>& a,
                                      const std::array<Vec3,3>& b){
  const auto an=cross(a[1]-a[0],a[2]-a[0]);
  const auto bn=cross(b[1]-b[0],b[2]-b[0]);
  constexpr double epsilon=1e-12;
  if(norm(cross(an,bn))<=epsilon*norm(an)*norm(bn)&&
     std::abs(dot(an,b[0]-a[0]))<=epsilon*norm(an))
    return coplanar_triangles_cross(a,b,an);
  for(std::size_t i=0;i<3U;++i){
    const auto a_shared=std::ranges::any_of(b,[&](Vec3 p){return norm(p-a[i])<=epsilon;})&&
                        std::ranges::any_of(b,[&](Vec3 p){return norm(p-a[(i+1U)%3U])<=epsilon;});
    const auto b_shared=std::ranges::any_of(a,[&](Vec3 p){return norm(p-b[i])<=epsilon;})&&
                        std::ranges::any_of(a,[&](Vec3 p){return norm(p-b[(i+1U)%3U])<=epsilon;});
    if(!a_shared&&segment_triangle_contact(a[i],a[(i+1U)%3U],b[0],b[1],b[2]))return true;
    if(!b_shared&&segment_triangle_contact(b[i],b[(i+1U)%3U],a[0],a[1],a[2]))return true;
  }
  return false;
}

AdvancingFrontFillResult fill_advancing_front_cavity(
    const AdvancingFrontFixture& fixture,const AdvancingFrontFillOptions& options){
  if(!fixture.audit.accepted)throw std::invalid_argument("advancing-front fill requires an accepted cavity");
  AdvancingFrontFillResult result;result.fixture_config=fixture.config;result.fill_options=options;result.vertices=fixture.outer_vertices;
  result.stable_vertex_ids.resize(result.vertices.size());
  for(std::size_t i=0;i<result.vertices.size();++i)result.stable_vertex_ids[i]=i+1U;
  const auto core_offset=static_cast<std::uint32_t>(result.vertices.size());
  result.vertices.insert(result.vertices.end(),fixture.core_vertices.begin(),fixture.core_vertices.end());
  for(std::size_t i=0;i<fixture.core_vertices.size();++i)result.stable_vertex_ids.push_back(0x4000000000000000ULL+i);
  std::map<Face,Face> active;
  for(const auto f:fixture.outer_triangles)active.emplace(key(f),f);
  for(auto f:fixture.core_boundary_triangles){f={{f[0]+core_offset,f[2]+core_offset,f[1]+core_offset}};active.emplace(key(f),f);}
  const auto prescribed_oriented=active;
  std::set<Face> prescribed;for(const auto& [face_key,unused]:active){static_cast<void>(unused);prescribed.insert(face_key);}
  result.audit.initial_faces=active.size();result.audit.cavity_volume=fixture.audit.cavity_volume;
  std::set<Tet> emitted;
  std::set<Tet> forbidden_existing;
  std::set<std::tuple<Face,double,double,double>> forbidden_steiner;
  std::optional<Face> priority_face;
  std::vector<Bounds> emitted_bounds;
  struct Transaction {
    std::vector<Tet> tetrahedra;
    std::vector<std::pair<Face,std::optional<Face>>> previous_active;
    std::uint32_t apex{};bool steiner{};bool pocket{};bool fan{};
  };
  std::vector<Transaction> history;
  std::vector<Bounds> core_bounds;core_bounds.reserve(fixture.core_tetrahedra.size());
  for(const auto core:fixture.core_tetrahedra)core_bounds.push_back(bounds({{fixture.core_vertices[core[0]],fixture.core_vertices[core[1]],fixture.core_vertices[core[2]],fixture.core_vertices[core[3]]}}));

  for(std::size_t step=0;step<options.maximum_steps&&!active.empty();++step){
    std::map<Edge,std::vector<Face>> active_edges;
    for(const auto& [face_key,face]:active)for(std::size_t edge_index=0;edge_index<3U;++edge_index){
      auto edge=std::array<std::uint32_t,2>{{face[edge_index],face[(edge_index+1U)%3U]}};
      std::sort(edge.begin(),edge.end());active_edges[edge].push_back(face_key);}
    std::map<Face,std::vector<Face>> active_sheet_neighbours;
    for(const auto& [edge,face_keys]:active_edges){std::vector<Face> oriented;oriented.reserve(face_keys.size());for(const auto face_key:face_keys)oriented.push_back(active.at(face_key));
      for(const auto& [first,second]:residual_sector_pairs(oriented,edge,result.vertices)){active_sheet_neighbours[first].push_back(second);active_sheet_neighbours[second].push_back(first);}}
    std::vector<Face> schedule;schedule.reserve(active.size());std::set<Face> scheduled;
    const auto append_component=[&](Face seed){std::deque<Face> pending{seed};scheduled.insert(seed);
      while(!pending.empty()){const auto current=pending.front();pending.pop_front();schedule.push_back(current);const auto face=active.at(current);
        for(std::size_t edge_index=0;edge_index<3U;++edge_index){auto edge=std::array<std::uint32_t,2>{{face[edge_index],face[(edge_index+1U)%3U]}};std::sort(edge.begin(),edge.end());
          for(const auto neighbour:active_sheet_neighbours[current])if(scheduled.insert(neighbour).second)pending.push_back(neighbour);}}};
    if(priority_face&&active.contains(*priority_face))append_component(*priority_face);
    priority_face.reset();
    for(const auto& [face_key,unused]:active){static_cast<void>(unused);if(!scheduled.contains(face_key))append_component(face_key);}
    bool step_accepted=false;
    for(unsigned int recovery=0U;recovery<2U&&!step_accepted;++recovery){
    for(const auto selected_key:schedule){const auto selected=active.find(selected_key);if(selected==active.end())continue;const auto base=selected->second;
    const auto a=result.vertices[base[0]],b=result.vertices[base[1]],c=result.vertices[base[2]];
    auto normal=cross(b-a,c-a);const auto magnitude=norm(normal);if(magnitude<1e-14)continue;normal=normal/magnitude;
    const auto centre=(a+b+c)/3.0;const auto scale=std::min({norm(a-b),norm(b-c),norm(c-a)});
    const auto ideal=centre-normal*(scale*0.7);
    struct Candidate {std::uint32_t index{};std::size_t cancellations{};double distance{};};
    std::vector<Candidate> candidates;
    for(std::uint32_t i=0;i<result.vertices.size();++i){if(i==base[0]||i==base[1]||i==base[2])continue;
      std::size_t cancellations{};for(const auto side:sides(base,i,result.vertices))cancellations+=active.contains(key(side))?1U:0U;
      const auto delta=result.vertices[i]-ideal;candidates.push_back({i,cancellations,dot(delta,delta)});}
    std::ranges::sort(candidates,[](const Candidate& left,const Candidate& right){
      if(left.cancellations!=right.cancellations)return left.cancellations>right.cancellations;
      return left.distance!=right.distance?left.distance<right.distance:left.index<right.index;});
    const auto candidate_limit=recovery==0U?options.existing_candidate_limit:options.recovery_candidate_limit;
    if(candidates.size()>candidate_limit)candidates.resize(candidate_limit);
    bool accepted=false;
    std::vector<Face> adjacent_bases;
    std::array<std::vector<Face>,3> vertex_fans;
    if(options.maximum_atomic_faces>1U)for(const auto& [other_key,other]:active){
      if(other_key==key(base))continue;
      std::size_t shared{};
      for(const auto v:base)shared+=std::ranges::find(other,v)!=other.end()?1U:0U;
      if(shared==2U)adjacent_bases.push_back(other);
    }
    for(const auto& [unused,face]:active){static_cast<void>(unused);for(std::size_t i=0;i<3U;++i)
      if(std::ranges::find(face,base[i])!=face.end())vertex_fans[i].push_back(face);}
    std::vector<Face> base_component;{
      std::deque<Face> pending{key(base)};std::set<Face> seen{key(base)};
      while(!pending.empty()){const auto current=pending.front();pending.pop_front();base_component.push_back(active.at(current));
        const auto face=active.at(current);for(std::size_t edge_index=0;edge_index<3U;++edge_index){
          auto edge=std::array<std::uint32_t,2>{{face[edge_index],face[(edge_index+1U)%3U]}};std::sort(edge.begin(),edge.end());
          for(const auto neighbour:active_sheet_neighbours[current])if(seen.insert(neighbour).second)pending.push_back(neighbour);}}
    }
    const auto try_bases=[&](const std::vector<Face>& bases,std::uint32_t apex,
                             bool pocket_fill=false,bool require_front_reduction=false)->bool{
      const bool proposed_steiner=apex>=result.stable_vertex_ids.size();const auto apex_point=result.vertices[apex];
      std::vector<Tet> proposal;std::vector<Bounds> proposal_bounds;proposal.reserve(bases.size());proposal_bounds.reserve(bases.size());
      std::set<Tet> proposal_keys;
      for(const auto proposal_base:bases){
        Tet tet{{proposal_base[0],proposal_base[2],proposal_base[1],apex}};
        const auto six_volume=six(result.vertices[tet[0]],result.vertices[tet[1]],result.vertices[tet[2]],result.vertices[tet[3]]);
        const auto minimum_six=pocket_fill?6e-14:
            std::max(1e-14,6.0*result.audit.cavity_volume*options.minimum_tetrahedron_volume_fraction);
        if(six_volume<=minimum_six){++result.audit.rejected_small_volume;return false;}
        if(emitted.contains(key(tet))||!proposal_keys.insert(key(tet)).second||
           (!proposed_steiner&&forbidden_existing.contains(key(tet)))||
           (proposed_steiner&&forbidden_steiner.contains({key(proposal_base),apex_point.x,apex_point.y,apex_point.z}))){++result.audit.rejected_duplicate;return false;}
        const std::array<Vec3,4> points{{result.vertices[tet[0]],result.vertices[tet[1]],result.vertices[tet[2]],result.vertices[tet[3]]}};
        const auto point_bounds=bounds(points);
        for(std::uint32_t v=0;v<result.vertices.size();++v){if(v==tet[0]||v==tet[1]||v==tet[2]||v==tet[3])continue;const auto p=result.vertices[v];
          if(p.x<=point_bounds.minimum.x||p.x>=point_bounds.maximum.x||p.y<=point_bounds.minimum.y||p.y>=point_bounds.maximum.y||p.z<=point_bounds.minimum.z||p.z>=point_bounds.maximum.z)continue;
          if(contains_strict(points,p)){++result.audit.rejected_vertex_inside;return false;}}
        for(std::size_t i=0;i<result.tetrahedra.size();++i){if(!overlaps(point_bounds,emitted_bounds[i]))continue;const auto other=result.tetrahedra[i];const std::array<Vec3,4> q{{result.vertices[other[0]],result.vertices[other[1]],result.vertices[other[2]],result.vertices[other[3]]}};
          if(strict_tetrahedra_overlap(points,q)){++result.audit.rejected_overlap;return false;}}
        for(std::size_t i=0;i<fixture.core_tetrahedra.size();++i){if(!overlaps(point_bounds,core_bounds[i]))continue;const auto core=fixture.core_tetrahedra[i];const std::array<Vec3,4> q{{fixture.core_vertices[core[0]],fixture.core_vertices[core[1]],fixture.core_vertices[core[2]],fixture.core_vertices[core[3]]}};
          if(strict_tetrahedra_overlap(points,q)){++result.audit.rejected_overlap;return false;}}
        for(std::size_t i=0;i<proposal.size();++i){if(!overlaps(point_bounds,proposal_bounds[i]))continue;const auto other=proposal[i];const std::array<Vec3,4> q{{result.vertices[other[0]],result.vertices[other[1]],result.vertices[other[2]],result.vertices[other[3]]}};
          if(strict_tetrahedra_overlap(points,q)){++result.audit.rejected_overlap;return false;}}
        proposal.push_back(tet);proposal_bounds.push_back(point_bounds);
      }
      auto updated=active;for(const auto proposal_base:bases)updated.erase(key(proposal_base));
      std::map<Edge,int> edge_count_delta;
      const auto adjust_edges=[&](Face face,int delta){for(std::size_t i=0;i<3U;++i){Edge edge{{face[i],face[(i+1U)%3U]}};std::sort(edge.begin(),edge.end());
          edge_count_delta[edge]+=delta;}};
      for(const auto proposal_base:bases)adjust_edges(proposal_base,-1);
      std::set<Face> touched;for(const auto proposal_base:bases)touched.insert(key(proposal_base));
      for(std::size_t t=0;t<proposal.size();++t)for(const auto side:sides(bases[t],apex,result.vertices)){
        const auto side_key=key(side);touched.insert(side_key);const auto found=updated.find(side_key);
        if(found==updated.end()){updated.emplace(side_key,side);adjust_edges(side,1);}
        else{if(!opposite_winding(side,found->second)){++result.audit.same_sided_face_matches;++result.audit.rejected_orientation;return false;}adjust_edges(found->second,-1);updated.erase(found);}}
      if(require_front_reduction&&updated.size()>=active.size())return false;
      for(const auto& [edge,delta]:edge_count_delta){const auto found=active_edges.find(edge);
        const auto current=found==active_edges.end()?0:static_cast<int>(found->second.size());const auto count=current+delta;
        if(count<0||(count%2)!=0){++result.audit.rejected_front_incidence;return false;}}
      std::set<Face> base_keys;for(const auto proposal_base:bases)base_keys.insert(key(proposal_base));
      for(const auto touched_key:touched){const auto candidate=updated.find(touched_key);if(candidate==updated.end())continue;
        for(const auto& [other_key,other]:updated){if(other_key==touched_key||base_keys.contains(other_key))continue;
          if(triangle_crosses(result.vertices,candidate->second,other)){++result.audit.rejected_front_crossing;return false;}}}
      Transaction transaction;transaction.tetrahedra=proposal;transaction.apex=apex;transaction.steiner=proposed_steiner;
      for(const auto touched_key:touched){const auto old=active.find(touched_key);transaction.previous_active.push_back({touched_key,old==active.end()?std::nullopt:std::optional<Face>{old->second}});}
      active=std::move(updated);
      for(std::size_t i=0;i<proposal.size();++i){result.tetrahedra.push_back(proposal[i]);emitted_bounds.push_back(proposal_bounds[i]);emitted.insert(key(proposal[i]));}
      if(proposal.size()>1U){++result.audit.atomic_join_transactions;result.audit.atomic_join_tetrahedra+=proposal.size();}
      history.push_back(std::move(transaction));return true;
    };
    const auto try_apex=[&](std::uint32_t apex,bool allow_fan)->bool{
      const bool local_repair=base_component.size()<=options.maximum_pocket_faces;
      if(allow_fan)for(const auto& incident:vertex_fans){std::vector<Face> fan;for(const auto face:incident)
          if(std::ranges::find(face,apex)==face.end())fan.push_back(face);
        if(fan.size()>=3U&&fan.size()<=options.maximum_atomic_faces&&try_bases(fan,apex,local_repair,true)){history.back().fan=true;++result.audit.atomic_fan_transactions;return true;}}
      for(const auto other:adjacent_bases)if(std::ranges::find(other,apex)==other.end()&&
          try_bases({base,other},apex,local_repair))return true;
      return try_bases({base},apex,local_repair);
    };
    auto component_representative=key(base_component.front());
    for(const auto face:base_component)component_representative=std::min(component_representative,key(face));
    if(base_component.size()<=options.maximum_pocket_faces&&key(base)==component_representative){
      std::set<std::uint32_t> component_vertices;for(const auto face:base_component)for(const auto vertex:face)component_vertices.insert(vertex);
      Vec3 kernel{};for(const auto vertex:component_vertices)kernel=kernel+result.vertices[vertex];kernel=kernel/static_cast<double>(component_vertices.size());
      const auto target_margin=std::cbrt(result.audit.cavity_volume)*1e-9;bool kernel_found=false;
      for(std::size_t iteration=0;iteration<options.kernel_projection_iterations;++iteration){
        double worst=-1.0;Vec3 worst_normal{};
        for(const auto face:base_component){const auto p=result.vertices[face[0]];auto n=cross(result.vertices[face[1]]-p,result.vertices[face[2]]-p);const auto magnitude=norm(n);if(magnitude<=1e-14){worst=1.0;break;}n=n/magnitude;
          const auto violation=dot(n,kernel-p)+target_margin;if(violation>worst){worst=violation;worst_normal=n;}}
        if(worst<=0.0){kernel_found=true;break;}kernel=kernel-worst_normal*(worst+target_margin);
      }
      if(kernel_found){const auto index=static_cast<std::uint32_t>(result.vertices.size());result.vertices.push_back(kernel);
        if(try_bases(base_component,index,true)){history.back().pocket=true;result.stable_vertex_ids.push_back(0x9000000000000000ULL+result.audit.pocket_repairs+1U);++result.audit.pocket_repairs;result.audit.pocket_repair_tetrahedra+=base_component.size();accepted=true;}
        else{++result.audit.rejected_kernel_fill;result.vertices.pop_back();}}
      else ++result.audit.kernel_search_failures;
    }
    if(accepted){step_accepted=true;break;}
    for(const auto candidate:candidates)if(try_apex(candidate.index,candidate.cancellations>0U)){result.audit.existing_vertex_insertions+=history.back().tetrahedra.size();accepted=true;break;}else ++result.audit.rejected_candidates;
    if(!accepted&&recovery==0U){for(const double fraction:{0.7,0.45,0.25,0.125}){const auto index=static_cast<std::uint32_t>(result.vertices.size());result.vertices.push_back(centre-normal*(scale*fraction));
        if(try_apex(index,false)){result.stable_vertex_ids.push_back(0x8000000000000000ULL+result.audit.steiner_vertex_insertions+1U);++result.audit.steiner_vertex_insertions;accepted=true;break;}
        result.vertices.pop_back();++result.audit.rejected_candidates;}}
    if(accepted){step_accepted=true;break;}
    }
    }
    if(!step_accepted){
      if(result.audit.pocket_expansions<options.maximum_pocket_expansions){
        std::set<Face> obstruction_component;std::deque<Face> pending{schedule.front()};obstruction_component.insert(schedule.front());
        while(!pending.empty()){const auto current=pending.front();pending.pop_front();const auto face=active.at(current);
          for(std::size_t edge_index=0;edge_index<3U;++edge_index){auto edge=std::array<std::uint32_t,2>{{face[edge_index],face[(edge_index+1U)%3U]}};std::sort(edge.begin(),edge.end());
            for(const auto neighbour:active_sheet_neighbours[current])if(obstruction_component.insert(neighbour).second)pending.push_back(neighbour);}}
        std::vector<std::size_t> remove;
        for(std::size_t i=0;i<result.tetrahedra.size()&&remove.size()<options.maximum_expansion_tetrahedra;++i){
          bool adjacent=false;for(const auto face:outward_faces(result.tetrahedra[i],result.vertices))adjacent=adjacent||obstruction_component.contains(key(face));
          if(adjacent)remove.push_back(i);}
        if(!remove.empty()){
          for(auto iterator=remove.rbegin();iterator!=remove.rend();++iterator){const auto index=*iterator;const auto removed=result.tetrahedra[index];
            for(const auto face:outward_faces(removed,result.vertices)){const auto face_key=key(face);const auto found=active.find(face_key);if(found==active.end())active.emplace(face_key,face);else active.erase(found);}
            emitted.erase(key(removed));result.tetrahedra.erase(result.tetrahedra.begin()+static_cast<std::ptrdiff_t>(index));emitted_bounds.erase(emitted_bounds.begin()+static_cast<std::ptrdiff_t>(index));}
          ++result.audit.pocket_expansions;result.audit.pocket_expansion_tetrahedra+=remove.size();history.clear();forbidden_existing.clear();forbidden_steiner.clear();continue;
        }
      }
      if(history.empty()||result.audit.rollbacks>=options.maximum_rollbacks){result.audit.obstruction_found=true;break;}
      const auto transaction=history.back();history.pop_back();
      const auto first_base=transaction.previous_active.front().first;
      if(transaction.steiner){const auto p=result.vertices[transaction.apex];for(const auto removed:transaction.tetrahedra)forbidden_steiner.emplace(key(Face{{removed[0],removed[2],removed[1]}}),p.x,p.y,p.z);}
      else for(const auto removed:transaction.tetrahedra)forbidden_existing.insert(key(removed));
      for(const auto& [changed_key,previous]:transaction.previous_active){active.erase(changed_key);if(previous)active.emplace(changed_key,*previous);}
      priority_face=first_base;
      for(std::size_t i=0;i<transaction.tetrahedra.size();++i){const auto removed=result.tetrahedra.back();emitted.erase(key(removed));result.tetrahedra.pop_back();emitted_bounds.pop_back();}
      if(transaction.tetrahedra.size()>1U){--result.audit.atomic_join_transactions;result.audit.atomic_join_tetrahedra-=transaction.tetrahedra.size();}
      if(transaction.fan)--result.audit.atomic_fan_transactions;
      if(transaction.steiner){result.vertices.pop_back();result.stable_vertex_ids.pop_back();if(transaction.pocket){--result.audit.pocket_repairs;result.audit.pocket_repair_tetrahedra-=transaction.tetrahedra.size();}else --result.audit.steiner_vertex_insertions;}
      else result.audit.existing_vertex_insertions-=transaction.tetrahedra.size();
      ++result.audit.rollbacks;continue;
    }
  }
  for(const auto& [unused,f]:active){static_cast<void>(unused);result.active_faces.push_back(f);}
  result.audit.remaining_faces=active.size();result.audit.positive_tetrahedra=true;
  result.audit.unique_tetrahedra=true;result.audit.consistently_oriented_faces=true;
  std::map<Face,std::size_t> face_uses;std::map<Face,std::vector<int>> face_sides;
  std::set<Tet> audited_tets;
  for(const auto tet:result.tetrahedra){const auto volume=six(result.vertices[tet[0]],result.vertices[tet[1]],result.vertices[tet[2]],result.vertices[tet[3]])/6.0;
    result.audit.positive_tetrahedra&=volume>1e-14;result.audit.tetrahedra_volume+=volume;
    if(!audited_tets.insert(key(tet)).second){result.audit.unique_tetrahedra=false;++result.audit.duplicate_tetrahedra;}
    for(std::size_t opposite=0;opposite<4U;++opposite){Face face{};std::size_t out{};for(std::size_t i=0;i<4U;++i)if(i!=opposite)face[out++]=tet[i];const auto canonical=key(face);++face_uses[canonical];
      const auto n=cross(result.vertices[canonical[1]]-result.vertices[canonical[0]],result.vertices[canonical[2]]-result.vertices[canonical[0]]);
      face_sides[canonical].push_back(dot(n,result.vertices[tet[opposite]]-result.vertices[canonical[0]])>0.0?1:-1);}}
  result.audit.exact_volume=active.empty()&&std::abs(result.audit.tetrahedra_volume-result.audit.cavity_volume)<=1e-10;
  result.audit.remaining_volume=result.audit.cavity_volume-result.audit.tetrahedra_volume;
  std::set<Face> actual_boundary;std::map<Face,Face> actual_boundary_oriented;bool incidence=true;for(const auto& [face,count]:face_uses){if(count==1U)actual_boundary.insert(face);else if(count!=2U)incidence=false;
    if(count==2U&&face_sides[face][0]==face_sides[face][1])result.audit.consistently_oriented_faces=false;}
  for(const auto tet:result.tetrahedra)for(const auto face:outward_faces(tet,result.vertices))if(face_uses[key(face)]==1U)actual_boundary_oriented.emplace(key(face),face);
  result.audit.exact_boundary=active.empty()&&incidence&&actual_boundary==prescribed;
  result.audit.exact_oriented_boundary=result.audit.exact_boundary;
  if(result.audit.exact_oriented_boundary)for(const auto& [face_key,face]:prescribed_oriented)
    result.audit.exact_oriented_boundary=result.audit.exact_oriented_boundary&&same_winding(actual_boundary_oriented.at(face_key),face);
  std::set<Face> accounted_boundary=actual_boundary;
  for(const auto& [active_key,unused]:active){static_cast<void>(unused);accounted_boundary.insert(active_key);}
  result.audit.frozen_faces_preserved_partial=true;
  for(const auto frozen:prescribed){const auto actual=actual_boundary_oriented.find(frozen);const auto unresolved=active.find(frozen);
    if(!accounted_boundary.contains(frozen)||(actual!=actual_boundary_oriented.end()&&!same_winding(actual->second,prescribed_oriented.at(frozen)))||
       (unresolved!=active.end()&&!same_winding(unresolved->second,prescribed_oriented.at(frozen)))){
      result.audit.frozen_faces_preserved_partial=false;++result.audit.missing_frozen_faces;}}
  result.audit.frozen_input_vertices_unchanged=result.vertices.size()>=fixture.outer_vertices.size()+fixture.core_vertices.size();
  for(std::size_t i=0;i<fixture.outer_vertices.size()&&result.audit.frozen_input_vertices_unchanged;++i)
    result.audit.frozen_input_vertices_unchanged=same_point(result.vertices[i],fixture.outer_vertices[i]);
  for(std::size_t i=0;i<fixture.core_vertices.size()&&result.audit.frozen_input_vertices_unchanged;++i)
    result.audit.frozen_input_vertices_unchanged=same_point(result.vertices[core_offset+i],fixture.core_vertices[i]);
  std::map<std::array<std::uint32_t,2>,std::vector<std::size_t>> edge_faces;
  std::map<Face,std::size_t> active_face_index;
  for(std::size_t i=0;i<result.active_faces.size();++i)for(std::size_t e=0;e<3U;++e){
    active_face_index.emplace(key(result.active_faces[i]),i);
    auto edge=std::array<std::uint32_t,2>{{result.active_faces[i][e],result.active_faces[i][(e+1U)%3U]}};
    std::sort(edge.begin(),edge.end());edge_faces[edge].push_back(i);}
  result.audit.residual_sheets_closed=true;
  std::vector<std::vector<std::size_t>> sheet_neighbours(result.active_faces.size());
  std::map<std::pair<std::size_t,Edge>,std::size_t> paired_edge_uses;
  for(const auto& [edge,indices]:edge_faces){std::vector<Face> oriented;for(const auto index:indices)oriented.push_back(result.active_faces[index]);
    for(const auto& [first,second]:residual_sector_pairs(oriented,edge,result.vertices)){const auto a=active_face_index.at(first),b=active_face_index.at(second);sheet_neighbours[a].push_back(b);sheet_neighbours[b].push_back(a);++paired_edge_uses[{a,edge}];++paired_edge_uses[{b,edge}];}}
  for(std::size_t i=0;i<result.active_faces.size();++i)for(std::size_t e=0;e<3U;++e){Edge edge{{result.active_faces[i][e],result.active_faces[i][(e+1U)%3U]}};std::sort(edge.begin(),edge.end());if(paired_edge_uses[{i,edge}]!=1U)result.audit.residual_sheets_closed=false;}
  std::vector<std::size_t> component(result.active_faces.size(),static_cast<std::size_t>(-1));
  std::size_t component_index{};
  for(std::size_t seed=0;seed<result.active_faces.size();++seed)if(component[seed]==static_cast<std::size_t>(-1)){
    std::vector<std::size_t> pending{seed};component[seed]=component_index;
    while(!pending.empty()){const auto face_index=pending.back();pending.pop_back();
      for(const auto other:sheet_neighbours[face_index])if(component[other]==static_cast<std::size_t>(-1)){component[other]=component_index;pending.push_back(other);}}
    ++component_index;
  }
  result.audit.unresolved_components=component_index;
  result.audit.no_strict_overlap=true;
  for(std::size_t i=0;i<result.tetrahedra.size();++i){const auto left=result.tetrahedra[i];const std::array<Vec3,4> a{{result.vertices[left[0]],result.vertices[left[1]],result.vertices[left[2]],result.vertices[left[3]]}};
      for(std::size_t j=i+1U;j<result.tetrahedra.size();++j){if(!overlaps(emitted_bounds[i],emitted_bounds[j]))continue;const auto right=result.tetrahedra[j];const std::array<Vec3,4> b{{result.vertices[right[0]],result.vertices[right[1]],result.vertices[right[2]],result.vertices[right[3]]}};if(strict_tetrahedra_overlap(a,b))result.audit.no_strict_overlap=false;}
      for(std::size_t j=0;j<fixture.core_tetrahedra.size();++j){if(!overlaps(emitted_bounds[i],core_bounds[j]))continue;const auto right=fixture.core_tetrahedra[j];const std::array<Vec3,4> b{{fixture.core_vertices[right[0]],fixture.core_vertices[right[1]],fixture.core_vertices[right[2]],fixture.core_vertices[right[3]]}};if(strict_tetrahedra_overlap(a,b))result.audit.no_strict_overlap=false;}}
  result.audit.accepted=result.audit.positive_tetrahedra&&result.audit.unique_tetrahedra&&
      result.audit.consistently_oriented_faces&&result.audit.no_strict_overlap&&
      result.audit.exact_boundary&&result.audit.exact_oriented_boundary&&
      result.audit.frozen_input_vertices_unchanged&&result.audit.exact_volume;
  return result;
}

std::string make_advancing_front_replay_data(const AdvancingFrontFillResult& result){
  std::ostringstream out;out.precision(17);out<<"{\"format\":\"advancing-front-replay-v2\",\"fixture\":{\"gridResolution\":"<<result.fixture_config.grid_resolution
     <<",\"coreRedDepth\":"<<result.fixture_config.core_red_depth<<",\"surfaceHeight\":"<<result.fixture_config.surface_height
     <<",\"noiseAmplitude\":"<<result.fixture_config.noise_amplitude<<",\"noiseFrequency\":"<<result.fixture_config.noise_frequency
     <<",\"coreClearance\":"<<result.fixture_config.core_clearance<<"},\"options\":{\"maximumSteps\":"<<result.fill_options.maximum_steps
     <<",\"existingCandidateLimit\":"<<result.fill_options.existing_candidate_limit<<",\"recoveryCandidateLimit\":"<<result.fill_options.recovery_candidate_limit
     <<",\"maximumAtomicFaces\":"<<result.fill_options.maximum_atomic_faces<<",\"maximumPocketFaces\":"<<result.fill_options.maximum_pocket_faces
     <<",\"maximumPocketExpansions\":"<<result.fill_options.maximum_pocket_expansions<<",\"maximumExpansionTetrahedra\":"<<result.fill_options.maximum_expansion_tetrahedra
     <<",\"minimumTetrahedronVolumeFraction\":"<<result.fill_options.minimum_tetrahedron_volume_fraction<<"},\"vertices\":[";
  for(std::size_t i=0;i<result.vertices.size();++i){if(i)out<<',';const auto p=result.vertices[i];out<<'['<<p.x<<','<<p.y<<','<<p.z<<']';}
  out<<"],\"stableVertexIds\":[";for(std::size_t i=0;i<result.stable_vertex_ids.size();++i){if(i)out<<',';out<<result.stable_vertex_ids[i];}
  const auto write_indices=[&](const char* name,const auto& values){out<<"],\""<<name<<"\":[";for(std::size_t i=0;i<values.size();++i){if(i)out<<',';out<<'[';for(std::size_t j=0;j<values[i].size();++j){if(j)out<<',';out<<values[i][j];}out<<']';}};
  write_indices("tetrahedra",result.tetrahedra);write_indices("activeFaces",result.active_faces);
  out<<"],\"audit\":{\"accepted\":"<<(result.audit.accepted?"true":"false")
     <<",\"obstructionFound\":"<<(result.audit.obstruction_found?"true":"false")
     <<",\"positiveTetrahedra\":"<<(result.audit.positive_tetrahedra?"true":"false")
     <<",\"noStrictOverlap\":"<<(result.audit.no_strict_overlap?"true":"false")
     <<",\"frozenFacesPreserved\":"<<(result.audit.frozen_faces_preserved_partial?"true":"false")
     <<",\"residualSheetsClosed\":"<<(result.audit.residual_sheets_closed?"true":"false")
     <<",\"initialFaces\":"<<result.audit.initial_faces
     <<",\"remainingFaces\":"<<result.audit.remaining_faces
     <<",\"unresolvedComponents\":"<<result.audit.unresolved_components
     <<",\"rejectedSmallVolume\":"<<result.audit.rejected_small_volume
     <<",\"rejectedOverlap\":"<<result.audit.rejected_overlap
     <<",\"rejectedFrontCrossing\":"<<result.audit.rejected_front_crossing
     <<",\"rejectedFrontIncidence\":"<<result.audit.rejected_front_incidence
     <<",\"cavityVolume\":"<<result.audit.cavity_volume
     <<",\"tetrahedraVolume\":"<<result.audit.tetrahedra_volume
     <<",\"remainingVolume\":"<<result.audit.remaining_volume<<"}}";
  return out.str();
}

} // namespace tetra::probes
