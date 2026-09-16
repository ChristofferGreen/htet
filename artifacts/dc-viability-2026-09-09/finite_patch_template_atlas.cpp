// Finite DC-patch arrangement atlas and one bounded real-corpus template.
//
// The existing one-tet kernel deliberately rejects a finite triangle whenever
// its plane section reaches beyond the triangle boundary.  This probe names
// those cases canonically before attempting more templates.  A signature is
// entirely geometric/topological: clipped-polygon vertex origins, strict
// lattice-edge cuts, triangle-boundary/tet-face crossings, and the number of
// frozen triangles meeting the source tet.  No source traversal index or
// floating point coordinate participates in its key.

#define LOCAL_BUFFER_CLEAVING_WITNESS_TEST
#include "local_buffer_cleaving_witness.cpp"
#undef LOCAL_BUFFER_CLEAVING_WITNESS_TEST

#include <bit>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numbers>

namespace {
using namespace tetra::probes;

constexpr double kAtlasEpsilon=1.0e-10;
using AtlasFace=std::array<std::uint64_t,3>;
using AtlasTet=std::array<std::uint64_t,4>;

enum class AtlasVertexKind : std::uint8_t { triangle_vertex, boundary_face, lattice_edge };

struct AtlasVertex { Vec3 p{}; AtlasVertexKind kind{}; unsigned a{}; unsigned b{}; };

// Sutherland-Hodgman clipping, retaining the provenance of each resulting
// polygon vertex.  `boundary_face` means an actual finite triangle boundary
// crossed a tet face; `lattice_edge` means the tet edge pierces the frozen
// triangle plane.  The latter is only accepted as an exact frozen-patch event
// if the point lies in the finite triangle.
std::vector<AtlasVertex> clip_triangle_to_tet(const std::array<Vec3,3>& tri,
                                               const std::array<Vec3,4>& tet) {
  constexpr std::array<std::array<unsigned,3>,4> faces{{{{1U,2U,3U}},{{0U,3U,2U}},{{0U,1U,3U}},{{0U,2U,1U}}}};
  std::array<double,4> orientation{};
  for(unsigned f=0;f<4;++f) { const auto x=faces[f];orientation[f]=signed_six_volume(tet[x[0]],tet[x[1]],tet[x[2]],tet[f]); }
  std::vector<AtlasVertex> polygon;
  for(unsigned v=0;v<3;++v)polygon.push_back({tri[v],AtlasVertexKind::triangle_vertex,v,0U});
  for(unsigned f=0;f<4;++f) {
    std::vector<AtlasVertex> out;if(polygon.empty())break;
    const auto x=faces[f];
    const auto value=[&](const Vec3& p) { const auto raw=signed_six_volume(tet[x[0]],tet[x[1]],tet[x[2]],p);return orientation[f]>0.0?raw:-raw; };
    AtlasVertex prev=polygon.back();double pv=value(prev.p);
    for(const auto cur:polygon) {
      const double cv=value(cur.p);const bool pi=pv>=-kAtlasEpsilon,ci=cv>=-kAtlasEpsilon;
      if(pi!=ci) {
        const auto t=pv/(pv-cv); // exact same directed edge evaluation as its owner
        out.push_back({prev.p+(cur.p-prev.p)*t,AtlasVertexKind::boundary_face,f,0U});
      }
      if(ci)out.push_back(cur);prev=cur;pv=cv;
    }
    polygon=std::move(out);
  }
  return polygon;
}

bool atlas_inside_finite_triangle(const std::array<Vec3,3>& tri,const Vec3& p) {
  const auto n=cross(tri[1]-tri[0],tri[2]-tri[0]);
  if(std::abs(dot(n,p-tri[0]))>kAtlasEpsilon)return false;
  const auto a=dot(n,cross(tri[1]-tri[0],p-tri[0]));
  const auto b=dot(n,cross(tri[2]-tri[1],p-tri[1]));
  const auto c=dot(n,cross(tri[0]-tri[2],p-tri[2]));
  return (a>=-kAtlasEpsilon&&b>=-kAtlasEpsilon&&c>=-kAtlasEpsilon)||
         (a<=kAtlasEpsilon&&b<=kAtlasEpsilon&&c<=kAtlasEpsilon);
}

struct ArrangementSignature {
  unsigned polygon_vertices{};
  unsigned triangle_vertices_inside{};
  unsigned boundary_face_crossings{};
  unsigned strict_lattice_edge_cuts{};
  unsigned plane_edge_cuts{};
  unsigned local_triangle_count{};
  // Face and edge slots are defined against the canonically sorted SourceTet;
  // they identify *where* the finite boundary enters, not merely how often.
  std::uint8_t boundary_face_mask{};
  std::uint8_t finite_lattice_edge_mask{};
  std::uint8_t plane_lattice_edge_mask{};
  auto operator<=>(const ArrangementSignature&) const=default;
};

std::string signature_name(const ArrangementSignature& s) {
  return "P"+std::to_string(s.polygon_vertices)+"-V"+std::to_string(s.triangle_vertices_inside)+
    "-B"+std::to_string(s.boundary_face_crossings)+"-L"+std::to_string(s.strict_lattice_edge_cuts)+
    "-E"+std::to_string(s.plane_edge_cuts)+"-T"+std::to_string(s.local_triangle_count)+
    "-BF"+std::to_string(s.boundary_face_mask)+"-FE"+std::to_string(s.finite_lattice_edge_mask)+"-PE"+std::to_string(s.plane_lattice_edge_mask);
}

struct ContactRecord { ArrangementSignature signature{}; SourceTet source{}; AtlasFace triangle{}; std::array<Vec3,3> points{}; };

std::vector<ContactRecord> atlas_contacts(const SandwichConfig& config) {
  const auto collar=run_probe(config);const auto core=select_conservative_core(config);const auto universe=build_source_universe(config,2U*config.resolution);
  struct Raw { SourceTet source{};AtlasFace face{};std::array<Vec3,3> tri{}; };
  std::vector<Raw> raw;
  for(const auto& source:universe.tetrahedra) {
    if(core.contains(source))continue;
    std::array<Vec3,4> tet{};for(unsigned i=0;i<4;++i)tet[i]=lattice_position({KeyKind::lattice,source[i]},config.resolution);
    for(const auto face:collar.selected.collar.expected_inner) {
      const std::array<Vec3,3> tri{{collar.selected.collar.vertices.at(face[0]),collar.selected.collar.vertices.at(face[1]),collar.selected.collar.vertices.at(face[2])}};
      if(triangle_tet_contact(tri,tet)==TriangleTetContact::strict)raw.push_back({source,face,tri});
    }
  }
  std::map<SourceTet,unsigned> counts;for(const auto& r:raw)++counts[r.source];
  std::vector<ContactRecord> result;
  for(const auto& r:raw) {
    std::array<Vec3,4> tet{};for(unsigned i=0;i<4;++i)tet[i]=lattice_position({KeyKind::lattice,r.source[i]},config.resolution);
    const auto polygon=clip_triangle_to_tet(r.tri,tet);ArrangementSignature s{};s.polygon_vertices=static_cast<unsigned>(polygon.size());s.local_triangle_count=counts.at(r.source);
    for(const auto& v:polygon) { if(v.kind==AtlasVertexKind::triangle_vertex)++s.triangle_vertices_inside;else if(v.kind==AtlasVertexKind::boundary_face) {++s.boundary_face_crossings;s.boundary_face_mask=static_cast<std::uint8_t>(s.boundary_face_mask|(1U<<v.a));} }
    const auto slice=make_slice(r.source,r.face,r.tri,config.resolution);s.plane_edge_cuts=slice.cut_count;
    for(unsigned e=0;e<tet_edges.size();++e)if(slice.has_cut[e]) {s.plane_lattice_edge_mask=static_cast<std::uint8_t>(s.plane_lattice_edge_mask|(1U<<e));if(atlas_inside_finite_triangle(r.tri,slice.cuts[e])) {++s.strict_lattice_edge_cuts;s.finite_lattice_edge_mask=static_cast<std::uint8_t>(s.finite_lattice_edge_mask|(1U<<e));}}
    if(!slice.full_triangle_section)result.push_back({s,r.source,r.face,r.tri});
  }
  std::sort(result.begin(),result.end(),[](const auto& a,const auto& b) { return std::tie(a.signature,a.source,a.triangle)<std::tie(b.signature,b.source,b.triangle); });
  return result;
}

struct AtlasFixture { std::string name;std::map<ArrangementSignature,std::size_t> histogram;std::vector<ContactRecord> records; };

AtlasFixture scan_fixture(const char* fixture) { AtlasFixture out;out.name=fixture;out.records=atlas_contacts(bridge_fixture(fixture));for(const auto& r:out.records)++out.histogram[r.signature];return out; }

// The leading exact corpus class is P4-V0-B4-L2-E4-T3-BF11-FE10-PE30: a
// quadrilateral finite patch crosses four tet faces and covers only two of
// the plane's four lattice-edge cuts.  It cannot use the old plane cleavage:
// the two remaining cuts are outside the frozen finite patch.
struct AtlasResult {
  std::array<AtlasFixture,5> fixtures;ArrangementSignature selected{};std::size_t selected_total{};
  bool deterministic{},template_valid{},quality{},plane_template_rejected{};
  SourceTet witness_source{};AtlasFace witness_triangle{};unsigned witness_plane_cuts{},witness_finite_cuts{};
};
AtlasResult run_finite_patch_template_atlas() {
  AtlasResult out;constexpr std::array<const char*,5> names{{"n6","n8","n8-nearzero","n8-phase2","n8-phase3"}};
  std::map<ArrangementSignature,std::size_t> total;
  for(std::size_t i=0;i<names.size();++i){out.fixtures[i]=scan_fixture(names[i]);for(const auto& [s,n]:out.fixtures[i].histogram)total[s]+=n;}
  // Deterministic tie break is the structural signature, never first-seen.
  for(const auto& [s,n]:total)if(n>out.selected_total||(n==out.selected_total&&s<out.selected)){out.selected=s;out.selected_total=n;}
  const auto& records=out.fixtures[0].records;const auto it=std::find_if(records.begin(),records.end(),[&](const auto& r){return r.signature==out.selected;});
  // Selection might be absent from N6.  Use the first canonical fixture that contains it.
  const ContactRecord* chosen=it==records.end()?nullptr:&*it;
  if(!chosen)for(const auto& fixture:out.fixtures) { const auto hit=std::find_if(fixture.records.begin(),fixture.records.end(),[&](const auto& r){return r.signature==out.selected;});if(hit!=fixture.records.end()){chosen=&*hit;break;} }
  out.deterministic=chosen!=nullptr;
  if(chosen) {
    out.witness_source=chosen->source;out.witness_triangle=chosen->triangle;
    const auto slice=make_slice(chosen->source,chosen->triangle,chosen->points,bridge_fixture(out.fixtures[0].name.c_str()).resolution);
    out.witness_plane_cuts=slice.cut_count;out.witness_finite_cuts=chosen->signature.strict_lattice_edge_cuts;
    // A finite patch whose plane has additional tet-edge cuts cannot be fed
    // to 1:3/3:1/2:2: that grammar would emit those outside points as frozen
    // interface vertices, necessarily extending the DC triangle.  Reject it
    // before emission rather than making a valid-looking wrong surface.
    out.plane_template_rejected=out.witness_plane_cuts>out.witness_finite_cuts;
  }
  return out;
}

int finite_patch_template_atlas_main() {
  const auto start=std::chrono::steady_clock::now();const auto r=run_finite_patch_template_atlas();
  std::cout<<std::setprecision(17)<<"{\"probe\":\"dc_finite_patch_template_atlas/v1\",\"histograms\":{";
  for(std::size_t i=0;i<r.fixtures.size();++i){if(i)std::cout<<',';std::cout<<'\"'<<r.fixtures[i].name<<"\":{";bool first=true;for(const auto& [s,n]:r.fixtures[i].histogram){if(!first)std::cout<<',';first=false;std::cout<<'\"'<<signature_name(s)<<"\":"<<n;}std::cout<<'}';}
  std::cout<<"},\"selected\":{\"signature\":\""<<signature_name(r.selected)<<"\",\"count\":"<<r.selected_total<<",\"witness_source\":["<<r.witness_source[0]<<','<<r.witness_source[1]<<','<<r.witness_source[2]<<','<<r.witness_source[3]<<"],\"witness_triangle\":["<<r.witness_triangle[0]<<','<<r.witness_triangle[1]<<','<<r.witness_triangle[2]<<"]},\"bounded_attempt\":{\"construction\":\"existing_1_3_3_1_2_2_plane_template\",\"plane_edge_cuts\":"<<r.witness_plane_cuts<<",\"finite_triangle_edge_cuts\":"<<r.witness_finite_cuts<<",\"rejected_before_emission\":"<<(r.plane_template_rejected?"true":"false")<<",\"reason\":\"some plane cuts lie outside the finite frozen patch; emitting them would extend the DC surface\",\"required_cavity\":\"at least the owner-neighbor star around two boundary-face cuts; one source tet cannot close the patch\"},\"bounds\":{\"classified_source_tets\":1,\"frozen_triangle_halo_cells\":1,\"output_tets\":0,\"peak_new_vertices\":0},\"result\":{\"atlas_deterministic\":"<<(r.deterministic?"true":"false")<<",\"template_accepted\":false,\"quality_evaluated\":false,\"scope\":\"real common regression identified; an owner-neighbor finite-patch cavity template is required before construction\"},\"elapsed_ms\":"<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<"}\n";
  return r.deterministic&&r.plane_template_rejected?0:1;
}
} // namespace

#ifdef FINITE_PATCH_TEMPLATE_ATLAS_TEST
int finite_patch_template_atlas_probe_main() { return finite_patch_template_atlas_main(); }
#else
int main() { try{return finite_patch_template_atlas_main();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;} }
#endif
