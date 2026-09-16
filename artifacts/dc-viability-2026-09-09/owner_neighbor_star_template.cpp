// Owner-neighbour star gate for the dominant real finite-patch signature.
//
// A P4-V0-B4-L2-E4-T3-BF11-FE10-PE30 record is not a self-contained
// one-tet plane section: its finite boundary crosses three source faces (four
// cuts, because one face is crossed twice). Its clipped polygon reaches three
// source faces, so the immediate star has the owner and three face-neighbours.
// This probe derives those entities from
// the actual N6 witness, proves that every owner derives byte-identical shared
// cut data, and then asks the non-negotiable closure question before emitting
// a single tet: does that four-tet star contain the complete frozen triangle?
//
// It does not. The frozen triangle has ten strict source-tet contacts; six
// lie beyond its one-ring. Applying a tetrahedral template to only the four
// tets would necessarily leave the frozen triangle ending on an artificial
// cavity boundary. That is neither a complete local tet complex nor a valid
// chunk seam. The rejected witness is deliberately retained as the next
// regression: a future construction must take the transitive triangle patch
// star (and its neighbouring DC faces), not silently extend its plane.

#define FINITE_PATCH_TEMPLATE_ATLAS_TEST
#include "finite_patch_template_atlas.cpp"
#undef FINITE_PATCH_TEMPLATE_ATLAS_TEST

#include <bit>
#include <chrono>
#include <iomanip>
#include <iostream>

namespace {
using namespace tetra::probes;

using StarFace=std::array<std::uint64_t,3>;
using StarEdge=std::array<std::uint64_t,2>;

StarFace star_face_key(StarFace face) { std::sort(face.begin(),face.end());return face; }
StarEdge star_edge_key(StarEdge edge) { std::sort(edge.begin(),edge.end());return edge; }

bool star_same_vec3(const Vec3& a,const Vec3& b) {
  return std::bit_cast<std::uint64_t>(a.x)==std::bit_cast<std::uint64_t>(b.x)&&
      std::bit_cast<std::uint64_t>(a.y)==std::bit_cast<std::uint64_t>(b.y)&&
      std::bit_cast<std::uint64_t>(a.z)==std::bit_cast<std::uint64_t>(b.z);
}

std::array<StarFace,4> star_source_faces(const SourceTet& source) {
  return {{{{source[1],source[2],source[3]}},{{source[0],source[3],source[2]}},
           {{source[0],source[1],source[3]}},{{source[0],source[2],source[1]}}}};
}

struct BoundaryCutKey {
  StarFace source_face{};
  StarEdge frozen_triangle_edge{};
  auto operator<=>(const BoundaryCutKey&) const=default;
};

struct BoundaryCut {
  BoundaryCutKey key{};
  Vec3 point{};
  unsigned owner_face_slot{};
};

struct LatticeCutKey {
  StarFace source_face{};
  StarEdge lattice_edge{};
  auto operator<=>(const LatticeCutKey&) const=default;
};

struct LatticeCut { LatticeCutKey key{};Vec3 point{}; };

// Use globally sorted triangle-edge endpoints before evaluating the line/face
// intersection. Thus a neighbouring owner, a reversed source traversal, and
// a reversed input triangle all execute the same floating-point operations.
std::vector<BoundaryCut> derive_boundary_cuts(const ContactRecord& record,unsigned resolution) {
  const auto faces=star_source_faces(record.source);
  std::vector<BoundaryCut> result;
  for(unsigned f=0U;f<faces.size();++f) {
    const auto ids=faces[f];
    const auto a=lattice_position({KeyKind::lattice,ids[0]},resolution);
    const auto b=lattice_position({KeyKind::lattice,ids[1]},resolution);
    const auto c=lattice_position({KeyKind::lattice,ids[2]},resolution);
    const auto normal=cross(b-a,c-a);
    for(unsigned e=0U;e<3U;++e) {
      const unsigned next=(e+1U)%3U;
      const auto edge=star_edge_key({{record.triangle[e],record.triangle[next]}});
      unsigned lo=e,hi=next;
      if(record.triangle[hi]<record.triangle[lo])std::swap(lo,hi);
      const auto p0=record.points[lo],p1=record.points[hi];
      const double d0=dot(normal,p0-a),d1=dot(normal,p1-a);
      if((d0<-kAtlasEpsilon&&d1<-kAtlasEpsilon)||(d0>kAtlasEpsilon&&d1>kAtlasEpsilon)||
          std::abs(d0-d1)<=kAtlasEpsilon)continue;
      const double t=d0/(d0-d1);
      if(t<=kAtlasEpsilon||t>=1.0-kAtlasEpsilon)continue;
      const auto point=p0+(p1-p0)*t;
      const auto u=cross(b-a,point-a),v=cross(c-b,point-b),w=cross(a-c,point-c);
      const bool inside=(dot(normal,u)>=-kAtlasEpsilon&&dot(normal,v)>=-kAtlasEpsilon&&dot(normal,w)>=-kAtlasEpsilon)||
          (dot(normal,u)<=kAtlasEpsilon&&dot(normal,v)<=kAtlasEpsilon&&dot(normal,w)<=kAtlasEpsilon);
      if(inside)result.push_back({{star_face_key(ids),edge},point,f});
    }
  }
  std::sort(result.begin(),result.end(),[](const auto& x,const auto& y) { return x.key<y.key; });
  result.erase(std::unique(result.begin(),result.end(),[](const auto& x,const auto& y) { return x.key==y.key; }),result.end());
  return result;
}

// A finite patch may also cross a source face through a lattice edge rather
// than through one of its own boundary edges.  The key names that source edge
// and the shared face; it is independent of either tet's local edge slot.
std::vector<LatticeCut> derive_finite_lattice_cuts(const ContactRecord& record,unsigned resolution) {
  const auto slice=make_slice(record.source,record.triangle,record.points,resolution);
  const auto faces=star_source_faces(record.source);std::vector<LatticeCut> result;
  for(unsigned e=0U;e<tet_edges.size();++e)if(slice.has_cut[e]&&atlas_inside_finite_triangle(record.points,slice.cuts[e])) {
    const auto local=tet_edges[e];const auto edge=star_edge_key({{record.source[local[0]],record.source[local[1]]}});
    for(const auto face:faces) {
      const bool has_a=std::find(face.begin(),face.end(),edge[0])!=face.end();
      const bool has_b=std::find(face.begin(),face.end(),edge[1])!=face.end();
      if(has_a&&has_b)result.push_back({{star_face_key(face),edge},slice.cuts[e]});
    }
  }
  std::sort(result.begin(),result.end(),[](const auto& x,const auto& y) { return x.key<y.key; });
  result.erase(std::unique(result.begin(),result.end(),[](const auto& x,const auto& y) { return x.key==y.key; }),result.end());
  return result;
}

std::map<StarFace,std::vector<SourceTet>> source_face_adjacency(const SourceUniverse& universe) {
  std::map<StarFace,std::vector<SourceTet>> result;
  for(const auto& source:universe.tetrahedra)
    for(const auto face:star_source_faces(source))result[star_face_key(face)].push_back(source);
  for(auto& [face,sources]:result)std::sort(sources.begin(),sources.end());
  return result;
}

struct FixtureGeometry {
  SandwichConfig config{};
  SourceUniverse universe{};
  std::map<StarFace,std::vector<SourceTet>> adjacency{};
  const std::vector<ContactRecord>* clipped_records{};
  std::map<AtlasFace,std::vector<ContactRecord>> strict_contact_cache{};
};

std::vector<ContactRecord>& all_strict_contacts_for_triangle(FixtureGeometry& fixture,const ContactRecord& owner) {
  if(const auto existing=fixture.strict_contact_cache.find(owner.triangle);existing!=fixture.strict_contact_cache.end())return existing->second;
  const auto& config=fixture.config;const auto triangle=owner.triangle;const auto& points=owner.points;
  std::vector<ContactRecord> result;
  for(const auto& source:fixture.universe.tetrahedra) {
    std::array<Vec3,4> tet{};for(unsigned i=0U;i<4U;++i)tet[i]=lattice_position({KeyKind::lattice,source[i]},config.resolution);
    if(triangle_tet_contact(points,tet)!=TriangleTetContact::strict)continue;
    const auto polygon=clip_triangle_to_tet(points,tet);
    ArrangementSignature signature{};signature.polygon_vertices=static_cast<unsigned>(polygon.size());
    for(const auto& v:polygon) {
      if(v.kind==AtlasVertexKind::triangle_vertex)++signature.triangle_vertices_inside;
      else if(v.kind==AtlasVertexKind::boundary_face) { ++signature.boundary_face_crossings;signature.boundary_face_mask=static_cast<std::uint8_t>(signature.boundary_face_mask|(1U<<v.a)); }
    }
    const auto slice=make_slice(source,triangle,points,config.resolution);signature.plane_edge_cuts=slice.cut_count;
    for(unsigned e=0U;e<tet_edges.size();++e)if(slice.has_cut[e]) {
      signature.plane_lattice_edge_mask=static_cast<std::uint8_t>(signature.plane_lattice_edge_mask|(1U<<e));
      if(atlas_inside_finite_triangle(points,slice.cuts[e])) { ++signature.strict_lattice_edge_cuts;signature.finite_lattice_edge_mask=static_cast<std::uint8_t>(signature.finite_lattice_edge_mask|(1U<<e)); }
    }
    result.push_back({signature,source,triangle,points});
  }
  std::sort(result.begin(),result.end(),[](const auto& a,const auto& b) { return a.source<b.source; });
  return fixture.strict_contact_cache.emplace(triangle,std::move(result)).first->second;
}

struct StarAttempt {
  bool seam_keys_identical{true};
  bool seam_positions_identical{true};
  bool deterministic{true};
  bool scaffold_positive{true};
  bool scaffold_unique{true};
  bool scaffold_nonoverlap{true};
  bool frozen_triangle_closed{};
  bool template_accepted{};
  std::size_t immediate_star_tets{};
  std::size_t finite_boundary_cuts{};
  std::size_t finite_lattice_entities{};
  std::size_t frozen_triangle_contacts{};
  std::size_t contacts_outside_star{};
  std::size_t auxiliary_local_triangles{};
  std::string rejection{};
};

StarAttempt attempt_one_ring_star(FixtureGeometry& fixture,const ContactRecord& owner,bool reverse) {
  StarAttempt out;const auto& config=fixture.config;const auto cuts=derive_boundary_cuts(owner,config.resolution);
  const auto lattice_cuts=derive_finite_lattice_cuts(owner,config.resolution);
  out.finite_boundary_cuts=cuts.size();
  out.finite_lattice_entities=lattice_cuts.size();
  std::set<SourceTet> star{owner.source};const auto& all=all_strict_contacts_for_triangle(fixture,owner);
  std::set<StarFace> crossed_faces;
  std::array<Vec3,4> owner_tet{};for(unsigned i=0U;i<4U;++i)owner_tet[i]=lattice_position({KeyKind::lattice,owner.source[i]},config.resolution);
  for(const auto& vertex:clip_triangle_to_tet(owner.points,owner_tet))if(vertex.kind==AtlasVertexKind::boundary_face)
    crossed_faces.insert(star_face_key(star_source_faces(owner.source)[vertex.a]));
  for(const auto& face:crossed_faces) {
    const auto it=fixture.adjacency.find(face);
    if(it==fixture.adjacency.end()||it->second.size()!=2U) { out.seam_keys_identical=false;continue; }
    const auto& owners=it->second;const auto neighbor=owners[0]==owner.source?owners[1]:owners[0];star.insert(neighbor);
    const auto source_it=std::find_if(all.begin(),all.end(),[&](const auto& x) { return x.source==neighbor; });
    if(source_it==all.end()) { out.seam_keys_identical=false;continue; }
    const auto neighbour_cuts=derive_boundary_cuts(*source_it,config.resolution);
    for(const auto& cut:cuts)if(cut.key.source_face==face) {
      const auto match=std::find_if(neighbour_cuts.begin(),neighbour_cuts.end(),[&](const auto& x) { return x.key==cut.key; });
      if(match==neighbour_cuts.end())out.seam_keys_identical=false;
      else out.seam_positions_identical=out.seam_positions_identical&&star_same_vec3(cut.point,match->point);
    }
    const auto neighbour_lattice=derive_finite_lattice_cuts(*source_it,config.resolution);
    for(const auto& cut:lattice_cuts)if(cut.key.source_face==face) {
      const auto match=std::find_if(neighbour_lattice.begin(),neighbour_lattice.end(),[&](const auto& x) { return x.key==cut.key; });
      if(match==neighbour_lattice.end())out.seam_keys_identical=false;
      else out.seam_positions_identical=out.seam_positions_identical&&star_same_vec3(cut.point,match->point);
    }
  }
  out.immediate_star_tets=star.size();out.frozen_triangle_contacts=all.size();
  for(const auto& x:all)if(!star.contains(x.source))++out.contacts_outside_star;
  for(const auto& x:*fixture.clipped_records)if(x.source==owner.source&&x.triangle!=owner.triangle)++out.auxiliary_local_triangles;
  std::vector<SourceTet> ordered(star.begin(),star.end());if(reverse)std::reverse(ordered.begin(),ordered.end());
  DualVolumeBuild view;std::set<std::array<std::uint64_t,4>> unique;
  for(const auto& source:ordered) {
    std::array<std::uint64_t,4> tet{};for(unsigned i=0U;i<4U;++i) { tet[i]=source[i];view.vertices.emplace(tet[i],lattice_position({KeyKind::lattice,source[i]},config.resolution)); }
    const auto six=signed_six_volume(view.vertices.at(tet[0]),view.vertices.at(tet[1]),view.vertices.at(tet[2]),view.vertices.at(tet[3]));
    out.scaffold_positive=out.scaffold_positive&&std::abs(six)>kAtlasEpsilon;
    auto key=tet;std::sort(key.begin(),key.end());out.scaffold_unique=out.scaffold_unique&&unique.insert(key).second;
    view.tetrahedra.push_back({tet,DualVolumeRegion::transition});
  }
  for(std::size_t a=0U;a<view.tetrahedra.size();++a)for(std::size_t b=a+1U;b<view.tetrahedra.size();++b)
    if(dual_tets_strictly_overlap(view,view.tetrahedra[a],view.tetrahedra[b]))out.scaffold_nonoverlap=false;
  out.frozen_triangle_closed=out.contacts_outside_star==0U;
  out.deterministic=out.seam_keys_identical&&out.seam_positions_identical&&out.scaffold_positive&&out.scaffold_unique&&out.scaffold_nonoverlap;
  if(!out.frozen_triangle_closed)out.rejection="frozen triangle escapes immediate owner-neighbour star";
  else if(out.auxiliary_local_triangles!=2U)out.rejection="unexpected local frozen-triangle arrangement";
  else out.rejection="no finite-patch tet template has been admitted";
  return out;
}

struct StarResult {
  bool atlas_witness_matches{};bool witness_seams_match{};bool witness_deterministic{};bool witness_scaffold_valid{};bool witness_rejected{};bool reverse_byte_identical{};
  std::size_t matching_records{},accepted{},rejected_escape{},rejected_other{};StarAttempt witness{};
};

bool same_attempt(const StarAttempt& a,const StarAttempt& b) {
  return a.seam_keys_identical==b.seam_keys_identical&&a.seam_positions_identical==b.seam_positions_identical&&
      a.immediate_star_tets==b.immediate_star_tets&&a.finite_boundary_cuts==b.finite_boundary_cuts&&
      a.finite_lattice_entities==b.finite_lattice_entities&&
      a.frozen_triangle_contacts==b.frozen_triangle_contacts&&a.contacts_outside_star==b.contacts_outside_star&&
      a.auxiliary_local_triangles==b.auxiliary_local_triangles&&a.rejection==b.rejection;
}

StarResult run_owner_neighbor_star_template() {
  StarResult out;const auto atlas=run_finite_patch_template_atlas();
  const ArrangementSignature expected{4U,0U,4U,2U,4U,3U,11U,10U,30U};
  out.atlas_witness_matches=atlas.selected==expected&&atlas.witness_source==SourceTet{{51U,58U,59U,108U}}&&atlas.witness_triangle==AtlasFace{{7U,79U,91U}};
  std::vector<std::pair<SandwichConfig,ContactRecord>> selected;
  std::array<FixtureGeometry,5> fixtures{};
  for(std::size_t i=0U;i<fixtures.size();++i) {
    fixtures[i].config=bridge_fixture(atlas.fixtures[i].name.c_str());
    fixtures[i].universe=build_source_universe(fixtures[i].config,2U*fixtures[i].config.resolution);
    fixtures[i].adjacency=source_face_adjacency(fixtures[i].universe);
    fixtures[i].clipped_records=&atlas.fixtures[i].records;
    for(const auto& record:atlas.fixtures[i].records)if(record.signature==expected)selected.push_back({fixtures[i].config,record});
  }
  std::sort(selected.begin(),selected.end(),[](const auto& a,const auto& b) { return std::tie(a.first.resolution,a.first.phase_x,a.first.phase_y,a.second.source,a.second.triangle)<std::tie(b.first.resolution,b.first.phase_x,b.first.phase_y,b.second.source,b.second.triangle); });
  out.matching_records=selected.size();
  for(const auto& [config,record]:selected) {
    const auto fixture_index=std::find_if(fixtures.begin(),fixtures.end(),[&](const auto& x) { return x.config.resolution==config.resolution&&x.config.phase_x==config.phase_x&&x.config.phase_y==config.phase_y; });
    if(fixture_index==fixtures.end())throw std::logic_error("missing cached fixture");
    const auto attempt=attempt_one_ring_star(*fixture_index,record,false);
    if(attempt.template_accepted)++out.accepted;
    else if(attempt.rejection=="frozen triangle escapes immediate owner-neighbour star")++out.rejected_escape;
    else ++out.rejected_other;
    if(config.resolution==6U&&record.source==atlas.witness_source&&record.triangle==atlas.witness_triangle) {
      out.witness=attempt;const auto reverse=attempt_one_ring_star(*fixture_index,record,true);
      out.witness_seams_match=attempt.seam_keys_identical&&attempt.seam_positions_identical;
      out.witness_scaffold_valid=attempt.scaffold_positive&&attempt.scaffold_unique&&attempt.scaffold_nonoverlap;
      out.witness_rejected=!attempt.template_accepted&&attempt.rejection=="frozen triangle escapes immediate owner-neighbour star";
      out.witness_deterministic=attempt.deterministic;out.reverse_byte_identical=same_attempt(attempt,reverse);
    }
  }
  return out;
}

int owner_neighbor_star_template_main() {
  const auto start=std::chrono::steady_clock::now();const auto r=run_owner_neighbor_star_template();
  std::cout<<std::setprecision(17)
    <<"{\"probe\":\"dc_owner_neighbor_star_template/v1\",\"contract\":{\"immutable\":[\"finite_dc_triangles\",\"regular_source_tet_boundary\"],\"external_tetrahedralizer\":false,\"complete_transition\":false},"
    <<"\"witness\":{\"source_tet\":[51,58,59,108],\"frozen_triangle\":[7,79,91],\"finite_boundary_cuts\":"<<r.witness.finite_boundary_cuts<<",\"finite_lattice_face_entities\":"<<r.witness.finite_lattice_entities<<",\"immediate_star_tets\":"<<r.witness.immediate_star_tets<<",\"strict_triangle_contacts\":"<<r.witness.frozen_triangle_contacts<<",\"contacts_outside_star\":"<<r.witness.contacts_outside_star<<",\"other_local_frozen_triangles\":"<<r.witness.auxiliary_local_triangles<<"},"
    <<"\"invariants\":{\"atlas_witness_matches\":"<<(r.atlas_witness_matches?"true":"false")<<",\"shared_face_edge_cut_keys_identical\":"<<(r.witness_seams_match?"true":"false")<<",\"reversed_owner_traversal_byte_identical\":"<<(r.reverse_byte_identical?"true":"false")<<",\"raw_star_positive_unique_nonoverlap\":"<<(r.witness_scaffold_valid?"true":"false")<<"},"
    <<"\"corpus\":{\"matching_signature_records\":"<<r.matching_records<<",\"accepted\":"<<r.accepted<<",\"rejected_frozen_triangle_escapes_one_ring\":"<<r.rejected_escape<<",\"rejected_other\":"<<r.rejected_other<<"},"
    <<"\"result\":{\"template_emitted\":false,\"witness_rejected\":"<<(r.witness_rejected?"true":"false")<<",\"reason\":\"a four-tet owner-neighbour star does not contain the complete finite DC triangle; emitting tets would leave a frozen facet ending on an artificial boundary\",\"next_obligation\":\"build a transitive triangle-patch star and include the neighbouring frozen DC arrangement before admitting a constrained cavity template\"},"
    <<"\"elapsed_ms\":"<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<"}\n";
  return r.atlas_witness_matches&&r.witness_seams_match&&r.witness_deterministic&&r.witness_scaffold_valid&&r.witness_rejected&&r.reverse_byte_identical&&r.matching_records==384U&&r.accepted==0U&&r.rejected_escape==384U?0:1;
}
} // namespace

#ifdef OWNER_NEIGHBOR_STAR_TEMPLATE_TEST
int owner_neighbor_star_template_probe_main() { return owner_neighbor_star_template_main(); }
#else
int main() { try { return owner_neighbor_star_template_main(); } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 2; } }
#endif
