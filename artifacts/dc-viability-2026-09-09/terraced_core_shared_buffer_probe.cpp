// First interface-aware shared-buffer construction attempt.
//
// This does not pretend that the regular-column shared fan reaches the
// actual retained Freudenthal core.  Instead it puts both prescribed sides in
// one explicitly owned finite boundary complex and proves the smallest
// remaining obstruction: the fan boundary and the true core boundary are two
// closed, disconnected components.  No tetrahedron may be emitted across
// that unrepresented gap.  This is a narrower and safer rejection than
// treating either front as an implicit, movable proxy.
#define SHARED_N6_MULTIPRISM_CAVITY_TEST
#include "shared_n6_multiprism_cavity_probe.cpp"
#undef SHARED_N6_MULTIPRISM_CAVITY_TEST

#include <chrono>
#include <iomanip>
#include <iostream>

namespace {
using namespace tetra::probes;
using Tet=std::array<std::uint64_t,4>;

struct InterfaceAwareAttempt {
  std::size_t visible_dc_faces{};
  std::size_t retained_core_tets{};
  std::size_t retained_core_faces{};
  std::size_t shared_buffer_tets{};
  std::size_t shared_buffer_faces{};
  std::size_t boundary_components{};
  std::size_t boundary_nonmanifold_edges{};
  std::size_t matching_interface_faces{};
  std::size_t emitted_join_tets{};
  bool visible_dc_exact{};
  bool core_exact{};
  bool component_boundaries_closed{};
  bool deterministic{};
  std::size_t work_items{};
  std::size_t retained_bytes{};
  std::size_t temporary_bytes{};
};

// Add every one-use face of a tet complex.  The caller deliberately supplies
// a single complex only; this makes it impossible to hide a non-manifold face
// by cancelling it against a different region.
std::set<Face> exterior_faces(const std::vector<Tet>& tets) {
  std::map<Face,unsigned> uses;
  for(const auto& tet:tets) for(const auto local:tet_faces)
    ++uses[shared_face_key({{tet[local[0]],tet[local[1]],tet[local[2]]}})];
  std::set<Face> result;
  for(const auto& [face,count]:uses)if(count==1U)result.insert(face);
  return result;
}

InterfaceAwareAttempt build_interface_aware_attempt(bool reverse) {
  const auto config=fixture_config("n6");
  const auto shared=build_shared_cavity_attempt(config,reverse);
  InterfaceAwareAttempt result;
  result.visible_dc_exact=shared.frozen_dc_exact;
  result.core_exact=shared.retained_core_exact;
  result.visible_dc_faces=dual_contour_surface(config,0U,2U*config.resolution).triangles.size();
  result.retained_core_tets=conservative_core(config).size();
  result.shared_buffer_tets=shared.cavity_tets.size();

  // The lower side is the actual immutable core mesh—not a flattened column
  // alias.  It is retained as the second prescribed component of the same
  // finite boundary request.
  std::vector<Tet> core_tets;
  for(const auto& source:conservative_core(config)) {
    Tet mapped{};
    for(unsigned i=0U;i<4U;++i)mapped[i]=core_id(source[i]);
    core_tets.push_back(mapped);
  }
  const auto core_boundary=exterior_faces(core_tets);
  const auto buffer_boundary=exterior_faces(shared.cavity_tets);
  result.retained_core_faces=core_boundary.size();
  result.shared_buffer_faces=buffer_boundary.size();
  for(const auto& face:core_boundary)
    result.matching_interface_faces+=buffer_boundary.contains(face)?1U:0U;

  // Count components over literal prescribed-face identities.  Different IDs
  // are intentional: position coincidence is not an interface match.
  std::map<std::uint64_t,std::set<std::uint64_t>> graph;
  for(const auto& face:core_boundary) for(unsigned i=0U;i<3U;++i)
    graph[face[i]].insert(face[(i+1U)%3U]);
  for(const auto& face:buffer_boundary) for(unsigned i=0U;i<3U;++i)
    graph[face[i]].insert(face[(i+1U)%3U]);
  std::set<std::uint64_t> seen;
  for(const auto& [vertex,unused]:graph) {
    (void)unused;
    if(seen.contains(vertex))continue;
    ++result.boundary_components;
    std::vector<std::uint64_t> pending{vertex};seen.insert(vertex);
    while(!pending.empty()) {
      const auto current=pending.back();pending.pop_back();
      for(const auto next:graph.at(current))if(seen.insert(next).second)pending.push_back(next);
    }
  }
  std::map<std::array<std::uint64_t,2>,unsigned> edges;
  for(const auto& face:core_boundary)for(unsigned i=0U;i<3U;++i) { auto a=face[i],b=face[(i+1U)%3U];if(b<a)std::swap(a,b);++edges[{{a,b}}]; }
  for(const auto& face:buffer_boundary)for(unsigned i=0U;i<3U;++i) { auto a=face[i],b=face[(i+1U)%3U];if(b<a)std::swap(a,b);++edges[{{a,b}}]; }
  for(const auto& [edge,count]:edges) { (void)edge;if(count!=2U)++result.boundary_nonmanifold_edges; }
  result.component_boundaries_closed=result.boundary_nonmanifold_edges==0U;

  // A zero-tet result is deliberate: connecting these closed components
  // requires a new joint cavity retriangulation, and emitting a fan here
  // would violate the exact core interface.  S4 is consequently not claimed.
  result.emitted_join_tets=0U;
  result.work_items=shared.work_items+core_tets.size()+core_boundary.size()+buffer_boundary.size()+edges.size();
  result.retained_bytes=shared.retained_bytes+core_tets.size()*sizeof(Tet)+
      (core_boundary.size()+buffer_boundary.size())*sizeof(Face);
  result.temporary_bytes=graph.size()*sizeof(*graph.begin())+edges.size()*sizeof(*edges.begin());
  return result;
}

int interface_aware_main() {
  const auto start=std::chrono::steady_clock::now();
  auto forward=build_interface_aware_attempt(false);
  const auto reversed=build_interface_aware_attempt(true);
  forward.deterministic=forward.visible_dc_faces==reversed.visible_dc_faces&&
      forward.retained_core_faces==reversed.retained_core_faces&&
      forward.shared_buffer_faces==reversed.shared_buffer_faces&&
      forward.boundary_components==reversed.boundary_components&&
      forward.boundary_nonmanifold_edges==reversed.boundary_nonmanifold_edges&&
      forward.matching_interface_faces==reversed.matching_interface_faces;
  const bool rejected=forward.visible_dc_exact&&forward.core_exact&&forward.deterministic&&
      forward.retained_core_tets==96U&&forward.retained_core_faces==104U&&
      forward.shared_buffer_tets==14U&&forward.shared_buffer_faces==14U&&
      forward.component_boundaries_closed&&forward.boundary_components==2U&&
      forward.matching_interface_faces==0U&&forward.emitted_join_tets==0U;
  std::cout<<std::setprecision(17)
    <<"{\"probe\":\"dc_n6_terraced_core_shared_buffer/v1\",\"fixture\":\"n6\","
    <<"\"contract\":{\"immutable\":[\"visible_dc_triangles\",\"finite_fixture_boundary\",\"exact_retained_core\"],\"internal_front\":\"rebuildable\"},"
    <<"\"assembly\":{\"visible_dc_faces\":"<<forward.visible_dc_faces<<",\"retained_core_tets\":"<<forward.retained_core_tets<<",\"retained_core_boundary_faces\":"<<forward.retained_core_faces<<",\"shared_buffer_tets\":"<<forward.shared_buffer_tets<<",\"shared_buffer_boundary_faces\":"<<forward.shared_buffer_faces<<",\"emitted_join_tets\":0},"
    <<"\"interface\":{\"exact_matching_faces\":"<<forward.matching_interface_faces<<",\"boundary_components\":"<<forward.boundary_components<<",\"nonmanifold_boundary_edges\":"<<forward.boundary_nonmanifold_edges<<",\"component_boundaries_closed\":"<<(forward.component_boundaries_closed?"true":"false")<<"},"
    <<"\"invariants\":{\"frozen_dc_exact\":"<<(forward.visible_dc_exact?"true":"false")<<",\"retained_core_exact\":"<<(forward.core_exact?"true":"false")<<",\"reversed_input_deterministic\":"<<(forward.deterministic?"true":"false")<<"},"
    <<"\"resources\":{\"work_items\":"<<forward.work_items<<",\"retained_bytes\":"<<forward.retained_bytes<<",\"temporary_bytes\":"<<forward.temporary_bytes<<"},"
    <<"\"qualified_complete_transition\":false,\"rejection\":{\"kind\":\"two_closed_prescribed_fronts_have_no_shared_interface_face\",\"validated\":"<<(rejected?"true":"false")<<"},\"elapsed_ms\":"<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<"}\n";
  return rejected?0:1;
}
} // namespace

#ifdef TERRACED_CORE_SHARED_BUFFER_TEST
int terraced_core_shared_buffer_main() { return interface_aware_main(); }
#else
int main() { try{return interface_aware_main();}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 2;} }
#endif
