#include "tetra_core/bounded_front_buffer.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
#include <set>

namespace tetra { namespace {
using Tet=std::array<std::uint32_t,4>; using Face=std::array<std::uint32_t,3>;
BoundedFrontBuffer fail(BoundedFrontBufferRefusal why) { return {.refusal=why}; }
BoundedBufferPoint sub(BoundedBufferPoint a,BoundedBufferPoint b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
double dot(BoundedBufferPoint a,BoundedBufferPoint b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
BoundedBufferPoint cross(BoundedBufferPoint a,BoundedBufferPoint b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
double six(const std::vector<BoundedBufferPoint>& points,const Tet& t) { return dot(sub(points[t[1]],points[t[0]]),cross(sub(points[t[2]],points[t[0]]),sub(points[t[3]],points[t[0]]))); }
Face key(Face face) { std::sort(face.begin(),face.end()); return face; }
std::array<std::uint32_t,2> edge_key(std::uint32_t a,std::uint32_t b) { if(b<a)std::swap(a,b);return {{a,b}}; }
bool strict_overlap(const std::vector<BoundedBufferPoint>& points,const Tet& left,const Tet& right) {
  // Separating-axis theorem for two tetrahedra.  Strict separation treats a
  // shared face/edge/vertex as legal adjacency rather than material overlap.
  constexpr std::array<std::array<unsigned,3>,4> faces{{{{1,2,3}},{{0,3,2}},{{0,1,3}},{{0,2,1}}}};
  constexpr std::array<std::array<unsigned,2>,6> edges{{{{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  std::vector<BoundedBufferPoint> axes;
  for(const auto face:faces) {
    const auto la=sub(points[left[face[1]]],points[left[face[0]]]);
    const auto lb=sub(points[left[face[2]]],points[left[face[0]]]);
    const auto ra=sub(points[right[face[1]]],points[right[face[0]]]);
    const auto rb=sub(points[right[face[2]]],points[right[face[0]]]);
    axes.push_back(cross(la,lb)); axes.push_back(cross(ra,rb));
  }
  for(const auto a:edges) for(const auto b:edges) {
    const auto la=sub(points[left[a[1]]],points[left[a[0]]]);
    const auto rb=sub(points[right[b[1]]],points[right[b[0]]]);
    axes.push_back(cross(la,rb));
  }
  for(const auto axis:axes) {
    const auto squared=dot(axis,axis); if(squared<=1e-30) continue;
    double lo=dot(points[left[0]],axis),hi=lo,other_lo=dot(points[right[0]],axis),other_hi=other_lo;
    for(std::size_t i=1;i<4;++i) { const auto first=dot(points[left[i]],axis),second=dot(points[right[i]],axis);lo=std::min(lo,first);hi=std::max(hi,first);other_lo=std::min(other_lo,second);other_hi=std::max(other_hi,second); }
    const auto tolerance=1e-12*std::max({1.,std::abs(lo),std::abs(hi),std::abs(other_lo),std::abs(other_hi)});
    if(hi<=other_lo+tolerance||other_hi<=lo+tolerance)return false;
  }
  return true;
}
std::pair<double,double> dihedral_range(const std::vector<BoundedBufferPoint>& points,const Tet& tet) {
  double minimum=180.,maximum=0.;
  for(std::size_t u=0;u<4U;++u) for(std::size_t v=u+1U;v<4U;++v) { std::array<std::size_t,2> other{};std::size_t n{};for(std::size_t i=0;i<4U;++i)if(i!=u&&i!=v)other[n++]=i;
    const auto a=points[tet[u]],b=points[tet[v]],c=points[tet[other[0]]],d=points[tet[other[1]]];const auto first=cross(sub(b,a),sub(c,a)),second=cross(sub(a,b),sub(d,b));const auto denominator=std::sqrt(dot(first,first)*dot(second,second));
    if(denominator==0.) return {0.,180.}; const auto angle=(std::acos(-1.)-std::acos(std::clamp(dot(first,second)/denominator,-1.,1.)))*180./std::acos(-1.);minimum=std::min(minimum,angle);maximum=std::max(maximum,angle);
  } return {minimum,maximum};
}
} }
namespace tetra {
BoundedFrontBuffer build_bounded_front_buffer(
    std::vector<BoundedBufferPoint> outer, std::vector<BoundedBufferPoint> inner,
    std::vector<BoundedBufferTriangle> triangles) {
  if(outer.empty()||outer.size()!=inner.size()) return fail(BoundedFrontBufferRefusal::mismatched_fronts);
  for(const auto point:outer) if(!std::isfinite(point.x)||!std::isfinite(point.y)||!std::isfinite(point.z)) return fail(BoundedFrontBufferRefusal::nonfinite_point);
  for(const auto point:inner) if(!std::isfinite(point.x)||!std::isfinite(point.y)||!std::isfinite(point.z)) return fail(BoundedFrontBufferRefusal::nonfinite_point);
  BoundedFrontBuffer out; out.vertices=outer; out.vertices.insert(out.vertices.end(),inner.begin(),inner.end());
  const auto offset=static_cast<std::uint32_t>(outer.size()); std::set<std::array<std::uint32_t,3>> unique_triangles;
  for(auto triangle:triangles) {
    auto ids=triangle.vertices; std::sort(ids.begin(),ids.end());
    if(ids[0]==ids[1]||ids[1]==ids[2]||ids[2]>=offset) return fail(BoundedFrontBufferRefusal::bad_triangle);
    if(!unique_triangles.insert(ids).second) return fail(BoundedFrontBufferRefusal::duplicate_triangle);
    // Canonical labels provide a shared, winding-independent side diagonal.
    const auto a=ids[0], b=ids[1], c=ids[2], ia=a+offset, ib=b+offset, ic=c+offset;
    std::array<Tet,3> prism{{{{a,b,c,ic}},{{a,b,ib,ic}},{{a,ia,ib,ic}}}};
    for(auto tet:prism) {
      const auto volume=six(out.vertices,tet);
      // Positivity is a geometric validity condition.  A fixed world-space
      // epsilon would incorrectly turn small valid DC cells into failures;
      // element size/shape is instead assessed by the independent S4 gate.
      if(!std::isfinite(volume)||volume==0.) return fail(BoundedFrontBufferRefusal::nonpositive_tetrahedron);
      if(volume<0.)std::swap(tet[0],tet[1]); out.tetrahedra.push_back(tet);
    }
  }
  std::sort(out.tetrahedra.begin(),out.tetrahedra.end());
  std::map<Face,std::vector<std::uint32_t>> use;
  constexpr std::array<std::array<unsigned,3>,4> faces{{{{1,2,3}},{{0,3,2}},{{0,1,3}},{{0,2,1}}}};
  for(const auto& tet:out.tetrahedra) for(std::size_t opposite=0;opposite<faces.size();++opposite) { const auto face=faces[opposite];use[key({{tet[face[0]],tet[face[1]],tet[face[2]]}})].push_back(tet[opposite]); }
  for(const auto& [face,opposites]:use) {
    if(opposites.size()>2U) return fail(BoundedFrontBufferRefusal::nonmanifold_output);
    if(opposites.size()==2U) { const auto a=out.vertices[face[0]],b=out.vertices[face[1]],c=out.vertices[face[2]]; const auto n=cross(sub(b,a),sub(c,a)); if(dot(n,sub(out.vertices[opposites[0]],a))*dot(n,sub(out.vertices[opposites[1]],a))>=0.) return fail(BoundedFrontBufferRefusal::inconsistent_shared_face); }
  }
  // The two supplied fronts are constraints, not suggestions.  Check their
  // actual output incidence so a later template change cannot silently drop
  // or duplicate a frozen DC facet.
  for(const auto triangle:triangles) {
    const auto outer_face=key(triangle.vertices);
    auto inner_face=triangle.vertices; for(auto& index:inner_face) index+=offset;
    if(use[outer_face].size()!=1U||use[key(inner_face)].size()!=1U) return fail(BoundedFrontBufferRefusal::nonmanifold_output);
  }
  std::map<std::array<std::uint32_t,2>,unsigned> front_edges;
  for(const auto triangle:triangles) for(std::size_t i=0;i<3U;++i) ++front_edges[edge_key(triangle.vertices[i],triangle.vertices[(i+1U)%3U])];
  std::set<Face> expected_boundary;
  for(const auto triangle:triangles) { expected_boundary.insert(key(triangle.vertices));auto inner_face=triangle.vertices;for(auto& index:inner_face)index+=offset;expected_boundary.insert(key(inner_face)); }
  for(const auto& [edge,count]:front_edges) {
    if(count>2U)return fail(BoundedFrontBufferRefusal::nonmanifold_output);
    if(count!=1U)continue;
    // The prism diagonal is selected from the complete triangle's canonical
    // order, not merely this edge.  Derive its two permitted side facets from
    // the emitted template, while insisting that they project to this one
    // boundary edge; this avoids accidentally accepting an internal cavity.
    for(const auto& [face,opposites]:use) {
      (void)opposites;
      std::set<std::uint32_t> projected;
      for(const auto vertex:face) projected.insert(vertex>=offset?vertex-offset:vertex);
      if(projected.size()==2U && *projected.begin()==edge[0] && *projected.rbegin()==edge[1]) expected_boundary.insert(face);
    }
  }
  for(const auto& [face,opposites]:use) if((opposites.size()==1U)!=(expected_boundary.contains(face))) return fail(BoundedFrontBufferRefusal::unexpected_boundary);
  for(std::size_t left=0;left<out.tetrahedra.size();++left) for(std::size_t right=left+1;right<out.tetrahedra.size();++right)
    if(strict_overlap(out.vertices,out.tetrahedra[left],out.tetrahedra[right])) return fail(BoundedFrontBufferRefusal::overlapping_tetrahedra);
  out.minimum_dihedral_degrees=180.; out.maximum_dihedral_degrees=0.;
  out.minimum_mean_ratio=1.; out.maximum_edge_ratio=0.;
  for(const auto& tet:out.tetrahedra) { const auto [minimum,maximum]=dihedral_range(out.vertices,tet);out.minimum_dihedral_degrees=std::min(out.minimum_dihedral_degrees,minimum);out.maximum_dihedral_degrees=std::max(out.maximum_dihedral_degrees,maximum);double sum{},lo=std::numeric_limits<double>::infinity(),hi{};for(std::size_t a=0;a<4;++a)for(std::size_t b=a+1;b<4;++b){const auto d=sub(out.vertices[tet[a]],out.vertices[tet[b]]);const auto e=std::sqrt(dot(d,d));sum+=e*e;lo=std::min(lo,e);hi=std::max(hi,e);}const auto ratio=12.*std::pow(std::abs(six(out.vertices,tet))/2.,2./3.)/sum;out.minimum_mean_ratio=std::min(out.minimum_mean_ratio,ratio);out.maximum_edge_ratio=std::max(out.maximum_edge_ratio,hi/lo); }
  out.dihedral_screen_passed=out.minimum_dihedral_degrees>=5.&&out.maximum_dihedral_degrees<=175.&&out.minimum_mean_ratio>=.01&&out.maximum_edge_ratio<=20.;
  return out;
}
} // namespace tetra
