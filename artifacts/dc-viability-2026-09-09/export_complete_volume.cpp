// Fresh finite terrain PLC: frozen DC roof + curtain + bottom cap, with one
// separately retained regular core cavity.  Unlike export_shell.cpp, this
// deliberately has no normal-offset lower DC scaffold boundary.
#include "../../src/tetra_probes/sandwich_probe.cpp"

#include <fstream>
#include <iostream>

int main(int argc,char** argv) {
  using namespace tetra::probes;
  if(argc<2||argc==4||argc>7)return 2;
  SandwichConfig config; config.resolution=argc>2?static_cast<unsigned>(std::stoul(argv[2])):8U;
  if(argc>3) { config.phase_x=std::stod(argv[3]); config.phase_y=std::stod(argv[4]); }
  const double bottom_z=argc>5?std::stod(argv[5]):-1.0;
  if(argc>6) {
    if(std::string_view{argv[6]}!="--planar")return 2;
    config.field=SandwichField::planar;
  }
  const auto n=config.resolution;
  const auto surface=dual_contour_surface(config,0U,2U*n);
  const auto core_id=[](std::uint64_t id) { return 0x100000000ULL+id; };
  const auto bottom_id=[](std::uint64_t id) { return 0x200000000ULL+id; };
  DualVolumeBuild mesh;
  std::vector<std::pair<DualFaceKey,int>> facets;
  struct BoundaryUse { std::uint64_t first{},second{}; unsigned count{}; };
  std::map<std::array<std::uint64_t,2>,BoundaryUse> edges;
  for(const auto& [cell,p]:surface.vertices) {
    mesh.vertices.emplace(cell,p);
    mesh.vertices.emplace(bottom_id(cell),Vec3{p.x,p.y,bottom_z});
  }
  for(const auto triangle:surface.triangles) {
    facets.push_back({triangle.vertices,1});
    for(unsigned edge=0;edge<3U;++edge) {
      const auto first=triangle.vertices[edge],second=triangle.vertices[(edge+1U)%3U];
      auto key=std::array<std::uint64_t,2>{{std::min(first,second),std::max(first,second)}};
      auto& use=edges[key]; if(use.count++==0U) { use.first=first;use.second=second; }
    }
  }
  std::map<std::uint64_t,std::uint64_t> bottom_next;
  for(const auto& [unused,use]:edges) if(use.count==1U) {
    static_cast<void>(unused);
    const auto a=use.first,b=use.second,A=bottom_id(a),B=bottom_id(b);
    facets.push_back({{{a,b,B}},4});
    facets.push_back({{{a,B,A}},4});
    if(!bottom_next.emplace(A,B).second)return 3;
  }
  const auto cross_2d=[](Vec3 a,Vec3 b,Vec3 c) {
    return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
  };
  std::set<std::uint64_t> visited_bottom;
  for(const auto& [start,unused]:bottom_next) {
    static_cast<void>(unused);
    if(visited_bottom.contains(start))continue;
    std::vector<std::uint64_t> loop; std::uint64_t current=start;
    do {
      if(!bottom_next.contains(current)||visited_bottom.contains(current))return 4;
      visited_bottom.insert(current);loop.push_back(current);current=bottom_next.at(current);
    } while(current!=start);
    // A terrain rim can be concave in its bottom projection, so a centre fan
    // can overlap itself. Deterministic ear clipping uses no invented grid
    // topology and keeps every rim edge exact. Holes are intentionally not
    // accepted by this small finite control yet.
    double twice_area{};
    for(std::size_t i=0;i<loop.size();++i) {
      const auto a=mesh.vertices.at(loop[i]),b=mesh.vertices.at(loop[(i+1U)%loop.size()]);
      twice_area+=a.x*b.y-a.y*b.x;
    }
    const bool ccw=twice_area>0.0;
    std::vector<std::uint64_t> remaining=loop;
    while(remaining.size()>3U) {
      bool clipped{};
      for(std::size_t i=0;i<remaining.size();++i) {
        const auto previous=remaining[(i+remaining.size()-1U)%remaining.size()];
        const auto current_vertex=remaining[i],next=remaining[(i+1U)%remaining.size()];
        const double turn=cross_2d(mesh.vertices.at(previous),mesh.vertices.at(current_vertex),mesh.vertices.at(next));
        if((ccw&&turn<=1.0e-12)||(!ccw&&turn>=-1.0e-12))continue;
        bool contains{};
        for(const auto candidate:remaining)if(candidate!=previous&&candidate!=current_vertex&&candidate!=next) {
          const auto& p=mesh.vertices.at(candidate);
          const double a=cross_2d(mesh.vertices.at(previous),mesh.vertices.at(current_vertex),p);
          const double b=cross_2d(mesh.vertices.at(current_vertex),mesh.vertices.at(next),p);
          const double c=cross_2d(mesh.vertices.at(next),mesh.vertices.at(previous),p);
          contains=contains||((a>1.0e-12&&b>1.0e-12&&c>1.0e-12)||(a<-1.0e-12&&b<-1.0e-12&&c<-1.0e-12));
        }
        if(contains)continue;
        facets.push_back({{{previous,current_vertex,next}},4});
        remaining.erase(remaining.begin()+static_cast<std::ptrdiff_t>(i));clipped=true;break;
      }
      if(!clipped)return 5;
    }
    facets.push_back({{{remaining[0],remaining[1],remaining[2]}},4});
  }
  std::map<DualFaceKey,unsigned> core_faces;
  for(unsigned i=2;i<2*n-2;++i)for(unsigned j=2;j<n-2;++j)for(unsigned k=1;k<n/2-1;++k) {
    std::array<std::uint64_t,8> cube;
    for(unsigned bit=0;bit<8;++bit) {
      const auto key=lattice_key(i+(bit&1),j+((bit>>1)&1),k+((bit>>2)&1),n);
      const auto id=core_id(key.payload); cube[bit]=id; mesh.vertices.emplace(id,lattice_position(key,n));
    }
    for(const auto permutation:cube_permutations) {
      const auto a=1U<<permutation[0],b=a|(1U<<permutation[1]);
      const std::array<std::uint64_t,4> ids{{cube[0],cube[a],cube[b],cube[7]}};
      add_dual_volume_tet(mesh,ids,DualVolumeRegion::core);
      for(const auto face:tet_faces)++core_faces[canonical_dual_face({ids[face[0]],ids[face[1]],ids[face[2]]})];
    }
  }
  for(const auto& [face,count]:core_faces)if(count==1U)facets.push_back({face,3});
  std::map<std::uint64_t,unsigned> ids;
  for(const auto& [face,kind]:facets) { static_cast<void>(kind);for(const auto id:face)ids.emplace(id,0U); }
  unsigned index{};for(auto& [id,value]:ids)value=index++;
  std::ofstream out(argv[1]);out<<std::setprecision(17)<<ids.size()<<" 3 0 0\n";
  for(const auto [id,value]:ids) { const auto p=mesh.vertices.at(id);out<<value<<' '<<p.x<<' '<<p.y<<' '<<p.z<<'\n'; }
  out<<facets.size()<<" 1\n";
  for(const auto& [face,kind]:facets) { out<<"1 0 "<<kind<<"\n3";for(const auto id:face)out<<' '<<ids.at(id);out<<'\n'; }
  // This seed removes the regular core cavity; the sidecar below restores its
  // exact implicit tetrahedra after the PLC shell is generated.
  out<<"1\n0 0 0 -0.5\n0\n";
  std::ofstream core(std::string(argv[1])+".core");core<<std::setprecision(17);
  std::set<std::uint64_t> all_core;for(const auto tet:mesh.tetrahedra)for(const auto id:tet.vertices)all_core.insert(id);
  core<<all_core.size()<<' '<<mesh.tetrahedra.size()<<'\n';
  for(const auto id:all_core) { const auto p=mesh.vertices.at(id);core<<id<<' '<<(ids.contains(id)?static_cast<int>(ids.at(id)):-1)<<' '<<p.x<<' '<<p.y<<' '<<p.z<<'\n'; }
  for(const auto tet:mesh.tetrahedra) { for(const auto id:tet.vertices)core<<id<<' ';core<<'\n'; }
  std::cout<<"surface="<<surface.triangles.size()<<" facets="<<facets.size()<<" core_tets="<<mesh.tetrahedra.size()<<'\n';
}
