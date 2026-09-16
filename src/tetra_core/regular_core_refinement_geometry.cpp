#include "tetra_core/regular_core_refinement_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace tetra { namespace {
constexpr std::uint64_t generated_id_base=std::uint64_t{1}<<63U;
RegularCoreMaterialization refuse(RegularCoreMaterializationRefusal r) { return {.refusal=r}; }
RegularCorePoint add(RegularCorePoint a,RegularCorePoint b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
RegularCorePoint scale(RegularCorePoint a,double s) { return {a.x*s,a.y*s,a.z*s}; }
double dot(RegularCorePoint a,RegularCorePoint b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
RegularCorePoint sub(RegularCorePoint a,RegularCorePoint b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
double det(RegularCorePoint a,RegularCorePoint b,RegularCorePoint c) { return dot(a,{b.y*c.z-b.z*c.y,b.z*c.x-b.x*c.z,b.x*c.y-b.y*c.x}); }
double six(const std::array<RegularCorePoint,4>& p) { return det(sub(p[1],p[0]),sub(p[2],p[0]),sub(p[3],p[0])); }
std::pair<double,double> dihedral_range(const std::array<RegularCorePoint,4>& p) {
  double minimum=180.,maximum=0.; constexpr std::array<std::array<int,2>,6> edges{{{{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  for(auto e:edges) { const auto u=static_cast<std::size_t>(e[0]),v=static_cast<std::size_t>(e[1]); std::size_t a=4U,b=4U; for(std::size_t i=0;i<4U;++i)if(i!=u&&i!=v){if(a==4U)a=i;else b=i;}
    const auto n1=[](RegularCorePoint a,RegularCorePoint b,RegularCorePoint c){return RegularCorePoint{(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y),(b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z),(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)};};
    const auto x=n1(p[u],p[v],p[a]),y=n1(p[v],p[u],p[b]); const auto d=std::sqrt(dot(x,x)*dot(y,y)); if(d<=1e-20)return {0.,180.};
    const auto angle=(std::acos(-1.)-std::acos(std::clamp(dot(x,y)/d,-1.,1.)))*180./std::acos(-1.); minimum=std::min(minimum,angle); maximum=std::max(maximum,angle); }
  return {minimum,maximum};
}
}}
namespace tetra {
RegularCoreMaterialization materialize_regular_core_red(const std::vector<RegularCoreParent>& topology,const RegularCoreRefinementResult& cut,RegularCoreGeometryDescriptor geometry) {
  if(!cut.accepted()) return refuse(RegularCoreMaterializationRefusal::rejected_cut);
  std::map<RegularCoreParentId,RegularCoreParent> topo; for(const auto& p:topology) if(!topo.emplace(p.id,p).second)return refuse(RegularCoreMaterializationRefusal::malformed_geometry);
  std::map<std::uint64_t,RegularCorePoint> points; for(const auto& v:geometry.vertices) { if(v.id>=generated_id_base||!std::isfinite(v.point.x)||!std::isfinite(v.point.y)||!std::isfinite(v.point.z)||!points.emplace(v.id,v.point).second)return refuse(RegularCoreMaterializationRefusal::malformed_geometry); }
  std::map<RegularCoreParentId,RegularCoreGeometricParent> parents; for(const auto& p:geometry.parents) if(!parents.emplace(p.id,p).second)return refuse(RegularCoreMaterializationRefusal::malformed_geometry);
  std::set<RegularCoreParentId> active; for(const auto& leaf:cut.active_leaves) { if(leaf.grammar_version!=regular_core_red_grammar_version||leaf.child>=8U) return refuse(RegularCoreMaterializationRefusal::rejected_cut); active.insert(leaf.parent); }
  if(cut.active_leaves.size()!=active.size()*8U) return refuse(RegularCoreMaterializationRefusal::rejected_cut);
  std::map<std::array<std::uint64_t,2>,std::uint64_t> midpoint;
  for(const auto id:active) {
    if(!topo.contains(id)||!parents.contains(id))return refuse(RegularCoreMaterializationRefusal::stable_id_mismatch);
    const auto& g=parents.at(id); std::set<std::uint64_t> unique(g.vertices.begin(),g.vertices.end()); if(unique.size()!=4U)return refuse(RegularCoreMaterializationRefusal::malformed_geometry);
    for(const auto v:g.vertices)if(!points.contains(v))return refuse(RegularCoreMaterializationRefusal::stable_id_mismatch);
    for(std::uint8_t f=0;f<4U;++f) { std::array<std::uint64_t,3> face{};std::size_t n{};for(std::uint8_t i=0;i<4;++i)if(i!=f)face[n++]=g.vertices[i];std::sort(face.begin(),face.end());auto declared=topo.at(id).face_vertices[f];std::sort(declared.begin(),declared.end());if(face!=declared)return refuse(RegularCoreMaterializationRefusal::stable_id_mismatch); }
    for(std::size_t i=0;i<4U;++i)for(std::size_t j=i+1U;j<4U;++j){auto e=std::array<std::uint64_t,2>{{g.vertices[i],g.vertices[j]}};if(e[1]<e[0])std::swap(e[0],e[1]);midpoint.emplace(e,0U);}
  }
  // The component currently supports the supplied two-parent adjacency
  // contract: reciprocal parents sharing a face must have apexes on opposite
  // sides. This is not presented as a general mesh-overlap oracle.
  for(const auto id:active) for(std::uint8_t f=0;f<4U;++f) {
    const auto& link=topo.at(id).neighbors[f]; if(!link||!active.contains(link->face.parent)||id>link->face.parent)continue;
    const auto& left=parents.at(id); const auto& right=parents.at(link->face.parent); std::uint64_t left_apex{},right_apex{};
    for(std::uint8_t i=0;i<4U;++i){if(i==f)left_apex=left.vertices[i];if(i==link->face.face)right_apex=right.vertices[i];}
    const auto face=topo.at(id).face_vertices[f]; const auto side=[&](std::uint64_t apex){return det(sub(points.at(face[1]),points.at(face[0])),sub(points.at(face[2]),points.at(face[0])),sub(points.at(apex),points.at(face[0])));};
    if(side(left_apex)*side(right_apex)>=-1e-14)return refuse(RegularCoreMaterializationRefusal::overlap);
  }
  std::uint64_t next=generated_id_base; for(auto& [edge,id]:midpoint) { id=next++; points.emplace(id,scale(add(points.at(edge[0]),points.at(edge[1])),.5)); }
  RegularCoreMaterialization out; for(const auto& [id,p]:points)out.vertices.push_back({id,p});
  auto mid=[&](std::uint64_t a,std::uint64_t b){if(b<a)std::swap(a,b);return midpoint.at({{a,b}});};
  for(const auto id:active) { const auto v=parents.at(id).vertices; const auto m01=mid(v[0],v[1]),m02=mid(v[0],v[2]),m03=mid(v[0],v[3]),m12=mid(v[1],v[2]),m13=mid(v[1],v[3]),m23=mid(v[2],v[3]);
    const std::array<std::array<std::uint64_t,4>,8> red{{{{v[0],m01,m02,m03}},{{m01,v[1],m12,m13}},{{m02,m12,v[2],m23}},{{m03,m13,m23,v[3]}},{{m01,m02,m03,m23}},{{m01,m02,m12,m23}},{{m01,m12,m13,m23}},{{m01,m03,m13,m23}}}};
    std::array<RegularCorePoint,4> parent_points{};for(std::size_t i=0;i<4U;++i)parent_points[i]=points.at(v[i]); const auto parent_volume=std::abs(six(parent_points)); double child_volume{};
    for(std::uint8_t c=0;c<8U;++c){auto t=red[c];std::array<RegularCorePoint,4> p{};for(std::size_t i=0;i<4U;++i)p[i]=points.at(t[i]);auto volume=six(p);if(std::abs(volume)<=1e-14)return refuse(RegularCoreMaterializationRefusal::nonpositive);if(volume<0.){std::swap(t[0],t[1]);std::swap(p[0],p[1]);volume=-volume;}child_volume+=volume;const auto quality=dihedral_range(p);out.minimum_dihedral_degrees=out.children.empty()?quality.first:std::min(out.minimum_dihedral_degrees,quality.first);out.maximum_dihedral_degrees=out.children.empty()?quality.second:std::max(out.maximum_dihedral_degrees,quality.second);out.children.push_back({{id,c,regular_core_red_grammar_version},t});}
    if(std::abs(parent_volume-child_volume)>1e-10*std::max(1.,parent_volume))return refuse(RegularCoreMaterializationRefusal::partition_failure);
  }
  if(out.minimum_dihedral_degrees<5.||out.maximum_dihedral_degrees>175.)return refuse(RegularCoreMaterializationRefusal::quality_failure);
  return out;
}
} // namespace tetra
