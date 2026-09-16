// External-TetGen oracle for independently closed DC shell chunks.
//
// This deliberately lives with the research artifacts rather than in the
// production probe library.  It preserves owned DC facets exactly, closes a
// chunk along the *surface-edge ownership boundary* (never by cutting a DC
// triangle), and leaves a one-source-cell core moat at the request cut.  The
// moat is important: a local closed PLC cannot also expose a core face which
// is shared directly by retained core tets on the other request.
#include "../../src/tetra_probes/sandwich_probe.cpp"

#include <fstream>
#include <iostream>
#include <queue>

namespace {
using namespace tetra::probes;

using Facet = std::pair<DualFaceKey, int>;

int main_impl(int argc,char** argv) {
  if(argc<3||argc>6) return 2;
  const bool left=std::string_view{argv[2]}=="left";
  if(!left&&std::string_view{argv[2]}!="right") return 2;
  SandwichConfig config;
  if(argc>=4) config.resolution=static_cast<unsigned>(std::stoul(argv[3]));
  if(argc==6) { config.phase_x=std::stod(argv[4]); config.phase_y=std::stod(argv[5]); }
  const unsigned n=config.resolution,split=n,span=2U*n;
  // This is a genuinely bounded request: the returned sheet contains only
  // triangles owned by this side.  It evaluates the one-cell vertex halo
  // needed to finish those triangles and one adjacent owner strip only to
  // name the curtain edges.  It never regenerates or filters the full sheet.
  const auto request=dual_contour_chunk_request(config,left?0U:split,left?split:span,span);
  const auto& owned=request.owned.triangles;
  std::map<DualEdge,unsigned> local_edges;
  for(const auto& triangle:owned) for(unsigned edge=0;edge<3;++edge) {
    auto a=triangle.vertices[edge],b=triangle.vertices[(edge+1U)%3U];
    if(b<a)std::swap(a,b);++local_edges[{{a,b}}];
  }
  if(owned.empty()) return 3;

  DualVolumeBuild mesh;
  for(const auto& triangle:owned) for(const auto cell:triangle.vertices) {
    const auto p=request.owned.vertices.at(cell);
    mesh.vertices.emplace(dual_volume_vertex_id(cell,0U),p);
    // The bottom copy is a declared artificial exterior closure.  Its stable
    // ID makes the whole ownership-boundary curtain bitwise identical.
    mesh.vertices.emplace(dual_volume_vertex_id(cell,1U),Vec3{p.x,p.y,-0.95});
  }
  std::vector<Facet> facets;
  for(const auto& triangle:owned) {
    facets.push_back({{{dual_volume_vertex_id(triangle.vertices[0],0U),dual_volume_vertex_id(triangle.vertices[1],0U),dual_volume_vertex_id(triangle.vertices[2],0U)}},1});
    facets.push_back({{{dual_volume_vertex_id(triangle.vertices[2],1U),dual_volume_vertex_id(triangle.vertices[1],1U),dual_volume_vertex_id(triangle.vertices[0],1U)}},2});
  }
  for(const auto& [edge,count]:local_edges) if(count==1U) {
    const auto a=dual_volume_vertex_id(edge[0],0U),b=dual_volume_vertex_id(edge[1],0U);
    const auto A=dual_volume_vertex_id(edge[0],1U),B=dual_volume_vertex_id(edge[1],1U);
    const bool seam=request.seam_edges.contains(edge);
    const int marker=seam?4:2;
    // The same globally ordered diagonal is emitted by both chunk requests.
    facets.push_back({{{a,b,A}},marker}); facets.push_back({{{b,A,B}},marker});
  }

  // Retain only an interior core.  The first/last source-cell layers are a
  // declared clearance region for the artificial outer closure; erase the
  // layer which touches the request cut as well.  The retained local core is
  // then a genuine closed hole, while the inter-request volume is wholly
  // shell and is separated by the canonical curtain above.
  const auto selected=select_retained_regular_core(config,left?0U:split,left?split:span);
  std::set<RetainedCoreTetKey> core;
  for(const auto tet:selected) {
    bool interior=true;
    for(const auto id:tet) {
      const auto coordinate=lattice_coordinates(id,n);
      const auto x=coordinate[0],y=coordinate[1],z=coordinate[2];
      interior=interior&&y>=1U&&y<n-1U&&z>=1U&&z<n/2U&&
          (left?(x>=1U&&x<split-1U):(x>split&&x<span-1U));
    }
    if(interior) core.insert(tet);
  }
  const auto core_id=[](std::uint64_t id) { return 0x100000000ULL+id; };
  std::map<DualFaceKey,unsigned> core_faces;
  for(const auto& tet:core) {
    std::array<std::uint64_t,4> ids{};
    for(unsigned i=0;i<4;++i) { ids[i]=core_id(tet[i]); mesh.vertices.emplace(ids[i],lattice_position({KeyKind::lattice,tet[i]},n)); }
    for(const auto face:tet_faces) ++core_faces[canonical_dual_face({{ids[face[0]],ids[face[1]],ids[face[2]]}})];
  }
  for(const auto& [face,count]:core_faces) if(count==1U) facets.push_back({face,3});

  std::map<std::uint64_t,unsigned> indexes;
  for(const auto& [face,kind]:facets) { (void)kind;for(const auto id:face)indexes.emplace(id,0U); }
  unsigned index{};for(auto& [id,value]:indexes)value=index++;
  std::ofstream ids(std::string(argv[1])+".ids");
  for(const auto& [id,value]:indexes) ids<<value<<' '<<id<<'\n';
  std::ofstream out(argv[1]); out<<std::setprecision(17)<<indexes.size()<<" 3 0 0\n";
  for(const auto& [id,value]:indexes) { const auto p=mesh.vertices.at(id);out<<value<<' '<<p.x<<' '<<p.y<<' '<<p.z<<'\n'; }
  out<<facets.size()<<" 1\n";
  for(const auto& [face,kind]:facets) { out<<"1 0 "<<kind<<"\n3";for(const auto id:face)out<<' '<<indexes.at(id);out<<'\n'; }
  // One interior point from every connected core component is enough to make
  // the retained tets a hole for TetGen.  In this height-field corpus the
  // component walk is deterministic by canonical tet order.
  std::map<RetainedCoreFaceKey,std::vector<std::size_t>> core_adjacency;
  std::vector<RetainedCoreTetKey> ordered(core.begin(),core.end());
  for(std::size_t i=0;i<ordered.size();++i) for(const auto face:tet_faces)
    core_adjacency[canonical_retained_core_face({{ordered[i][face[0]],ordered[i][face[1]],ordered[i][face[2]]}})].push_back(i);
  std::vector<bool> seen(ordered.size()); std::vector<Vec3> holes;
  for(std::size_t seed=0;seed<ordered.size();++seed) if(!seen[seed]) {
    seen[seed]=true;std::queue<std::size_t> pending;pending.push(seed);
    const auto& tet=ordered[seed]; Vec3 centre{};for(const auto id:tet)centre=centre+lattice_position({KeyKind::lattice,id},n);holes.push_back(centre/4.0);
    while(!pending.empty()) { const auto current=pending.front();pending.pop();for(const auto face:tet_faces) {
      const auto key=canonical_retained_core_face({{ordered[current][face[0]],ordered[current][face[1]],ordered[current][face[2]]}});
      for(const auto next:core_adjacency.at(key))if(!seen[next]){seen[next]=true;pending.push(next);}
    }}
  }
  out<<holes.size()<<'\n';for(std::size_t i=0;i<holes.size();++i) {
    const auto hole=holes[i];out<<i<<' '<<hole.x<<' '<<hole.y<<' '<<hole.z<<'\n';
  }out<<"0\n";
  std::ofstream sidecar(std::string(argv[1])+".core");sidecar<<std::setprecision(17)<<core.size()<<'\n';
  for(const auto& tet:core) { for(const auto id:tet)sidecar<<core_id(id)<<' ';sidecar<<'\n'; }
  std::cout<<"owned_triangles="<<owned.size()<<" core_tets="<<core.size()<<" facets="<<facets.size()<<" holes="<<holes.size()
           <<" requested_cells="<<request.owned.requested_cells<<" halo_cells="<<request.owned.halo_cells
           <<" seam_dependency_cells="<<request.seam_dependency_cells
           <<" peak_temporary_cells="<<request.peak_temporary_cells<<'\n';
  return 0;
}
} // namespace
int main(int argc,char** argv) { try { return main_impl(argc,argv); } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 5; } }
