// Whole-front eligibility check for the N6 shared-buffer/core construction.
//
// The 14-tet shared fan is a closed *volume* around the four quotient cells,
// not an open collar underside.  Before attempting an ever larger connecting
// side complex, establish whether its entire boundary and the retained core
// boundary are the two fronts of one annular gap.  They are not: the two
// positive complexes are spatially disjoint.  A surface "between" them would
// require cutting both closed components and filling their exterior, rather
// than filling a bounded collar-to-core gap.  That is a topology rejection of
// this particular buffer representation, not a rejection of joint buffering.
#define N6_CONNECTING_SIDE_COMPLEX_TEST
#include "n6_connecting_side_complex_probe.cpp"
#undef N6_CONNECTING_SIDE_COMPLEX_TEST

#include <chrono>
#include <iomanip>
#include <iostream>

namespace {
using namespace tetra::probes;
using Tet=std::array<std::uint64_t,4>;

struct DisjointFrontResult {
  std::size_t core_tets{}, buffer_tets{};
  std::size_t core_boundary_faces{}, buffer_boundary_faces{};
  std::size_t strict_cross_component_overlaps{};
  std::size_t shared_boundary_faces{};
  std::size_t work_items{}, retained_bytes{}, temporary_bytes{};
  bool core_positive{}, buffer_positive{}, deterministic{};
  bool rejected{};
};

std::vector<Tet> n6_mapped_core(const SandwichConfig& config) {
  std::vector<Tet> result;
  for(const auto& source:conservative_core(config)) {
    Tet tet{};
    for(unsigned i=0U;i<4U;++i)tet[i]=core_id(source[i]);
    result.push_back(tet);
  }
  return result;
}

DisjointFrontResult build_disjoint_front_attempt(bool reverse) {
  const auto config=fixture_config("n6");
  const auto shared=build_shared_cavity_attempt(config,reverse);
  const auto core=n6_mapped_core(config);
  auto vertices=shared_vertices(config,shared.cavity_tets);
  DisjointFrontResult result;
  result.core_tets=core.size();
  result.buffer_tets=shared.cavity_tets.size();
  result.core_boundary_faces=exterior_faces(core).size();
  result.buffer_boundary_faces=exterior_faces(shared.cavity_tets).size();
  const auto positive=[&](const std::vector<Tet>& tets) {
    for(const auto& tet:tets) if(std::abs(signed_six_volume(
        vertices.at(tet[0]),vertices.at(tet[1]),vertices.at(tet[2]),vertices.at(tet[3])))<=1.0e-13)
      return false;
    return true;
  };
  result.core_positive=positive(core);
  result.buffer_positive=positive(shared.cavity_tets);
  const auto core_boundary=exterior_faces(core);
  const auto buffer_boundary=exterior_faces(shared.cavity_tets);
  for(const auto& face:core_boundary)if(buffer_boundary.contains(face))++result.shared_boundary_faces;
  DualVolumeBuild view;view.vertices=vertices;
  for(const auto& core_tet:core)for(const auto& buffer_tet:shared.cavity_tets)
    if(dual_tets_strictly_overlap(view,{core_tet,DualVolumeRegion::core},
                                       {buffer_tet,DualVolumeRegion::transition}))
      ++result.strict_cross_component_overlaps;
  result.work_items=core.size()*shared.cavity_tets.size()+core_boundary.size()+buffer_boundary.size();
  result.retained_bytes=(core.size()+shared.cavity_tets.size())*sizeof(Tet)+
      vertices.size()*sizeof(*vertices.begin());
  result.temporary_bytes=(core_boundary.size()+buffer_boundary.size())*sizeof(Face);
  result.rejected=result.core_positive&&result.buffer_positive&&
      result.core_boundary_faces==104U&&result.buffer_boundary_faces==14U&&
      result.shared_boundary_faces==0U&&result.strict_cross_component_overlaps==0U;
  return result;
}

int disjoint_front_main() {
  const auto start=std::chrono::steady_clock::now();
  auto forward=build_disjoint_front_attempt(false);
  const auto reversed=build_disjoint_front_attempt(true);
  forward.deterministic=forward.core_tets==reversed.core_tets&&
      forward.buffer_tets==reversed.buffer_tets&&
      forward.core_boundary_faces==reversed.core_boundary_faces&&
      forward.buffer_boundary_faces==reversed.buffer_boundary_faces&&
      forward.shared_boundary_faces==reversed.shared_boundary_faces&&
      forward.strict_cross_component_overlaps==reversed.strict_cross_component_overlaps;
  forward.rejected=forward.rejected&&forward.deterministic;
  std::cout<<std::setprecision(17)
    <<"{\"probe\":\"dc_n6_disjoint_buffer_core/v1\",\"fixture\":\"n6\","
    <<"\"fronts\":{\"retained_core_tets\":"<<forward.core_tets
    <<",\"retained_core_boundary_faces\":"<<forward.core_boundary_faces
    <<",\"shared_buffer_tets\":"<<forward.buffer_tets
    <<",\"shared_buffer_boundary_faces\":"<<forward.buffer_boundary_faces
    <<",\"literal_shared_boundary_faces\":"<<forward.shared_boundary_faces<<"},"
    <<"\"geometry\":{\"core_positive\":"<<(forward.core_positive?"true":"false")
    <<",\"buffer_positive\":"<<(forward.buffer_positive?"true":"false")
    <<",\"strict_cross_component_overlaps\":"<<forward.strict_cross_component_overlaps<<"},"
    <<"\"invariants\":{\"reversed_input_deterministic\":"<<(forward.deterministic?"true":"false")<<"},"
    <<"\"resources\":{\"work_items\":"<<forward.work_items<<",\"retained_bytes\":"<<forward.retained_bytes<<",\"temporary_bytes\":"<<forward.temporary_bytes<<"},"
    <<"\"qualified_complete_transition\":false,\"rejection\":{\"kind\":\"shared_buffer_is_a_disjoint_closed_volume_not_an_annular_transition_front\",\"validated\":"<<(forward.rejected?"true":"false")<<"},\"elapsed_ms\":"<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<"}\n";
  return forward.rejected?0:1;
}
} // namespace

#ifdef N6_DISJOINT_BUFFER_CORE_TEST
int n6_disjoint_buffer_core_main() { return disjoint_front_main(); }
#else
int main() { try{return disjoint_front_main();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;} }
#endif
