#include "tetra_probes/nonmatching_red_frustum_control.hpp"
#include <algorithm>
#include <cmath>
#include <map>
namespace tetra::probes { namespace {
using Tet=std::array<std::uint32_t,4>; using Face=std::array<std::uint32_t,3>;
constexpr std::array<std::array<unsigned,3>,4> F{{{{1,2,3}},{{0,3,2}},{{0,1,3}},{{0,2,1}}}};
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}double dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}double len(Vec3 a){return std::sqrt(dot(a,a));}double six(const std::vector<Vec3>&p,const Tet&t){return dot(p[t[1]]-p[t[0]],cross(p[t[2]]-p[t[0]],p[t[3]]-p[t[0]]));}Tet pos(const std::vector<Vec3>&p,Tet t){if(six(p,t)<0)std::swap(t[0],t[1]);return t;}Face key(Face f){std::sort(f.begin(),f.end());return f;}
std::pair<double,double> q(const std::vector<Vec3>&p,const std::vector<Tet>&ts){double lo=180,hi=0;for(const auto&t:ts)for(unsigned u=0;u<4;++u)for(unsigned v=u+1;v<4;++v){unsigned a=4,b=4;for(unsigned i=0;i<4;++i)if(i!=u&&i!=v){if(a==4)a=i;else b=i;}auto n=cross(p[t[v]]-p[t[u]],p[t[a]]-p[t[u]]),m=cross(p[t[u]]-p[t[v]],p[t[b]]-p[t[v]]);const auto d=len(n)*len(m);if(d<1e-20)return {0,180};const auto x=std::acos(std::clamp(dot(n,m)/d,-1.,1.))*180./std::acos(-1.);lo=std::min(lo,x);hi=std::max(hi,x);}return {lo,hi};}
}
NonmatchingRedFrustumResult construct_nonmatching_red_frustum_control(const NonmatchingRedFrustumInput& in){
  NonmatchingRedFrustumResult r; Vec3 ci{},co{};for(unsigned i=0;i<4;++i){ci=ci+in.inner[i];co=co+in.outer[i];}ci=ci/4.;co=co/4.;
  // One positive homothety is the complete geometry contract for this control.
  double scale{};for(unsigned i=0;i<4;++i){const auto a=in.inner[i]-ci,b=in.outer[i]-co;const auto d=dot(a,a);if(d<1e-14)return r;const auto s=dot(a,b)/d;if(i==0)scale=s;if(std::abs(s-scale)>1e-10||len(b-a*scale)>1e-10||len(co-ci)>1e-10||scale<=1.)return r;}
  std::vector<Vec3> p;for(auto x:in.inner)p.push_back(x);for(auto x:in.outer)p.push_back(x);std::vector<Tet> ts;
  constexpr std::array<Face,4> parents{{{{4,6,5}},{{4,5,7}},{{4,7,6}},{{5,6,7}}}};
  for(const auto outer:parents){Face inner{};for(unsigned i=0;i<3;++i)inner[i]=outer[i]-4;auto o=outer,b=inner;std::sort(o.begin(),o.end());std::sort(b.begin(),b.end());ts.push_back(pos(p,{{o[0],o[1],o[2],b[0]}}));ts.push_back(pos(p,{{o[1],o[2],b[0],b[1]}}));ts.push_back(pos(p,{{o[2],b[0],b[1],b[2]}}));}
  constexpr std::array<std::array<unsigned,2>,6> edges{{{{4,5}},{{4,6}},{{4,7}},{{5,6}},{{5,7}},{{6,7}}}};
  for(const auto e:edges){const auto mid=static_cast<std::uint32_t>(p.size());p.push_back((p[e[0]]+p[e[1]])/2.);std::vector<Tet> next;for(const auto&t:ts){const bool a=std::find(t.begin(),t.end(),e[0])!=t.end(),b=std::find(t.begin(),t.end(),e[1])!=t.end();if(!a||!b){next.push_back(t);continue;}std::array<std::uint32_t,2> other{};unsigned c{};for(auto x:t)if(x!=e[0]&&x!=e[1])other[c++]=x;next.push_back(pos(p,{{e[0],mid,other[0],other[1]}}));next.push_back(pos(p,{{mid,e[1],other[0],other[1]}}));}ts=std::move(next);}
  SurfaceCoreTransitionInput check;check.vertices.assign(p.begin(),p.begin()+8);check.stable_vertex_ids={10,11,12,13,20,21,22,23};check.retained_core_tetrahedra={{{0,1,2,3}}};check.coordinate_scale=in.scale;for(const auto f:parents){check.outer_faces.push_back(f);FrozenFacetIdentity id{{check.stable_vertex_ids[f[0]],check.stable_vertex_ids[f[1]],check.stable_vertex_ids[f[2]]}};std::sort(id.vertex_ids.begin(),id.vertex_ids.end());check.outer_parent_facets.push_back({id,FacetPreservationMode::geometric});}
  r.output.tetrahedra=ts;r.output.tetrahedra.push_back({{0,1,2,3}});for(std::size_t i=8;i<p.size();++i){r.output.owned_vertices.push_back(p[i]);r.output.owned_vertex_ids.push_back(100+i);}
  // Bind actual red boundary faces to parent exact barycentrics.
  std::map<Face,unsigned> uses;for(const auto&t:ts)for(const auto f:F)++uses[key({{t[f[0]],t[f[1]],t[f[2]]}})];
  for(std::size_t pi=0;pi<parents.size();++pi){auto split=split_frozen_facet(check.outer_parent_facets[pi].identity,FacetPreservationMode::geometric,0,0);const auto roots=parents[pi];std::array<unsigned,3> sorted=roots;std::sort(sorted.begin(),sorted.end());for(const auto&exact:split.subfaces){OutputFacetSubface out;out.exact=exact;for(unsigned c=0;c<3;++c){const auto&b=exact.corners[c];for(unsigned i=0;i<3;++i)if(b.numerator[i]==b.denominator)out.vertices[c]=sorted[i];if(b.denominator==2U){std::array<unsigned,2> endpoints{};unsigned endpoint_count{};for(unsigned i=0;i<3;++i)if(b.numerator[i]==1U)endpoints[endpoint_count++]=sorted[i];if(endpoint_count!=2U)return r;auto x=endpoints[0],y=endpoints[1];if(y<x)std::swap(x,y);for(unsigned ei=0;ei<6;++ei)if(edges[ei][0]==x&&edges[ei][1]==y)out.vertices[c]=8+ei;}}r.output.outer_preserved_facets.push_back(out);}}
  r.validation=validate_surface_core_transition_output(check,r.output);const auto range=q(p,r.output.tetrahedra);r.minimum_dihedral=range.first;r.maximum_dihedral=range.second;if(!r.validation.valid){r.output={};r.failure=NonmatchingRedFrustumFailure::geometry_rejected;return r;}if(range.first<5||range.second>175){r.output={};r.failure=NonmatchingRedFrustumFailure::quality_rejected;return r;}r.failure=NonmatchingRedFrustumFailure::none;return r;
}
}
