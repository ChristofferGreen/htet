// Bounded scaffolded-buffer investigation.
//
// The qualified collar ends at a free, normal-offset DC front while the
// retained core begins at exact exposed Freudenthal faces.  This probe tests
// the smallest honest "common scaffold" between them: zero, one, and two
// face-adjacency rings of *unaltered* regular lattice tetrahedra around the
// actual conservative core.  The rings are finite, explicitly enumerated,
// and share the exact core faces.  A direct template may only join a collar
// face to an exposed scaffold face when their three coordinates are bitwise
// identical; it may not weld or move either front.
//
// This is deliberately a rejection experiment, not a hidden global mesher.
// It establishes a useful lower bound on the next construction: an uncut
// regular-tet scaffold cannot be the missing buffer.  A future cleaving pass
// must create conforming facets through the scaffold and audit the resulting
// bounded cells; simply adding more unchanged lattice rings cannot close the
// gap to the free collar front.

#define TERRACED_CORE_INTERFACE_PROBE_TEST
#include "terraced_core_interface_probe.cpp"
#undef TERRACED_CORE_INTERFACE_PROBE_TEST

#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>

namespace {
using namespace tetra::probes;

constexpr std::uint64_t kScaffoldTag=0x4000000000000000ULL;
using SourceTet=std::array<std::uint64_t,4>;
using SourceFace=std::array<std::uint64_t,3>;
using PositionFace=std::array<std::uint64_t,9>;

SourceTet source_tet_key(SourceTet value) {
  std::sort(value.begin(),value.end());
  return value;
}

SourceFace source_face_key(SourceFace value) {
  std::sort(value.begin(),value.end());
  return value;
}

std::uint64_t scaffold_id(std::uint64_t lattice) { return kScaffoldTag|lattice; }

PositionFace position_face_key(const std::map<std::uint64_t,Vec3>& vertices,SourceFace face) {
  std::array<std::array<std::uint64_t,3>,3> encoded{};
  for(unsigned index=0;index<3U;++index) {
    const auto& p=vertices.at(face[index]);
    encoded[index]={{std::bit_cast<std::uint64_t>(p.x),std::bit_cast<std::uint64_t>(p.y),
                     std::bit_cast<std::uint64_t>(p.z)}};
  }
  std::sort(encoded.begin(),encoded.end());
  PositionFace result{};
  for(unsigned index=0;index<3U;++index)for(unsigned axis=0;axis<3U;++axis)
    result[index*3U+axis]=encoded[index][axis];
  return result;
}

PositionFace collar_position_face_key(const Collar& collar,FaceKey face) {
  return position_face_key(collar.vertices,{{face[0],face[1],face[2]}});
}

struct SourceUniverse {
  std::set<SourceTet> tetrahedra;
  std::map<SourceFace,std::set<SourceTet>> face_users;
  std::map<std::uint64_t,Vec3> vertices;
  std::size_t source_cells{};
  bool positive{true};
};

SourceUniverse build_source_universe(const SandwichConfig& config,unsigned int x_end) {
  SourceUniverse result;
  const auto n=config.resolution;
  result.source_cells=static_cast<std::size_t>(x_end)*n*n;
  for(unsigned int i=0;i<x_end;++i)for(unsigned int j=0;j<n;++j)for(unsigned int k=0;k<n;++k) {
    std::array<VertexKey,8> cube{};
    for(unsigned bit=0;bit<8U;++bit)
      cube[bit]=lattice_key(i+(bit&1U),j+((bit>>1U)&1U),k+((bit>>2U)&1U),n);
    for(const auto permutation:cube_permutations) {
      const auto a=1U<<permutation[0],b=a|(1U<<permutation[1]);
      result.tetrahedra.insert(source_tet_key({{cube[0].payload,cube[a].payload,cube[b].payload,cube[7].payload}}));
    }
  }
  for(const auto& tet:result.tetrahedra) {
    for(const auto vertex:tet)result.vertices.emplace(scaffold_id(vertex),
        lattice_position({KeyKind::lattice,vertex},n));
    for(const auto indices:tet_faces)
      result.face_users[source_face_key({{tet[indices[0]],tet[indices[1]],tet[indices[2]]}})].insert(tet);
    const auto& a=result.vertices.at(scaffold_id(tet[0]));
    const auto& b=result.vertices.at(scaffold_id(tet[1]));
    const auto& c=result.vertices.at(scaffold_id(tet[2]));
    const auto& d=result.vertices.at(scaffold_id(tet[3]));
    result.positive=result.positive&&std::abs(signed_six_volume(a,b,c,d))>1.0e-13;
  }
  return result;
}

struct ScaffoldRing {
  std::set<SourceTet> tetrahedra;
  std::set<SourceFace> boundary_faces;
  std::size_t interface_faces{};
  std::size_t nonmanifold_faces{};
  std::size_t directly_matching_collar_faces{};
  bool all_positive{};
};

ScaffoldRing build_ring(const SourceUniverse& universe,const std::set<SourceTet>& core,
                        unsigned int rings) {
  ScaffoldRing result;
  result.tetrahedra=core;
  for(unsigned ring=0;ring<rings;++ring) {
    std::set<SourceTet> addition;
    for(const auto& tet:result.tetrahedra)for(const auto indices:tet_faces) {
      const auto face=source_face_key({{tet[indices[0]],tet[indices[1]],tet[indices[2]]}});
      for(const auto& neighbour:universe.face_users.at(face))addition.insert(neighbour);
    }
    result.tetrahedra.insert(addition.begin(),addition.end());
  }
  for(const auto& [face,users]:universe.face_users) {
    std::size_t uses{};
    for(const auto& tet:users)uses+=result.tetrahedra.contains(tet)?1U:0U;
    if(uses==1U)result.boundary_faces.insert(face);
    if(uses>2U)++result.nonmanifold_faces;
  }
  for(const auto& tet:core)for(const auto indices:tet_faces) {
    const auto face=source_face_key({{tet[indices[0]],tet[indices[1]],tet[indices[2]]}});
    const auto& users=universe.face_users.at(face);
    const auto core_users=std::count_if(users.begin(),users.end(),[&](const auto& user) { return core.contains(user); });
    if(core_users==1U)++result.interface_faces;
  }
  result.all_positive=universe.positive;
  return result;
}

bool exact_core_reconstruction(const TerracedCore& core,const SandwichConfig& config) {
  for(const auto& [id,p]:core.vertices) {
    const auto lattice=id&~kTerracedCoreTag;
    if(!exact_vec3(p,lattice_position({KeyKind::lattice,lattice},config.resolution)))return false;
  }
  return true;
}

struct BufferResult {
  std::array<ScaffoldRing,3> rings;
  std::size_t collar_faces{};
  std::size_t source_cells{};
  std::size_t source_tetrahedra{};
  bool collar_qualified{};
  bool core_exact{};
  bool flat_control_rejected{};
  bool direct_control_rejected{};
  bool local_source_bounded{};
  bool deterministic{};
  bool rejected_for_nonconforming_front{};
};

BufferResult run_scaffolded_buffer(const SandwichConfig& config) {
  const auto collar=run_probe(config);
  const auto core=build_terraced_core(config);
  const unsigned int x_end=2U*config.resolution;
  const auto universe=build_source_universe(config,x_end);
  std::set<SourceTet> selected;
  for(const auto& tet:select_retained_regular_core(config,0U,x_end))selected.insert(source_tet_key(tet));
  if(selected.size()!=core.tetrahedra.size())throw std::logic_error("terraced core and scaffold seed disagree");

  BufferResult result;
  result.collar_faces=collar.selected.collar.expected_inner.size();
  result.source_cells=universe.source_cells;
  result.source_tetrahedra=universe.tetrahedra.size();
  for(unsigned ring=0;ring<=2U;++ring) {
    result.rings[ring]=build_ring(universe,selected,ring);
    std::set<PositionFace> faces;
    for(const auto face:result.rings[ring].boundary_faces) {
      const SourceFace tagged{{scaffold_id(face[0]),scaffold_id(face[1]),scaffold_id(face[2])}};
      faces.insert(position_face_key(universe.vertices,tagged));
    }
    for(const auto face:collar.selected.collar.expected_inner)
      result.rings[ring].directly_matching_collar_faces+=faces.contains(collar_position_face_key(collar.selected.collar,face))?1U:0U;
  }
  // A proposed buffer has no emitted tets because its only permitted uncut
  // face template has no matching input face.  Calling that empty output
  // closed would hide both exposed fronts, so zero matches is the rejection.
  const auto flat=run_bridge(config);
  const auto direct=audit_direct_attachment(collar.selected,core);
  result.collar_qualified=collar.selected.accepted&&collar.deterministic;
  result.core_exact=core.exact_lattice_positions&&exact_core_reconstruction(core,config);
  result.flat_control_rejected=flat.rejected_for_topological_collapse;
  result.direct_control_rejected=direct.rejected_for_unfilled_transition;
  result.rejected_for_nonconforming_front=true;
  for(const auto& ring:result.rings)
    result.rejected_for_nonconforming_front=result.rejected_for_nonconforming_front&&
        ring.directly_matching_collar_faces==0U&&ring.all_positive&&ring.nonmanifold_faces==0U&&ring.interface_faces>0U;

  const auto request=dual_contour_chunk_request(config,0U,config.resolution,x_end);
  result.local_source_bounded=true;
  for(const unsigned multiplier:{4U,8U}) {
    const auto grown=dual_contour_chunk_request(config,0U,config.resolution,multiplier*config.resolution);
    result.local_source_bounded=result.local_source_bounded&&
        request.owned.requested_cells==grown.owned.requested_cells&&request.owned.halo_cells==grown.owned.halo_cells&&
        request.seam_dependency_cells==grown.seam_dependency_cells&&request.peak_temporary_cells==grown.peak_temporary_cells;
  }
  const auto repeat=build_source_universe(config,x_end);
  const auto repeat_ring=build_ring(repeat,selected,2U);
  result.deterministic=repeat.tetrahedra==universe.tetrahedra&&
      repeat_ring.tetrahedra==result.rings[2].tetrahedra&&repeat_ring.boundary_faces==result.rings[2].boundary_faces;
  return result;
}

int scaffolded_buffer_main(const char* fixture) {
  const auto config=bridge_fixture(fixture);
  const auto start=std::chrono::steady_clock::now();
  const auto result=run_scaffolded_buffer(config);
  const auto elapsed=std::chrono::duration<double,std::milli>{std::chrono::steady_clock::now()-start}.count();
  const bool passed=result.collar_qualified&&result.core_exact&&result.flat_control_rejected&&
      result.direct_control_rejected&&result.local_source_bounded&&result.deterministic&&
      result.rejected_for_nonconforming_front;
  std::cout<<std::setprecision(17)
    <<"{\"probe\":\"dc_scaffolded_interfront_buffer/v1\",\"fixture\":\""<<fixture<<"\","
    <<"\"contract\":{\"outer_front\":\"exact_frozen_dc\",\"collar_front\":\"qualified_normal_offset\","
      "\"core\":\"actual_conservative_freudenthal\",\"scaffold\":\"unaltered_regular_tet_face_rings\","
      "\"result\":\"rejected_before_buffer_emit\"},"
    <<"\"caps\":{\"owner_cells\":"<<config.resolution*config.resolution*config.resolution
      <<",\"vertex_halo_cells\":"<<config.resolution*config.resolution
      <<",\"seam_support_cells\":"<<2U*config.resolution*config.resolution
      <<",\"scaffold_source_cells\":"<<result.source_cells<<",\"scaffold_source_tets\":"<<result.source_tetrahedra
      <<",\"rings_tested\":3,\"candidate_sites\":0,\"templates\":1,\"template_output_tets\":0,\"retries\":0},"
    <<"\"fronts\":{\"collar_inner_faces\":"<<result.collar_faces;
  for(unsigned ring=0;ring<=2U;++ring) {
    const auto& value=result.rings[ring];
    std::cout<<",\"ring"<<ring<<"\":{\"tets\":"<<value.tetrahedra.size()
      <<",\"boundary_faces\":"<<value.boundary_faces.size()<<",\"core_interface_faces\":"<<value.interface_faces
      <<",\"matching_collar_faces\":"<<value.directly_matching_collar_faces<<"}";
  }
  std::cout<<"},\"invariants\":{\"collar_qualified\":"<<(result.collar_qualified?"true":"false")
    <<",\"exact_core_coordinates\":"<<(result.core_exact?"true":"false")
    <<",\"flat_grid_control_rejected\":"<<(result.flat_control_rejected?"true":"false")
    <<",\"direct_terraced_control_rejected\":"<<(result.direct_control_rejected?"true":"false")
    <<",\"remote_growth_source_bound\":"<<(result.local_source_bounded?"true":"false")
    <<",\"canonical_rebuild_deterministic\":"<<(result.deterministic?"true":"false")<<"},"
    <<"\"rejection\":{\"uncut_scaffold_cannot_pair_either_front\":"<<(result.rejected_for_nonconforming_front?"true":"false")
    <<",\"complete_closed_tet_volume\":false},\"elapsed_ms\":"<<elapsed
    <<",\"qualified_complete_sandwich\":false,\"rejection_control_passed\":"<<(passed?"true":"false")<<"}\n";
  return passed?0:1;
}
} // namespace

#ifdef SCAFFOLDED_INTERFRONT_BUFFER_PROBE_TEST
int scaffolded_interfront_buffer_probe_main(const char* fixture) { return scaffolded_buffer_main(fixture); }
#else
int main(int argc,char** argv) {
  try { return scaffolded_buffer_main(argc>1?argv[1]:"n8"); }
  catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 2; }
}
#endif
