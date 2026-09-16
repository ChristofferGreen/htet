// Focused S4 refinement experiment.
//
// This constructs the canonical *selection* for a one-level, locally refined
// hexahedral patch around the worst coarse DC triangle.  A uniform fine-grid
// surface is used only as an oracle for the additional degrees of freedom.
// It deliberately does not claim to be a crack-free adaptive DC extractor:
// that requires the coarse/fine transition templates which this probe is
// intended to justify and test next.
#include "../../src/tetra_probes/sandwich_probe.cpp"

#include <iomanip>
#include <iostream>
#include <set>

namespace {
using namespace tetra::probes;

double minimum_angle(const DualSurfaceBuild& surface,const DualTriangle& triangle) {
  std::array<Vec3,3> p{{surface.vertices.at(triangle.vertices[0]),
                        surface.vertices.at(triangle.vertices[1]),
                        surface.vertices.at(triangle.vertices[2])}};
  double result=180.0;
  for(unsigned int i=0;i<3U;++i) {
    const auto a=p[(i+1U)%3U]-p[i],b=p[(i+2U)%3U]-p[i];
    result=std::min(result,std::acos(std::clamp(dot(a,b)/(length(a)*length(b)),-1.0,1.0))*180.0/std::numbers::pi);
  }
  return result;
}

std::array<unsigned int,3> coordinates(std::uint64_t id,unsigned int resolution) {
  return {static_cast<unsigned int>(id/(static_cast<std::uint64_t>(resolution)*resolution)),
          static_cast<unsigned int>((id/resolution)%resolution),
          static_cast<unsigned int>(id%resolution)};
}

std::uint64_t id(std::array<unsigned int,3> c,unsigned int resolution) {
  return (static_cast<std::uint64_t>(c[0])*resolution+c[1])*resolution+c[2];
}

struct Plan {
  std::set<std::uint64_t> seed_cells;
  std::set<std::uint64_t> closure_cells;
  double coarse_angle{180.0};
};

Plan make_plan(const SandwichConfig& config,const DualSurfaceBuild& coarse) {
  Plan plan; const auto& triangles=coarse.triangles;
  const auto worst=std::min_element(triangles.begin(),triangles.end(),[&](const auto& a,const auto& b) {
    return minimum_angle(coarse,a)<minimum_angle(coarse,b);
  });
  if(worst==triangles.end())throw std::runtime_error("coarse surface has no triangles");
  plan.coarse_angle=minimum_angle(coarse,*worst);
  for(const auto cell:worst->vertices)plan.seed_cells.insert(cell);
  for(const auto cell:plan.seed_cells) {
    const auto c=coordinates(cell,config.resolution);
    for(int di=-1;di<=1;++di)for(int dj=-1;dj<=1;++dj)for(int dk=-1;dk<=1;++dk) {
      const int i=static_cast<int>(c[0])+di,j=static_cast<int>(c[1])+dj,k=static_cast<int>(c[2])+dk;
      if(i>=0&&i<static_cast<int>(config.resolution)*2&&j>=0&&j<static_cast<int>(config.resolution)&&k>=0&&k<static_cast<int>(config.resolution))
        plan.closure_cells.insert(id({static_cast<unsigned int>(i),static_cast<unsigned int>(j),static_cast<unsigned int>(k)},config.resolution));
    }
  }
  return plan;
}

bool belongs_to_refined_parent(std::uint64_t fine_id,const Plan& plan,unsigned int coarse_resolution) {
  const auto fine=coordinates(fine_id,coarse_resolution*2U);
  return plan.closure_cells.contains(id({fine[0]/2U,fine[1]/2U,fine[2]/2U},coarse_resolution));
}

double refined_oracle_angle(const SandwichConfig& fine_config,const DualSurfaceBuild& fine,const Plan& plan) {
  double result=180.0; std::size_t count{};
  for(const auto& triangle:fine.triangles) {
    if(!belongs_to_refined_parent(triangle.vertices[0],plan,fine_config.resolution/2U)||
       !belongs_to_refined_parent(triangle.vertices[1],plan,fine_config.resolution/2U)||
       !belongs_to_refined_parent(triangle.vertices[2],plan,fine_config.resolution/2U))continue;
    result=std::min(result,minimum_angle(fine,triangle));++count;
  }
  if(count==0U)throw std::runtime_error("refined closure produced no fine triangles");
  return result;
}

} // namespace

int main() {
  try {
    SandwichConfig coarse_config;
    coarse_config.resolution=8U;
    const auto coarse=dual_contour_surface(coarse_config,0U,16U);
    const auto plan=make_plan(coarse_config,coarse);
    SandwichConfig fine_config=coarse_config;fine_config.resolution=16U;
    const auto fine=dual_contour_surface(fine_config,0U,32U);
    const auto fine_angle=refined_oracle_angle(fine_config,fine,plan);
    const bool one_level_2_to_1=true; // coarse leaves are level 0, selected children are level 1.
    const bool improved=fine_angle>plan.coarse_angle+1.0e-9;
    std::cout<<std::setprecision(17)
      <<"{\"probe\":\"dc_local_hex_refinement/v1\",\"coarse_resolution\":8,\"refined_resolution\":16,"
      <<"\"seed_cells\":"<<plan.seed_cells.size()<<",\"closure_cells\":"<<plan.closure_cells.size()<<","
      <<"\"refined_child_hexes\":"<<plan.closure_cells.size()*8U<<","
      <<"\"one_level_2_to_1\":"<<(one_level_2_to_1?"true":"false")<<","
      <<"\"coarse_worst_angle_degrees\":"<<plan.coarse_angle<<","
      <<"\"uniform_fine_oracle_min_angle_degrees\":"<<fine_angle<<","
      <<"\"uniform_fine_oracle_improves_patch\":"<<(improved?"true":"false")<<","
      <<"\"adaptive_dc_transition_templates_implemented\":false,"
      <<"\"qualified\":false,"
      <<"\"decision\":\"selection_and_fine_oracle_only; implement canonical coarse_fine_dual_transition_templates before claiming local adaptive DC\"}\n";
    return improved&&validate_dual_surface(coarse).valid&&validate_dual_surface(fine).valid?0:1;
  } catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';return 2;
  }
}
