#include "tetra_probes/refined_core_transaction.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <cmath>
namespace tetra::probes {
namespace {
using IndexTet=std::array<std::uint32_t,4>;
using IndexFace=std::array<std::uint32_t,3>;
constexpr std::array<std::array<unsigned,3>,4> kTetFaces{{{{1,2,3}},{{0,3,2}},{{0,1,3}},{{0,2,1}}}};
IndexFace key(IndexFace f){std::sort(f.begin(),f.end());return f;}
Vec3 vec(RegularCorePoint p){return {p.x,p.y,p.z};}
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
double dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
double length(Vec3 a){return std::sqrt(dot(a,a));}
double six(const std::vector<RegularCorePoint>& p,const IndexTet&t){const auto a=vec(p[t[0]]),b=vec(p[t[1]]),c=vec(p[t[2]]),d=vec(p[t[3]]);return dot(b-a,cross(c-a,d-a));}
IndexTet positive(const std::vector<RegularCorePoint>& p,IndexTet t){if(six(p,t)<0.)std::swap(t[0],t[1]);return t;}
std::pair<double,double> quality(const std::vector<RegularCorePoint>& p,const std::vector<IndexTet>& ts){
  double lo=180.,hi=0.; for(const auto&t:ts)for(std::size_t u=0;u<4;++u)for(std::size_t v=u+1;v<4;++v){std::size_t a=4,b=4;for(std::size_t i=0;i<4;++i)if(i!=u&&i!=v){if(a==4)a=i;else b=i;}const auto U=vec(p[t[u]]),V=vec(p[t[v]]),A=vec(p[t[a]]),B=vec(p[t[b]]);auto n1=cross(V-U,A-U),n2=cross(U-V,B-V);const auto den=length(n1)*length(n2);if(den<=1e-20)return {0.,180.};const auto degrees=std::acos(std::clamp(dot(n1,n2)/den,-1.,1.))*180./std::acos(-1.);lo=std::min(lo,degrees);hi=std::max(hi,degrees);}
  return {lo,hi};
}
}
RefinedCoreTransactionResult begin_refined_core_transaction(RefinedCoreTransactionInput input) {
  RefinedCoreTransactionResult out;
  const auto clear_publishable=[&] {
    out.cut={}; out.core={}; out.outer_red_vertices.clear(); out.combined_vertices.clear();
    out.combined_core_tetrahedra.clear(); out.shell_tetrahedra.clear(); out.expected_outer_boundary.clear();
    out.interface_subfaces.clear(); out.core_subface_provenance.clear(); out.outer_subface_provenance.clear();
  };
  // This deliberately small transaction supports the one-parent control and
  // the explicit, reciprocal two-parent descriptor only.
  if(input.topology.empty()||input.topology.size()>2U||input.topology.size()!=input.core_geometry.parents.size()){out.failure=RefinedCoreTransactionFailure::unsupported_parent_topology;return out;}
  out.cut=refine_regular_core(input.topology,input.request,input.limits);
  if(!out.cut.accepted()) { out.failure=RefinedCoreTransactionFailure::core_refinement_refused; clear_publishable(); return out; }
  out.core=materialize_regular_core_red(input.topology,out.cut,input.core_geometry);
  if(!out.core.accepted()) { out.failure=RefinedCoreTransactionFailure::core_materialization_refused; clear_publishable(); return out; }
  std::map<std::uint64_t,RegularCorePoint> roots;
  for(const auto& v:input.outer_root_vertices) if(!roots.emplace(v.id,v.point).second) {out.failure=RefinedCoreTransactionFailure::invalid_outer_descriptor;clear_publishable();return out;}
  std::set<std::uint64_t> required;
  for(const auto& p:input.core_geometry.parents) required.insert(p.vertices.begin(),p.vertices.end());
  if(roots.size()!=required.size()||!std::all_of(required.begin(),required.end(),[&](auto id){return roots.contains(id);})){out.failure=RefinedCoreTransactionFailure::invalid_outer_descriptor;clear_publishable();return out;}
  // Core materialization gives a deterministic sorted vertex stream; root IDs
  // are shared provenance and generated IDs retain the same sorted-edge order.
  std::set<std::array<std::uint64_t,2>> edges;
  for(const auto& p:input.core_geometry.parents) for(std::size_t a=0;a<4U;++a)for(std::size_t b=a+1U;b<4U;++b) {auto e=std::array<std::uint64_t,2>{{p.vertices[a],p.vertices[b]}};if(e[1]<e[0])std::swap(e[0],e[1]);edges.insert(e);}
  std::map<std::uint64_t,RegularCorePoint> generated; std::map<std::array<std::uint64_t,2>,std::uint64_t> midpoint_ids; std::uint64_t next=(std::uint64_t{1}<<63U);
  for(const auto&e:edges) {midpoint_ids.emplace(e,next);generated.emplace(next++,RegularCorePoint{(roots.at(e[0]).x+roots.at(e[1]).x)/2.0,(roots.at(e[0]).y+roots.at(e[1]).y)/2.0,(roots.at(e[0]).z+roots.at(e[1]).z)/2.0});}
  for(const auto& v:out.core.vertices) {
    if(roots.contains(v.id)) out.outer_red_vertices.push_back({v.id,roots.at(v.id)});
    else {
      out.outer_red_vertices.push_back({v.id,generated.at(v.id)});
    }
  }
  // Materialize the one supported combined domain.  The core and outer use
  // separate index spaces even though their logical red addresses coincide.
  std::map<std::uint64_t,std::uint32_t> core_index,outer_index;
  for(const auto& v:out.core.vertices) {core_index.emplace(v.id,static_cast<std::uint32_t>(out.combined_vertices.size()));out.combined_vertices.push_back(v.point);}
  for(const auto& v:out.outer_red_vertices) {outer_index.emplace(v.id,static_cast<std::uint32_t>(out.combined_vertices.size()));out.combined_vertices.push_back(v.point);}
  for(const auto& child:out.core.children) {IndexTet t{};for(std::size_t i=0;i<4;++i)t[i]=core_index.at(child.vertices[i]);out.combined_core_tetrahedra.push_back(positive(out.combined_vertices,t));}
  std::map<IndexFace,unsigned> core_uses;
  for(const auto&t:out.combined_core_tetrahedra)for(const auto f:kTetFaces)++core_uses[key({{t[f[0]],t[f[1]],t[f[2]]}})];
  for(const auto&[inner,n]:core_uses)if(n==1U) {
    out.interface_subfaces.push_back(inner); IndexFace outer{};
    for(std::size_t i=0;i<3;++i) { const auto core_id=out.core.vertices[inner[i]].id; outer[i]=outer_index.at(core_id); }
    std::sort(outer.begin(),outer.end()); std::array<std::uint32_t,3> bottom=inner;std::sort(bottom.begin(),bottom.end());
    out.shell_tetrahedra.push_back(positive(out.combined_vertices,{{outer[0],outer[1],outer[2],bottom[0]}}));
    out.shell_tetrahedra.push_back(positive(out.combined_vertices,{{outer[1],outer[2],bottom[0],bottom[1]}}));
    out.shell_tetrahedra.push_back(positive(out.combined_vertices,{{outer[2],bottom[0],bottom[1],bottom[2]}}));
    out.expected_outer_boundary.push_back(outer);
  }
  std::sort(out.shell_tetrahedra.begin(),out.shell_tetrahedra.end());
  // Every red parent face has the same exact four-subface record on the core
  // and outer side. The red descriptor is the only supported provenance.
  std::map<RegularCoreParentId,const RegularCoreParent*> topology_by_id;
  for(const auto& parent:input.topology) topology_by_id.emplace(parent.id,&parent);
  for(const auto& parent:input.core_geometry.parents) for(std::size_t omit=0;omit<4;++omit) {
    if(topology_by_id.at(parent.id)->neighbors[omit]) continue; // shared core faces stay internal
    FrozenFacetIdentity id{};std::size_t c{};for(std::size_t i=0;i<4;++i)if(i!=omit)id.vertex_ids[c++]=parent.vertices[i];
    const auto split=split_frozen_facet(id,FacetPreservationMode::geometric,0U,0U);out.core_subface_provenance.insert(out.core_subface_provenance.end(),split.subfaces.begin(),split.subfaces.end());out.outer_subface_provenance.insert(out.outer_subface_provenance.end(),split.subfaces.begin(),split.subfaces.end());
  }
  SurfaceCoreTransitionInput check; for(const auto&p:out.combined_vertices)check.vertices.push_back(vec(p)); check.retained_core_tetrahedra=out.combined_core_tetrahedra; check.coordinate_scale=3.;
  for(std::size_t i=0;i<out.combined_vertices.size();++i) check.stable_vertex_ids.push_back(i<out.core.vertices.size()?static_cast<std::uint64_t>(i+1U):0x4000000000000000ULL+static_cast<std::uint64_t>(i));
  Vec3 center{}; for(const auto& t:out.combined_core_tetrahedra) for(const auto index:t) center=center+vec(out.combined_vertices[index]); center=center/static_cast<double>(out.combined_core_tetrahedra.size()*4U);
  // The public validation input names the four original outer parents, not
  // their red children.  The emitted child boundary below is bound back to
  // those parents through exact barycentrics and actual output indices.
  std::map<FrozenFacetIdentity,std::array<std::uint64_t,3>> roots_by_outer_parent;
  for(const auto& parent:input.core_geometry.parents) for(std::size_t omit=0;omit<4;++omit) {
    if(topology_by_id.at(parent.id)->neighbors[omit]) continue;
    IndexFace outer{};std::array<std::uint64_t,3> parent_roots{};std::size_t c{};for(std::size_t i=0;i<4;++i)if(i!=omit){outer[c]=outer_index.at(parent.vertices[i]);parent_roots[c++]=parent.vertices[i];}const auto a=check.vertices[outer[0]],b=check.vertices[outer[1]],cc=check.vertices[outer[2]];if(dot(cross(b-a,cc-a),center-a)>0.)std::swap(outer[1],outer[2]);std::sort(parent_roots.begin(),parent_roots.end());check.outer_faces.push_back(outer);FrozenFacetIdentity identity{{check.stable_vertex_ids[outer[0]],check.stable_vertex_ids[outer[1]],check.stable_vertex_ids[outer[2]]}};std::sort(identity.vertex_ids.begin(),identity.vertex_ids.end());roots_by_outer_parent.emplace(identity,parent_roots);check.outer_parent_facets.push_back({identity,FacetPreservationMode::geometric});
  }
  SurfaceCoreTransitionOutput assembled; assembled.tetrahedra=out.shell_tetrahedra;assembled.tetrahedra.insert(assembled.tetrahedra.end(),out.combined_core_tetrahedra.begin(),out.combined_core_tetrahedra.end());
  for(const auto& contract:check.outer_parent_facets) {
    const auto split=split_frozen_facet(contract.identity,FacetPreservationMode::geometric,0U,0U);
    const auto roots_for_parent=roots_by_outer_parent.at(contract.identity);
    for(const auto& exact:split.subfaces) {OutputFacetSubface report;report.exact=exact;for(std::size_t corner=0;corner<3;++corner){const auto& bary=exact.corners[corner];std::uint64_t address{};for(std::size_t i=0;i<3;++i)if(bary.numerator[i]==bary.denominator)address=roots_for_parent[i];if(address==0U&&bary.denominator==2U){std::array<std::uint64_t,2> edge{};std::size_t e{};for(std::size_t i=0;i<3;++i)if(bary.numerator[i]==1U)edge[e++]=roots_for_parent[i];if(e==2U){if(edge[1]<edge[0])std::swap(edge[0],edge[1]);address=midpoint_ids.at(edge);}}if(address==0U){out.failure=RefinedCoreTransactionFailure::geometry_rejected;clear_publishable();return out;}report.vertices[corner]=outer_index.at(address);}assembled.outer_preserved_facets.push_back(report);}
  }
  // Outer records use the actual outer stable-parent identities above, not a
  // copied core record. Their vertex bindings are validated by `check`.
  out.outer_subface_provenance.clear();
  for(const auto& report:assembled.outer_preserved_facets) out.outer_subface_provenance.push_back(report.exact);
  const auto audit=validate_surface_core_transition_output(check,assembled);
  out.validation=audit;
  out.positive=audit.positive_tetrahedra;out.no_strict_overlap=audit.no_strict_tetrahedron_overlap;out.closed_oriented_boundary=audit.closed_two_manifold&&audit.consistently_oriented_shared_faces;
  std::map<IndexFace,std::vector<std::pair<bool,IndexTet>>> interface_uses;
  for(const auto& t:out.combined_core_tetrahedra)for(const auto f:kTetFaces)interface_uses[key({{t[f[0]],t[f[1]],t[f[2]]}})].push_back({true,t});
  for(const auto& t:out.shell_tetrahedra)for(const auto f:kTetFaces)interface_uses[key({{t[f[0]],t[f[1]],t[f[2]]}})].push_back({false,t});
  out.interface_two_sided=true;
  for(const auto& f:out.interface_subfaces) {const auto& uses=interface_uses.at(f);if(uses.size()!=2U||uses[0].first==uses[1].first){out.interface_two_sided=false;continue;}const auto a=vec(out.combined_vertices[f[0]]),normal=cross(vec(out.combined_vertices[f[1]])-a,vec(out.combined_vertices[f[2]])-a);const auto opposite=[&](const IndexTet&t){for(const auto i:t)if(i!=f[0]&&i!=f[1]&&i!=f[2])return i;return f[0];};out.interface_two_sided&=dot(normal,vec(out.combined_vertices[opposite(uses[0].second)])-a)*dot(normal,vec(out.combined_vertices[opposite(uses[1].second)])-a)<0.;}
  const auto range=quality(out.combined_vertices,assembled.tetrahedra);out.minimum_dihedral_degrees=range.first;out.maximum_dihedral_degrees=range.second;out.s4=range.first>=5.&&range.second<=175.;
  if(!audit.valid||!out.interface_two_sided){out.failure=RefinedCoreTransactionFailure::geometry_rejected;clear_publishable();return out;}
  if(!out.s4){out.failure=RefinedCoreTransactionFailure::quality_rejected;clear_publishable();return out;}
  out.failure=RefinedCoreTransactionFailure::none; return out;
}
} // namespace tetra::probes
