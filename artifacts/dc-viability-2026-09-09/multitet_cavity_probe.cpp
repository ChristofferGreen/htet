// Bounded S4 experiment: replace a closed *two-tetrahedron* cavity around the
// canonical worst element with a single interior Steiner star.  In contrast to
// steiner_cavity_probe.cpp this removes the old interface between the two
// tetrahedra; only the cavity's outer faces survive.  Thus a point may be
// chosen with knowledge of the nearby frozen/core/curtain boundary without
// moving, splitting, or deleting any of those faces.
//
// This is intentionally a finite research family, not a general mesher.  The
// selection, point stencil, retry budget, and all temporary buffers have
// explicit fixed maxima below.
#define main quality_repair_probe_legacy_main
#include "quality_repair_probe.cpp"
#undef main

#include <chrono>
#include <iomanip>

namespace {
using namespace tetra::probes;

constexpr double kMinDihedralDegrees=5.0;
constexpr double kMaxDihedralDegrees=175.0;
constexpr double kMinMeanRatio=0.01;
constexpr std::size_t kMaximumRepairRounds=8U;
constexpr std::size_t kMaximumInputTetsPerCavity=2U;
constexpr std::size_t kMaximumCavityCandidatesPerRound=4U;
constexpr std::size_t kMaximumCandidateSitesPerCavity=24U;
constexpr std::size_t kMaximumInsertedSites=kMaximumRepairRounds;
constexpr std::size_t kMaximumCandidateSiteTrials=kMaximumRepairRounds*kMaximumCavityCandidatesPerRound*kMaximumCandidateSitesPerCavity;
constexpr std::size_t kMaximumBoundaryFacesPerCavity=6U;
constexpr std::size_t kMaximumOutputTetsPerCavity=6U;
constexpr std::size_t kMaximumNetAddedTets=kMaximumRepairRounds*4U;
// One two-tet input, one six-tet replacement, and six boundary records.  The
// 24 sites are evaluated one at a time, rather than retained as candidates.
constexpr std::size_t kMaximumTemporaryTetEquivalents=14U;

struct CavityQuality {
  double minimum_dihedral{180.0};
  double maximum_dihedral{};
  double minimum_mean_ratio{1.0};
  std::size_t below_min_dihedral{};
  std::size_t above_max_dihedral{};
  std::size_t below_mean_ratio{};
};

std::pair<double,double> tet_dihedral_range(const std::array<Vec3,4>& p) {
  double minimum=180.0,maximum{};
  for(std::size_t first=0;first<tet_faces.size();++first)
    for(std::size_t second=first+1U;second<tet_faces.size();++second) {
      const auto outward=[&](std::array<unsigned int,3> face,unsigned int opposite) {
        auto normal=cross(p[face[1]]-p[face[0]],p[face[2]]-p[face[0]]);
        return dot(normal,p[opposite]-p[face[0]])>0.0?normal*-1.0:normal;
      };
      const auto a=outward(tet_faces[first],static_cast<unsigned int>(first));
      const auto b=outward(tet_faces[second],static_cast<unsigned int>(second));
      if(length(a)<=0.0||length(b)<=0.0)return {0.0,180.0};
      const auto angle=(std::numbers::pi-std::acos(std::clamp(dot(a,b)/(length(a)*length(b)),-1.0,1.0)))*180.0/std::numbers::pi;
      minimum=std::min(minimum,angle);maximum=std::max(maximum,angle);
    }
  return {minimum,maximum};
}

CavityQuality cavity_quality(const TetInput& mesh,std::span<const std::array<std::uint64_t,4>> tets) {
  CavityQuality result;
  for(const auto& tet:tets) {
    std::array<Vec3,4> p{};
    for(unsigned i=0;i<4;++i)p[i]=mesh.points.at(tet[i]);
    const auto [minimum,maximum]=tet_dihedral_range(p);
    const auto ratio=tet_mean_ratio(p);
    result.minimum_dihedral=std::min(result.minimum_dihedral,minimum);
    result.maximum_dihedral=std::max(result.maximum_dihedral,maximum);
    result.minimum_mean_ratio=std::min(result.minimum_mean_ratio,ratio);
    result.below_min_dihedral+=minimum<kMinDihedralDegrees?1U:0U;
    result.above_max_dihedral+=maximum>kMaxDihedralDegrees?1U:0U;
    result.below_mean_ratio+=ratio<kMinMeanRatio?1U:0U;
  }
  return result;
}

CavityQuality cavity_quality(const TetInput& mesh) { return cavity_quality(mesh,mesh.tets); }

bool better(const CavityQuality& candidate,const CavityQuality& baseline) {
  return std::make_tuple(candidate.below_min_dihedral,candidate.above_max_dihedral,candidate.below_mean_ratio,
                         -candidate.minimum_dihedral,candidate.maximum_dihedral,-candidate.minimum_mean_ratio)<
         std::make_tuple(baseline.below_min_dihedral,baseline.above_max_dihedral,baseline.below_mean_ratio,
                         -baseline.minimum_dihedral,baseline.maximum_dihedral,-baseline.minimum_mean_ratio);
}

bool same_quality(const CavityQuality& a,const CavityQuality& b) {
  return a.minimum_dihedral==b.minimum_dihedral&&a.maximum_dihedral==b.maximum_dihedral&&
      a.minimum_mean_ratio==b.minimum_mean_ratio&&a.below_min_dihedral==b.below_min_dihedral&&
      a.above_max_dihedral==b.above_max_dihedral&&a.below_mean_ratio==b.below_mean_ratio;
}

struct BoundaryFace {
  DualFaceKey face{};
  std::size_t source_tet{};
  int prescribed_kind{}; // 1 DC, 3 core, 4 curtain, 0 other/artificial.
};

struct TwoTetCavity {
  std::array<std::size_t,2> input{};
  std::array<BoundaryFace,kMaximumBoundaryFacesPerCavity> boundary{};
  std::size_t boundary_count{};
};

std::array<std::uint64_t,4> cavity_key(const TetInput& mesh,const TwoTetCavity& cavity) {
  // The shared face is the stable identity of a two-tet cavity.  Its canonical
  // key does not depend on input element listing order.
  std::set<std::uint64_t> shared;
  for(const auto id:mesh.tets[cavity.input[0]])
    for(const auto other:mesh.tets[cavity.input[1]])if(id==other)shared.insert(id);
  if(shared.size()!=3U)throw std::runtime_error("two-tet cavity is not face adjacent");
  return {{*shared.begin(),*std::next(shared.begin()),*std::next(shared.begin(),2),
           std::min(cavity.input[0],cavity.input[1])}};
}

std::array<std::uint64_t,4> canonical_tet_at(const TetInput& mesh,std::size_t index) {
  return canonical_dual_tet(mesh.tets.at(index));
}

std::optional<std::size_t> canonical_worst_tet(const TetInput& mesh) {
  std::optional<std::size_t> result;
  CavityQuality result_quality;
  for(std::size_t index=0;index<mesh.tets.size();++index) {
    const std::array<std::array<std::uint64_t,4>,1> only{{mesh.tets[index]}};
    const auto quality=cavity_quality(mesh,only);
    const bool failing=quality.below_min_dihedral!=0U||quality.above_max_dihedral!=0U||quality.below_mean_ratio!=0U;
    if(!failing)continue;
    if(!result||better(quality,result_quality)||
       (same_quality(quality,result_quality)&&canonical_tet_at(mesh,index)<canonical_tet_at(mesh,*result))) {
      result=index;result_quality=quality;
    }
  }
  return result;
}

std::vector<TwoTetCavity> two_tet_cavities(const Plc& plc,const TetInput& mesh,std::size_t seed) {
  const auto faces=repair_face_uses(mesh);
  std::vector<TwoTetCavity> result;
  const auto& parent=mesh.tets.at(seed);
  for(const auto face_indices:tet_faces) {
    const auto shared=canonical_dual_face({parent[face_indices[0]],parent[face_indices[1]],parent[face_indices[2]]});
    const auto found=faces.find(shared);
    if(found==faces.end()||found->second.size()!=2U||plc.facets.contains(shared))continue;
    const auto other=found->second[0]==seed?found->second[1]:found->second[0];
    TwoTetCavity cavity{{seed,other},{},0U};
    for(const auto index:cavity.input) for(const auto local_face:tet_faces) {
      const auto face=canonical_dual_face({mesh.tets[index][local_face[0]],mesh.tets[index][local_face[1]],mesh.tets[index][local_face[2]]});
      const auto uses=faces.at(face);
      const bool interior_to_cavity=uses.size()==2U&&
          ((uses[0]==cavity.input[0]&&uses[1]==cavity.input[1])||(uses[1]==cavity.input[0]&&uses[0]==cavity.input[1]));
      if(interior_to_cavity)continue;
      if(cavity.boundary_count>=kMaximumBoundaryFacesPerCavity)throw std::runtime_error("two-tet cavity boundary cap exceeded");
      const auto marker=plc.facets.find(face);
      cavity.boundary[cavity.boundary_count++]={face,index,marker==plc.facets.end()?0:marker->second};
    }
    if(cavity.boundary_count!=kMaximumBoundaryFacesPerCavity)throw std::runtime_error("two-tet cavity did not have six boundary faces");
    std::sort(cavity.boundary.begin(),cavity.boundary.begin()+static_cast<std::ptrdiff_t>(cavity.boundary_count),
              [](const BoundaryFace& a,const BoundaryFace& b) { return a.face<b.face; });
    result.push_back(cavity);
  }
  std::sort(result.begin(),result.end(),[&](const TwoTetCavity& a,const TwoTetCavity& b) { return cavity_key(mesh,a)<cavity_key(mesh,b); });
  if(result.size()>kMaximumCavityCandidatesPerRound)throw std::runtime_error("two-tet cavity candidate cap exceeded");
  return result;
}

Vec3 tet_centroid(const TetInput& mesh,const std::array<std::uint64_t,4>& tet) {
  Vec3 result{};
  for(const auto id:tet)result=result+mesh.points.at(id);
  return result/4.0;
}

std::uint64_t face_opposite(const std::array<std::uint64_t,4>& tet,const DualFaceKey& face) {
  return opposite_vertex(tet,face);
}

std::vector<Vec3> interface_aware_sites(const TetInput& mesh,const TwoTetCavity& cavity) {
  // Fixed 23-site stencil.  The first five sample the removed internal face;
  // the remaining eighteen start at each surviving boundary-face centroid and
  // step inward toward that face's source tetrahedron.  This deliberately
  // includes DC/core/curtain faces when present, while retaining them exactly.
  std::vector<Vec3> result;
  result.reserve(kMaximumCandidateSitesPerCavity);
  const auto left=tet_centroid(mesh,mesh.tets[cavity.input[0]]);
  const auto right=tet_centroid(mesh,mesh.tets[cavity.input[1]]);
  result.push_back(left);
  result.push_back(right);
  for(const double fraction:{0.25,0.5,0.75})result.push_back(left*(1.0-fraction)+right*fraction);
  for(std::size_t i=0;i<cavity.boundary_count;++i) {
    const auto& boundary=cavity.boundary[i];
    const auto& tet=mesh.tets.at(boundary.source_tet);
    const auto centre=(mesh.points.at(boundary.face[0])+mesh.points.at(boundary.face[1])+mesh.points.at(boundary.face[2]))/3.0;
    const auto inward=mesh.points.at(face_opposite(tet,boundary.face));
    // A marker is intentionally read as part of the fixed policy: all actual
    // interfaces use the shallower safe offsets, while non-PLC closure faces
    // also receive a centre-reaching sample.  No face itself is altered.
    const auto fractions=boundary.prescribed_kind==0?
        std::array<double,3>{{0.2,0.4,0.6}}:std::array<double,3>{{0.15,0.3,0.45}};
    for(const auto fraction:fractions)result.push_back(centre*(1.0-fraction)+inward*fraction);
  }
  if(result.size()!=23U||result.size()>kMaximumCandidateSitesPerCavity)throw std::runtime_error("invalid interface-aware site stencil");
  return result;
}

std::vector<std::array<std::uint64_t,4>> cone_boundary(const TwoTetCavity& cavity,std::uint64_t new_point) {
  std::vector<std::array<std::uint64_t,4>> result;
  result.reserve(cavity.boundary_count);
  for(std::size_t i=0;i<cavity.boundary_count;++i) {
    const auto& face=cavity.boundary[i].face;
    result.push_back({{new_point,face[0],face[1],face[2]}});
  }
  return result;
}

double total_six_volume(const TetInput& mesh) {
  double result{};
  for(const auto& tet:mesh.tets)result+=std::abs(signed_six_volume(mesh.points.at(tet[0]),mesh.points.at(tet[1]),mesh.points.at(tet[2]),mesh.points.at(tet[3])));
  return result;
}

struct MultiTetRepair {
  TetInput mesh;
  CavityQuality before;
  CavityQuality after;
  std::size_t repair_rounds{};
  std::size_t accepted_cavities{};
  std::size_t cavity_candidates_examined{};
  std::size_t candidate_sites_examined{};
  std::size_t rejected_non_star{};
  std::size_t rejected_no_local_improvement{};
  std::size_t rejected_no_global_improvement{};
  std::size_t maximum_input_tets{};
  std::size_t maximum_output_tets{};
  std::size_t net_added_tets{};
  std::size_t maximum_temporary_tet_equivalents{};
  bool frozen_faces_preserved{};
  bool volume_consistent{};
};

MultiTetRepair bounded_multitet_repair(const Plc& plc,const TetInput& input) {
  MultiTetRepair result{input,cavity_quality(input),{},0U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U,false,false};
  std::uint64_t next=0U;
  for(const auto& [id,unused]:result.mesh.points) { (void)unused;next=std::max(next,id+1U); }
  const auto original_volume=total_six_volume(input);
  for(std::size_t round=0;round<kMaximumRepairRounds;++round) {
    ++result.repair_rounds;
    const auto seed=canonical_worst_tet(result.mesh);
    if(!seed)break;
    const auto cavities=two_tet_cavities(plc,result.mesh,*seed);
    struct Choice { TwoTetCavity cavity{}; Vec3 site{}; std::vector<std::array<std::uint64_t,4>> replacement; CavityQuality global{}; CavityQuality local{}; };
    std::optional<Choice> best;
    for(const auto& cavity:cavities) {
      ++result.cavity_candidates_examined;
      result.maximum_input_tets=std::max(result.maximum_input_tets,kMaximumInputTetsPerCavity);
      result.maximum_temporary_tet_equivalents=std::max(result.maximum_temporary_tet_equivalents,kMaximumTemporaryTetEquivalents);
      const std::array<std::array<std::uint64_t,4>,2> old{{result.mesh.tets[cavity.input[0]],result.mesh.tets[cavity.input[1]]}};
      const auto old_local=cavity_quality(result.mesh,old);
      for(const auto& site:interface_aware_sites(result.mesh,cavity)) {
        ++result.candidate_sites_examined;
        TetInput prospective=result.mesh;
        prospective.points.emplace(next,site);
        auto replacement=cone_boundary(cavity,next);
        result.maximum_output_tets=std::max(result.maximum_output_tets,replacement.size());
        std::span<std::array<std::uint64_t,4>> replacement_span{replacement};
        if(!valid_replacement(prospective,old,replacement_span)) { ++result.rejected_non_star;continue; }
        const auto local=cavity_quality(prospective,replacement);
        if(!better(local,old_local)) { ++result.rejected_no_local_improvement;continue; }
        auto globally_repaired=prospective;
        replace_tetrahedra(globally_repaired,{cavity.input[0],cavity.input[1]},replacement);
        const auto global=cavity_quality(globally_repaired);
        if(!better(global,cavity_quality(result.mesh))) { ++result.rejected_no_global_improvement;continue; }
        if(!best||better(global,best->global)||
           (same_quality(global,best->global)&&std::tie(site.x,site.y,site.z)<std::tie(best->site.x,best->site.y,best->site.z))) {
          best=Choice{cavity,site,replacement,global,local};
        }
      }
    }
    if(!best)break;
    result.mesh.points.emplace(next,best->site);
    replace_tetrahedra(result.mesh,{best->cavity.input[0],best->cavity.input[1]},best->replacement);
    ++next;
    ++result.accepted_cavities;
    result.net_added_tets+=best->replacement.size()-kMaximumInputTetsPerCavity;
  }
  result.after=cavity_quality(result.mesh);
  result.frozen_faces_preserved=prescribed_faces_preserved(plc,result.mesh);
  const auto repaired_volume=total_six_volume(result.mesh);
  result.volume_consistent=std::abs(repaired_volume-original_volume)<=1.0e-10*std::max(1.0,original_volume);
  return result;
}

std::string fixture_name(int argc,char** argv) { return argc>2?argv[2]:"shell-n8"; }

} // namespace

#ifdef MULTITET_CAVITY_PROBE_TEST
int multitet_cavity_probe_main(int argc,char** argv) {
#else
int main(int argc,char** argv) {
#endif
  try {
    const std::string root=argc>1?argv[1]:"artifacts/dc-viability-2026-09-09";
    const auto fixture=fixture_name(argc,argv);
    const auto plc=read_poly(root+"/"+fixture+".poly");
    const auto input=read_tets(root+"/"+fixture);
    const auto start=std::chrono::steady_clock::now();
    const auto repaired=bounded_multitet_repair(plc,input);
    const auto elapsed=std::chrono::duration<double,std::milli>{std::chrono::steady_clock::now()-start}.count();
    const auto audit=audit_repair(plc,input,repaired.mesh);
    auto reversed=input;
    std::reverse(reversed.tets.begin(),reversed.tets.end());
    const auto reverse=bounded_multitet_repair(plc,reversed);
    const bool deterministic=canonical_tetrahedron_hash(repaired.mesh)==canonical_tetrahedron_hash(reverse.mesh)&&
        same_quality(repaired.after,reverse.after)&&repaired.accepted_cavities==reverse.accepted_cavities;
    const bool qualified=repaired.after.below_min_dihedral==0U&&repaired.after.above_max_dihedral==0U&&
        repaired.after.below_mean_ratio==0U&&audit.positive&&audit.unique&&audit.paired_interior_faces&&audit.no_overlap&&
        audit.complete_boundary_unchanged&&repaired.frozen_faces_preserved&&repaired.volume_consistent&&deterministic;
    std::cout<<std::setprecision(17)
      <<"{\"probe\":\"dc_bounded_multitet_cavity/v1\",\"fixture\":\""<<fixture<<"\","
      <<"\"contract\":{\"diagnostic_only\":true,\"min_dihedral_degrees\":"<<kMinDihedralDegrees
      <<",\"max_dihedral_degrees\":"<<kMaxDihedralDegrees<<",\"min_mean_ratio\":"<<kMinMeanRatio<<"},"
      <<"\"limits\":{\"maximum_repair_rounds\":"<<kMaximumRepairRounds<<",\"maximum_input_tets_per_cavity\":"<<kMaximumInputTetsPerCavity
      <<",\"maximum_cavity_candidates_per_round\":"<<kMaximumCavityCandidatesPerRound<<",\"maximum_candidate_sites_per_cavity\":"<<kMaximumCandidateSitesPerCavity
      <<",\"maximum_inserted_sites\":"<<kMaximumInsertedSites<<",\"maximum_candidate_site_trials\":"<<kMaximumCandidateSiteTrials
      <<",\"maximum_output_tets_per_cavity\":"<<kMaximumOutputTetsPerCavity<<",\"maximum_net_added_tets\":"<<kMaximumNetAddedTets
      <<",\"maximum_temporary_tet_equivalents\":"<<kMaximumTemporaryTetEquivalents<<"},"
      <<"\"work\":{\"repair_rounds\":"<<repaired.repair_rounds<<",\"accepted_cavities\":"<<repaired.accepted_cavities
      <<",\"cavity_candidates_examined\":"<<repaired.cavity_candidates_examined<<",\"candidate_sites_examined\":"<<repaired.candidate_sites_examined
      <<",\"rejected_non_star\":"<<repaired.rejected_non_star<<",\"rejected_no_local_improvement\":"<<repaired.rejected_no_local_improvement
      <<",\"rejected_no_global_improvement\":"<<repaired.rejected_no_global_improvement<<",\"net_added_tets\":"<<repaired.net_added_tets
      <<",\"elapsed_ms\":"<<elapsed<<"},"
      <<"\"before\":{\"min_dihedral_degrees\":"<<repaired.before.minimum_dihedral<<",\"max_dihedral_degrees\":"<<repaired.before.maximum_dihedral
      <<",\"min_mean_ratio\":"<<repaired.before.minimum_mean_ratio<<",\"below_min_dihedral\":"<<repaired.before.below_min_dihedral
      <<",\"above_max_dihedral\":"<<repaired.before.above_max_dihedral<<",\"below_mean_ratio\":"<<repaired.before.below_mean_ratio<<"},"
      <<"\"after\":{\"min_dihedral_degrees\":"<<repaired.after.minimum_dihedral<<",\"max_dihedral_degrees\":"<<repaired.after.maximum_dihedral
      <<",\"min_mean_ratio\":"<<repaired.after.minimum_mean_ratio<<",\"below_min_dihedral\":"<<repaired.after.below_min_dihedral
      <<",\"above_max_dihedral\":"<<repaired.after.above_max_dihedral<<",\"below_mean_ratio\":"<<repaired.after.below_mean_ratio<<"},"
      <<"\"invariants\":{\"complete_boundary_unchanged\":"<<(audit.complete_boundary_unchanged?"true":"false")
      <<",\"frozen_plc_faces_preserved\":"<<(repaired.frozen_faces_preserved?"true":"false")
      <<",\"retained_core_faces_preserved\":"<<(audit.retained_core_faces?"true":"false")
      <<",\"seam_curtain_faces_preserved\":"<<(audit.seam_curtain_faces?"true":"false")
      <<",\"no_stray_exterior_faces\":"<<(audit.complete_boundary_unchanged?"true":"false")
      <<",\"positive_unique_opposite_nonoverlapping\":"<<(audit.positive&&audit.unique&&audit.paired_interior_faces&&audit.no_overlap?"true":"false")
      <<",\"volume_consistent\":"<<(repaired.volume_consistent?"true":"false")
      <<",\"permutation_order_independent\":"<<(deterministic?"true":"false")<<"},\"qualified\":"<<(qualified?"true":"false")<<"}\n";
    return qualified?0:1;
  } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 2; }
}
