// Boundary-edge fan control for a bounded N6 collar/buffer reconstruction.
//
// Historical note: this filename used to contain an invalid "frozen wedge"
// impossibility claim. Two unsplit prescribed boundary triangles sharing an
// edge do not force one tetrahedron to own both triangles: an interior face
// through that edge may fan their wedge into several tetrahedra. This probe
// keeps the actual N6 wedge measurement as diagnostic information and proves
// that local conforming freedom with a closed, geometry-valid control.
#define BOUNDED_N6_AUTHORITATIVE_JOINT_TEST
#include "bounded_n6_authoritative_joint_probe.cpp"
#undef BOUNDED_N6_AUTHORITATIVE_JOINT_TEST

#include <chrono>
#include <iomanip>
#include <iostream>

namespace {
using namespace tetra::probes;

struct FrozenWedge { std::array<std::uint64_t, 2> shared_edge{}; Face first{}, second{}; double interior_dihedral_degrees{}; };

Face oriented_owner_face(const Domain& domain, const Face& canonical, std::optional<std::size_t> owner = std::nullopt) {
  for (std::size_t ti=0; ti<domain.tets.size(); ++ti) {
    if (owner && ti != *owner) continue;
    const auto& tet=domain.tets[ti];
    for (const auto local : tet_faces) {
      Face face{{tet[local[0]], tet[local[1]], tet[local[2]]}};
      if (key(face) != canonical) continue;
      std::uint64_t opposite=tet[0];
      for (const auto id : tet) if (id!=face[0] && id!=face[1] && id!=face[2]) { opposite=id; break; }
      const auto& a=domain.vertices.at(face[0]);
      if (dot(cross(domain.vertices.at(face[1])-a,domain.vertices.at(face[2])-a),domain.vertices.at(opposite)-a)>0.0) std::swap(face[1],face[2]);
      return face;
    }
  }
  throw std::runtime_error("face has no requested owning tetrahedron");
}

double interior_wedge_degrees(const Domain& domain, const Face& first, const Face& second) {
  const auto& a=domain.vertices.at(first[0]); const auto& b=domain.vertices.at(second[0]);
  const auto n1=cross(domain.vertices.at(first[1])-a,domain.vertices.at(first[2])-a);
  const auto n2=cross(domain.vertices.at(second[1])-b,domain.vertices.at(second[2])-b);
  if(length(n1)<=0.0 || length(n2)<=0.0) throw std::runtime_error("degenerate face");
  return (std::numbers::pi-std::acos(std::clamp(dot(n1,n2)/(length(n1)*length(n2)),-1.0,1.0)))*180.0/std::numbers::pi;
}

std::optional<FrozenWedge> worst_frozen_visible_wedge(const Domain& domain) {
  std::vector<Face> visible; for(const auto& [face,kind]:domain.prescribed) if(kind==1) visible.push_back(face);
  std::optional<FrozenWedge> result;
  for(std::size_t i=0;i<visible.size();++i) for(std::size_t j=i+1;j<visible.size();++j) {
    std::array<std::uint64_t,2> shared{}; std::size_t count{};
    for(const auto a:visible[i]) for(const auto b:visible[j]) if(a==b) { if(count<2U) shared[count]=a; ++count; }
    if(count!=2U) continue;
    std::sort(shared.begin(),shared.end());
    const FrozenWedge candidate{shared,visible[i],visible[j],interior_wedge_degrees(domain,oriented_owner_face(domain,visible[i]),oriented_owner_face(domain,visible[j]))};
    if(!result || candidate.interior_dihedral_degrees>result->interior_dihedral_degrees) result=candidate;
  }
  return result;
}

Domain boundary_edge_fan_control() {
  // Edge (0,1) has two immutable boundary faces forming 179.8 degrees.
  // Interior face (0,1,4) partitions it into two 89.9-degree tet wedges,
  // without splitting either prescribed face; all six outer faces close it.
  Domain d;
  d.vertices.emplace(0U,Vec3{0.0,0.0,0.0}); d.vertices.emplace(1U,Vec3{1.0,0.0,0.0}); d.vertices.emplace(2U,Vec3{0.5,1.0,0.0});
  const double outer=179.8*std::numbers::pi/180.0, middle=89.9*std::numbers::pi/180.0;
  d.vertices.emplace(3U,Vec3{0.5,std::cos(outer),std::sin(outer)}); d.vertices.emplace(4U,Vec3{0.5,std::cos(middle),std::sin(middle)});
  d.tets.push_back({{0U,1U,2U,4U}}); d.tets.push_back({{0U,1U,4U,3U}});
  for(const Face face : {Face{{0U,1U,2U}},Face{{0U,3U,1U}},Face{{0U,2U,4U}},Face{{1U,4U,2U}},Face{{0U,4U,3U}},Face{{1U,3U,4U}}}) d.prescribed.emplace(key(face),1);
  return d;
}

std::size_t face_use_count(const Domain& domain, Face wanted) {
  std::size_t count{}; wanted=key(wanted);
  for(const auto& tet:domain.tets) for(const auto local:tet_faces) if(key({{tet[local[0]],tet[local[1]],tet[local[2]]}})==wanted) ++count;
  return count;
}

int preflight_run() {
  const auto start=std::chrono::steady_clock::now();
  const auto candidate=build_candidate(false); const auto reversed=build_candidate(true);
  const auto n6_wedge=worst_frozen_visible_wedge(candidate.domain), reverse_wedge=worst_frozen_visible_wedge(reversed.domain);
  const auto fan=boundary_edge_fan_control(); const auto geometry=audit(fan,2U);
  const Face first=key({{0U,1U,2U}}), second=key({{0U,1U,3U}}), divider=key({{0U,1U,4U}});
  const double first_piece=interior_wedge_degrees(fan,oriented_owner_face(fan,first,0U),oriented_owner_face(fan,divider,0U));
  const double second_piece=interior_wedge_degrees(fan,oriented_owner_face(fan,divider,1U),oriented_owner_face(fan,second,1U));
  const bool partitions=first_piece>5.0 && first_piece<175.0 && second_piece>5.0 && second_piece<175.0;
  const bool preserves_unsplit=face_use_count(fan,first)==1U && face_use_count(fan,second)==1U && face_use_count(fan,divider)==2U;
  const bool deterministic=n6_wedge && reverse_wedge && n6_wedge->shared_edge==reverse_wedge->shared_edge && n6_wedge->interior_dihedral_degrees==reverse_wedge->interior_dihedral_degrees;
  std::cout<<std::setprecision(17)<<"{\"probe\":\"n6_boundary_edge_fan_control/v2\",\"n6_measurement\":{\"largest_visible_boundary_wedge_degrees\":"<<(n6_wedge?n6_wedge->interior_dihedral_degrees:0.0)<<"},"
    <<"\"control\":{\"boundary_wedge_degrees\":"<<interior_wedge_degrees(fan,oriented_owner_face(fan,first),oriented_owner_face(fan,second))<<",\"fan_piece_degrees\":["<<first_piece<<','<<second_piece<<"],\"unsplit_boundary_facets_preserved\":"<<(preserves_unsplit?"true":"false")<<",\"shared_interior_divider_uses\":"<<face_use_count(fan,divider)<<",\"geometry_valid\":"<<(geometry.geometry?"true":"false")<<"},"
    <<"\"conclusion\":{\"boundary_wedge_is_not_an_s4_impossibility_certificate\":true,\"validated\":"<<(geometry.geometry&&preserves_unsplit&&partitions&&deterministic?"true":"false")<<"},\"elapsed_ms\":"<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<"}\n";
  return geometry.geometry&&preserves_unsplit&&partitions&&deterministic?0:1;
}
} // namespace

#ifdef N6_FROZEN_BOUNDARY_S4_PREFLIGHT_TEST
int n6_frozen_boundary_s4_preflight_main() { return preflight_run(); }
#else
int main() { try { return preflight_run(); } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 2; } }
#endif
