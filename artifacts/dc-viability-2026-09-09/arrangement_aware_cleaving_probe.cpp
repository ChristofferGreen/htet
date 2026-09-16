// Finite-triangle, shared-face arrangement-cleaving witness.
//
// This is intentionally the smallest honest extension of the plane-only
// 1:3/3:1/2:2 kernel.  Two adjacent Freudenthal-like tetrahedra share the
// grid face ABC.  The *finite* frozen triangle BDE crosses that face along
// the triangle-boundary edge DE.  Its canonical intersection X with ABC is
// therefore a real arrangement entity which both neighbouring owners must
// derive identically.  A deterministic 2-to-3 cavity retriangulation then
// replaces ABCD + ABCE by ABDE + BCDE + CADE.  BDE is retained exactly as an
// interface face; no plane facet beyond BDE is emitted.
//
// It is a bounded reference, not a claim that arbitrary clipped DC patches
// are solved.  In particular, the production corpus has configurations with
// several triangle boundaries in a tet/cavity.  Those require additional
// arrangement templates and remain explicitly rejected by this probe.

#define CONFORMING_SCAFFOLD_CLEAVING_PROBE_TEST
#include "conforming_scaffold_cleaving_probe.cpp"
#undef CONFORMING_SCAFFOLD_CLEAVING_PROBE_TEST

#include <bit>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numbers>

namespace {
using namespace tetra::probes;

using ArrangementTet=std::array<std::uint64_t,4>;
using ArrangementFace=std::array<std::uint64_t,3>;
using ArrangementEdge=std::array<std::uint64_t,2>;

constexpr std::uint64_t kA=10U,kB=20U,kC=30U,kD=40U,kE=50U;
constexpr double kAuditEpsilon=1.0e-12;

ArrangementFace arrangement_face_key(ArrangementFace face) { std::sort(face.begin(),face.end());return face; }
ArrangementTet arrangement_tet_key(ArrangementTet tet) { std::sort(tet.begin(),tet.end());return tet; }

struct SharedFaceCutKey {
  ArrangementFace grid_face{};
  ArrangementEdge triangle_boundary_edge{};
  auto operator<=>(const SharedFaceCutKey&) const=default;
};

struct OwnerCutView {
  SharedFaceCutKey key{};
  Vec3 position{};
  std::uint64_t owner_cell{};
};

// Each cell request has the frozen triangle in its one-cell triangle halo.
// The key does not contain request order, local edge slots, or a floating
// coordinate.  It is the exact identity a later chunk implementation needs.
OwnerCutView derive_shared_face_cut(std::uint64_t owner_cell,bool reverse_triangle_order) {
  const std::array<Vec3,3> shared{{{0.0,0.0,0.0},{1.0,0.0,0.0},{0.0,1.0,0.0}}};
  Vec3 d{0.25,0.25,-0.8};
  Vec3 e{0.25,0.25,0.8};
  if(reverse_triangle_order)std::swap(d,e);
  // ABC is z=0.  Keep the calculation in the edge's supplied order: both
  // endpoint orders evaluate the same IEEE value for this symmetric witness,
  // and the test below records bit identity rather than tolerance equality.
  const double dz=d.z,ez=e.z;
  const Vec3 x=d+(e-d)*(dz/(dz-ez));
  (void)shared;
  return {{{kA,kB,kC},{kD,kE}},x,owner_cell};
}

struct ArrangementMesh {
  std::map<std::uint64_t,Vec3> vertices;
  std::vector<ArrangementTet> tetrahedra;
  ArrangementFace frozen_triangle{{kB,kD,kE}};
  SharedFaceCutKey shared_cut_key{{kA,kB,kC},{kD,kE}};
  Vec3 shared_cut{};
  std::uint64_t canonical_cavity_owner{};
};

void add_arrangement_tet(ArrangementMesh& mesh,ArrangementTet tet) {
  const auto six=signed_six_volume(mesh.vertices.at(tet[0]),mesh.vertices.at(tet[1]),
                                   mesh.vertices.at(tet[2]),mesh.vertices.at(tet[3]));
  if(six<0.0)std::swap(tet[1],tet[2]);
  mesh.tetrahedra.push_back(tet);
}

ArrangementMesh build_arrangement_candidate(std::array<std::uint64_t,2> visited_cells) {
  // 0 and 1 identify the two cells on opposite sides of ABC.  A real request
  // may visit them in either order; min(cell id) owns the cross-face cavity.
  ArrangementMesh result;
  result.vertices={{kA,{0.0,0.0,0.0}},{kB,{1.0,0.0,0.0}},{kC,{0.0,1.0,0.0}},
                   {kD,{0.25,0.25,-0.8}},{kE,{0.25,0.25,0.8}}};
  std::array<OwnerCutView,2> views{};
  for(std::size_t i=0;i<visited_cells.size();++i)
    views[i]=derive_shared_face_cut(visited_cells[i],i==1U);
  result.shared_cut_key=views[0].key;
  result.shared_cut=views[0].position;
  result.canonical_cavity_owner=std::min(visited_cells[0],visited_cells[1]);
  // The equality is checked by the caller before a candidate is accepted.
  // X is deliberately an arrangement entity, not a new volume vertex: the
  // 2-to-3 triangulation crosses the artificial cell face and its trace there
  // is ABX, BCX and CAX.  It is retained for seam verification/export.
  add_arrangement_tet(result,{{kA,kB,kD,kE}});
  add_arrangement_tet(result,{{kB,kC,kD,kE}});
  add_arrangement_tet(result,{{kC,kA,kD,kE}});
  return result;
}

bool arrangement_exact_vec3(const Vec3& a,const Vec3& b) {
  return std::bit_cast<std::uint64_t>(a.x)==std::bit_cast<std::uint64_t>(b.x)&&
      std::bit_cast<std::uint64_t>(a.y)==std::bit_cast<std::uint64_t>(b.y)&&
      std::bit_cast<std::uint64_t>(a.z)==std::bit_cast<std::uint64_t>(b.z);
}

bool arrangement_triangle_contains(const std::array<Vec3,3>& triangle,const Vec3& point) {
  const auto normal=cross(triangle[1]-triangle[0],triangle[2]-triangle[0]);
  if(std::abs(dot(normal,point-triangle[0]))>kAuditEpsilon)return false;
  const auto a=dot(normal,cross(triangle[1]-triangle[0],point-triangle[0]));
  const auto b=dot(normal,cross(triangle[2]-triangle[1],point-triangle[1]));
  const auto c=dot(normal,cross(triangle[0]-triangle[2],point-triangle[2]));
  return (a>=-kAuditEpsilon&&b>=-kAuditEpsilon&&c>=-kAuditEpsilon)||
      (a<=kAuditEpsilon&&b<=kAuditEpsilon&&c<=kAuditEpsilon);
}

double arrangement_min_dihedral(const ArrangementMesh& mesh,std::size_t& below_five) {
  double minimum=180.0;below_five=0U;
  for(const auto& tet:mesh.tetrahedra) {
    std::array<Vec3,4> p{};for(unsigned i=0U;i<4U;++i)p[i]=mesh.vertices.at(tet[i]);
    double local=180.0;
    for(unsigned a=0U;a<tet_faces.size();++a)for(unsigned b=a+1U;b<tet_faces.size();++b) {
      const auto outward=[&](std::array<unsigned,3> face,unsigned opposite) {
        auto n=cross(p[face[1]]-p[face[0]],p[face[2]]-p[face[0]]);
        return dot(n,p[opposite]-p[face[0]])>0.0?n*-1.0:n;
      };
      const auto n0=outward(tet_faces[a],a),n1=outward(tet_faces[b],b);
      const auto radians=std::numbers::pi-std::acos(std::clamp(dot(n0,n1)/(length(n0)*length(n1)),-1.0,1.0));
      local=std::min(local,radians*180.0/std::numbers::pi);
    }
    minimum=std::min(minimum,local);if(local<5.0)++below_five;
  }
  return minimum;
}

struct ArrangementAudit {
  bool positive{true},unique{true},manifold{true},opposite_sides{true},no_overlap{true};
  bool volume_conserved{true},frozen_triangle_exact{true},closed_boundary{true};
  std::size_t nonpositive{},duplicates{},nonmanifold{},same_side{},overlaps{};
  std::size_t frozen_triangle_uses{},coplanar_extension_faces{};
  double volume_error{};
};

ArrangementAudit audit_arrangement(const ArrangementMesh& mesh) {
  ArrangementAudit result;
  std::map<ArrangementFace,std::vector<std::uint64_t>> uses;
  std::set<ArrangementTet> unique;
  double output_volume{};
  for(const auto& tet:mesh.tetrahedra) {
    const auto six=signed_six_volume(mesh.vertices.at(tet[0]),mesh.vertices.at(tet[1]),mesh.vertices.at(tet[2]),mesh.vertices.at(tet[3]));
    if(six<=kAuditEpsilon){result.positive=false;++result.nonpositive;}
    output_volume+=six/6.0;
    if(!unique.insert(arrangement_tet_key(tet)).second){result.unique=false;++result.duplicates;}
    for(unsigned f=0U;f<tet_faces.size();++f) {
      const auto i=tet_faces[f];uses[arrangement_face_key({{tet[i[0]],tet[i[1]],tet[i[2]]}})].push_back(tet[f]);
    }
  }
  const auto frozen=arrangement_face_key(mesh.frozen_triangle);
  const auto frozen_normal=cross(mesh.vertices.at(kD)-mesh.vertices.at(kB),mesh.vertices.at(kE)-mesh.vertices.at(kB));
  for(const auto& [face,opposites]:uses) {
    if(opposites.size()>2U){result.manifold=false;++result.nonmanifold;}
    if(opposites.size()==2U) {
      const auto& p=mesh.vertices.at(face[0]);const auto n=cross(mesh.vertices.at(face[1])-p,mesh.vertices.at(face[2])-p);
      if(dot(n,mesh.vertices.at(opposites[0])-p)*dot(n,mesh.vertices.at(opposites[1])-p)>=0.0){result.opposite_sides=false;++result.same_side;}
    }
    if(face==frozen)result.frozen_triangle_uses=opposites.size();
    // A plane-only implementation would introduce another triangular facet
    // coplanar with BDE.  The finite witness permits exactly BDE itself.
    const bool all_coplanar=std::all_of(face.begin(),face.end(),[&](const auto id) {
      return std::abs(dot(frozen_normal,mesh.vertices.at(id)-mesh.vertices.at(kB)))<kAuditEpsilon;
    });
    if(all_coplanar&&face!=frozen)++result.coplanar_extension_faces;
  }
  result.frozen_triangle_exact=result.frozen_triangle_uses==2U&&result.coplanar_extension_faces==0U;
  const std::set<ArrangementFace> expected_boundary{
      arrangement_face_key({{kA,kB,kD}}),arrangement_face_key({{kB,kC,kD}}),arrangement_face_key({{kC,kA,kD}}),
      arrangement_face_key({{kA,kB,kE}}),arrangement_face_key({{kB,kC,kE}}),arrangement_face_key({{kC,kA,kE}})};
  std::set<ArrangementFace> actual_boundary;
  for(const auto& [face,opposites]:uses)if(opposites.size()==1U)actual_boundary.insert(face);
  result.closed_boundary=actual_boundary==expected_boundary;
  DualVolumeBuild view;view.vertices=mesh.vertices;
  for(std::size_t a=0;a<mesh.tetrahedra.size();++a)for(std::size_t b=a+1U;b<mesh.tetrahedra.size();++b)
    if(dual_tets_strictly_overlap(view,{mesh.tetrahedra[a],DualVolumeRegion::transition},
                                  {mesh.tetrahedra[b],DualVolumeRegion::transition})) {result.no_overlap=false;++result.overlaps;}
  const std::array<Vec3,4> lower{{mesh.vertices.at(kA),mesh.vertices.at(kB),mesh.vertices.at(kC),mesh.vertices.at(kD)}};
  const std::array<Vec3,4> upper{{mesh.vertices.at(kA),mesh.vertices.at(kC),mesh.vertices.at(kB),mesh.vertices.at(kE)}};
  const auto source_volume=(std::abs(signed_six_volume(lower[0],lower[1],lower[2],lower[3]))+
                            std::abs(signed_six_volume(upper[0],upper[1],upper[2],upper[3])))/6.0;
  result.volume_error=std::abs(source_volume-output_volume);
  result.volume_conserved=result.volume_error<kAuditEpsilon;
  return result;
}

struct ArrangementResult {
  bool shared_cut_bit_identical{},owner_order_identical{},seam_owner_rule{};
  bool clipped_patch_control_rejected{},geometry_valid{},quality_pass{};
  std::size_t output_tets{},peak_vertices{},below_five{};
  double min_dihedral{};
  ArrangementAudit audit{};
};

ArrangementResult run_arrangement_aware_cleaving_probe() {
  ArrangementResult result;
  const auto lower=derive_shared_face_cut(0U,false),upper_view=derive_shared_face_cut(1U,true);
  result.shared_cut_bit_identical=lower.key==upper_view.key&&arrangement_exact_vec3(lower.position,upper_view.position);
  const auto forward=build_arrangement_candidate({{0U,1U}});
  const auto reverse=build_arrangement_candidate({{1U,0U}});
  result.owner_order_identical=forward.tetrahedra==reverse.tetrahedra&&forward.shared_cut_key==reverse.shared_cut_key&&
      arrangement_exact_vec3(forward.shared_cut,reverse.shared_cut)&&forward.canonical_cavity_owner==reverse.canonical_cavity_owner;
  // Independent requests may both derive the candidate, but exactly the
  // canonical lower-id owner emits it.  This is the bounded seam rule.
  result.seam_owner_rule=forward.canonical_cavity_owner==0U&&reverse.canonical_cavity_owner==0U;
  // B-D-E' is a companion finite triangle that ends inside the upper source
  // tet.  Its plane also reaches E, which lies outside the frozen triangle;
  // reusing the positive BDE template would extend that surface.  Keep this
  // as an explicit rejection until a true clipped-patch template exists.
  const std::array<Vec3,3> clipped{{forward.vertices.at(kB),forward.vertices.at(kD),{0.25,0.25,0.35}}};
  const std::array<Vec3,4> upper_tet{{forward.vertices.at(kA),forward.vertices.at(kC),forward.vertices.at(kB),forward.vertices.at(kE)}};
  result.clipped_patch_control_rejected=triangle_tet_contact(clipped,upper_tet)==TriangleTetContact::strict&&
      !arrangement_triangle_contains(clipped,forward.vertices.at(kE));
  result.audit=audit_arrangement(forward);result.output_tets=forward.tetrahedra.size();result.peak_vertices=forward.vertices.size()+1U; // + shared-face cut X
  result.min_dihedral=arrangement_min_dihedral(forward,result.below_five);
  result.geometry_valid=result.shared_cut_bit_identical&&result.owner_order_identical&&result.seam_owner_rule&&
      result.clipped_patch_control_rejected&&
      result.audit.positive&&result.audit.unique&&result.audit.manifold&&result.audit.opposite_sides&&result.audit.no_overlap&&
      result.audit.volume_conserved&&result.audit.frozen_triangle_exact&&result.audit.closed_boundary;
  result.quality_pass=result.below_five==0U;
  return result;
}

int arrangement_aware_cleaving_main() {
  const auto start=std::chrono::steady_clock::now();const auto result=run_arrangement_aware_cleaving_probe();
  const auto elapsed=std::chrono::duration<double,std::milli>{std::chrono::steady_clock::now()-start}.count();
  std::cout<<std::setprecision(17)
    <<"{\"probe\":\"dc_arrangement_aware_cleaving/v1\",\"fixture\":\"two_adjacent_tets_one_finite_triangle\","
    <<"\"contract\":{\"immutable\":[\"finite_frozen_triangle_BDE\",\"source_outer_boundary\"],\"construction\":\"canonical_2_to_3_cross_face_cavity_retriangulation\",\"complete_volume\":false},"
    <<"\"arrangement\":{\"shared_grid_face\":\"ABC\",\"finite_triangle\":\"BDE\",\"triangle_boundary_edge\":\"DE\",\"shared_face_cut\":\"X=DE_intersect_ABC\",\"plane_extension_faces\":"<<result.audit.coplanar_extension_faces<<"},"
    <<"\"bounds\":{\"source_tets\":2,\"triangle_count\":1,\"shared_face_cuts\":1,\"output_tets\":"<<result.output_tets<<",\"peak_local_vertices_including_cut\":"<<result.peak_vertices<<",\"one_cell_triangle_halo\":true},"
    <<"\"invariants\":{\"shared_cut_bit_identical\":"<<(result.shared_cut_bit_identical?"true":"false")
      <<",\"reversed_owner_order_identical\":"<<(result.owner_order_identical?"true":"false")
      <<",\"independent_request_seam_owner_rule\":"<<(result.seam_owner_rule?"true":"false")
      <<",\"clipped_patch_control_rejected\":"<<(result.clipped_patch_control_rejected?"true":"false")
      <<",\"positive\":"<<(result.audit.positive?"true":"false")<<",\"unique\":"<<(result.audit.unique?"true":"false")
      <<",\"manifold\":"<<(result.audit.manifold?"true":"false")<<",\"opposite_shared_faces\":"<<(result.audit.opposite_sides?"true":"false")
      <<",\"no_overlap\":"<<(result.audit.no_overlap?"true":"false")<<",\"volume_error\":"<<result.audit.volume_error
      <<",\"closed_boundary\":"<<(result.audit.closed_boundary?"true":"false")<<",\"frozen_triangle_exact\":"<<(result.audit.frozen_triangle_exact?"true":"false")<<"},"
    <<"\"quality\":{\"min_dihedral\":"<<result.min_dihedral<<",\"below_5_degrees\":"<<result.below_five<<"},"
    <<"\"result\":{\"geometry_valid\":"<<(result.geometry_valid?"true":"false")<<",\"quality_pass\":"<<(result.quality_pass?"true":"false")
      <<",\"honest_scope\":\"positive finite-edge seam primitive only; arbitrary clipped multi-triangle DC arrangements remain unimplemented\"},"
    <<"\"elapsed_ms\":"<<elapsed<<",\"control_passed\":"<<(result.geometry_valid&&result.quality_pass?"true":"false")<<"}\n";
  return result.geometry_valid&&result.quality_pass?0:1;
}
} // namespace

#ifdef ARRANGEMENT_AWARE_CLEAVING_PROBE_TEST
int arrangement_aware_cleaving_probe_main() { return arrangement_aware_cleaving_main(); }
#else
int main() { try { return arrangement_aware_cleaving_main(); } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 2; } }
#endif
