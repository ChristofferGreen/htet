// Bounded structural bridge investigation.  This is deliberately the next
// step after the failed single flat plane: construct the *actual* boundary of
// a conservative material Freudenthal core, retaining every lattice address
// (i,j,k).  It then tests the smallest possible attachment grammar -- direct
// pairing of the qualified collar's inner faces to that boundary.
//
// It is a rejection control, not a hidden remesher.  A terraced core removes
// the old many-to-one (i,j) vertex merge, but its exposed triangulation is not
// the normal-offset collar's triangulation.  Pairing no faces leaves both
// fronts exterior, so emitting core plus collar would contain an unfilled
// transition volume.  A succeeding experiment must explicitly construct a
// bounded buffer whose two independently prescribed front triangulations are
// both retained.  This probe makes that obligation measurable.

#define TWO_FRONT_CORE_BRIDGE_PROBE_TEST
#include "two_front_core_bridge_probe.cpp"
#undef TWO_FRONT_CORE_BRIDGE_PROBE_TEST

#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>

namespace {
using namespace tetra::probes;

constexpr std::uint64_t kTerracedCoreTag=0x5000000000000000ULL;
using Face=std::array<std::uint64_t,3>;
using Tet=std::array<std::uint64_t,4>;

struct TerracedCore {
  std::map<std::uint64_t,Vec3> vertices;
  std::vector<Tet> tetrahedra;
  std::set<Face> exposed_faces;
  std::size_t vertical_exposed_faces{};
  std::size_t flattened_face_collapses{};
  std::size_t distinct_xy_columns_with_layers{};
  bool exact_lattice_positions{true};
};

std::uint64_t core_id(std::uint64_t lattice) { return kTerracedCoreTag|lattice; }

bool exact_terraced_vec3(const Vec3& a,const Vec3& b) {
  return std::bit_cast<std::uint64_t>(a.x)==std::bit_cast<std::uint64_t>(b.x)&&
      std::bit_cast<std::uint64_t>(a.y)==std::bit_cast<std::uint64_t>(b.y)&&
      std::bit_cast<std::uint64_t>(a.z)==std::bit_cast<std::uint64_t>(b.z);
}

TerracedCore build_terraced_core(const SandwichConfig& config) {
  TerracedCore result;
  const auto selected=select_retained_regular_core(config,0U,2U*config.resolution);
  std::map<Face,unsigned> uses;
  std::map<std::array<unsigned,2>,std::set<unsigned>> column_layers;
  for(const auto& source:selected) {
    Tet tet{};
    for(unsigned index=0;index<4U;++index) {
      tet[index]=core_id(source[index]);
      const auto expected=lattice_position({KeyKind::lattice,source[index]},config.resolution);
      const auto [it,inserted]=result.vertices.emplace(tet[index],expected);
      result.exact_lattice_positions=result.exact_lattice_positions&&(inserted||exact_terraced_vec3(it->second,expected));
      const auto address=lattice_coordinates(source[index],config.resolution);
      column_layers[{{address[0],address[1]}}].insert(address[2]);
    }
    result.tetrahedra.push_back(tet);
    for(const auto indices:tet_faces) ++uses[face_key({{tet[indices[0]],tet[indices[1]],tet[indices[2]]}})];
  }
  for(const auto& [face,count]:uses) if(count==1U) {
    result.exposed_faces.insert(face);
    std::set<std::uint64_t> z;
    std::set<std::array<unsigned,2>> flattened;
    for(const auto id:face) {
      const auto address=lattice_coordinates(id&~kTerracedCoreTag,config.resolution);
      z.insert(std::bit_cast<std::uint64_t>(result.vertices.at(id).z));
      flattened.insert({{address[0],address[1]}});
    }
    if(z.size()>1U)++result.vertical_exposed_faces;
    if(flattened.size()<3U)++result.flattened_face_collapses;
  }
  for(const auto& [column,layers]:column_layers) {
    (void)column;
    if(layers.size()>1U)++result.distinct_xy_columns_with_layers;
  }
  return result;
}

struct DirectAttachmentAudit {
  std::size_t inner_faces{};
  std::size_t core_faces{};
  std::size_t directly_paired_faces{};
  std::size_t unmatched_inner_faces{};
  std::size_t unmatched_core_faces{};
  std::size_t direct_bridge_candidates{};
  bool input_order_deterministic{};
  bool rejected_for_unfilled_transition{};
};

// Direct pairing is the only possible zero-buffer fixed template: a collar
// inner triangle would have to be literally an exposed core triangle.  IDs
// and positions are both compared so coincident aliases cannot conceal the
// old collapse issue.
DirectAttachmentAudit audit_direct_attachment(const Candidate& collar,const TerracedCore& core) {
  DirectAttachmentAudit result;
  result.inner_faces=collar.collar.expected_inner.size();
  result.core_faces=core.exposed_faces.size();
  // Position equality is bitwise by contract.  The explicit key avoids
  // relying on any tolerance or on Vec3 ordering.
  const auto position_key=[](std::array<Vec3,3> points) {
    std::array<std::array<std::uint64_t,3>,3> encoded{};
    for(unsigned index=0;index<3U;++index)
      encoded[index]={{std::bit_cast<std::uint64_t>(points[index].x),
                       std::bit_cast<std::uint64_t>(points[index].y),
                       std::bit_cast<std::uint64_t>(points[index].z)}};
    std::sort(encoded.begin(),encoded.end());
    std::array<std::uint64_t,9> key{};
    for(unsigned index=0;index<3U;++index)for(unsigned axis=0;axis<3U;++axis)
      key[index*3U+axis]=encoded[index][axis];
    return key;
  };
  std::set<std::array<std::uint64_t,9>> core_positions;
  for(const auto face:core.exposed_faces) {
    std::array<Vec3,3> points{{core.vertices.at(face[0]),core.vertices.at(face[1]),core.vertices.at(face[2])}};
    core_positions.insert(position_key(points));
  }
  for(const auto face:collar.collar.expected_inner) {
    std::array<Vec3,3> points{{collar.collar.vertices.at(face[0]),collar.collar.vertices.at(face[1]),collar.collar.vertices.at(face[2])}};
    if(core_positions.contains(position_key(points)))++result.directly_paired_faces;
  }
  result.unmatched_inner_faces=result.inner_faces-result.directly_paired_faces;
  result.unmatched_core_faces=result.core_faces-result.directly_paired_faces;
  result.direct_bridge_candidates=result.directly_paired_faces;
  result.rejected_for_unfilled_transition=result.directly_paired_faces==0U&&
      result.unmatched_inner_faces==result.inner_faces&&result.unmatched_core_faces==result.core_faces;
  return result;
}

struct TerracedProbeResult {
  TerracedCore core;
  DirectAttachmentAudit direct;
  bool collar_qualified{};
  bool flat_control_rejected{};
  bool local_source_bounded{};
};

TerracedProbeResult run_terraced_probe(const SandwichConfig& config) {
  const auto collar=run_probe(config);
  const auto core=build_terraced_core(config);
  auto direct=audit_direct_attachment(collar.selected,core);
  auto reversed=collar.selected;
  std::reverse(reversed.collar.tets.begin(),reversed.collar.tets.end());
  const auto reverse_direct=audit_direct_attachment(reversed,core);
  direct.input_order_deterministic=direct.inner_faces==reverse_direct.inner_faces&&
      direct.core_faces==reverse_direct.core_faces&&direct.directly_paired_faces==reverse_direct.directly_paired_faces&&
      direct.rejected_for_unfilled_transition==reverse_direct.rejected_for_unfilled_transition;

  // The source request follows the existing ownership policy.  Core selection
  // is a per-request finite lattice scan, never a complete DC-sheet scan.
  const auto local=dual_contour_chunk_request(config,0U,config.resolution,2U*config.resolution);
  bool bounded=true;
  for(const unsigned multiplier:{4U,8U}) {
    const auto grown=dual_contour_chunk_request(config,0U,config.resolution,multiplier*config.resolution);
    bounded=bounded&&local.owned.requested_cells==grown.owned.requested_cells&&
        local.owned.halo_cells==grown.owned.halo_cells&&local.seam_dependency_cells==grown.seam_dependency_cells&&
        local.peak_temporary_cells==grown.peak_temporary_cells;
  }
  const auto flat=run_bridge(config);
  return {core,direct,collar.selected.accepted&&collar.deterministic,
      flat.rejected_for_topological_collapse,bounded};
}

int terraced_main(const std::string& fixture) {
  const auto config=bridge_fixture(fixture);
  const auto start=std::chrono::steady_clock::now();
  const auto result=run_terraced_probe(config);
  const auto elapsed=std::chrono::duration<double,std::milli>{std::chrono::steady_clock::now()-start}.count();
  const auto& c=result.core;const auto& a=result.direct;
  const bool passed=result.collar_qualified&&c.exact_lattice_positions&&c.tetrahedra.size()>0U&&
      c.exposed_faces.size()>0U&&c.vertical_exposed_faces>0U&&c.flattened_face_collapses>0U&&
      c.distinct_xy_columns_with_layers>0U&&result.flat_control_rejected&&a.rejected_for_unfilled_transition&&
      a.input_order_deterministic&&result.local_source_bounded;
  std::cout<<std::setprecision(17)
    <<"{\"probe\":\"dc_two_front_terraced_core_interface/v1\",\"fixture\":\""<<fixture<<"\","
    <<"\"contract\":{\"outer_front\":\"exact_frozen_dc\",\"collar\":\"qualified_normal_offset\","
      "\"core\":\"complete_wholly_material_freudenthal_tets\",\"interface\":\"actual_exposed_terraced_core_faces\","
      "\"result\":\"zero_buffer_direct_attachment_rejected_before_bridge_emit\"},"
    <<"\"caps\":{\"owned_cells\":"<<config.resolution*config.resolution*config.resolution
      <<",\"halo_cells\":"<<config.resolution*config.resolution<<",\"seam_support_cells\":"<<2U*config.resolution*config.resolution
      <<",\"inner_faces\":"<<a.inner_faces<<",\"terraced_faces\":"<<a.core_faces
      <<",\"core_tets\":"<<c.tetrahedra.size()<<",\"core_sites\":"<<c.vertices.size()
      <<",\"direct_bridge_candidates\":"<<a.direct_bridge_candidates<<",\"retries\":0,\"emitted_bridge_tets\":0,"
      "\"temporary_tet_equivalents\":0,\"retained_core_tets\":"<<c.tetrahedra.size()<<"},"
    <<"\"invariants\":{\"collar_qualified\":"<<(result.collar_qualified?"true":"false")
      <<",\"exact_lattice_core_coordinates\":"<<(c.exact_lattice_positions?"true":"false")
      <<",\"distinct_full_ijk_layers_retained\":"<<(c.distinct_xy_columns_with_layers>0U?"true":"false")
      <<",\"flat_ij_control_rejected\":"<<(result.flat_control_rejected?"true":"false")
      <<",\"direct_audit_deterministic\":"<<(a.input_order_deterministic?"true":"false")
      <<",\"remote_growth_source_bound\":"<<(result.local_source_bounded?"true":"false")<<"},"
    <<"\"terrace\":{\"exposed_faces_with_multiple_z\":"<<c.vertical_exposed_faces
      <<",\"faces_collapsed_if_flattened_to_ij\":"<<c.flattened_face_collapses
      <<",\"xy_columns_with_multiple_k\":"<<c.distinct_xy_columns_with_layers<<"},"
    <<"\"rejection\":{\"directly_paired_faces\":"<<a.directly_paired_faces<<",\"unmatched_inner_faces\":"<<a.unmatched_inner_faces
      <<",\"unmatched_core_faces\":"<<a.unmatched_core_faces<<",\"unfilled_transition_detected\":"<<(a.rejected_for_unfilled_transition?"true":"false")<<"},"
    <<"\"elapsed_ms\":"<<elapsed<<",\"qualified_complete_sandwich\":false,\"rejection_control_passed\":"<<(passed?"true":"false")<<"}\n";
  return passed?0:1;
}
} // namespace

#ifdef TERRACED_CORE_INTERFACE_PROBE_TEST
int terraced_core_interface_probe_main(const char* fixture) { return terraced_main(fixture); }
#else
int main(int argc,char** argv) {
  try { return terraced_main(argc>1?argv[1]:"n8"); }
  catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 2; }
}
#endif
