// Bounded local cleaving witness for the two-front transition.
//
// This is deliberately smaller than a full buffer mesher.  It implements the
// actual 1:3, 3:1 and 2:2 tetrahedron/plane cleavage grammars, including the
// artificial-face retriangulations that an uncut scaffold cannot express.
// A corpus scan then asks the important qualification question: does a frozen
// *triangle* cover the entire planar section of an affected regular tet?  If
// yes, this kernel is an exact local construction.  If no, extending the
// plane past the triangle would alter the prescribed inner DC front, so the
// case is retained as a small, reproducible clipped-patch witness rather than
// silently accepted as a bridge.
//
// The visible DC sheet and retained core are never modified.  The current
// experiment only uses non-core band tets.  It is not a complete volume or a
// GPU proposal; it establishes the first concrete finite local topology that
// the next arrangement-aware buffer implementation must generalize.

#define CONFORMING_SCAFFOLD_CLEAVING_PROBE_TEST
#include "conforming_scaffold_cleaving_probe.cpp"
#undef CONFORMING_SCAFFOLD_CLEAVING_PROBE_TEST

#include <chrono>
#include <iomanip>
#include <iostream>
#include <numbers>

namespace {
using namespace tetra::probes;

constexpr double kPlaneEpsilon=1.0e-11;
constexpr std::uint64_t kWitnessLatticeTag=0x2000000000000000ULL;
constexpr std::uint64_t kWitnessCutTag=0x3000000000000000ULL;
using WitnessTet=std::array<std::uint64_t,4>;
using WitnessFace=std::array<std::uint64_t,3>;
using WitnessTetKey=std::array<std::uint64_t,4>;

WitnessFace witness_face_key(WitnessFace face) { std::sort(face.begin(),face.end());return face; }
WitnessTetKey witness_tet_key(WitnessTet tet) { std::sort(tet.begin(),tet.end());return tet; }

struct PlaneSlice {
  SourceTet source{};
  FaceKey inner_face{};
  std::array<Vec3,3> triangle{};
  std::array<Vec3,4> parent{};
  std::array<double,4> signed_distance{};
  std::array<Vec3,6> cuts{};
  std::array<bool,6> has_cut{};
  unsigned cut_count{};
  unsigned low_count{};
  bool full_triangle_section{};
};

double signed_plane_distance(const std::array<Vec3,3>& triangle,const Vec3& p) {
  return dot(cross(triangle[1]-triangle[0],triangle[2]-triangle[0]),p-triangle[0]);
}

bool strictly_inside_triangle(const std::array<Vec3,3>& triangle,const Vec3& p) {
  const auto n=cross(triangle[1]-triangle[0],triangle[2]-triangle[0]);
  const auto a=dot(n,cross(triangle[1]-triangle[0],p-triangle[0]));
  const auto b=dot(n,cross(triangle[2]-triangle[1],p-triangle[1]));
  const auto c=dot(n,cross(triangle[0]-triangle[2],p-triangle[2]));
  return (a>kPlaneEpsilon&&b>kPlaneEpsilon&&c>kPlaneEpsilon)||
      (a<-kPlaneEpsilon&&b<-kPlaneEpsilon&&c<-kPlaneEpsilon);
}

unsigned edge_slot(unsigned a,unsigned b) {
  for(unsigned index=0U;index<tet_edges.size();++index) {
    const auto edge=tet_edges[index];
    if((edge[0]==a&&edge[1]==b)||(edge[0]==b&&edge[1]==a))return index;
  }
  throw std::logic_error("missing tetrahedron edge");
}

PlaneSlice make_slice(const SourceTet& source,FaceKey face,const std::array<Vec3,3>& triangle,
                      unsigned resolution) {
  PlaneSlice result;result.source=source;result.inner_face=witness_face_key(face);result.triangle=triangle;
  for(unsigned i=0U;i<4U;++i) {
    result.parent[i]=lattice_position({KeyKind::lattice,source[i]},resolution);
    result.signed_distance[i]=signed_plane_distance(triangle,result.parent[i]);
    if(std::abs(result.signed_distance[i])<=kPlaneEpsilon)
      throw std::logic_error("witness scan encountered a plane through lattice vertex");
    result.low_count+=result.signed_distance[i]<0.0?1U:0U;
  }
  for(unsigned edge_index=0U;edge_index<tet_edges.size();++edge_index) {
    const auto edge=tet_edges[edge_index];const auto da=result.signed_distance[edge[0]],db=result.signed_distance[edge[1]];
    if((da<0.0)==(db<0.0))continue;
    result.cuts[edge_index]=result.parent[edge[0]]+(result.parent[edge[1]]-result.parent[edge[0]])*(da/(da-db));
    result.has_cut[edge_index]=true;++result.cut_count;
  }
  result.full_triangle_section=result.cut_count==3U||result.cut_count==4U;
  for(unsigned edge=0U;edge<tet_edges.size();++edge)if(result.has_cut[edge])
    result.full_triangle_section=result.full_triangle_section&&strictly_inside_triangle(triangle,result.cuts[edge]);
  return result;
}

struct CleavedTet {
  std::map<std::uint64_t,Vec3> vertices;
  std::vector<WitnessTet> tets;
  std::set<WitnessFace> cut_faces;
};

void add_positive(CleavedTet& result,WitnessTet tet) {
  const auto six=signed_six_volume(result.vertices.at(tet[0]),result.vertices.at(tet[1]),
                                   result.vertices.at(tet[2]),result.vertices.at(tet[3]));
  if(six<0.0)std::swap(tet[1],tet[2]);
  result.tets.push_back(tet);
}

// Canonical 3-tet prism grammar.  Corresponding vertices in a and b must be
// ordered by their globally named source endpoint / cut-edge identity.
void add_prism(CleavedTet& result,std::array<std::uint64_t,3> a,std::array<std::uint64_t,3> b) {
  add_positive(result,{{a[0],a[1],a[2],b[0]}});
  add_positive(result,{{a[1],a[2],b[0],b[1]}});
  add_positive(result,{{a[2],b[0],b[1],b[2]}});
}

CleavedTet cleave_full_plane_section(const PlaneSlice& slice) {
  if(!slice.full_triangle_section)throw std::logic_error("attempted a clipped rather than full triangle section");
  CleavedTet result;
  std::array<std::uint64_t,4> source_ids{};
  for(unsigned i=0U;i<4U;++i) {
    source_ids[i]=kWitnessLatticeTag|slice.source[i];result.vertices.emplace(source_ids[i],slice.parent[i]);
  }
  std::array<std::uint64_t,6> cut_ids{};
  for(unsigned edge=0U;edge<tet_edges.size();++edge)if(slice.has_cut[edge]) {
    // This ID is only a local printable encoding.  The cross-cell identity is
    // the existing (frozen inner-face, globally sorted lattice-edge) CutKey;
    // no traversal counter participates in geometry construction.
    cut_ids[edge]=kWitnessCutTag|static_cast<std::uint64_t>(edge);
    result.vertices.emplace(cut_ids[edge],slice.cuts[edge]);
  }
  std::array<unsigned,4> low{},high{};unsigned l{},h{};
  for(unsigned i=0U;i<4U;++i)(slice.signed_distance[i]<0.0?low[l++]:high[h++])=i;
  const auto cut=[&](unsigned a,unsigned b) { return cut_ids[edge_slot(a,b)]; };
  if(l==1U||h==1U) {
    const bool lone_low=l==1U;const auto lone=lone_low?low[0]:high[0];
    const auto& triple=lone_low?high:low;
    std::array<std::uint64_t,3> section{{cut(lone,triple[0]),cut(lone,triple[1]),cut(lone,triple[2])}};
    add_positive(result,{{source_ids[lone],section[0],section[1],section[2]}});
    std::array<std::uint64_t,3> source_face{{source_ids[triple[0]],source_ids[triple[1]],source_ids[triple[2]]}};
    add_prism(result,source_face,section);
    result.cut_faces.insert(witness_face_key(section));
  } else if(l==2U&&h==2U) {
    const auto l0=low[0],l1=low[1],h0=high[0],h1=high[1];
    const auto c00=cut(l0,h0),c01=cut(l0,h1),c10=cut(l1,h0),c11=cut(l1,h1);
    add_prism(result,{{source_ids[l0],c00,c01}},{{source_ids[l1],c10,c11}});
    add_prism(result,{{source_ids[h0],c00,c10}},{{source_ids[h1],c01,c11}});
    // The quadrilateral surface uses the same stable diagonal in both half
    // prisms; its two triangles are the locally retriangulable artificial
    // interface created by this cleavage grammar.
    // `add_prism` chooses the c01--c10 diagonal on both sides of the
    // interface.  Naming that actual shared triangulation is essential: the
    // opposite c00--c11 diagonal would look plausible in isolation but leave
    // two unpaired artificial cut faces between the two half-prisms.
    result.cut_faces.insert(witness_face_key({{c00,c01,c10}}));
    result.cut_faces.insert(witness_face_key({{c01,c10,c11}}));
  } else throw std::logic_error("invalid plane-slice signature");
  return result;
}

struct CleavageAudit {
  bool positive{true},unique{true},manifold{true},opposite_sides{true},no_overlap{true};
  bool volume_conserved{true},cut_faces_paired{true};
  std::size_t nonpositive{},duplicates{},nonmanifold{},same_side{},overlaps{},unpaired_cut_faces{};
  double volume_error{};
};

CleavageAudit audit_cleavage(const PlaneSlice& parent,const CleavedTet& mesh) {
  CleavageAudit result;std::map<WitnessFace,std::vector<std::uint64_t>> uses;std::set<WitnessTetKey> unique;
  double child_volume{};
  for(const auto& tet:mesh.tets) {
    const auto six=signed_six_volume(mesh.vertices.at(tet[0]),mesh.vertices.at(tet[1]),mesh.vertices.at(tet[2]),mesh.vertices.at(tet[3]));
    if(six<=1e-13){result.positive=false;++result.nonpositive;}child_volume+=six/6.0;
    if(!unique.insert(witness_tet_key(tet)).second){result.unique=false;++result.duplicates;}
    for(unsigned f=0U;f<tet_faces.size();++f) { const auto x=tet_faces[f];uses[witness_face_key({{tet[x[0]],tet[x[1]],tet[x[2]]}})].push_back(tet[f]); }
  }
  for(const auto& [face,opposites]:uses) {
    if(opposites.size()>2U){result.manifold=false;++result.nonmanifold;}
    if(opposites.size()==2U) {
      const auto& p=mesh.vertices.at(face[0]);const auto normal=cross(mesh.vertices.at(face[1])-p,mesh.vertices.at(face[2])-p);
      if(dot(normal,mesh.vertices.at(opposites[0])-p)*dot(normal,mesh.vertices.at(opposites[1])-p)>=0.0){result.opposite_sides=false;++result.same_side;}
    }
  }
  for(const auto face:mesh.cut_faces) { const auto it=uses.find(face);if(it==uses.end()||it->second.size()!=2U){result.cut_faces_paired=false;++result.unpaired_cut_faces;} }
  DualVolumeBuild view;view.vertices=mesh.vertices;
  for(std::size_t a=0;a<mesh.tets.size();++a)for(std::size_t b=a+1U;b<mesh.tets.size();++b) {
    const DualVolumeTet left{mesh.tets[a],DualVolumeRegion::transition},right{mesh.tets[b],DualVolumeRegion::transition};
    if(dual_tets_strictly_overlap(view,left,right)){result.no_overlap=false;++result.overlaps;}
  }
  const auto parent_volume=std::abs(signed_six_volume(parent.parent[0],parent.parent[1],parent.parent[2],parent.parent[3]))/6.0;
  result.volume_error=std::abs(parent_volume-child_volume);result.volume_conserved=result.volume_error<1.0e-11;
  return result;
}

std::pair<double,std::size_t> cleavage_min_dihedral(const CleavedTet& mesh) {
  double minimum=180.0;std::size_t below{};
  for(const auto& tet:mesh.tets) {
    std::array<Vec3,4> points{};for(unsigned i=0U;i<4U;++i)points[i]=mesh.vertices.at(tet[i]);
    for(unsigned a=0U;a<tet_faces.size();++a)for(unsigned b=a+1U;b<tet_faces.size();++b) {
      const auto outward=[&](std::array<unsigned,3> face,unsigned opposite) { auto n=cross(points[face[1]]-points[face[0]],points[face[2]]-points[face[0]]);return dot(n,points[opposite]-points[face[0]])>0.0?n*-1.0:n; };
      const auto n0=outward(tet_faces[a],a),n1=outward(tet_faces[b],b);
      const auto angle=(std::numbers::pi-std::acos(std::clamp(dot(n0,n1)/(length(n0)*length(n1)),-1.0,1.0)))*180.0/std::numbers::pi;
      minimum=std::min(minimum,angle);
    }
  }
  for(const auto& tet:mesh.tets) { std::array<Vec3,4> p{};for(unsigned i=0;i<4;++i)p[i]=mesh.vertices.at(tet[i]); double local=180.0;for(unsigned a=0;a<tet_faces.size();++a)for(unsigned b=a+1U;b<tet_faces.size();++b){const auto out=[&](std::array<unsigned,3> f,unsigned o){auto n=cross(p[f[1]]-p[f[0]],p[f[2]]-p[f[0]]);return dot(n,p[o]-p[f[0]])>0?n*-1.0:n;};const auto x=out(tet_faces[a],a),y=out(tet_faces[b],b);local=std::min(local,(std::numbers::pi-std::acos(std::clamp(dot(x,y)/(length(x)*length(y)),-1.0,1.0)))*180.0/std::numbers::pi);}below+=local<5.0?1U:0U; }
  return {minimum,below};
}

struct WitnessResult {
  std::size_t strict_contacts{},full_sections{},clipped_sections{};
  bool core_separated{},deterministic{},kernel_constructed{},kernel_valid{},quality_pass{};
  CleavageAudit audit{};double min_dihedral{180.0};std::size_t below_five{},eligible_tets{},
      eligible_sections{},emitted_tets{},cut_face_count{};PlaneSlice witness{};
};

WitnessResult run_local_buffer_cleaving_witness(const SandwichConfig& config) {
  const auto collar=run_probe(config);const auto core=select_conservative_core(config);const auto universe=build_source_universe(config,2U*config.resolution);
  std::vector<PlaneSlice> full;WitnessResult result;result.core_separated=true;std::vector<PlaneSlice> clipped;
  const auto collect=[&](bool reverse,std::vector<PlaneSlice>& full_out,std::vector<PlaneSlice>& clipped_out,
                         bool& core_is_separated) {
    std::vector<SourceTet> sources(universe.tetrahedra.begin(),universe.tetrahedra.end());
    std::vector<FaceKey> faces(collar.selected.collar.expected_inner.begin(),collar.selected.collar.expected_inner.end());
    if(reverse) { std::reverse(sources.begin(),sources.end());std::reverse(faces.begin(),faces.end()); }
    for(const auto& source:sources) {
    const bool retained=core.contains(source);
    for(const auto face:faces) {
      const std::array<Vec3,3> triangle{{collar.selected.collar.vertices.at(face[0]),collar.selected.collar.vertices.at(face[1]),collar.selected.collar.vertices.at(face[2])}};
      std::array<Vec3,4> parent{};for(unsigned i=0;i<4;++i)parent[i]=lattice_position({KeyKind::lattice,source[i]},config.resolution);
      if(triangle_tet_contact(triangle,parent)!=TriangleTetContact::strict)continue;
      if(retained){core_is_separated=false;continue;}
      const auto slice=make_slice(source,face,triangle,config.resolution);
      if(slice.full_triangle_section)full_out.push_back(slice);else clipped_out.push_back(slice);
    }
    }
  };
  collect(false,full,clipped,result.core_separated);
  result.strict_contacts=full.size()+clipped.size();
  for(const auto& source:core)for(const auto face:collar.selected.collar.expected_inner) { std::array<Vec3,4> p{};for(unsigned i=0;i<4;++i)p[i]=lattice_position({KeyKind::lattice,source[i]},config.resolution);const std::array<Vec3,3> t{{collar.selected.collar.vertices.at(face[0]),collar.selected.collar.vertices.at(face[1]),collar.selected.collar.vertices.at(face[2])}};if(triangle_tet_contact(t,p)!=TriangleTetContact::none)result.core_separated=false; }
  result.full_sections=full.size();result.clipped_sections=clipped.size();
  std::vector<PlaneSlice> reverse_full,reverse_clipped;bool reverse_core=true;
  collect(true,reverse_full,reverse_clipped,reverse_core);
  const auto canonical_order=[](const PlaneSlice& a,const PlaneSlice& b){return std::tie(a.inner_face,a.source)<std::tie(b.inner_face,b.source);};
  std::sort(full.begin(),full.end(),canonical_order);std::sort(reverse_full.begin(),reverse_full.end(),canonical_order);
  std::sort(clipped.begin(),clipped.end(),canonical_order);std::sort(reverse_clipped.begin(),reverse_clipped.end(),canonical_order);
  result.deterministic=reverse_core==result.core_separated&&full.size()==reverse_full.size()&&clipped.size()==reverse_clipped.size();
  for(std::size_t i=0;i<full.size()&&result.deterministic;++i)
    { result.deterministic=result.deterministic&&full[i].inner_face==reverse_full[i].inner_face&&full[i].source==reverse_full[i].source;
      for(unsigned edge=0U;edge<tet_edges.size();++edge)
        result.deterministic=result.deterministic&&full[i].has_cut[edge]==reverse_full[i].has_cut[edge]&&
            (!full[i].has_cut[edge]||(
              std::bit_cast<std::uint64_t>(full[i].cuts[edge].x)==std::bit_cast<std::uint64_t>(reverse_full[i].cuts[edge].x)&&
              std::bit_cast<std::uint64_t>(full[i].cuts[edge].y)==std::bit_cast<std::uint64_t>(reverse_full[i].cuts[edge].y)&&
              std::bit_cast<std::uint64_t>(full[i].cuts[edge].z)==std::bit_cast<std::uint64_t>(reverse_full[i].cuts[edge].z)));
    }
  for(std::size_t i=0;i<clipped.size()&&result.deterministic;++i)
    result.deterministic=result.deterministic&&clipped[i].inner_face==reverse_clipped[i].inner_face&&clipped[i].source==reverse_clipped[i].source;
  // Quality is a property of every eligible local construction, not of the
  // printable first witness.  Retain the first one solely as a small geometry
  // regression fixture, while screening the entire sorted corpus below.
  for(const auto& section:full) {
    const auto mesh=cleave_full_plane_section(section);
    const auto [dihedral,below]=cleavage_min_dihedral(mesh);
    result.min_dihedral=std::min(result.min_dihedral,dihedral);
    result.below_five+=below;
    result.eligible_sections++;
    result.eligible_tets+=mesh.tets.size();
  }
  result.quality_pass=!full.empty()&&result.below_five==0U;
  if(!full.empty()) {
    result.witness=full.front();const auto mesh=cleave_full_plane_section(result.witness);result.audit=audit_cleavage(result.witness,mesh);
    result.emitted_tets=mesh.tets.size();result.cut_face_count=mesh.cut_faces.size();
    result.kernel_constructed=true;result.kernel_valid=result.audit.positive&&result.audit.unique&&result.audit.manifold&&result.audit.opposite_sides&&result.audit.no_overlap&&result.audit.volume_conserved&&result.audit.cut_faces_paired;
    const auto repeat=cleave_full_plane_section(result.witness);result.deterministic=result.deterministic&&mesh.tets.size()==repeat.tets.size()&&mesh.vertices.size()==repeat.vertices.size();
    for(std::size_t i=0;i<mesh.tets.size()&&result.deterministic;++i)result.deterministic=result.deterministic&&mesh.tets[i]==repeat.tets[i];
  } else { result.deterministic=true; }
  return result;
}

int local_buffer_cleaving_main(const char* fixture) {
  const auto start=std::chrono::steady_clock::now();const auto result=run_local_buffer_cleaving_witness(bridge_fixture(fixture));
  const auto elapsed=std::chrono::duration<double,std::milli>{std::chrono::steady_clock::now()-start}.count();
  const bool passed=result.core_separated&&result.strict_contacts>0U&&result.clipped_sections>0U&&result.kernel_constructed&&result.kernel_valid&&result.deterministic;
  std::cout<<std::setprecision(17)<<"{\"probe\":\"dc_local_buffer_cleaving_witness/v1\",\"fixture\":\""<<fixture<<"\","
    <<"\"contract\":{\"immutable\":[\"visible_dc_surface\",\"retained_freudenthal_core_interface\"],\"construction\":\"canonical_1_3_3_1_2_2_plane_cleavage_of_noncore_tet\",\"complete_volume\":false},"
    <<"\"contacts\":{\"strict_band_contacts\":"<<result.strict_contacts<<",\"full_triangle_sections\":"<<result.full_sections<<",\"clipped_triangle_sections\":"<<result.clipped_sections<<",\"retained_core_separated\":"<<(result.core_separated?"true":"false")<<"},"
    <<"\"bounds\":{\"constructed_cavity_source_tets\":1,\"peak_output_tets\":6,\"cross_chunk_stitching\":\"not_attempted; this is a one-tet kernel\"},"
    <<"\"kernel\":{\"constructed\":"<<(result.kernel_constructed?"true":"false")<<",\"tetrahedra\":"<<result.emitted_tets<<",\"cut_faces\":"<<result.cut_face_count<<",\"positive\":"<<(result.audit.positive?"true":"false")<<",\"unique\":"<<(result.audit.unique?"true":"false")<<",\"manifold\":"<<(result.audit.manifold?"true":"false")<<",\"opposite_sides\":"<<(result.audit.opposite_sides?"true":"false")<<",\"overlap_pairs\":"<<result.audit.overlaps<<",\"volume_error\":"<<result.audit.volume_error<<",\"cut_faces_paired\":"<<(result.audit.cut_faces_paired?"true":"false")<<",\"eligible_sections\":"<<result.eligible_sections<<",\"eligible_tets\":"<<result.eligible_tets<<",\"min_dihedral\":"<<result.min_dihedral<<",\"below_5_degrees\":"<<result.below_five<<"},"
    <<"\"result\":{\"kernel_valid\":"<<(result.kernel_valid?"true":"false")<<",\"deterministic\":"<<(result.deterministic?"true":"false")<<",\"quality_pass\":"<<(result.quality_pass?"true":"false")<<",\"next_obligation\":\"clip_and_stitch_triangle_boundaries_on_shared_grid_faces; plane-only cleavage must not extend a frozen triangle\"},\"elapsed_ms\":"<<elapsed<<",\"control_passed\":"<<(passed?"true":"false")<<"}\n";
  return passed?0:1;
}
} // namespace

#ifdef LOCAL_BUFFER_CLEAVING_WITNESS_TEST
int local_buffer_cleaving_witness_probe_main(const char* fixture) { return local_buffer_cleaving_main(fixture); }
#else
int main(int argc,char** argv) { try { return local_buffer_cleaving_main(argc>1?argv[1]:"n8"); } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 2; } }
#endif
