// Targeted final S4 repair search for the three remaining N6 regions.
// This is deliberately a separate experiment while its candidate family is
// being measured against the authoritative non-star baseline.
#define N6_NONSTAR_CAVITY_TETRAHEDRALIZER_TEST
#include "n6_nonstar_cavity_tetrahedralizer_probe.cpp"
#undef N6_NONSTAR_CAVITY_TETRAHEDRALIZER_TEST

#include <iostream>
#include <iomanip>

namespace {
using namespace tetra::probes;

Vec3 centroid(const Domain& d, const Region& r) {
  Vec3 p{}; for (const auto id : r.vertices) p=p+d.vertices.at(id);
  return p/static_cast<double>(r.vertices.size());
}
bool in_cavity_kernel(const Domain& d,const Region& r,const Vec3& p) {
  for(const auto& f:r.boundary) {
    std::uint64_t opposite{}; bool found{};
    for(const auto cell:r.cells) {
      const auto& t=d.tets[cell]; bool owns=true; for(const auto id:f) if(std::find(t.begin(),t.end(),id)==t.end()) owns=false;
      if(!owns)continue; for(const auto id:t) if(id!=f[0]&&id!=f[1]&&id!=f[2]) {opposite=id;found=true;break;} break;
    }
    if(!found)return false;
    const auto& a=d.vertices.at(f[0]); const auto n=cross(d.vertices.at(f[1])-a,d.vertices.at(f[2])-a);
    const double interior=dot(n,d.vertices.at(opposite)-a), candidate=dot(n,p-a);
    if(interior*candidate<=1e-12)return false;
  }
  return true;
}

// Exhaustive deterministic local stencil.  Each point is first subjected to
// the whole-domain geometry test, so a quality score cannot promote a cone
// that exits its cavity.
[[maybe_unused]] void scan_star(const Domain& base, [[maybe_unused]] std::size_t shell, const Region& r, std::size_t ri) {
  const auto c=centroid(base,r); Vec3 lo=c,hi=c;
  for(const auto id:r.vertices) { const auto& p=base.vertices.at(id); lo.x=std::min(lo.x,p.x);lo.y=std::min(lo.y,p.y);lo.z=std::min(lo.z,p.z);hi.x=std::max(hi.x,p.x);hi.y=std::max(hi.y,p.y);hi.z=std::max(hi.z,p.z); }
  const Vec3 span=hi-lo; Summary best{}; best.minimum=-1; Vec3 bestp{}; std::size_t valid{};
  for(int ix=-8;ix<=8;++ix)for(int iy=-8;iy<=8;++iy)for(int iz=-8;iz<=8;++iz) {
    const Vec3 p{c.x+span.x*static_cast<double>(ix)/32.0,c.y+span.y*static_cast<double>(iy)/32.0,c.z+span.z*static_cast<double>(iz)/32.0};
    const auto apex=kGeneratedTag|0x6f000000ULL|ri;
    if(!in_cavity_kernel(base,r,p))continue;
    ++valid; const auto q=region_quality(base,r,apex,p,true);
    if(best.minimum<0 || better(q,best)) {best=q;bestp=p;}
  }
  std::cout<<std::setprecision(17)<<"region "<<ri<<" valid "<<valid<<" min "<<best.minimum<<" below5 "<<best.below_five<<" p "<<bestp.x<<" "<<bestp.y<<" "<<bestp.z<<"\n";
}

struct ApexChoice { bool found{}; Vec3 p{}; Summary quality{}; std::size_t points_tested{}; };
Region make_subregion(const Domain& d,const std::vector<std::size_t>& cells);

// This fixed barycentric stencil is expressed in the input cavity's own
// affine frame.  Unlike the former saved coordinates it therefore follows a
// rigidly transformed input exactly.  It is deliberately finite: a caller
// receives an ordinary failed candidate if no point meets the quality gate.
ApexChoice choose_kernel_apex(const Domain& d, const Region& r,
                               std::uint64_t apex,
                               const std::vector<Tet>& context) {
  const auto c=centroid(d,r); Vec3 lo=c,hi=c;
  for(const auto id:r.vertices) { const auto& p=d.vertices.at(id);
    lo.x=std::min(lo.x,p.x); lo.y=std::min(lo.y,p.y); lo.z=std::min(lo.z,p.z);
    hi.x=std::max(hi.x,p.x); hi.y=std::max(hi.y,p.y); hi.z=std::max(hi.z,p.z); }
  const Vec3 span=hi-lo; ApexChoice best{}; best.quality.minimum=-1.0;
  for(int ix=-8;ix<=8;++ix) for(int iy=-8;iy<=8;++iy) for(int iz=-8;iz<=8;++iz) {
    const Vec3 p{c.x+span.x*static_cast<double>(ix)/32.0,
                 c.y+span.y*static_cast<double>(iy)/32.0,
                 c.z+span.z*static_cast<double>(iz)/32.0};
    if(!in_cavity_kernel(d,r,p)) continue;
    ++best.points_tested;
    Domain view=d; view.vertices.emplace(apex,p); auto tets=context;
    for(const auto& f:r.boundary) tets.push_back(positive(view,{{f[0],f[1],f[2],apex}}));
    const auto q=quality_of_tets(view,tets);
    if(!best.found || better(q,best.quality)) best.found=true,best.p=p,best.quality=q;
  }
  return best;
}

bool cells_are_face_connected(const Domain& d,const std::vector<std::size_t>& cells) {
  if(cells.empty()) return false;
  std::set<std::size_t> pending(cells.begin(),cells.end());
  std::vector<std::size_t> todo{*pending.begin()}; pending.erase(pending.begin());
  while(!todo.empty()) { const auto cell=todo.back(); todo.pop_back();
    for(const auto other:std::vector<std::size_t>(pending.begin(),pending.end())) {
      std::size_t common{}; for(const auto a:d.tets[cell]) for(const auto b:d.tets[other]) if(a==b) ++common;
      if(common==3U) { pending.erase(other); todo.push_back(other); }
    }
  }
  return pending.empty();
}

struct SubconeChoice { bool found{}; std::vector<std::size_t> cells; Vec3 p{}; Summary quality{}; std::size_t subsets_tested{},points_tested{}; };

SubconeChoice choose_subcone(const Domain& d,const Region& whole,std::uint64_t apex) {
  SubconeChoice best{}; best.quality.minimum=-1.0;
  if(whole.cells.size()>8U) throw std::runtime_error("repair limit exceeded: subcavity cells");
  const auto combinations=std::uint64_t{1}<<whole.cells.size();
  for(std::uint64_t mask=1;mask+1<combinations;++mask) {
    std::vector<std::size_t> cells; for(std::size_t bit=0;bit<whole.cells.size();++bit)
      if((mask>>bit)&1U) cells.push_back(whole.cells[bit]);
    if(!cells_are_face_connected(d,cells)) continue;
    ++best.subsets_tested;
    const auto sub=make_subregion(d,cells);
    std::vector<Tet> context; for(const auto cell:whole.cells)
      if(!std::binary_search(cells.begin(),cells.end(),cell)) context.push_back(d.tets[cell]);
    const auto choice=choose_kernel_apex(d,sub,apex,context); best.points_tested+=choice.points_tested;
    if(choice.found && (!best.found || better(choice.quality,best.quality))) {
      best.found=true; best.cells=std::move(cells); best.p=choice.p; best.quality=choice.quality;
    }
  }
  return best;
}

struct StellarChoice { std::size_t cell{}; Vec3 p{}; Summary quality{}; };
[[maybe_unused]] StellarChoice scan_one_stellar_split(const Domain& d,const Region& r) {
  StellarChoice best{}; best.quality.minimum=-1.0;
  for(const auto cell:r.cells) {
    const auto& old=d.tets[cell];
    for(int a=1;a<=7;++a)for(int b=1;b<=8-a;++b)for(int c=1;c<=8-a-b;++c) {
      const int e=8-a-b-c; if(e<1)continue;
      const auto p=(d.vertices.at(old[0])*a+d.vertices.at(old[1])*b+d.vertices.at(old[2])*c+d.vertices.at(old[3])*e)/8.0;
      std::vector<Tet> trial; for(const auto id:r.cells) if(id!=cell)trial.push_back(d.tets[id]);
      const auto apex=kGeneratedTag|0x6f800000ULL|cell;
      Domain view=d; view.vertices.emplace(apex,p);
      for(const auto f:tet_faces) trial.push_back(positive(view,{{old[f[0]],old[f[1]],old[f[2]],apex}}));
      const auto q=quality_of_tets(view,trial);
      if(best.quality.minimum<0 || better(q,best.quality))best={cell,p,q};
    }
  }
  return best;
}

Region make_subregion(const Domain& d,const std::vector<std::size_t>& cells) {
  Region r; r.cells=cells; std::map<Face,std::size_t> count;
  for(const auto cell:cells) { for(const auto id:d.tets[cell])r.vertices.insert(id); for(const auto f:tet_faces)++count[key({{d.tets[cell][f[0]],d.tets[cell][f[1]],d.tets[cell][f[2]]}})]; }
  for(const auto& [f,n]:count)if(n==1)r.boundary.push_back(f); return r;
}
struct ConeChoice { Region region; Vec3 p{}; Summary quality{}; std::size_t valid{}; };
[[maybe_unused]] ConeChoice scan_subcone(const Domain& d,const Region& whole,const std::vector<std::size_t>& cells) {
  ConeChoice best; best.region=make_subregion(d,cells); best.quality.minimum=-1.; const auto& r=best.region;
  const auto c=centroid(d,r);Vec3 lo=c,hi=c;for(const auto id:r.vertices){const auto&p=d.vertices.at(id);lo.x=std::min(lo.x,p.x);lo.y=std::min(lo.y,p.y);lo.z=std::min(lo.z,p.z);hi.x=std::max(hi.x,p.x);hi.y=std::max(hi.y,p.y);hi.z=std::max(hi.z,p.z);}const auto span=hi-lo;
  for(int ix=-8;ix<=8;++ix)for(int iy=-8;iy<=8;++iy)for(int iz=-8;iz<=8;++iz){const Vec3 p{c.x+span.x*ix/32.,c.y+span.y*iy/32.,c.z+span.z*iz/32.};if(!in_cavity_kernel(d,r,p))continue;++best.valid;Domain view=d;const auto apex=kGeneratedTag|0x6f900000ULL;view.vertices.emplace(apex,p);std::vector<Tet> tets;for(const auto cell:whole.cells)if(std::find(cells.begin(),cells.end(),cell)==cells.end())tets.push_back(d.tets[cell]);for(const auto&f:r.boundary)tets.push_back(positive(view,{{f[0],f[1],f[2],apex}}));const auto q=quality_of_tets(view,tets);if(best.quality.minimum<0||better(q,best.quality)){best.p=p;best.quality=q;}}
  return best;
}

// Retained solely as the fixed output regression witness.  The active builder
// below must not take repair choices from this function.
Candidate build_final_three_regions_witness() {
  constexpr const char* prefix="artifacts/dc-viability-2026-09-09/shell-n6";
  Domain original=read_domain(prefix,6U); std::ifstream in(std::string(prefix)+".1.ele");std::size_t original_shell{};unsigned a{},b{};in>>original_shell>>a>>b;
  std::size_t seeds{},selected{}; auto regions=find_regions(original,original_shell,seeds,selected);
  std::sort(regions.begin(),regions.end(),[](const Region& a,const Region& b){ return a.cells.front()<b.cells.front(); });
  auto out=build_nonstar_candidate();
  const std::array<Vec3,2> points{{
    {-0.77742489158732075,0.49530890073689549,-0.054305974822908006},
    {-0.67108819641009854,-0.51938064122702998,0.035834138376939467}}};
  const std::array<std::size_t,2> ids{{0,5}};
  for(std::size_t n=0;n<ids.size();++n) {
    const auto ri=ids[n]; const auto apex=kGeneratedTag|0x6e362000ULL|static_cast<std::uint64_t>(ri);
    const auto old_shell=out.domain.tets.size()-96U;
    std::vector<Tet> shell_tets; shell_tets.reserve(old_shell);
    for(std::size_t i=0;i<old_shell;++i) if(std::find(out.domain.tets[i].begin(),out.domain.tets[i].end(),apex)==out.domain.tets[i].end()) shell_tets.push_back(out.domain.tets[i]);
    out.domain.vertices.at(apex)=points[n];
    for(const auto& f:regions[ri].boundary) shell_tets.push_back(positive(out.domain,{{f[0],f[1],f[2],apex}}));
    shell_tets.insert(shell_tets.end(),out.domain.tets.begin()+static_cast<std::ptrdiff_t>(old_shell),out.domain.tets.end());
    out.domain.tets=std::move(shell_tets);
  }
  // Region 8 is not a star-shaped four-cell cavity. Its two-cell subcavity
  // {106,155} is star-shaped, however; coning precisely that six-face
  // boundary replaces the poor original divider without altering the
  // remaining cells or any prescribed exterior/core facet.
  const auto sub=make_subregion(original,{106U,155U});
  const auto apex=kGeneratedTag|0x6f900000ULL;
  const Vec3 p{0.63305457790988418,0.68120083601978454,-0.36977008075020074};
  const auto old_shell=out.domain.tets.size()-96U; std::set<Tet> remove;
  for(const auto cell:sub.cells) { auto t=original.tets[cell]; std::sort(t.begin(),t.end()); remove.insert(t); }
  std::vector<Tet> shell_tets; shell_tets.reserve(old_shell+4U);
  for(std::size_t i=0;i<old_shell;++i) { auto t=out.domain.tets[i];std::sort(t.begin(),t.end());if(!remove.contains(t))shell_tets.push_back(out.domain.tets[i]); }
  out.domain.vertices.emplace(apex,p);
  for(const auto& f:sub.boundary) shell_tets.push_back(positive(out.domain,{{f[0],f[1],f[2],apex}}));
  shell_tets.insert(shell_tets.end(),out.domain.tets.begin()+static_cast<std::ptrdiff_t>(old_shell),out.domain.tets.end());
  out.domain.tets=std::move(shell_tets);
  return out;
}

Candidate build_data_driven_n6_candidate(Domain original,std::size_t shell) {
  constexpr std::size_t kMaxKernelPointEvaluations=65536U;
  constexpr std::size_t kMaxSubcavities=256U;
  constexpr std::size_t kMaxGeneratedVertices=32U;
  constexpr std::size_t kMaxExtraShellTets=256U;
  constexpr std::size_t kMaxRetainedBytes=65536U;
  constexpr std::size_t kMaxTemporaryBytes=131072U;
  if(shell>original.tets.size()) throw std::runtime_error("invalid input: shell exceeds tetrahedron count");
  const auto core_tets=original.tets.size()-shell;
  std::size_t seeds{},selected{}; const auto regions=find_regions(original,shell,seeds,selected);
  if(regions.size()>32U) throw std::runtime_error("repair limit exceeded: regions");
  for(const auto& r:regions) {
    if(r.cells.size()>32U) throw std::runtime_error("repair limit exceeded: cells per region");
    if(r.boundary.size()>64U) throw std::runtime_error("repair limit exceeded: boundary faces per region");
  }
  auto out=build_nonstar_candidate(original,shell,true);
  std::size_t generated=out.generated_vertices, search_points{}, search_subsets{};
  // Rebuild every star region from the same bounded geometry stencil.  Only
  // promote a point when it improves that region's S4 score over the centroid
  // cone selected by the baseline constructor.
  for(std::size_t ri=0;ri<regions.size();++ri) {
    const auto& r=regions[ri]; if(ri>=out.region_reports.size() || !out.region_reports[ri].cone) continue;
    const auto apex=kGeneratedTag|0x6e362000ULL|static_cast<std::uint64_t>(ri);
    const auto base=out.region_reports[ri];
    if(base.below_five==0U) continue;
    const auto choice=choose_kernel_apex(original,r,apex,{});
    search_points+=choice.points_tested;
    if(search_points>kMaxKernelPointEvaluations) throw std::runtime_error("repair limit exceeded: kernel point evaluations");
    if(!choice.found || !better(choice.quality,{base.minimum,0U,base.below_five})) continue;
    const auto old_shell=out.domain.tets.size()-core_tets; std::vector<Tet> rebuilt; rebuilt.reserve(old_shell);
    for(std::size_t i=0;i<old_shell;++i)
      if(std::find(out.domain.tets[i].begin(),out.domain.tets[i].end(),apex)==out.domain.tets[i].end()) rebuilt.push_back(out.domain.tets[i]);
    out.domain.vertices.at(apex)=choice.p;
    for(const auto& f:r.boundary) rebuilt.push_back(positive(out.domain,{{f[0],f[1],f[2],apex}}));
    rebuilt.insert(rebuilt.end(),out.domain.tets.begin()+static_cast<std::ptrdiff_t>(old_shell),out.domain.tets.end());
    out.domain.tets=std::move(rebuilt); out.region_reports[ri].minimum=choice.quality.minimum;
    out.region_reports[ri].below_five=choice.quality.below_five;
  }
  // Non-star regions retain their valid divider unless a face-connected proper
  // subcavity has a strictly better bounded cone.  The subset is derived from
  // adjacency, never from original-cell positions.
  for(std::size_t ri=0;ri<regions.size();++ri) {
    const auto& r=regions[ri]; if(ri>=out.region_reports.size() || out.region_reports[ri].cone) continue;
    const auto base=out.region_reports[ri];
    if(base.below_five==0U) continue;
    const auto apex=kGeneratedTag|0x6f900000ULL|static_cast<std::uint64_t>(ri);
    const auto choice=choose_subcone(original,r,apex);
    search_points+=choice.points_tested; search_subsets+=choice.subsets_tested;
    if(search_points>kMaxKernelPointEvaluations) throw std::runtime_error("repair limit exceeded: kernel point evaluations");
    if(search_subsets>kMaxSubcavities) throw std::runtime_error("repair limit exceeded: subcavities");
    if(!choice.found || !better(choice.quality,{base.minimum,0U,base.below_five})) continue;
    const auto sub=make_subregion(original,choice.cells); std::set<Tet> remove;
    for(const auto cell:sub.cells) { auto t=original.tets[cell]; std::sort(t.begin(),t.end()); remove.insert(t); }
    const auto old_shell=out.domain.tets.size()-core_tets; std::vector<Tet> rebuilt; rebuilt.reserve(old_shell+sub.boundary.size());
    for(std::size_t i=0;i<old_shell;++i) { auto t=out.domain.tets[i]; std::sort(t.begin(),t.end()); if(!remove.contains(t)) rebuilt.push_back(out.domain.tets[i]); }
    out.domain.vertices.emplace(apex,choice.p);
    for(const auto& f:sub.boundary) rebuilt.push_back(positive(out.domain,{{f[0],f[1],f[2],apex}}));
    rebuilt.insert(rebuilt.end(),out.domain.tets.begin()+static_cast<std::ptrdiff_t>(old_shell),out.domain.tets.end());
    out.domain.tets=std::move(rebuilt); ++generated;
    if(generated>kMaxGeneratedVertices) throw std::runtime_error("repair limit exceeded: generated vertices");
    out.region_reports[ri].fill="bounded_geometry_subcone";
    out.region_reports[ri].minimum=choice.quality.minimum; out.region_reports[ri].below_five=choice.quality.below_five;
    out.region_reports[ri].output_tets=sub.boundary.size();
  }
  out.generated_vertices=generated;
  out.search_points=search_points; out.search_subcavities=search_subsets;
  if(out.domain.tets.size()-core_tets>shell+kMaxExtraShellTets) throw std::runtime_error("repair limit exceeded: output shell tetrahedra");
  // Packed-domain accounting is intentionally independent of std::map
  // allocator details: it is the minimum serializable vertex/tet payload.
  out.retained_bytes=out.domain.vertices.size()*(sizeof(std::uint64_t)+sizeof(Vec3))+out.domain.tets.size()*sizeof(Tet);
  // Each stencil candidate copies one domain and holds one bounded cavity
  // replacement.  Candidates are scored serially, so this is a peak rather
  // than a sum over the reported work items.
  out.temporary_bytes=out.retained_bytes+(out.max_region_cells+out.max_boundary_faces)*sizeof(Tet)+out.max_region_cells*sizeof(std::uint64_t);
  if(out.retained_bytes>kMaxRetainedBytes) throw std::runtime_error("repair limit exceeded: retained bytes");
  if(out.temporary_bytes>kMaxTemporaryBytes) throw std::runtime_error("repair limit exceeded: temporary bytes");
  return out;
}

Candidate build_final_three_regions_candidate() {
  constexpr const char* prefix="artifacts/dc-viability-2026-09-09/shell-n6";
  Domain d=read_domain(prefix,6U); std::ifstream in(std::string(prefix)+".1.ele");
  std::size_t shell{}; unsigned corners{},attributes{}; in>>shell>>corners>>attributes;
  return build_data_driven_n6_candidate(std::move(d),shell);
}

int final_run() {
  constexpr const char* prefix="artifacts/dc-viability-2026-09-09/shell-n6";
  Domain d=read_domain(prefix,6U); std::ifstream in(std::string(prefix)+".1.ele");std::size_t shell{};unsigned a{},b{};in>>shell>>a>>b;
  std::size_t seeds{},selected{}; const auto regions=find_regions(d,shell,seeds,selected);
  const auto c=build_final_three_regions_candidate(); const auto again=build_final_three_regions_candidate();
  const auto shell_after=c.domain.tets.size()-96U; const auto report=audit(c.domain,shell_after); const auto q=quality_of(c.domain);
  DualVolumeBuild full_quality; full_quality.vertices=c.domain.vertices;
  for(const auto& t:c.domain.tets)add_dual_volume_tet(full_quality,t,DualVolumeRegion::transition);
  const auto full_metrics=evaluate_dual_volume_quality(full_quality);
  auto missing=c.domain; missing.prescribed.erase(missing.prescribed.begin());
  auto overlap=c.domain; overlap.tets.push_back(overlap.tets.front());
  auto moved=c.domain; moved.vertices.at(*moved.interface_vertices.begin()).x+=1e-4;
  const bool core_exact=exact_retained_core(d,shell,c.domain);
  const bool controls=same_domain(c,again)&&exact_interface(c.domain)&&exact_frozen_vertices(c.domain)&&core_exact&&!audit(missing,shell_after).geometry&&!audit(overlap,shell_after).geometry&&!exact_interface(moved);
  std::cout<<std::setprecision(17)<<"{\"probe\":\"n6_data_driven_repair/v1\""
    <<",\"regions\":[";
  for(std::size_t i=0;i<c.region_reports.size();++i) { const auto& r=c.region_reports[i];
    std::cout<<(i?",":"")<<"{\"id\":"<<i<<",\"cells\":"<<r.cells<<",\"boundary_faces\":"<<r.boundary_faces<<",\"fill\":\""<<r.fill<<"\",\"minimum\":"<<r.minimum<<",\"below_five\":"<<r.below_five<<"}";
  }
  std::cout<<"]"
    <<",\"quality\":{\"minimum\":"<<q.minimum<<",\"below_five\":"<<q.below_five<<",\"above_175\":"<<full_metrics.dihedrals_above_175_degrees<<",\"qualified\":"<<(report.quality?"true":"false")<<"}"
    <<",\"geometry\":{\"valid\":"<<(report.geometry?"true":"false")<<",\"overlaps\":"<<report.overlaps<<",\"same_side\":"<<report.same_side<<",\"volume_error\":"<<report.volume_error<<"}"
    <<",\"resources\":{\"generated_vertices\":"<<c.generated_vertices<<",\"shell_tets\":"<<shell_after<<",\"max_region_cells\":"<<c.max_region_cells<<",\"max_boundary_faces\":"<<c.max_boundary_faces<<",\"kernel_points_tested\":"<<c.search_points<<",\"subcavities_tested\":"<<c.search_subcavities<<",\"retained_packed_bytes\":"<<c.retained_bytes<<",\"temporary_peak_packed_bytes\":"<<c.temporary_bytes<<",\"limits\":{\"regions\":32,\"cells_per_region\":32,\"boundary_faces_per_region\":64,\"subcavity_cells\":8,\"kernel_stencil_points\":4913,\"kernel_point_evaluations\":65536,\"subcavities\":256,\"generated_vertices\":32,\"extra_shell_tets\":256,\"retained_packed_bytes\":65536,\"temporary_packed_bytes\":131072}}"
    <<",\"controls\":{\"deterministic\":"<<(same_domain(c,again)?"true":"false")<<",\"exact_interface\":"<<(exact_interface(c.domain)?"true":"false")<<",\"exact_frozen_vertices\":"<<(exact_frozen_vertices(c.domain)?"true":"false")<<",\"exact_retained_core\":"<<(core_exact?"true":"false")<<",\"missing_face_rejected\":"<<(!audit(missing,shell_after).geometry?"true":"false")<<",\"overlap_rejected\":"<<(!audit(overlap,shell_after).geometry?"true":"false")<<",\"moved_interface_rejected\":"<<(!exact_interface(moved)?"true":"false")<<"}}\n";
  return controls&&report.geometry&&report.quality&&exact_frozen_vertices(c.domain)&&q.below_five==0U&&full_metrics.dihedrals_above_175_degrees==0U?0:1;
}
}
#ifdef N6_FINAL_THREE_REGIONS_TEST
int n6_final_three_regions_main(){return final_run();}
#else
int main(){return final_run();}
#endif
