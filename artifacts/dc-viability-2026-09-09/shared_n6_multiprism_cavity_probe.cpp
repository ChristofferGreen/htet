// A single-owner follow-up to bounded_n6_joint_retriangulator_probe.cpp.
//
// The four collapsed N6 bridge prisms are first represented only as a
// *topological union*: their quotient-tet faces are cancelled before any new
// tet is emitted.  A candidate repair is then one shared-centre fan of that
// union boundary.  This deliberately differs from emitting four quotient
// fans and hoping their interior faces happen to agree.
#define BOUNDED_N6_JOINT_RETRIANGULATOR_TEST
#include "bounded_n6_joint_retriangulator_probe.cpp"
#undef BOUNDED_N6_JOINT_RETRIANGULATOR_TEST

#include <chrono>
#include <iomanip>
#include <iostream>

namespace {
using namespace tetra::probes;
constexpr std::uint64_t kSharedCavityCentre=0x7200000000000000ULL;
using Face=std::array<std::uint64_t,3>;

struct SharedCavityAttempt {
  // Retained solely for a subsequent interface-aware cavity experiment.  The
  // fan remains a rejected regular-column proxy; exposing its exact faces
  // avoids reconstructing or silently changing that witness downstream.
  std::vector<std::array<std::uint64_t,4>> cavity_tets;
  std::size_t collapsed_prisms{};
  std::size_t source_quotient_tets{};
  std::size_t cavity_boundary_faces{};
  std::size_t boundary_nonmanifold_edges{};
  std::size_t emitted_tets{};
  std::size_t nonpositive_tets{};
  std::size_t duplicate_tets{};
  std::size_t nonmanifold_faces{};
  std::size_t same_side_faces{};
  std::size_t strict_overlaps{};
  bool cavity_boundary_closed{};
  bool local_s4{};
  double local_minimum_mean_ratio{};
  double local_minimum_dihedral_degrees{};
  std::size_t unmatched_retained_core_faces{};
  std::size_t unmatched_bridge_lower_faces{};
  bool frozen_dc_exact{};
  bool retained_core_exact{};
  bool deterministic{};
  std::size_t work_items{};
  std::size_t retained_bytes{};
  std::size_t temporary_bytes{};
};

Face shared_face_key(Face face) { std::sort(face.begin(),face.end());return face; }

void append_positive(std::map<std::uint64_t,Vec3>& vertices,
                     std::vector<std::array<std::uint64_t,4>>& tets,
                     std::array<std::uint64_t,4> tet,std::size_t& nonpositive) {
  const auto six=signed_six_volume(vertices.at(tet[0]),vertices.at(tet[1]),
                                   vertices.at(tet[2]),vertices.at(tet[3]));
  if(six<0.0)std::swap(tet[1],tet[2]);
  if(std::abs(six)<=1.0e-13)++nonpositive;
  tets.push_back(tet);
}

SharedCavityAttempt build_shared_cavity_attempt(const SandwichConfig& config,bool reverse) {
  auto surface=dual_contour_surface(config,0U,2U*config.resolution);
  if(reverse)std::reverse(surface.triangles.begin(),surface.triangles.end());
  const auto candidate=make_candidate(config,surface,kCanonicalOffsetInCells/static_cast<double>(config.resolution));
  SharedCavityAttempt result;result.frozen_dc_exact=candidate.accepted;
  const auto core=conservative_core(config);result.retained_core_exact=!core.empty();
  std::map<std::uint64_t,Vec3> vertices=candidate.collar.vertices;

  // These are scratch tets used solely to derive the boundary of the *one*
  // cavity.  They are never joined to the output mesh.
  std::vector<std::array<std::uint64_t,4>> quotient_cells;
  for(const auto tri:surface.triangles) {
    auto cells=tri.vertices;std::sort(cells.begin(),cells.end());
    const std::array<std::uint64_t,3> top{{inner_id(cells[0]),inner_id(cells[1]),inner_id(cells[2])}};
    const std::array<std::uint64_t,3> bottom{{quotient_node(cells[0],config.resolution),quotient_node(cells[1],config.resolution),quotient_node(cells[2],config.resolution)}};
    for(unsigned i=0U;i<3U;++i)vertices.emplace(bottom[i],regular_dual_grid_position(cells[i],1U,config.resolution/2U-1U,config.resolution));
    if(bottom[0]!=bottom[1]&&bottom[1]!=bottom[2]&&bottom[0]!=bottom[2])continue;
    unsigned a=3U,b=3U,c=3U;
    for(unsigned i=0U;i<3U;++i)for(unsigned j=i+1U;j<3U;++j)if(bottom[i]==bottom[j]) {a=i;b=j;}
    for(unsigned i=0U;i<3U;++i)if(i!=a&&i!=b)c=i;
    if(c==3U)throw std::logic_error("invalid collapsed N6 quotient");
    quotient_cells.push_back({{top[0],top[1],top[2],bottom[a]}});
    quotient_cells.push_back({{top[a],top[c],bottom[a],bottom[c]}});
    quotient_cells.push_back({{top[b],top[c],bottom[a],bottom[c]}});
    ++result.collapsed_prisms;
  }
  result.source_quotient_tets=quotient_cells.size();

  std::map<Face,std::vector<std::array<std::uint64_t,4>>> boundary_uses;
  for(auto tet:quotient_cells) {
    const auto six=signed_six_volume(vertices.at(tet[0]),vertices.at(tet[1]),vertices.at(tet[2]),vertices.at(tet[3]));
    if(six<0.0)std::swap(tet[1],tet[2]);
    for(const auto local:tet_faces) {
      const Face face{{tet[local[0]],tet[local[1]],tet[local[2]]}};
      boundary_uses[shared_face_key(face)].push_back(tet);
    }
  }
  std::vector<Face> boundary;
  for(const auto& [face,uses]:boundary_uses)if(uses.size()==1U)boundary.push_back(face);
  result.cavity_boundary_faces=boundary.size();
  std::map<std::array<std::uint64_t,2>,std::size_t> edge_uses;
  for(const auto& face:boundary)for(unsigned edge=0U;edge<3U;++edge) {
    auto a=face[edge],b=face[(edge+1U)%3U];if(b<a)std::swap(a,b);++edge_uses[{{a,b}}];
  }
  for(const auto& [edge,uses]:edge_uses) { static_cast<void>(edge);if(uses!=2U)++result.boundary_nonmanifold_edges; }
  result.cavity_boundary_closed=!boundary.empty()&&result.boundary_nonmanifold_edges==0U;

  // One node has one stable cavity identity; it is intentionally not a
  // triangle-local centroid.  The fan is emitted even when its boundary
  // rejects so the exact failed complex remains inspectable.
  std::set<std::uint64_t> boundary_vertices;
  Vec3 centre{};for(const auto& face:boundary)for(const auto id:face)if(boundary_vertices.insert(id).second)centre=centre+vertices.at(id);
  if(!boundary_vertices.empty())vertices.emplace(kSharedCavityCentre,centre/static_cast<double>(boundary_vertices.size()));
  std::vector<std::array<std::uint64_t,4>> output;
  for(const auto& face:boundary)append_positive(vertices,output,{{kSharedCavityCentre,face[0],face[1],face[2]}},result.nonpositive_tets);
  result.emitted_tets=output.size();
  result.cavity_tets=output;
  std::set<std::array<std::uint64_t,4>> keys;
  std::map<Face,std::vector<std::uint64_t>> faces;
  DualVolumeBuild overlap_view;overlap_view.vertices=vertices;
  for(const auto& tet:output) {
    auto key=tet;std::sort(key.begin(),key.end());if(!keys.insert(key).second)++result.duplicate_tets;
    for(unsigned i=0U;i<4U;++i) {
      const auto local=tet_faces[i];
      faces[shared_face_key({{tet[local[0]],tet[local[1]],tet[local[2]]}})].push_back(tet[i]);
    }
  }
  for(const auto& [face,uses]:faces) {
    if(uses.size()>2U)++result.nonmanifold_faces;
    if(uses.size()==2U) {
      const auto& p=vertices.at(face[0]);const auto normal=cross(vertices.at(face[1])-p,vertices.at(face[2])-p);
      if(dot(normal,vertices.at(uses[0])-p)*dot(normal,vertices.at(uses[1])-p)>=0.0)++result.same_side_faces;
    }
  }
  for(std::size_t i=0U;i<output.size();++i)for(std::size_t j=i+1U;j<output.size();++j)
    if(dual_tets_strictly_overlap(overlap_view,{output[i],DualVolumeRegion::transition},{output[j],DualVolumeRegion::transition}))++result.strict_overlaps;
  DualVolumeBuild local_quality;local_quality.vertices=vertices;
  for(const auto& tet:output)add_dual_volume_tet(local_quality,tet,DualVolumeRegion::transition);
  const auto quality=evaluate_dual_volume_quality(local_quality);
  result.local_s4=quality.diagnostic_thresholds_met;
  result.local_minimum_mean_ratio=quality.minimum_mean_ratio;
  result.local_minimum_dihedral_degrees=quality.minimum_dihedral_degrees;

  // A local cavity must also meet the actual retained-core front; sharing a
  // regular-column coordinate is not enough.  Assemble the surrounding N6
  // pieces only to count this interface mismatch.  This is deliberately not
  // presented as a closed-volume audit while it remains unmatched.
  std::map<Face,std::size_t> assembled_faces;
  const auto count_faces=[&](const std::array<std::uint64_t,4>& tet) {
    for(const auto local:tet_faces)++assembled_faces[shared_face_key({{tet[local[0]],tet[local[1]],tet[local[2]]}})];
  };
  for(const auto& tet:candidate.collar.tets)count_faces(tet.vertices);
  for(const auto& tet:core) {
    std::array<std::uint64_t,4> mapped{};
    for(unsigned i=0U;i<4U;++i)mapped[i]=core_id(tet[i]);
    count_faces(mapped);
  }
  for(const auto& tri:surface.triangles) {
    auto cells=tri.vertices;std::sort(cells.begin(),cells.end());
    const std::array<std::uint64_t,3> top{{inner_id(cells[0]),inner_id(cells[1]),inner_id(cells[2])}};
    const std::array<std::uint64_t,3> bottom{{quotient_node(cells[0],config.resolution),quotient_node(cells[1],config.resolution),quotient_node(cells[2],config.resolution)}};
    if(bottom[0]==bottom[1]||bottom[1]==bottom[2]||bottom[0]==bottom[2])continue;
    for(const auto tet:std::array<std::array<std::uint64_t,4>,3>{{{{top[0],top[1],top[2],bottom[0]}},{{top[1],top[2],bottom[0],bottom[1]}},{{top[2],bottom[0],bottom[1],bottom[2]}}}})count_faces(tet);
  }
  for(const auto& tet:output)count_faces(tet);
  for(const auto& tet:core)for(const auto local:tet_faces) {
    const Face face{{core_id(tet[local[0]]),core_id(tet[local[1]]),core_id(tet[local[2]])}};
    if(assembled_faces.at(shared_face_key(face))==1U)++result.unmatched_retained_core_faces;
  }
  for(const auto& face:boundary)if(assembled_faces.at(shared_face_key(face))==1U)++result.unmatched_bridge_lower_faces;
  result.work_items=surface.triangles.size()+quotient_cells.size()+boundary.size()+output.size()+faces.size();
  result.retained_bytes=vertices.size()*sizeof(std::pair<const std::uint64_t,Vec3>)+output.size()*sizeof(output.front());
  result.temporary_bytes=quotient_cells.size()*sizeof(quotient_cells.front())+boundary_uses.size()*sizeof(*boundary_uses.begin())+keys.size()*sizeof(*keys.begin());
  return result;
}

int shared_cavity_run() {
  const auto start=std::chrono::steady_clock::now();
  auto forward=build_shared_cavity_attempt(fixture_config("n6"),false);
  const auto reverse=build_shared_cavity_attempt(fixture_config("n6"),true);
  forward.deterministic=forward.collapsed_prisms==reverse.collapsed_prisms&&forward.source_quotient_tets==reverse.source_quotient_tets&&forward.cavity_boundary_faces==reverse.cavity_boundary_faces&&forward.boundary_nonmanifold_edges==reverse.boundary_nonmanifold_edges&&forward.emitted_tets==reverse.emitted_tets&&forward.nonpositive_tets==reverse.nonpositive_tets&&forward.duplicate_tets==reverse.duplicate_tets&&forward.nonmanifold_faces==reverse.nonmanifold_faces&&forward.same_side_faces==reverse.same_side_faces&&forward.strict_overlaps==reverse.strict_overlaps;
  const bool rejected=forward.frozen_dc_exact&&forward.retained_core_exact&&forward.deterministic&&forward.collapsed_prisms==4U&&forward.source_quotient_tets==12U&&forward.cavity_boundary_closed&&forward.nonpositive_tets==0U&&forward.duplicate_tets==0U&&forward.nonmanifold_faces==0U&&forward.same_side_faces==0U&&forward.strict_overlaps==0U&&forward.unmatched_retained_core_faces>0U;
  std::cout<<std::setprecision(17)<<"{\"probe\":\"dc_shared_n6_multiprism_cavity/v1\",\"fixture\":\"n6\","
    <<"\"contract\":{\"immutable\":[\"visible_dc_triangles\",\"finite_fixture_boundary\",\"exact_retained_core\"],\"internal_front\":\"rebuildable\"},"
    <<"\"shared_cavity\":{\"collapsed_prisms\":"<<forward.collapsed_prisms<<",\"source_quotient_tets\":"<<forward.source_quotient_tets<<",\"boundary_faces\":"<<forward.cavity_boundary_faces<<",\"boundary_nonmanifold_edges\":"<<forward.boundary_nonmanifold_edges<<",\"boundary_closed\":"<<(forward.cavity_boundary_closed?"true":"false")<<",\"shared_centres\":1,\"emitted_tets\":"<<forward.emitted_tets<<",\"nonpositive_tets\":"<<forward.nonpositive_tets<<",\"duplicate_tets\":"<<forward.duplicate_tets<<",\"nonmanifold_faces\":"<<forward.nonmanifold_faces<<",\"same_side_faces\":"<<forward.same_side_faces<<",\"strict_overlaps\":"<<forward.strict_overlaps<<",\"s4_pass\":"<<(forward.local_s4?"true":"false")<<",\"minimum_mean_ratio\":"<<forward.local_minimum_mean_ratio<<",\"minimum_dihedral_degrees\":"<<forward.local_minimum_dihedral_degrees<<"},"
    <<"\"invariants\":{\"frozen_dc_exact\":"<<(forward.frozen_dc_exact?"true":"false")<<",\"retained_core_exact\":"<<(forward.retained_core_exact?"true":"false")<<",\"reversed_input_deterministic\":"<<(forward.deterministic?"true":"false")<<"},"
    <<"\"assembled_n6\":{\"unmatched_retained_core_faces\":"<<forward.unmatched_retained_core_faces<<",\"unmatched_shared_cavity_faces\":"<<forward.unmatched_bridge_lower_faces<<"},"
    <<"\"resources\":{\"work_items\":"<<forward.work_items<<",\"retained_bytes\":"<<forward.retained_bytes<<",\"temporary_bytes\":"<<forward.temporary_bytes<<"},"
    <<"\"qualified_complete_transition\":false,\"rejection\":{\"kind\":\"shared_cavity_is_locally_valid_but_does_not_conform_to_terraced_retained_core\",\"validated\":"<<(rejected?"true":"false")<<"},\"elapsed_ms\":"<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<"}\n";
  return rejected?0:1;
}
} // namespace

#ifdef SHARED_N6_MULTIPRISM_CAVITY_TEST
int shared_n6_multiprism_cavity_main() { return shared_cavity_run(); }
#else
int main() { try{return shared_cavity_run();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;} }
#endif
