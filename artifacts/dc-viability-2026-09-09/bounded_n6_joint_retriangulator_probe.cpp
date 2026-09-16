// The first bounded in-process repair of the N6 collapsed bridge.  The
// direct-prism rejection is caused by two lower names coalescing, not by a
// forbidden frozen surface.  This probe replaces each such prism by the
// deterministic tetrahedralization of its five-vertex quotient polyhedron.
// It deliberately audits the four repairs together: independently repairing
// each quotient is not a valid transition if adjacent repairs duplicate a tet
// or leave a cavity.  That is the exact local condition this probe measures.
#define BOUNDED_N6_JOINT_TRANSITION_TEST
#include "bounded_n6_joint_transition_probe.cpp"
#undef BOUNDED_N6_JOINT_TRANSITION_TEST

#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>

namespace {
using namespace tetra::probes;
constexpr std::uint64_t kQuotientBridgeTag=0x7100000000000000ULL;

struct QuotientAttempt {
  std::size_t collapsed_prisms{};
  std::size_t quotient_tets{};
  std::size_t nonpositive{};
  std::size_t duplicate_tets{};
  std::size_t nonmanifold_faces{};
  std::size_t same_side_faces{};
  std::size_t strict_overlaps{};
  bool frozen_dc_exact{};
  bool retained_core_exact{};
  bool deterministic{};
  std::size_t work_items{};
  std::size_t retained_bytes{};
  std::size_t temporary_bytes{};
};

std::uint64_t quotient_node(std::uint64_t cell,unsigned n) {
  const auto c=direct_column(cell,n);
  return kQuotientBridgeTag|(static_cast<std::uint64_t>(c[0])<<32U)|c[1];
}

void append_oriented(QuotientAttempt& result,std::map<std::uint64_t,Vec3>& vertices,
                     std::vector<std::array<std::uint64_t,4>>& tets,
                     std::array<std::uint64_t,4> tet) {
  const auto six=signed_six_volume(vertices.at(tet[0]),vertices.at(tet[1]),vertices.at(tet[2]),vertices.at(tet[3]));
  if(six<0.0)std::swap(tet[1],tet[2]);
  if(std::abs(six)<=1e-13)++result.nonpositive;
  tets.push_back(tet);
}

// A quotient prism has one repeated lower node P and one other lower node Q.
// Its three tetrahedra are a deterministic fan.  It preserves the top
// triangle and the quotient's exterior faces exactly; no visible DC point is
// moved or split.
void append_quotient(QuotientAttempt& result,std::map<std::uint64_t,Vec3>& vertices,
                     std::vector<std::array<std::uint64_t,4>>& tets,
                     const std::array<std::uint64_t,3>& top,
                     const std::array<std::uint64_t,3>& bottom) {
  unsigned a=3U,b=3U,c=3U;
  for(unsigned i=0U;i<3U;++i)for(unsigned j=i+1U;j<3U;++j)
    if(bottom[i]==bottom[j]) { a=i;b=j; }
  if(a==3U)throw std::logic_error("quotient requested for noncollapsed prism");
  for(unsigned i=0U;i<3U;++i)if(i!=a&&i!=b)c=i;
  if(c==3U)throw std::logic_error("all lower quotient nodes coincided");
  append_oriented(result,vertices,tets,{{top[0],top[1],top[2],bottom[a]}});
  append_oriented(result,vertices,tets,{{top[a],top[c],bottom[a],bottom[c]}});
  append_oriented(result,vertices,tets,{{top[b],top[c],bottom[a],bottom[c]}});
  ++result.collapsed_prisms;result.quotient_tets+=3U;
}

QuotientAttempt build_quotient_attempt(const SandwichConfig& config,bool reverse) {
  auto surface=dual_contour_surface(config,0U,2U*config.resolution);
  if(reverse)std::reverse(surface.triangles.begin(),surface.triangles.end());
  const auto candidate=make_candidate(config,surface,kCanonicalOffsetInCells/static_cast<double>(config.resolution));
  QuotientAttempt result;result.frozen_dc_exact=candidate.accepted;
  const auto core=conservative_core(config);result.retained_core_exact=!core.empty();
  std::map<std::uint64_t,Vec3> vertices=candidate.collar.vertices;
  std::vector<std::array<std::uint64_t,4>> tets;
  for(const auto& tet:candidate.collar.tets)tets.push_back(tet.vertices);
  // The conservative core is carried verbatim even in this rejected attempt.
  // Its vertices use the existing exact source IDs/coordinates, so a local
  // bridge may not conceal a core alteration by omitting it from the audit.
  for(const auto& tet:core) {
    std::array<std::uint64_t,4> mapped{};
    for(unsigned i=0U;i<4U;++i) {
      mapped[i]=core_id(tet[i]);
      vertices.emplace(mapped[i],cartesian_lattice_position({KeyKind::lattice,tet[i]},config.resolution));
    }
    tets.push_back(mapped);
  }
  for(const auto& tri:surface.triangles) {
    auto cells=tri.vertices;std::sort(cells.begin(),cells.end());
    const std::array<std::uint64_t,3> top{{inner_id(cells[0]),inner_id(cells[1]),inner_id(cells[2])}};
    const std::array<std::uint64_t,3> bottom{{quotient_node(cells[0],config.resolution),quotient_node(cells[1],config.resolution),quotient_node(cells[2],config.resolution)}};
    for(unsigned i=0U;i<3U;++i)vertices.emplace(bottom[i],regular_dual_grid_position(cells[i],1U,config.resolution/2U-1U,config.resolution));
    const bool collapsed=bottom[0]==bottom[1]||bottom[1]==bottom[2]||bottom[0]==bottom[2];
    if(collapsed)append_quotient(result,vertices,tets,top,bottom);
    else for(const auto tet:std::array<std::array<std::uint64_t,4>,3>{{{{top[0],top[1],top[2],bottom[0]}},{{top[1],top[2],bottom[0],bottom[1]}},{{top[2],bottom[0],bottom[1],bottom[2]}}}})
      append_oriented(result,vertices,tets,tet);
  }
  std::set<std::array<std::uint64_t,4>> keys;
  struct Use { std::uint64_t opposite{}; };
  std::map<std::array<std::uint64_t,3>,std::vector<Use>> faces;
  DualVolumeBuild overlap_view;overlap_view.vertices=vertices;
  for(const auto& tet:tets) {
    auto key=tet;std::sort(key.begin(),key.end());if(!keys.insert(key).second)++result.duplicate_tets;
    for(unsigned i=0U;i<4U;++i) {
      const auto f=tet_faces[i];
      auto face=std::array<std::uint64_t,3>{{tet[f[0]],tet[f[1]],tet[f[2]]}};std::sort(face.begin(),face.end());
      faces[face].push_back({tet[i]});
    }
  }
  for(const auto& [face,uses]:faces) {
    if(uses.size()>2U)++result.nonmanifold_faces;
    if(uses.size()==2U) {
      const auto& p=vertices.at(face[0]);const auto normal=cross(vertices.at(face[1])-p,vertices.at(face[2])-p);
      if(dot(normal,vertices.at(uses[0].opposite)-p)*dot(normal,vertices.at(uses[1].opposite)-p)>=0.0)++result.same_side_faces;
    }
  }
  for(std::size_t i=0U;i<tets.size();++i)for(std::size_t j=i+1U;j<tets.size();++j)
    if(dual_tets_strictly_overlap(overlap_view,{tets[i],DualVolumeRegion::transition},{tets[j],DualVolumeRegion::transition}))++result.strict_overlaps;
  result.work_items=surface.triangles.size()+tets.size()+faces.size();
  result.retained_bytes=vertices.size()*sizeof(std::pair<const std::uint64_t,Vec3>)+tets.size()*sizeof(tets.front());
  result.temporary_bytes=keys.size()*sizeof(*keys.begin())+faces.size()*sizeof(*faces.begin());
  return result;
}

int retriangulator_run() {
  const auto start=std::chrono::steady_clock::now();
  auto forward=build_quotient_attempt(fixture_config("n6"),false);
  const auto reversed=build_quotient_attempt(fixture_config("n6"),true);
  forward.deterministic=forward.collapsed_prisms==reversed.collapsed_prisms&&forward.quotient_tets==reversed.quotient_tets&&
      forward.nonpositive==reversed.nonpositive&&forward.duplicate_tets==reversed.duplicate_tets&&
      forward.nonmanifold_faces==reversed.nonmanifold_faces&&forward.same_side_faces==reversed.same_side_faces&&forward.strict_overlaps==reversed.strict_overlaps;
  const bool rejected=forward.frozen_dc_exact&&forward.retained_core_exact&&forward.deterministic&&forward.collapsed_prisms==4U&&
      forward.nonpositive==0U&&forward.duplicate_tets>0U;
  std::cout<<std::setprecision(17)<<"{\"probe\":\"dc_bounded_n6_joint_retriangulator/v1\",\"fixture\":\"n6\","
    <<"\"contract\":{\"immutable\":[\"visible_dc_triangles\",\"finite_fixture_boundary\",\"exact_retained_core\"],\"internal_front\":\"rebuildable\"},"
    <<"\"quotient_repair\":{\"collapsed_prisms\":"<<forward.collapsed_prisms<<",\"replacement_tets\":"<<forward.quotient_tets<<",\"nonpositive_tets\":"<<forward.nonpositive<<",\"duplicate_tets\":"<<forward.duplicate_tets<<",\"nonmanifold_faces\":"<<forward.nonmanifold_faces<<",\"same_side_faces\":"<<forward.same_side_faces<<",\"strict_overlaps\":"<<forward.strict_overlaps<<"},"
    <<"\"invariants\":{\"frozen_dc_exact\":"<<(forward.frozen_dc_exact?"true":"false")<<",\"retained_core_exact\":"<<(forward.retained_core_exact?"true":"false")<<",\"reversed_input_deterministic\":"<<(forward.deterministic?"true":"false")<<"},"
    <<"\"resources\":{\"work_items\":"<<forward.work_items<<",\"retained_bytes\":"<<forward.retained_bytes<<",\"temporary_bytes\":"<<forward.temporary_bytes<<"},"
    <<"\"qualified_complete_transition\":false,\"rejection\":{\"kind\":\"adjacent_quotient_prisms_share_duplicate_tetrahedra\",\"validated\":"<<(rejected?"true":"false")<<"},\"elapsed_ms\":"<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<"}\n";
  return rejected?0:1;
}
} // namespace

#ifdef BOUNDED_N6_JOINT_RETRIANGULATOR_TEST
int bounded_n6_joint_retriangulator_main() { return retriangulator_run(); }
#else
int main() { try{return retriangulator_run();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;} }
#endif
