// Bounded S4 experiment: insert an interior Steiner point into a single bad
// tetrahedron and retetrahedralize that closed one-tet cavity as a four-tet
// star.  It deliberately freezes every face of the input PLC: the old tet's
// boundary becomes the new cavity boundary verbatim.  This is a diagnostic
// candidate family, not a general tetrahedralizer.
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
constexpr std::size_t kMaximumCavities=12U;
constexpr std::size_t kMaximumCandidateSites=35U;

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
    const auto [dihedral,maximum_dihedral]=tet_dihedral_range(p);
    const auto ratio=tet_mean_ratio(p);
    result.minimum_dihedral=std::min(result.minimum_dihedral,dihedral);
    result.maximum_dihedral=std::max(result.maximum_dihedral,maximum_dihedral);
    result.minimum_mean_ratio=std::min(result.minimum_mean_ratio,ratio);
    result.below_min_dihedral+=dihedral<kMinDihedralDegrees?1U:0U;
    result.above_max_dihedral+=maximum_dihedral>kMaxDihedralDegrees?1U:0U;
    result.below_mean_ratio+=ratio<kMinMeanRatio?1U:0U;
  }
  return result;
}

bool better(const CavityQuality& candidate,const CavityQuality& baseline) {
  return std::make_tuple(candidate.below_min_dihedral,candidate.above_max_dihedral,candidate.below_mean_ratio,
                         -candidate.minimum_dihedral,candidate.maximum_dihedral,-candidate.minimum_mean_ratio)<
         std::make_tuple(baseline.below_min_dihedral,baseline.above_max_dihedral,baseline.below_mean_ratio,
                         -baseline.minimum_dihedral,baseline.maximum_dihedral,-baseline.minimum_mean_ratio);
}

std::vector<std::array<unsigned,4>> candidate_barycentrics() {
  // A fixed, symmetric, strictly-interior 35-site stencil, represented as
  // integer barycentric coordinates on denominator 12.  No continuous
  // optimization or retry expansion occurs, making work deterministic.
  std::vector<std::array<unsigned,4>> result;
  constexpr std::array<std::array<unsigned,4>,5> prototypes{{
      {{3U,3U,3U,3U}},{{6U,2U,2U,2U}},{{5U,3U,2U,2U}},{{4U,4U,2U,2U}},{{4U,3U,3U,2U}}}};
  for(const auto prototype:prototypes) {
    auto permutation=prototype;
    std::sort(permutation.begin(),permutation.end());
    do { result.push_back(permutation); } while(std::next_permutation(permutation.begin(),permutation.end()));
  }
  std::sort(result.begin(),result.end());
  result.erase(std::unique(result.begin(),result.end()),result.end());
  if(result.size()!=kMaximumCandidateSites)throw std::runtime_error("invalid fixed Steiner stencil");
  return result;
}

Vec3 barycentric_point(const TetInput& mesh,const std::array<std::uint64_t,4>& tet,
                       const std::array<unsigned,4>& weights) {
  Vec3 result{};
  for(unsigned i=0;i<4;++i)result=result+mesh.points.at(tet[i])*(static_cast<double>(weights[i])/12.0);
  return result;
}

std::array<std::array<std::uint64_t,4>,4> star(const std::array<std::uint64_t,4>& tet,std::uint64_t point) {
  return {{{{point,tet[1],tet[2],tet[3]}},{{tet[0],point,tet[2],tet[3]}},
           {{tet[0],tet[1],point,tet[3]}},{{tet[0],tet[1],tet[2],point}}}};
}

struct SteinerRepair {
  TetInput mesh;
  CavityQuality before;
  CavityQuality after;
  std::size_t accepted_cavities{};
  std::size_t candidates_examined{};
  std::size_t candidates_rejected{};
  std::size_t max_cavity_tets{};
  std::size_t net_added_tets{};
  bool frozen_faces_preserved{};
  bool deterministic{};
};

struct OneTetLimit {
  std::size_t input_index{};
  CavityQuality before;
  CavityQuality best;
  std::array<unsigned,4> best_site{};
};

OneTetLimit one_tet_limit(const TetInput& mesh,const std::vector<std::array<unsigned,4>>& sites) {
  std::size_t index{};
  double worst=180.0;
  for(std::size_t i=0;i<mesh.tets.size();++i) {
    const std::array<std::array<std::uint64_t,4>,1> span{{mesh.tets[i]}};
    const auto quality=cavity_quality(mesh,span);
    if(quality.minimum_dihedral<worst) { worst=quality.minimum_dihedral;index=i; }
  }
  const std::array<std::array<std::uint64_t,4>,1> old{{mesh.tets[index]}};
  const auto before=cavity_quality(mesh,old);
  OneTetLimit result{index,before,{},sites.front()};
  bool have_candidate{};
  const std::uint64_t new_id=mesh.points.rbegin()->first+1U;
  for(const auto& site:sites) {
    TetInput prospective=mesh;
    prospective.points.emplace(new_id,barycentric_point(mesh,old[0],site));
    auto children=star(old[0],new_id);
    std::span<std::array<std::uint64_t,4>> child_span{children};
    if(!valid_replacement(prospective,old,child_span))continue;
    const auto quality=cavity_quality(prospective,children);
    // This asks the most charitable question of a one-tet cavity: which
    // fixed site maximises its worst child, even if it still fails S4.
    if(!have_candidate||std::make_tuple(quality.minimum_dihedral,-quality.maximum_dihedral,quality.minimum_mean_ratio)>
        std::make_tuple(result.best.minimum_dihedral,-result.best.maximum_dihedral,result.best.minimum_mean_ratio)) {
      result.best=quality;result.best_site=site;have_candidate=true;
    }
  }
  return result;
}

SteinerRepair bounded_steiner_repair(const Plc& plc,const TetInput& input) {
  SteinerRepair result{input,cavity_quality(input,input.tets),{},0U,0U,0U,1U,0U,false,true};
  const auto sites=candidate_barycentrics();
  std::uint64_t next=0U;
  for(const auto& [id,unused]:result.mesh.points) { (void)unused;next=std::max(next,id+1U); }
  for(std::size_t round=0;round<kMaximumCavities;++round) {
    struct Choice { std::size_t index{}; Vec3 point{}; std::array<std::array<std::uint64_t,4>,4> tets{}; CavityQuality quality{}; };
    std::optional<Choice> best;
    // Every candidate begins with a closed existing tet cavity.  Scanning the
    // complete finite witness is intentional here; no neighbour expansion or
    // global remeshing is permitted.
    for(std::size_t index=0;index<result.mesh.tets.size();++index) {
      const auto& old=result.mesh.tets[index];
      const std::array<std::array<std::uint64_t,4>,1> old_span{{old}};
      const auto old_quality=cavity_quality(result.mesh,old_span);
      if(old_quality.below_min_dihedral==0U&&old_quality.above_max_dihedral==0U&&old_quality.below_mean_ratio==0U)continue;
      for(const auto& site:sites) {
        ++result.candidates_examined;
        const auto point=barycentric_point(result.mesh,old,site);
        auto replacement=star(old,next);
        TetInput prospective=result.mesh;
        prospective.points.emplace(next,point);
        std::span<std::array<std::uint64_t,4>> replacement_span{replacement};
        if(!valid_replacement(prospective,old_span,replacement_span)) { ++result.candidates_rejected;continue; }
        const auto quality=cavity_quality(prospective,replacement);
        if(!better(quality,old_quality)) { ++result.candidates_rejected;continue; }
        if(!best||better(quality,best->quality))best=Choice{index,point,replacement,quality};
      }
    }
    if(!best)break;
    result.mesh.points.emplace(next,best->point);
    replace_tetrahedra(result.mesh,{best->index},best->tets);
    ++next;
    ++result.accepted_cavities;
    result.net_added_tets+=3U;
  }
  result.after=cavity_quality(result.mesh,result.mesh.tets);
  result.frozen_faces_preserved=prescribed_faces_preserved(plc,result.mesh);
  return result;
}

std::string fixture_name(int argc,char** argv) { return argc>2?argv[2]:"shell-n8"; }

} // namespace

#ifdef STEINER_CAVITY_PROBE_TEST
int steiner_cavity_probe_main(int argc,char** argv) {
#else
int main(int argc,char** argv) {
#endif
  try {
    const std::string root=argc>1?argv[1]:"artifacts/dc-viability-2026-09-09";
    const auto fixture=fixture_name(argc,argv);
    const auto plc=read_poly(root+"/"+fixture+".poly");
    const auto input=read_tets(root+"/"+fixture);
    const auto start=std::chrono::steady_clock::now();
    const auto repaired=bounded_steiner_repair(plc,input);
    const auto elapsed=std::chrono::duration<double,std::milli>{std::chrono::steady_clock::now()-start}.count();
    const auto audit=audit_repair(plc,input,repaired.mesh);
    auto reversed=input;
    std::reverse(reversed.tets.begin(),reversed.tets.end());
    const auto reverse=bounded_steiner_repair(plc,reversed);
    const bool deterministic=canonical_tetrahedron_hash(repaired.mesh)==canonical_tetrahedron_hash(reverse.mesh)&&
        repaired.after.minimum_dihedral==reverse.after.minimum_dihedral&&repaired.after.minimum_mean_ratio==reverse.after.minimum_mean_ratio;
    const bool qualified=repaired.after.below_min_dihedral==0U&&repaired.after.above_max_dihedral==0U&&
        repaired.after.below_mean_ratio==0U&&audit.positive&&audit.unique&&audit.paired_interior_faces&&audit.no_overlap&&
        audit.complete_boundary_unchanged&&repaired.frozen_faces_preserved&&deterministic;
    const auto limit=one_tet_limit(input,candidate_barycentrics());
    std::cout<<std::setprecision(17)
      <<"{\"probe\":\"dc_bounded_steiner_cavity/v1\",\"fixture\":\""<<fixture<<"\","
      <<"\"contract\":{\"diagnostic_only\":true,\"min_dihedral_degrees\":"<<kMinDihedralDegrees
      <<",\"max_dihedral_degrees\":"<<kMaxDihedralDegrees<<",\"min_mean_ratio\":"<<kMinMeanRatio<<"},"
      <<"\"limits\":{\"maximum_cavities\":"<<kMaximumCavities<<",\"maximum_candidate_sites_per_cavity\":"<<kMaximumCandidateSites
      <<",\"maximum_input_tets_per_cavity\":1,\"maximum_net_added_tets\":"<<kMaximumCavities*3U<<"},"
      <<"\"work\":{\"accepted_cavities\":"<<repaired.accepted_cavities<<",\"candidate_sites_examined\":"<<repaired.candidates_examined
      <<",\"candidate_sites_rejected\":"<<repaired.candidates_rejected<<",\"net_added_tets\":"<<repaired.net_added_tets
      <<",\"elapsed_ms\":"<<elapsed<<"},"
      <<"\"before\":{\"min_dihedral_degrees\":"<<repaired.before.minimum_dihedral<<",\"max_dihedral_degrees\":"<<repaired.before.maximum_dihedral
      <<",\"min_mean_ratio\":"<<repaired.before.minimum_mean_ratio<<",\"below_min_dihedral\":"<<repaired.before.below_min_dihedral
      <<",\"above_max_dihedral\":"<<repaired.before.above_max_dihedral<<",\"below_mean_ratio\":"<<repaired.before.below_mean_ratio<<"},"
      <<"\"after\":{\"min_dihedral_degrees\":"<<repaired.after.minimum_dihedral<<",\"max_dihedral_degrees\":"<<repaired.after.maximum_dihedral
      <<",\"min_mean_ratio\":"<<repaired.after.minimum_mean_ratio<<",\"below_min_dihedral\":"<<repaired.after.below_min_dihedral
      <<",\"above_max_dihedral\":"<<repaired.after.above_max_dihedral<<",\"below_mean_ratio\":"<<repaired.after.below_mean_ratio<<"},"
      <<"\"worst_one_tet_star_limit\":{\"input_tet_index\":"<<limit.input_index
      <<",\"before_min_dihedral_degrees\":"<<limit.before.minimum_dihedral
      <<",\"best_child_min_dihedral_degrees\":"<<limit.best.minimum_dihedral
      <<",\"best_child_max_dihedral_degrees\":"<<limit.best.maximum_dihedral
      <<",\"best_child_min_mean_ratio\":"<<limit.best.minimum_mean_ratio
      <<",\"best_site_barycentric_twelfths\":["<<limit.best_site[0]<<','<<limit.best_site[1]<<','<<limit.best_site[2]<<','<<limit.best_site[3]<<"]},"
      <<"\"invariants\":{\"complete_boundary_unchanged\":"<<(audit.complete_boundary_unchanged?"true":"false")
      <<",\"frozen_plc_faces_preserved\":"<<(repaired.frozen_faces_preserved?"true":"false")
      <<",\"retained_core_faces_preserved\":"<<(audit.retained_core_faces?"true":"false")
      <<",\"seam_curtain_faces_preserved\":"<<(audit.seam_curtain_faces?"true":"false")
      <<",\"positive_unique_opposite_nonoverlapping\":"<<(audit.positive&&audit.unique&&audit.paired_interior_faces&&audit.no_overlap?"true":"false")
      <<",\"permutation_order_independent\":"<<(deterministic?"true":"false")<<"},\"qualified\":"<<(qualified?"true":"false")<<"}\n";
    return qualified?0:1;
  } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 2; }
}
