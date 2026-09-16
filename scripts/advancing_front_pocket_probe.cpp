#include "tetra_probes/advancing_front_pocket.hpp"
#include "tetra_probes/canonical_delaunay_seed.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <cmath>
#include <algorithm>
#include <map>
#include <set>
#include <string>

int main(int argc,char** argv){
  if(argc!=2){std::cerr<<"usage: advancing_front_pocket_probe CHECKPOINT.json\n";return 2;}
  std::ifstream input(argv[1]);
  if(!input){std::cerr<<"cannot open checkpoint\n";return 2;}
  const std::string json{std::istreambuf_iterator<char>{input},std::istreambuf_iterator<char>{}};
  try{
    const auto checkpoint=tetra::probes::parse_advancing_front_replay_data(json);
    const auto pockets=tetra::probes::extract_advancing_front_pockets(checkpoint);
    const auto* smallest=tetra::probes::smallest_advancing_front_pocket(pockets);
    std::cout<<"pockets="<<pockets.size();
    if(smallest){const auto expanded=tetra::probes::expand_advancing_front_pocket_to_manifold(checkpoint,*smallest);
      tetra::probes::CanonicalDelaunaySeedInput seed_input;
      for(const auto index:expanded.repair_vertex_indices){seed_input.vertices.push_back(checkpoint.vertices[index]);seed_input.stable_vertex_ids.push_back(checkpoint.stable_vertex_ids[index]);}
      const auto seed=tetra::probes::build_canonical_delaunay_seed(seed_input);
      std::map<std::uint32_t,std::uint32_t> local;for(std::uint32_t i=0;i<expanded.repair_vertex_indices.size();++i)local[expanded.repair_vertex_indices[i]]=i;
      std::set<std::array<std::uint32_t,3>> seed_faces;for(const auto tet:seed.tetrahedra)for(std::size_t omitted=0;omitted<4U;++omitted){std::array<std::uint32_t,3> face{};std::size_t cursor{};for(std::size_t i=0;i<4U;++i)if(i!=omitted)face[cursor++]=tet[i];std::ranges::sort(face);seed_faces.insert(face);}
      std::size_t represented_boundary_faces{};for(const auto global_face:expanded.repair_boundary_faces){std::array<std::uint32_t,3> face{{local[global_face[0]],local[global_face[1]],local[global_face[2]]}};std::ranges::sort(face);represented_boundary_faces+=seed_faces.contains(face)?1U:0U;}
      tetra::probes::AdvancingFrontPocketRepairOptions options;options.maximum_search_states=8U;
      for(const auto face:smallest->pocket_faces)options.candidate_vertex_indices.insert(
          options.candidate_vertex_indices.end(),face.begin(),face.end());
      for(const auto index:smallest->halo_tetrahedron_indices){const auto tet=checkpoint.tetrahedra[index];
        options.candidate_vertex_indices.insert(options.candidate_vertex_indices.end(),tet.begin(),tet.end());}
      std::ranges::sort(options.candidate_vertex_indices);options.candidate_vertex_indices.erase(
          std::unique(options.candidate_vertex_indices.begin(),options.candidate_vertex_indices.end()),options.candidate_vertex_indices.end());
      const auto initial=tetra::probes::make_advancing_front_pocket_repair_region(checkpoint,*smallest,{});
      const auto existing_solve=tetra::probes::search_advancing_front_pocket_existing_vertices(checkpoint,initial,options);
      auto solve=existing_solve;auto region=initial;std::set<std::size_t> mutable_tets;
      std::map<std::array<std::uint32_t,3>,std::size_t> face_attempts;
      const auto point=[&](std::uint32_t index){return index<checkpoint.vertices.size()?checkpoint.vertices[index]:
          options.deterministic_steiner_candidates[index-checkpoint.vertices.size()];};
      for(std::size_t iteration=0;iteration<64U&&!solve.accepted()&&solve.has_blocking_face;++iteration){
        bool expanded_region=false;for(const auto index:solve.blocking_obstacle_tetrahedra)
          expanded_region=mutable_tets.insert(index).second||expanded_region;
        if(expanded_region){const std::vector<std::size_t> selected(mutable_tets.begin(),mutable_tets.end());
          region=tetra::probes::make_advancing_front_pocket_repair_region(checkpoint,*smallest,selected);
          solve=tetra::probes::search_advancing_front_pocket_existing_vertices(checkpoint,region,options);continue;}
        const auto face=solve.blocking_face;const auto a=point(face[0]);
        const auto b=point(face[1]);const auto c=point(face[2]);
        const auto ab=b-a,ac=c-a;auto normal=tetra::Vec3{ab.y*ac.z-ab.z*ac.y,ab.z*ac.x-ab.x*ac.z,ab.x*ac.y-ab.y*ac.x};
        const auto length=[](tetra::Vec3 v){return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);};const auto magnitude=length(normal);if(magnitude<=1e-14)break;normal=normal/magnitude;
        const auto scale=std::min({length(a-b),length(b-c),length(c-a)});const auto centre=(a+b+c)/3.0;
        const auto attempt=face_attempts[face]++;
        const auto fraction=std::min(0.35,0.005*std::pow(1.55,static_cast<double>(attempt)));
        options.deterministic_steiner_candidates.push_back(centre-normal*(fraction*scale));
        solve=tetra::probes::search_advancing_front_pocket_existing_vertices(checkpoint,region,options);
      }
      std::cout<<" smallest_volume="<<smallest->pocket_volume
      <<" pocket_faces="<<smallest->pocket_faces.size()
      <<" halo_tets="<<smallest->halo_tetrahedron_indices.size()
      <<" repair_faces="<<smallest->repair_boundary_faces.size()
      <<" repair_vertices="<<smallest->repair_vertex_indices.size()
      <<" repair_volume="<<smallest->repair_volume
      <<" pocket_closed="<<smallest->audit.pocket_closed
      <<" pocket_oriented="<<smallest->audit.pocket_oriented
      <<" pocket_no_crossing="<<smallest->audit.pocket_self_intersection_free
      <<" halo_positive="<<smallest->audit.halo_tetrahedra_exactly_positive
      <<" interface_cancelled="<<smallest->audit.interface_cancels_exactly
      <<" repair_closed="<<smallest->audit.repair_boundary_closed
      <<" repair_oriented="<<smallest->audit.repair_boundary_oriented
      <<" repair_no_crossing="<<smallest->audit.repair_boundary_self_intersection_free
      <<" expanded_halo_tets="<<expanded.halo_tetrahedron_indices.size()
      <<" expanded_faces="<<expanded.repair_boundary_faces.size()
      <<" expanded_vertices="<<expanded.repair_vertex_indices.size()
      <<" expanded_closed="<<expanded.audit.repair_boundary_closed
      <<" delaunay_accepted="<<seed.accepted()
      <<" delaunay_tets="<<seed.tetrahedra.size()
      <<" delaunay_boundary_faces="<<represented_boundary_faces<<'/'<<expanded.repair_boundary_faces.size()
      <<" existing_solve_accepted="<<existing_solve.accepted()
      <<" existing_solve_failure="<<static_cast<int>(existing_solve.failure)
      <<" solve_accepted="<<solve.accepted()
      <<" solve_failure="<<static_cast<int>(solve.failure)
      <<" solve_tets="<<solve.tetrahedra.size()
      <<" solve_states="<<solve.search_states
      <<" solve_candidates="<<solve.candidate_tests
      <<" adaptive_mutable_tets="<<mutable_tets.size()
      <<" adaptive_boundary_faces="<<region.repair_boundary_faces.size()
      <<" steiner_candidates="<<options.deterministic_steiner_candidates.size()
      <<" reject_orientation="<<solve.rejected_orientation
      <<" reject_inside="<<solve.rejected_vertex_inside
      <<" reject_overlap="<<solve.rejected_overlap
      <<" reject_crossing="<<solve.rejected_front_crossing;
      if(solve.has_blocking_face)std::cout<<" blocking_face="<<solve.blocking_face[0]<<','<<solve.blocking_face[1]<<','<<solve.blocking_face[2];
    }
    std::cout<<'\n';return smallest?0:1;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 2;}
}
