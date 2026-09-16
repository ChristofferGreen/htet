// Bounded conforming-scaffold cleaving investigation.
//
// This begins with the qualified immutable two-front collar and the exact
// wholly-material Freudenthal core.  Before attempting a tetrahedral stencil,
// it measures the unavoidable contact relation between the free collar inner
// sheet and retained core tets.  A retained core tet is never eligible for a
// cleaving template: if the immutable sheet enters one, no finite outer band
// can bridge the fronts without either deleting/splitting that core tet or
// moving/splitting the sheet.  Those are deliberately excluded by the stated
// sandwich contract.
//
// The probe is intentionally an existence/rejection gate, not a visual shell
// or an external mesher.  It uses only a finite source universe (two chunk
// widths), canonical integer keys, and a conservative strict triangle/tet
// intersection predicate.  Boundary/coplanar contacts are reported
// separately and fail closed; only a strict interior intersection is used to
// reject the immutable-front + retained-core cleaving hypothesis.

#define SCAFFOLDED_INTERFRONT_BUFFER_PROBE_TEST
#include "scaffolded_interfront_buffer_probe.cpp"
#undef SCAFFOLDED_INTERFRONT_BUFFER_PROBE_TEST

#include <chrono>
#include <iomanip>
#include <iostream>
#include <set>

namespace {
using namespace tetra::probes;

constexpr double kEpsilon=1.0e-12;
using SourceTet=std::array<std::uint64_t,4>;
using EdgeKey=std::array<std::uint64_t,2>;
using CutKey=std::array<std::uint64_t,5>; // inner face ID + canonical lattice edge

enum class TriangleTetContact : std::uint8_t { none, boundary_or_degenerate, strict };

// Clip the triangle against the four oriented tetrahedron half spaces.  This
// finds face-through-face contacts that vertex-in-tet plus tet-edge tests
// miss.  A positive-area clipped polygon whose centroid is strictly inside
// the tet is strict; every lower-dimensional or tolerance-band contact fails
// closed as boundary_or_degenerate.
TriangleTetContact triangle_tet_contact(const std::array<Vec3,3>& triangle,
                                        const std::array<Vec3,4>& tet) {
  const Vec3 normal=cross(triangle[1]-triangle[0],triangle[2]-triangle[0]);
  if(length(normal)<=kEpsilon)return TriangleTetContact::boundary_or_degenerate;
  constexpr std::array<std::array<unsigned,3>,4> faces{{{{1U,2U,3U}},{{0U,3U,2U}},
                                                           {{0U,1U,3U}},{{0U,2U,1U}}}};
  std::array<double,4> orientation{};
  for(unsigned face=0U;face<4U;++face) {
    const auto f=faces[face];
    orientation[face]=signed_six_volume(tet[f[0]],tet[f[1]],tet[f[2]],tet[face]);
    if(std::abs(orientation[face])<=kEpsilon)return TriangleTetContact::boundary_or_degenerate;
  }
  auto inward_value=[&](const Vec3& point,unsigned face) {
    const auto f=faces[face];
    const double value=signed_six_volume(tet[f[0]],tet[f[1]],tet[f[2]],point);
    return orientation[face]>0.0?value:-value;
  };
  std::vector<Vec3> polygon{triangle.begin(),triangle.end()};
  for(unsigned face=0U;face<4U;++face) {
    if(polygon.empty())return TriangleTetContact::none;
    std::vector<Vec3> clipped;
    Vec3 previous=polygon.back();
    double previous_value=inward_value(previous,face);
    for(const Vec3 current:polygon) {
      const double current_value=inward_value(current,face);
      const bool previous_inside=previous_value>=0.0;
      const bool current_inside=current_value>=0.0;
      if(previous_inside!=current_inside) {
        const double denominator=previous_value-current_value;
        if(std::abs(denominator)>0.0)
          clipped.push_back(previous+(current-previous)*(previous_value/denominator));
      }
      if(current_inside)clipped.push_back(current);
      previous=current;previous_value=current_value;
    }
    polygon=std::move(clipped);
  }
  if(polygon.size()<3U)return TriangleTetContact::boundary_or_degenerate;
  Vec3 doubled_area{};
  for(std::size_t index=1U;index+1U<polygon.size();++index)
    doubled_area=doubled_area+cross(polygon[index]-polygon[0],polygon[index+1U]-polygon[0]);
  if(length(doubled_area)<=kEpsilon)return TriangleTetContact::boundary_or_degenerate;
  Vec3 centroid{};
  for(const auto point:polygon)centroid=centroid+point;
  centroid=centroid/static_cast<double>(polygon.size());
  for(unsigned face=0U;face<4U;++face)
    if(inward_value(centroid,face)<=kEpsilon)return TriangleTetContact::boundary_or_degenerate;
  return TriangleTetContact::strict;
}

// This is the deliberately moated regular core used by the existing external
// topology witness.  It is not chosen from DC coordinates: for a resolution
// N it retains only the fixed, wholly interior lattice-cell box documented in
// dc-viability-review.  That makes the inner core independently regenerable
// and reserves a finite one-or-more-cell construction band for the collar.
std::set<SourceTet> select_conservative_core(const SandwichConfig& config) {
  std::set<SourceTet> result;
  const unsigned n=config.resolution;
  for(unsigned i=2U;i<2U*n-2U;++i)for(unsigned j=2U;j<n-2U;++j)for(unsigned k=1U;k<n/2U-1U;++k) {
    std::array<VertexKey,8> cube{};
    for(unsigned bit=0;bit<8U;++bit)
      cube[bit]=lattice_key(i+(bit&1U),j+((bit>>1U)&1U),k+((bit>>2U)&1U),n);
    for(const auto permutation:cube_permutations) {
      const auto a=1U<<permutation[0],b=a|(1U<<permutation[1]);
      auto tet=source_tet_key({{cube[0].payload,cube[a].payload,cube[b].payload,cube[7].payload}});
      bool material=true;
      for(const auto vertex:tet)
        material=material&&inside(field_value(config,lattice_position({KeyKind::lattice,vertex},n)),{KeyKind::lattice,vertex});
      if(!material)throw std::logic_error("conservative core cell left material side");
      result.insert(tet);
    }
  }
  return result;
}

TerracedCore build_conservative_core(const SandwichConfig& config) {
  TerracedCore result;
  std::map<Face,unsigned> uses;
  for(const auto& source:select_conservative_core(config)) {
    Tet tet{};
    for(unsigned index=0;index<4U;++index) {
      tet[index]=core_id(source[index]);
      const auto expected=lattice_position({KeyKind::lattice,source[index]},config.resolution);
      const auto [it,inserted]=result.vertices.emplace(tet[index],expected);
      result.exact_lattice_positions=result.exact_lattice_positions&&(inserted||exact_collar_vec3(it->second,expected));
    }
    result.tetrahedra.push_back(tet);
    for(const auto indices:tet_faces)++uses[face_key({{tet[indices[0]],tet[indices[1]],tet[indices[2]]}})];
  }
  for(const auto& [face,count]:uses)if(count==1U)result.exposed_faces.insert(face);
  return result;
}

EdgeKey edge_key(std::uint64_t a,std::uint64_t b) {
  if(b<a)std::swap(a,b);
  return {{a,b}};
}

CutKey cut_key(FaceKey face,EdgeKey edge) {
  std::sort(face.begin(),face.end());
  return {{face[0],face[1],face[2],edge[0],edge[1]}};
}

// Returns a strict triangle-interior / edge-interior crossing.  The three
// non-strict classes are never snapped or epsilon-shifted: a production
// cleaver would need an explicit vertex-, edge-, and coplanar-event stencil.
// Recording them here is what makes this rejection fail closed rather than
// accidentally treating a near-degenerate intersection as a shared cut.
enum class EdgeTriangleEvent : std::uint8_t { none, strict, endpoint, boundary, coplanar };

EdgeTriangleEvent edge_triangle_event(Vec3 a,Vec3 b,const std::array<Vec3,3>& tri,Vec3& hit) {
  const Vec3 normal=cross(tri[1]-tri[0],tri[2]-tri[0]);
  if(length(normal)<=kEpsilon)return EdgeTriangleEvent::boundary;
  const double da=dot(normal,a-tri[0]),db=dot(normal,b-tri[0]);
  if(std::abs(da)<=kEpsilon&&std::abs(db)<=kEpsilon)return EdgeTriangleEvent::coplanar;
  if(std::abs(da)<=kEpsilon||std::abs(db)<=kEpsilon)return EdgeTriangleEvent::endpoint;
  if((da<0.0)==(db<0.0))return EdgeTriangleEvent::none;
  const double t=da/(da-db);
  if(t<=kEpsilon||t>=1.0-kEpsilon)return EdgeTriangleEvent::endpoint;
  hit=a+(b-a)*t;
  const double s0=dot(normal,cross(tri[1]-tri[0],hit-tri[0]));
  const double s1=dot(normal,cross(tri[2]-tri[1],hit-tri[1]));
  const double s2=dot(normal,cross(tri[0]-tri[2],hit-tri[2]));
  const bool positive=s0>kEpsilon&&s1>kEpsilon&&s2>kEpsilon;
  const bool negative=s0<-kEpsilon&&s1<-kEpsilon&&s2<-kEpsilon;
  if(positive||negative)return EdgeTriangleEvent::strict;
  if(s0>=-kEpsilon&&s1>=-kEpsilon&&s2>=-kEpsilon)return EdgeTriangleEvent::boundary;
  return EdgeTriangleEvent::none;
}

struct CleavingContactResult {
  std::size_t inner_triangles{};
  std::size_t retained_core_tets{};
  std::size_t strict_core_contacts{};
  std::size_t core_boundary_contacts{};
  std::size_t strict_band_contacts{};
  std::size_t band_boundary_contacts{};
  std::size_t affected_band_tets{};
  std::size_t strict_contacts_without_lattice_edge_cut{};
  std::size_t canonical_strict_cut_entities{};
  std::size_t endpoint_events{};
  std::size_t triangle_boundary_events{};
  std::size_t coplanar_events{};
  std::size_t candidate_band_tets{};
  std::size_t source_cells{};
  bool collar_qualified{};
  bool core_exact{};
  bool locality_bounded{};
  bool deterministic{};
  bool canonical_cut_ids_consistent{};
  bool uncut_scaffold_control_rejected{};
  bool direct_face_pair_control_rejected{};
  bool conservative_core_separated{};
};

CleavingContactResult run_conforming_scaffold_cleaving(const SandwichConfig& config) {
  const auto collar=run_probe(config);
  const auto core=build_conservative_core(config);
  const auto source=build_source_universe(config,2U*config.resolution);
  CleavingContactResult result;
  result.inner_triangles=collar.selected.collar.expected_inner.size();
  result.retained_core_tets=core.tetrahedra.size();
  result.source_cells=source.source_cells;
  result.collar_qualified=collar.selected.accepted&&collar.deterministic;
  result.core_exact=core.exact_lattice_positions&&exact_core_reconstruction(core,config);
  std::map<CutKey,Vec3> canonical_cuts;
  result.canonical_cut_ids_consistent=true;
  const auto selected=select_conservative_core(config);
  std::set<SourceTet> affected_band;
  // This is deliberately a fixed finite source universe.  The core tets are
  // classified separately to prove the moat, while only non-core tets with a
  // contact become affected cleaving candidates.
  for(const auto& tet:source.tetrahedra) {
    const bool is_core=selected.contains(tet);
    std::array<Vec3,4> positions{};
    for(unsigned i=0;i<4U;++i)
      positions[i]=lattice_position({KeyKind::lattice,tet[i]},config.resolution);
    for(const auto face:collar.selected.collar.expected_inner) {
      std::array<Vec3,3> triangle{{collar.selected.collar.vertices.at(face[0]),
                                   collar.selected.collar.vertices.at(face[1]),
                                   collar.selected.collar.vertices.at(face[2])}};
      const auto contact=triangle_tet_contact(triangle,positions);
      // Every selected core tetrahedron independently processes all six
      // lattice edges.  A shared edge therefore visits this same canonical
      // entity twice; bitwise equal hits prove the proposed identity is not
      // traversal-local.  The output remains intentionally empty because
      // the same canonical entity twice.  This records the proposed shared
      // entity for the affected band; it never authorizes cutting core tets.
      bool has_strict_lattice_edge_cut=false;
      for(const auto edge:tet_edges) {
        Vec3 hit{};
        const auto event=edge_triangle_event(positions[edge[0]],positions[edge[1]],triangle,hit);
        const auto key=cut_key(face_key({{face[0],face[1],face[2]}}),edge_key(tet[edge[0]],tet[edge[1]]));
        if(!is_core&&event==EdgeTriangleEvent::strict) {
          has_strict_lattice_edge_cut=true;
          const auto [it,inserted]=canonical_cuts.emplace(key,hit);
          if(!inserted)result.canonical_cut_ids_consistent=result.canonical_cut_ids_consistent&&exact_collar_vec3(it->second,hit);
        } else if(event==EdgeTriangleEvent::endpoint)++result.endpoint_events;
        else if(event==EdgeTriangleEvent::boundary)++result.triangle_boundary_events;
        else if(event==EdgeTriangleEvent::coplanar)++result.coplanar_events;
      }
      switch(contact) {
        case TriangleTetContact::strict:
          if(is_core)++result.strict_core_contacts;
          else {
            ++result.strict_band_contacts;
            affected_band.insert(tet);
            if(!has_strict_lattice_edge_cut)++result.strict_contacts_without_lattice_edge_cut;
          }
          break;
        case TriangleTetContact::boundary_or_degenerate:
          if(is_core)++result.core_boundary_contacts;
          else { ++result.band_boundary_contacts;affected_band.insert(tet); }
          break;
        case TriangleTetContact::none: break;
      }
    }
  }
  result.affected_band_tets=affected_band.size();
  // Revisit exactly the same bounded source in the reverse source-tet and
  // collar-face order.  This is deliberately stronger than rebuilding an
  // ordered map: it exercises the entity identity that adjacent independent
  // cell/chunk jobs would use before any global ordering is imposed.
  std::vector<SourceTet> reverse_tets(source.tetrahedra.begin(),source.tetrahedra.end());
  std::vector<FaceKey> reverse_faces(collar.selected.collar.expected_inner.begin(),collar.selected.collar.expected_inner.end());
  std::reverse(reverse_tets.begin(),reverse_tets.end());
  std::reverse(reverse_faces.begin(),reverse_faces.end());
  std::map<CutKey,Vec3> reverse_cuts;
  for(const auto& tet:reverse_tets) {
    if(selected.contains(tet))continue;
    std::array<Vec3,4> positions{};
    for(unsigned i=0;i<4U;++i)
      positions[i]=lattice_position({KeyKind::lattice,tet[i]},config.resolution);
    for(const auto face:reverse_faces) {
      const std::array<Vec3,3> triangle{{collar.selected.collar.vertices.at(face[0]),
                                         collar.selected.collar.vertices.at(face[1]),
                                         collar.selected.collar.vertices.at(face[2])}};
      for(const auto edge:tet_edges) {
        Vec3 hit{};
        if(edge_triangle_event(positions[edge[0]],positions[edge[1]],triangle,hit)!=EdgeTriangleEvent::strict)continue;
        const auto key=cut_key(face,edge_key(tet[edge[0]],tet[edge[1]]));
        const auto [it,inserted]=reverse_cuts.emplace(key,hit);
        if(!inserted)result.canonical_cut_ids_consistent=result.canonical_cut_ids_consistent&&exact_collar_vec3(it->second,hit);
      }
    }
  }
  result.deterministic=canonical_cuts.size()==reverse_cuts.size();
  for(const auto& [key,p]:canonical_cuts) {
    const auto found=reverse_cuts.find(key);
    result.deterministic=result.deterministic&&found!=reverse_cuts.end()&&exact_collar_vec3(p,found->second);
  }
  // Negative controls remain meaningful with the conservative core: keeping
  // zero, one, or two rings intact never gives an exact face to pair with the
  // free inner front.  They distinguish a real future cleaver from merely
  // declaring its untouched background a bridge.
  result.direct_face_pair_control_rejected=
      audit_direct_attachment(collar.selected,core).directly_paired_faces==0U;
  result.uncut_scaffold_control_rejected=result.direct_face_pair_control_rejected;
  for(unsigned rings=0U;rings<=2U;++rings) {
    const auto ring=build_ring(source,selected,rings);
    std::set<PositionFace> positions;
    for(const auto face:ring.boundary_faces) {
      const SourceFace tagged{{scaffold_id(face[0]),scaffold_id(face[1]),scaffold_id(face[2])}};
      positions.insert(position_face_key(source.vertices,tagged));
    }
    for(const auto face:collar.selected.collar.expected_inner)
      result.uncut_scaffold_control_rejected=result.uncut_scaffold_control_rejected&&
          !positions.contains(collar_position_face_key(collar.selected.collar,face));
  }
  // The only selected band for this experiment is the finite non-core part of
  // the already materialized two-width source.  These counts are a declared
  // cap, not a request to scan an unbounded terrain domain.
  result.candidate_band_tets=source.tetrahedra.size()-result.retained_core_tets;
  result.canonical_strict_cut_entities=canonical_cuts.size();
  const auto request=dual_contour_chunk_request(config,0U,config.resolution,2U*config.resolution);
  result.locality_bounded=true;
  for(const unsigned multiplier:{4U,8U}) {
    const auto grown=dual_contour_chunk_request(config,0U,config.resolution,multiplier*config.resolution);
    result.locality_bounded=result.locality_bounded&&request.owned.requested_cells==grown.owned.requested_cells&&
        request.owned.halo_cells==grown.owned.halo_cells&&request.seam_dependency_cells==grown.seam_dependency_cells&&
        request.peak_temporary_cells==grown.peak_temporary_cells;
  }
  const auto repeat=run_probe(config);
  result.deterministic=result.deterministic&&repeat.selected.collar.expected_inner==collar.selected.collar.expected_inner&&
      repeat.selected.collar.vertices.size()==collar.selected.collar.vertices.size();
  for(const auto& [id,p]:collar.selected.collar.vertices) {
    const auto found=repeat.selected.collar.vertices.find(id);
    result.deterministic=result.deterministic&&found!=repeat.selected.collar.vertices.end()&&exact_collar_vec3(p,found->second);
  }
  result.conservative_core_separated=result.strict_core_contacts==0U&&result.core_boundary_contacts==0U;
  return result;
}

int conforming_scaffold_cleaving_main(const char* fixture) {
  const auto config=bridge_fixture(fixture);const auto start=std::chrono::steady_clock::now();
  const auto result=run_conforming_scaffold_cleaving(config);
  const auto elapsed=std::chrono::duration<double,std::milli>{std::chrono::steady_clock::now()-start}.count();
  const bool passed=result.collar_qualified&&result.core_exact&&result.locality_bounded&&result.deterministic&&
      result.canonical_cut_ids_consistent&&result.uncut_scaffold_control_rejected&&
      result.direct_face_pair_control_rejected&&result.conservative_core_separated;
  std::cout<<std::setprecision(17)
    <<"{\"probe\":\"dc_conforming_scaffold_cleaving/v1\",\"fixture\":\""<<fixture<<"\","
    <<"\"contract\":{\"outer_front\":\"exact_frozen_dc\",\"collar_inner\":\"immutable_qualified_normal_offset\","
      "\"core\":\"exact_moated_wholly_material_freudenthal\",\"construction\":\"contact precondition; reconstruction not attempted\"},"
    <<"\"caps\":{\"owner_cells\":"<<config.resolution*config.resolution*config.resolution
      <<",\"vertex_halo_cells\":"<<config.resolution*config.resolution<<",\"seam_support_cells\":"<<2U*config.resolution*config.resolution
      <<",\"source_cells\":"<<result.source_cells<<",\"candidate_band_tets\":"<<result.candidate_band_tets<<",\"affected_band_tets\":"<<result.affected_band_tets
      <<",\"inner_triangles\":"<<result.inner_triangles<<",\"retained_core_tets\":"<<result.retained_core_tets
      <<",\"strict_lattice_edge_cut_entities\":"<<result.canonical_strict_cut_entities
      <<",\"strict_contacts_without_lattice_edge_cut\":"<<result.strict_contacts_without_lattice_edge_cut
      <<",\"templates\":0,\"retries\":0,\"emitted_bridge_tets\":0},"
    <<"\"contacts\":{\"strict_inner_triangle_to_retained_core_tet\":"<<result.strict_core_contacts
      <<",\"retained_core_boundary_events\":"<<result.core_boundary_contacts
      <<",\"strict_inner_triangle_to_band_tet\":"<<result.strict_band_contacts
      <<",\"band_triangle_tet_boundary_events\":"<<result.band_boundary_contacts
      <<",\"edge_endpoint_events\":"<<result.endpoint_events
      <<",\"triangle_boundary_events\":"<<result.triangle_boundary_events
      <<",\"coplanar_edge_events\":"<<result.coplanar_events<<"},"
    <<"\"arrangement\":{\"constructed\":false,\"claim\":\"contact counts only; no local arrangement topology inferred\"},"
    <<"\"invariants\":{\"collar_qualified\":"<<(result.collar_qualified?"true":"false")
      <<",\"exact_core_coordinates\":"<<(result.core_exact?"true":"false")
      <<",\"remote_growth_source_bound\":"<<(result.locality_bounded?"true":"false")
      <<",\"canonical_rebuild_deterministic\":"<<(result.deterministic?"true":"false")
      <<",\"canonical_cut_ids_bit_identical\":"<<(result.canonical_cut_ids_consistent?"true":"false")<<"},"
    <<"\"controls\":{\"direct_face_pairing_rejected\":"<<(result.direct_face_pair_control_rejected?"true":"false")
      <<",\"uncut_0_to_2_ring_scaffold_rejected\":"<<(result.uncut_scaffold_control_rejected?"true":"false")
      <<",\"conservative_core_strictly_separated\":"<<(result.conservative_core_separated?"true":"false")<<"},"
    <<"\"result\":{\"complete_closed_tet_volume\":false,\"qualified_complete_sandwich\":false,"
      "\"precondition_result\":\"pass; a future cleaver must reconstruct the moated band\"},\"elapsed_ms\":"<<elapsed
      <<",\"precondition_control_passed\":"<<(passed?"true":"false")<<"}\n";
  return passed?0:1;
}
} // namespace

#ifdef CONFORMING_SCAFFOLD_CLEAVING_PROBE_TEST
int dc_conforming_scaffold_cleaving_probe_main(const char* fixture) { return conforming_scaffold_cleaving_main(fixture); }
#else
int main(int argc,char** argv) {
  try { return conforming_scaffold_cleaving_main(argc>1?argv[1]:"n8"); }
  catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 2; }
}
#endif
