// Bounded structural bridge control.  It deliberately carries the qualified
// field-normal two-front collar forward, then asks the smallest possible
// question about closing it against a shared regular-grid core top plane.
//
// A regular core cannot retain one different top-plane vertex per DC cell:
// cells in different k layers but the same (i,j) column must name the same
// regular-grid vertex.  This probe performs that mandatory canonical merge
// before emitting the three-tet prism bridge.  If a frozen inner DC triangle
// spans such a vertical step, its proposed bottom triangle collapses.  That is
// a concrete rejection of the single fixed-grid-interface construction, not a
// request to silently duplicate a core coordinate or to use an external
// tetrahedralizer.  A succeeding construction will need a finite stepped
// interface/cleaving rule before it can attach actual Freudenthal core faces.

#define TWO_FRONT_TRANSITION_PROBE_TEST
#include "two_front_transition_probe.cpp"
#undef TWO_FRONT_TRANSITION_PROBE_TEST

#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>

namespace {
using namespace tetra::probes;

constexpr std::uint64_t kGridIdTag=0x6000000000000000ULL;

struct BridgeTet { std::array<std::uint64_t,4> vertices{}; };
struct FixedInterfaceBridge {
  Collar collar;
  std::map<std::uint64_t,Vec3> vertices;
  std::vector<BridgeTet> tetrahedra;
  std::size_t regular_nodes{};
  std::size_t required_grid_triangles{};
  std::size_t collapsed_grid_triangles{};
  std::size_t degenerate_bridge_tetrahedra{};
  std::size_t duplicate_tetrahedra{};
  std::size_t same_side_faces{};
  std::size_t strict_overlaps{};
  bool frozen_faces_exact{true};
  bool deterministic{};
};

std::array<unsigned int,2> column_of(std::uint64_t cell,unsigned int n) {
  const auto i=static_cast<unsigned int>(cell/(static_cast<std::uint64_t>(n)*n));
  const auto j=static_cast<unsigned int>((cell/n)%n);
  return {{i,j}};
}

std::uint64_t regular_node_id(std::uint64_t cell,unsigned int n) {
  const auto column=column_of(cell,n);
  return kGridIdTag|(static_cast<std::uint64_t>(column[0])<<32U)|column[1];
}

// This is the exact globally named position of a node on the candidate
// regular core's top plane.  It intentionally has no DC/QEF coordinate in it.
Vec3 regular_core_top_position(std::uint64_t cell,unsigned int n) {
  return regular_dual_grid_position(cell,1U,n/2U-1U,n);
}

void add_oriented(FixedInterfaceBridge& result,std::array<std::uint64_t,4> tet) {
  const auto volume=signed_six_volume(result.vertices.at(tet[0]),result.vertices.at(tet[1]),
                                      result.vertices.at(tet[2]),result.vertices.at(tet[3]));
  if(volume<0.0)std::swap(tet[1],tet[2]);
  result.tetrahedra.push_back({tet});
}

FixedInterfaceBridge build_fixed_interface(const SandwichConfig& config,const Candidate& selected,
                                           const DualSurfaceBuild& source) {
  FixedInterfaceBridge result;
  result.collar=selected.collar;
  result.vertices=selected.collar.vertices;
  for(const auto& tet:selected.collar.tets)result.tetrahedra.push_back({tet.vertices});

  const auto n=config.resolution;
  const auto add_node=[&](std::uint64_t cell) {
    const auto id=regular_node_id(cell,n);
    const auto position=regular_core_top_position(cell,n);
    const auto [it,inserted]=result.vertices.emplace(id,position);
    if(!inserted && (std::bit_cast<std::uint64_t>(it->second.x)!=std::bit_cast<std::uint64_t>(position.x)||
                     std::bit_cast<std::uint64_t>(it->second.y)!=std::bit_cast<std::uint64_t>(position.y)||
                     std::bit_cast<std::uint64_t>(it->second.z)!=std::bit_cast<std::uint64_t>(position.z)))
      throw std::logic_error("regular core node position depended on DC cell k identity");
  };
  for(const auto triangle:source.triangles) {
    auto cells=triangle.vertices;
    std::sort(cells.begin(),cells.end());
    for(const auto cell:cells)add_node(cell);
    const std::array<std::uint64_t,3> top{{inner_id(cells[0]),inner_id(cells[1]),inner_id(cells[2])}};
    const std::array<std::uint64_t,3> bottom{{regular_node_id(cells[0],n),regular_node_id(cells[1],n),regular_node_id(cells[2],n)}};
    ++result.required_grid_triangles;
    if(bottom[0]==bottom[1]||bottom[1]==bottom[2]||bottom[0]==bottom[2])++result.collapsed_grid_triangles;
    add_oriented(result,{{top[0],top[1],top[2],bottom[0]}});
    add_oriented(result,{{top[1],top[2],bottom[0],bottom[1]}});
    add_oriented(result,{{top[2],bottom[0],bottom[1],bottom[2]}});
  }
  result.regular_nodes=result.vertices.size()-result.collar.vertices.size();

  std::set<TetKey> unique;
  struct Use { std::uint64_t opposite{}; };
  std::map<FaceKey,std::vector<Use>> faces;
  for(const auto& tet:result.tetrahedra) {
    const auto key=tet_key(tet.vertices);
    if(std::adjacent_find(key.begin(),key.end())!=key.end() || !unique.insert(key).second)++result.duplicate_tetrahedra;
    const auto& p=result.vertices;
    if(signed_six_volume(p.at(tet.vertices[0]),p.at(tet.vertices[1]),p.at(tet.vertices[2]),p.at(tet.vertices[3]))<=1e-13)
      ++result.degenerate_bridge_tetrahedra;
    for(unsigned i=0;i<tet_faces.size();++i) {
      const auto face=tet_faces[i];
      faces[face_key({{tet.vertices[face[0]],tet.vertices[face[1]],tet.vertices[face[2]]}})].push_back({tet.vertices[i]});
    }
  }
  for(const auto& face:result.collar.frozen_outer) {
    const auto found=faces.find(face);
    if(found==faces.end()||found->second.size()!=1U)result.frozen_faces_exact=false;
  }
  for(const auto& [face,uses]:faces)if(uses.size()==2U) {
    const auto& a=result.vertices.at(face[0]);
    const auto normal=cross(result.vertices.at(face[1])-a,result.vertices.at(face[2])-a);
    if(dot(normal,result.vertices.at(uses[0].opposite)-a)*dot(normal,result.vertices.at(uses[1].opposite)-a)>=0.0)
      ++result.same_side_faces;
  }
  // This is exhaustive over the finite candidate output, including pairs that
  // share a face.  It establishes that the rejection below is not hidden by a
  // collision of otherwise positive tets.
  DualVolumeBuild view;view.vertices=result.vertices;
  for(std::size_t a=0;a<result.tetrahedra.size();++a)for(std::size_t b=a+1U;b<result.tetrahedra.size();++b) {
    const DualVolumeTet left{result.tetrahedra[a].vertices,DualVolumeRegion::transition};
    const DualVolumeTet right{result.tetrahedra[b].vertices,DualVolumeRegion::transition};
    if(dual_tets_strictly_overlap(view,left,right))++result.strict_overlaps;
  }
  return result;
}

std::uint64_t fixed_bridge_hash(const FixedInterfaceBridge& bridge) {
  std::vector<TetKey> keys;keys.reserve(bridge.tetrahedra.size());
  for(const auto& tet:bridge.tetrahedra)keys.push_back(tet_key(tet.vertices));
  std::sort(keys.begin(),keys.end());
  std::uint64_t hash=1469598103934665603ULL;
  for(const auto& key:keys)for(const auto id:key) { hash^=id;hash*=1099511628211ULL; }
  return hash;
}

bool exact_regular_core_reconstruction(const FixedInterfaceBridge& bridge,const SandwichConfig& config) {
  for(const auto& [id,p]:bridge.vertices)if((id&kGridIdTag)==kGridIdTag) {
    const auto i=static_cast<unsigned int>((id&~kGridIdTag)>>32U);
    const auto j=static_cast<unsigned int>(id&0xffffffffU);
    const auto cell=(static_cast<std::uint64_t>(i)*config.resolution+j)*config.resolution;
    const auto expected=regular_core_top_position(cell,config.resolution);
    if(std::bit_cast<std::uint64_t>(p.x)!=std::bit_cast<std::uint64_t>(expected.x)||
       std::bit_cast<std::uint64_t>(p.y)!=std::bit_cast<std::uint64_t>(expected.y)||
       std::bit_cast<std::uint64_t>(p.z)!=std::bit_cast<std::uint64_t>(expected.z))return false;
  }
  return true;
}

struct BridgeResult {
  FixedInterfaceBridge bridge;
  bool collar_qualified{};
  bool exact_regular_positions{};
  bool rejected_for_topological_collapse{};
  bool zero_thickness_control_rejected{};
  bool boundary_cavity_control_rejected{};
  bool reversed_input_deterministic{};
  bool remote_growth_bounded{};
  std::size_t source_cell_equivalents{};
};

BridgeResult run_bridge(const SandwichConfig& config) {
  const auto source=dual_contour_surface(config,0U,2U*config.resolution);
  const auto collar=run_probe(config);
  const auto bridge=build_fixed_interface(config,collar.selected,source);
  // The one normal front is retained as a prerequisite.  The new failure is a
  // later interface-collapse failure, not a regression in the collar.
  const auto zero=make_candidate(config,source,0.0);
  // A real integrity negative control: removing one retained collar tet opens
  // an unlisted cavity face.  The exact-boundary audit must reject it rather
  // than accepting an arbitrary singly-used face as artificial closure.
  auto punctured=collar.selected.collar;
  punctured.tets.pop_back();
  const bool cavity_rejected=!audit_collar(config,source,punctured).valid();
  auto reversed=source;std::reverse(reversed.triangles.begin(),reversed.triangles.end());
  // The bridge's canonical keys cannot be permitted to depend on the input
  // triangle traversal.  Use the same already-selected collar because its
  // own run_probe has separately qualified reversed-front selection.
  const auto reversed_bridge=build_fixed_interface(config,collar.selected,reversed);
  const bool deterministic=fixed_bridge_hash(bridge)==fixed_bridge_hash(reversed_bridge)&&
      bridge.collapsed_grid_triangles==reversed_bridge.collapsed_grid_triangles&&
      bridge.degenerate_bridge_tetrahedra==reversed_bridge.degenerate_bridge_tetrahedra&&
      bridge.same_side_faces==reversed_bridge.same_side_faces&&bridge.strict_overlaps==reversed_bridge.strict_overlaps;
  // Only the owned source cells and their one-cell vertex halo are required
  // for a local request.  Growing an unrelated positive-x world cannot grow
  // that request's source allocation.  This does not claim that the rejected
  // bridge has solved chunk curtains.
  bool remote_growth=true;
  const auto local=dual_contour_surface(config,0U,config.resolution,false,2U*config.resolution);
  for(const unsigned int multiplier:{4U,8U}) {
    const auto grown=dual_contour_surface(config,0U,config.resolution,false,multiplier*config.resolution);
    remote_growth=remote_growth&&grown.requested_cells==local.requested_cells&&grown.halo_cells==local.halo_cells;
  }
  const bool rejected=bridge.collapsed_grid_triangles>0U&&bridge.degenerate_bridge_tetrahedra>0U&&
      bridge.duplicate_tetrahedra>0U&&bridge.frozen_faces_exact;
  return {bridge,collar.selected.accepted&&collar.deterministic,
          exact_regular_core_reconstruction(bridge,config),rejected,!zero.accepted,cavity_rejected,deterministic,remote_growth,
          source.requested_cells+source.halo_cells};
}

SandwichConfig bridge_fixture(const std::string& fixture) { return fixture_config(fixture); }

int bridge_main(const std::string& fixture) {
  const auto config=bridge_fixture(fixture);
  const auto start=std::chrono::steady_clock::now();
  const auto result=run_bridge(config);
  const auto elapsed=std::chrono::duration<double,std::milli>{std::chrono::steady_clock::now()-start}.count();
  const auto& b=result.bridge;
  const bool passed_rejection=result.collar_qualified&&result.exact_regular_positions&&
      result.rejected_for_topological_collapse&&result.zero_thickness_control_rejected&&
      result.boundary_cavity_control_rejected&&
      result.reversed_input_deterministic&&result.remote_growth_bounded;
  std::cout<<std::setprecision(17)
    <<"{\"probe\":\"dc_two_front_fixed_core_interface/v1\",\"fixture\":\""<<fixture<<"\","
    <<"\"contract\":{\"outer_front\":\"exact_frozen_dc\",\"collar\":\"qualified_normal_offset\","
      "\"core_top\":\"globally_named_shared_regular_nodes\",\"result\":\"rejected_before_core_emit\"},"
    <<"\"caps\":{\"source_cell_equivalents\":"<<result.source_cell_equivalents
    <<",\"offset_candidates\":4,\"bridge_triangles\":"<<b.required_grid_triangles
    <<",\"bridge_tets\":"<<(b.tetrahedra.size()-b.collar.tets.size())
    <<",\"regular_nodes\":"<<b.regular_nodes
    <<",\"temporary_sites_per_triangle\":3,\"retries\":0,\"retained_coordinate_bytes\":"<<(b.vertices.size()*3U*sizeof(double))<<"},"
    <<"\"invariants\":{\"frozen_faces_exact\":"<<(b.frozen_faces_exact?"true":"false")
    <<",\"exact_regular_core_positions\":"<<(result.exact_regular_positions?"true":"false")
    <<",\"collar_qualified\":"<<(result.collar_qualified?"true":"false")
    <<",\"zero_thickness_rejected\":"<<(result.zero_thickness_control_rejected?"true":"false")
    <<",\"boundary_cavity_rejected\":"<<(result.boundary_cavity_control_rejected?"true":"false")
    <<",\"reversed_input_deterministic\":"<<(result.reversed_input_deterministic?"true":"false")
    <<",\"remote_growth_source_bound\":"<<(result.remote_growth_bounded?"true":"false")<<"},"
    <<"\"rejection\":{\"collapsed_required_core_faces\":"<<b.collapsed_grid_triangles
    <<",\"degenerate_bridge_tets\":"<<b.degenerate_bridge_tetrahedra
    <<",\"duplicate_tets\":"<<b.duplicate_tetrahedra<<",\"same_side_faces\":"<<b.same_side_faces
    <<",\"strict_overlaps\":"<<b.strict_overlaps<<",\"topological_collapse_reproduced\":"<<(result.rejected_for_topological_collapse?"true":"false")<<"},"
    <<"\"elapsed_ms\":"<<elapsed<<",\"qualified_complete_sandwich\":false,\"rejection_control_passed\":"<<(passed_rejection?"true":"false")<<"}\n";
  return passed_rejection?0:1;
}
} // namespace

#ifdef TWO_FRONT_CORE_BRIDGE_PROBE_TEST
int two_front_core_bridge_probe_main(const char* fixture) { return bridge_main(fixture); }
#else
int main(int argc,char** argv) {
  try { return bridge_main(argc>1?argv[1]:"n8"); }
  catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 2; }
}
#endif
