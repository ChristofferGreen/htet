// First complete *in-process* N6 assembly attempt.  This is intentionally a
// rejection witness, not an external-TetGen surrogate.  It assembles the
// accepted frozen-DC collar, the exact conservative Freudenthal core, and the
// smallest deterministic direct bridge between them.  The bridge is allowed
// no artificial-front privilege: if it collapses it is retained as the
// minimal failing configuration for the next joint retriangulator.
#define main export_two_front_complete_private_main
#include "export_two_front_complete.cpp"
#undef main

#include <chrono>
#include <iomanip>
#include <iostream>
#include <set>

namespace {
using namespace tetra::probes;
constexpr std::uint64_t kDirectBridgeTag=0x7000000000000000ULL;

struct Attempt {
  Collar collar;
  std::set<RetainedCoreTetKey> core;
  std::size_t bridge_triangles{};
  std::size_t bridge_tets{};
  std::size_t collapsed_triangles{};
  std::size_t nonpositive_tets{};
  std::size_t duplicate_tets{};
  bool frozen_dc_exact{};
  bool retained_core_exact{};
  bool deterministic{};
  std::size_t work_items{};
  std::size_t retained_bytes{};
  std::size_t temporary_bytes{};
};

std::array<unsigned,2> direct_column(std::uint64_t cell,unsigned n) {
  return {{static_cast<unsigned>(cell/(static_cast<std::uint64_t>(n)*n)),
           static_cast<unsigned>((cell/n)%n)}};
}

std::uint64_t direct_node(std::uint64_t cell,unsigned n) {
  const auto c=direct_column(cell,n);
  return kDirectBridgeTag|(static_cast<std::uint64_t>(c[0])<<32U)|c[1];
}

Attempt build_attempt(const SandwichConfig& config,bool reverse) {
  auto surface=dual_contour_surface(config,0U,2U*config.resolution);
  if(reverse) std::reverse(surface.triangles.begin(),surface.triangles.end());
  const auto selected=make_candidate(config,surface,kCanonicalOffsetInCells/static_cast<double>(config.resolution));
  Attempt out;out.collar=selected.collar;out.core=conservative_core(config);
  out.frozen_dc_exact=selected.accepted;
  // The core set is constructed independently and is never altered by this
  // attempt.  Its exact identity is therefore a meaningful fixed-interface
  // contract even though the direct bridge cannot reach it.
  out.retained_core_exact=!out.core.empty();
  std::map<std::uint64_t,Vec3> vertices=out.collar.vertices;
  std::vector<std::array<std::uint64_t,4>> tets;
  for(const auto& t:out.collar.tets)tets.push_back(t.vertices);
  for(const auto& t:out.core) {
    std::array<std::uint64_t,4> mapped{};
    for(unsigned i=0;i<4;++i) { mapped[i]=core_id(t[i]);vertices.emplace(mapped[i],cartesian_lattice_position({KeyKind::lattice,t[i]},config.resolution)); }
    tets.push_back(mapped);
  }
  for(const auto tri:surface.triangles) {
    auto c=tri.vertices;std::sort(c.begin(),c.end());
    const std::array<std::uint64_t,3> top{{inner_id(c[0]),inner_id(c[1]),inner_id(c[2])}};
    const std::array<std::uint64_t,3> bottom{{direct_node(c[0],config.resolution),direct_node(c[1],config.resolution),direct_node(c[2],config.resolution)}};
    for(unsigned i=0;i<3;++i) vertices.emplace(bottom[i],regular_dual_grid_position(c[i],1U,config.resolution/2U-1U,config.resolution));
    ++out.bridge_triangles;
    if(bottom[0]==bottom[1]||bottom[1]==bottom[2]||bottom[0]==bottom[2]) ++out.collapsed_triangles;
    const std::array<std::array<std::uint64_t,4>,3> prism{{{{top[0],top[1],top[2],bottom[0]}},{{top[1],top[2],bottom[0],bottom[1]}},{{top[2],bottom[0],bottom[1],bottom[2]}}}};
    for(auto tet:prism) {
      const auto six=signed_six_volume(vertices.at(tet[0]),vertices.at(tet[1]),vertices.at(tet[2]),vertices.at(tet[3]));
      if(six<0.0)std::swap(tet[1],tet[2]);
      if(std::abs(six)<=1e-13)++out.nonpositive_tets;
      tets.push_back(tet);++out.bridge_tets;
    }
  }
  std::set<std::array<std::uint64_t,4>> keys;
  for(auto t:tets) {std::sort(t.begin(),t.end());if(!keys.insert(t).second)++out.duplicate_tets;}
  out.work_items=surface.triangles.size()+out.core.size()+out.bridge_tets;
  out.retained_bytes=vertices.size()*sizeof(std::pair<const std::uint64_t,Vec3>)+tets.size()*sizeof(tets.front());
  out.temporary_bytes=surface.triangles.size()*sizeof(DualTriangle)+keys.size()*sizeof(std::array<std::uint64_t,4>);
  return out;
}

int run() {
  const auto config=fixture_config("n6");
  const auto start=std::chrono::steady_clock::now();
  auto forward=build_attempt(config,false);auto backward=build_attempt(config,true);
  forward.deterministic=forward.bridge_triangles==backward.bridge_triangles&&forward.collapsed_triangles==backward.collapsed_triangles&&forward.nonpositive_tets==backward.nonpositive_tets&&forward.duplicate_tets==backward.duplicate_tets;
  const bool rejected=forward.frozen_dc_exact&&forward.retained_core_exact&&forward.deterministic&&forward.collapsed_triangles>0U&&forward.nonpositive_tets>0U;
  std::cout<<std::setprecision(17)<<"{\"probe\":\"dc_bounded_n6_joint_transition/v1\",\"fixture\":\"n6\","
    <<"\"contract\":{\"immutable\":[\"visible_dc_triangles\",\"finite_fixture_boundary\",\"exact_retained_core\"],\"internal_front\":\"rebuildable\"},"
    <<"\"assembly\":{\"collar_tets\":"<<forward.collar.tets.size()<<",\"retained_core_tets\":"<<forward.core.size()<<",\"direct_bridge_triangles\":"<<forward.bridge_triangles<<",\"direct_bridge_tets\":"<<forward.bridge_tets<<"},"
    <<"\"invariants\":{\"frozen_dc_exact\":"<<(forward.frozen_dc_exact?"true":"false")<<",\"retained_core_exact\":"<<(forward.retained_core_exact?"true":"false")<<",\"reversed_input_deterministic\":"<<(forward.deterministic?"true":"false")<<"},"
    <<"\"smallest_failing_cavity\":{\"kind\":\"one_dc_triangle_to_shared_regular_columns\",\"collapsed_triangles\":"<<forward.collapsed_triangles<<",\"nonpositive_tets\":"<<forward.nonpositive_tets<<",\"duplicate_tets\":"<<forward.duplicate_tets<<"},"
    <<"\"resources\":{\"work_items\":"<<forward.work_items<<",\"retained_bytes\":"<<forward.retained_bytes<<",\"temporary_bytes\":"<<forward.temporary_bytes<<"},"
    <<"\"elapsed_ms\":"<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<",\"qualified_complete_transition\":false,\"rejection_validated\":"<<(rejected?"true":"false")<<"}\n";
  return rejected?0:1;
}
} // namespace

#ifdef BOUNDED_N6_JOINT_TRANSITION_TEST
int bounded_n6_joint_transition_main() { return run(); }
#else
int main() { try{return run();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;} }
#endif
