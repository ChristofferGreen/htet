#include "tetra_probes/terrain_volume_request.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <algorithm>
#include <array>
#include <map>
#include <numbers>
#include <set>
#include <string>
#include <vector>

namespace {

// A single-valued, closed-by-the-standard-curtain terrain sheet with a deep
// central pit.  It keeps the same FrozenDualContourSurface -> terrain request
// route as production terrain, while making concavity explicit for diagnosis.
tetra::probes::TerrainVolumeRequestResult pit_request(
    double bottom,double core_x,double core_y,double core_z) {
  using namespace tetra::probes;
  FrozenDualContourSurface surface;
  surface.lattice_resolution=6U;
  // The three-by-three interior is a flat basin.  Its one-cell rim makes a
  // steep but finite terrain wall rather than an overhang or vertical facet.
  for(unsigned y=0U;y<5U;++y)for(unsigned x=0U;x<5U;++x) {
    const bool basin=x>0U&&x<4U&&y>0U&&y<4U;
    double px=-1.0+.5*x,py=-1.0+.5*y;
    // The closure cap needs a strictly polygonal loop; retain the terrain
    // footprint but bow each otherwise-collinear boundary run outward.
    const double bow=.04*std::sin(3.141592653589793*x/4.0);
    if(y==0U)py-=bow;
    if(y==4U)py+=bow;
    const double side_bow=.04*std::sin(3.141592653589793*y/4.0);
    if(x==0U)px-=side_bow;
    if(x==4U)px+=side_bow;
    surface.vertices.push_back({px,py,basin?-2.0:2.0});
  }
  for(std::uint64_t id=1U;id<=surface.vertices.size();++id)
    surface.stable_vertex_ids.push_back(id);
  for(unsigned y=0U;y<4U;++y)for(unsigned x=0U;x<4U;++x) {
    const auto lower=y*5U+x;
    surface.triangles.push_back({{lower,lower+1U,lower+6U}});
    surface.triangles.push_back({{lower,lower+6U,lower+5U}});
  }
  surface.boundary_edges={{{0,1},{1,2},{2,3},{3,4},{4,9},{9,14},{14,19},
                           {19,24},{24,23},{23,22},{22,21},{21,20},{20,15},
                           {15,10},{10,5},{5,0}}};
  surface.validation.valid=true;
  FrozenRegularCore core;
  core.lattice_resolution=6U;
  core.vertices={{{core_x,core_y,core_z},{core_x+.25,core_y,core_z},
                  {core_x,core_y+.25,core_z},{core_x,core_y,core_z+.25}}};
  core.stable_vertex_ids={101U,102U,103U,104U};
  core.tetrahedra={{{0,1,2,3}}};
  return make_heightfield_terrain_volume_request(surface,core,bottom);
}

} // namespace

struct TetDiagnostic {
  std::size_t cell{};
  double mean_ratio{};
  double minimum_dihedral{};
  double maximum_dihedral{};
  double edge_ratio{};
};

void print_worst_transition_tetrahedra(
    const tetra::probes::SurfaceCoreTransitionInput& input,
    const tetra::probes::SurfaceCoreTransitionOutput& output,
    const std::vector<tetra::probes::TerrainVolumeCellRegion>& regions) {
  std::vector<tetra::Vec3> points=input.vertices;
  points.insert(points.end(),output.owned_vertices.begin(),output.owned_vertices.end());
  using Face=std::array<std::uint32_t,3>;
  const auto face_key=[](Face face) { std::sort(face.begin(),face.end());return face; };
  std::set<Face> constrained_faces;
  for(const auto face:input.outer_faces) constrained_faces.insert(face_key(face));
  std::map<Face,unsigned> core_face_uses;
  for(const auto tet:input.retained_core_tetrahedra) for(std::size_t omitted=0U;omitted<4U;++omitted) {
    Face face{};std::size_t cursor{};
    for(std::size_t corner=0U;corner<4U;++corner) if(corner!=omitted) face[cursor++]=tet[corner];
    ++core_face_uses[face_key(face)];
  }
  for(const auto& [face,uses]:core_face_uses) if(uses==1U) constrained_faces.insert(face);
  std::map<Face,std::vector<std::size_t>> output_face_uses;
  for(std::size_t cell=0U;cell<output.tetrahedra.size();++cell)
    for(std::size_t omitted=0U;omitted<4U;++omitted) {
      Face face{};std::size_t cursor{};
      for(std::size_t corner=0U;corner<4U;++corner) if(corner!=omitted)
        face[cursor++]=output.tetrahedra[cell][corner];
      output_face_uses[face_key(face)].push_back(cell);
    }
  std::vector<TetDiagnostic> bad;
  const auto cross=[](tetra::Vec3 a,tetra::Vec3 b) {
    return tetra::Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
  };
  const auto dot=[](tetra::Vec3 a,tetra::Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; };
  const auto length=[&](tetra::Vec3 a) { return std::sqrt(dot(a,a)); };
  for(std::size_t cell=0U;cell<output.tetrahedra.size();++cell) {
    if(cell>=regions.size()||regions[cell]!=tetra::probes::TerrainVolumeCellRegion::transition) continue;
    const auto tet=output.tetrahedra[cell];
    const auto signed_six=dot(cross(points[tet[1]]-points[tet[0]],points[tet[2]]-points[tet[0]]),
                              points[tet[3]]-points[tet[0]]);
    double shortest=std::numeric_limits<double>::infinity(),longest{},sum_squared{};
    for(std::size_t a=0U;a<4U;++a) for(std::size_t b=a+1U;b<4U;++b) {
      const auto edge=length(points[tet[b]]-points[tet[a]]);
      shortest=std::min(shortest,edge);longest=std::max(longest,edge);sum_squared+=edge*edge;
    }
    TetDiagnostic diagnostic{cell,
      sum_squared>0.0?12.0*std::pow(std::abs(signed_six)/2.0,2.0/3.0)/sum_squared:0.0,
      180.0,0.0,longest/shortest};
    for(std::size_t first=0U;first<4U;++first) for(std::size_t second=first+1U;second<4U;++second) {
      const auto normal=[&](std::size_t omitted) {
        std::array<tetra::Vec3,3> face{};std::size_t cursor{};
        for(std::size_t corner=0U;corner<4U;++corner) if(corner!=omitted) face[cursor++]=points[tet[corner]];
        auto result=cross(face[1]-face[0],face[2]-face[0]);
        if(dot(result,points[tet[omitted]]-face[0])>0.0) result=result*-1.0;
        return result;
      };
      const auto left=normal(first),right=normal(second);
      const auto degrees=(std::numbers::pi-std::acos(std::clamp(dot(left,right)/(length(left)*length(right)),-1.0,1.0)))*180.0/std::numbers::pi;
      diagnostic.minimum_dihedral=std::min(diagnostic.minimum_dihedral,degrees);
      diagnostic.maximum_dihedral=std::max(diagnostic.maximum_dihedral,degrees);
    }
    if(diagnostic.mean_ratio<0.01||diagnostic.minimum_dihedral<5.0||
       diagnostic.maximum_dihedral>175.0||diagnostic.edge_ratio>20.0) bad.push_back(diagnostic);
  }
  std::sort(bad.begin(),bad.end(),[](const auto& a,const auto& b) {
    return std::tuple{a.minimum_dihedral,a.mean_ratio,-a.maximum_dihedral,-a.edge_ratio,a.cell}<
           std::tuple{b.minimum_dihedral,b.mean_ratio,-b.maximum_dihedral,-b.edge_ratio,b.cell};
  });
  for(std::size_t index=0U;index<std::min<std::size_t>(12U,bad.size());++index) {
    const auto& diagnostic=bad[index];
    const auto tet=output.tetrahedra[diagnostic.cell];
    std::size_t constrained{};
    std::size_t transition_neighbours{},core_neighbours{},boundary_faces{};
    std::size_t outer_vertices{},core_vertices{};
    std::set<std::uint32_t> core_vertex_set;
    for(const auto core:input.retained_core_tetrahedra)
      core_vertex_set.insert(core.begin(),core.end());
    std::set<std::uint32_t> outer_vertex_set;
    for(const auto outer:input.outer_faces)
      outer_vertex_set.insert(outer.begin(),outer.end());
    for(const auto vertex:tet) {
      outer_vertices+=outer_vertex_set.contains(vertex);
      core_vertices+=core_vertex_set.contains(vertex);
    }
    for(std::size_t omitted=0U;omitted<4U;++omitted) {
      Face face{};std::size_t cursor{};
      for(std::size_t corner=0U;corner<4U;++corner) if(corner!=omitted) face[cursor++]=tet[corner];
      constrained+=constrained_faces.contains(face_key(face));
      const auto& uses=output_face_uses.at(face_key(face));
      if(uses.size()==1U) ++boundary_faces;
      for(const auto other:uses) if(other!=diagnostic.cell) {
        if(regions[other]==tetra::probes::TerrainVolumeCellRegion::transition)
          ++transition_neighbours;
        else ++core_neighbours;
      }
    }
    std::cout<<"quality_bad_transition cell="<<diagnostic.cell
             <<" mean_ratio="<<diagnostic.mean_ratio
             <<" dihedral="<<diagnostic.minimum_dihedral<<'/'<<diagnostic.maximum_dihedral
             <<" edge_ratio="<<diagnostic.edge_ratio
             <<" constrained_faces="<<constrained
             <<" neighbours=T"<<transition_neighbours<<"/C"<<core_neighbours
             <<" boundary="<<boundary_faces
             <<" vertices_outer/core="<<outer_vertices<<'/'<<core_vertices
             <<" input_vertices="<<input.vertices.size()
             <<" vertices="<<tet[0]<<','<<tet[1]<<','<<tet[2]<<','<<tet[3]<<'\n';
  }
}

int main(int argc,char** argv) {
  if(argc!=7&&argc!=8&&argc!=9) {
    std::cerr<<"usage: terrain_wang_probe RESOLUTION FIELD AMPLITUDE FREQUENCY PHASE_X PHASE_Y [FHC_ATTEMPTS] [BOTTOM_Z]\n";
    return 2;
  }
  tetra::probes::SandwichConfig config;
  config.resolution=static_cast<unsigned>(std::stoul(argv[1]));
  const std::string field=argv[2];
  config.field=field=="planar"?
      tetra::probes::SandwichField::planar:
      tetra::probes::SandwichField::perlin_height;
  config.amplitude=std::stod(argv[3]);
  config.frequency=std::stod(argv[4]);
  config.phase_x=std::stod(argv[5]);
  config.phase_y=std::stod(argv[6]);
  const double bottom=argc==9?std::stod(argv[8]):-1.0;
  auto request=field=="pit"?pit_request(bottom,config.amplitude,
                                                config.frequency,config.phase_x):field=="structured"?
      tetra::probes::make_structured_two_hex_terrain_volume_request(config):
      tetra::probes::make_heightfield_terrain_volume_request(
          tetra::probes::extract_frozen_dual_contour_surface(config),
          tetra::probes::extract_conservative_regular_core(config),bottom);
  if(!request.accepted()) {
    std::cout<<"request_failure="<<static_cast<unsigned>(request.failure)
             <<" contract_failure="<<static_cast<unsigned>(request.validation.failure)
             <<" failing_element="<<request.validation.failing_element
             <<" related_element="<<request.validation.related_element
             <<" outer_boundary_edges="<<request.validation.outer_boundary_edges
             <<" outer_nonmanifold_edges="<<request.validation.outer_nonmanifold_edges
             <<" core_boundary_faces="<<request.validation.core_boundary_faces
             <<" core_nonmanifold_faces="<<request.validation.core_nonmanifold_faces
             <<" core_clearance="<<request.validation.minimum_core_outer_clearance
             <<'\n';
    return 3;
  }
  tetra::probes::WangConstrainedTetrahedralizationOptions options;
  options.recovery.maximum_vertices=16384U;
  options.recovery.maximum_facets=32768U;
  options.recovery.maximum_tetrahedra=262144U;
  options.recovery.maximum_fhc_steiner_insertions=512U;
  if(argc>=8)
    options.recovery.maximum_fhc_steiner_attempts_per_segment=
        static_cast<std::size_t>(std::stoull(argv[7]));
  options.recovery.maximum_edge_splits=128U;
  options.recovery.maximum_facet_splits=128U;
  const auto result=tetra::probes::run_terrain_wang_viability_experiment(
      request.request,options);
  std::cout<<"accepted="<<result.output_validation.valid
           <<" wang_failure="<<static_cast<unsigned>(result.wang_failure)
           <<" recovery_failure="<<static_cast<unsigned>(result.recovery_failure)
           <<" seed_failure="<<static_cast<unsigned>(result.seed_failure)
           <<" seed_invalid="<<static_cast<unsigned>(result.seed_invalid_reason)
           <<" unsupported="<<static_cast<unsigned>(result.unsupported_branch)
           <<" segment_fhc="<<result.owned_segment_fhc_insertions
           <<" segment_splits="<<result.segment_boundary_splits
           <<" segment_split_failure="
           <<static_cast<unsigned>(result.last_segment_boundary_failure)
           <<'/'<<static_cast<unsigned>(
               result.last_segment_constraint_split_failure)
           <<'/'<<static_cast<unsigned>(
               result.last_segment_split_insertion_failure)
           <<" facet_splits="<<result.facet_boundary_splits
           <<" restored="<<result.boundary_points_restored<<'/'
           <<result.boundary_restoration_attempts
           <<" reverse="<<result.reverse_boundary_restoration_complete
           <<" mesh="<<result.tetrahedra_inspected
           <<" output="<<result.output_validation.valid
           <<" output_failure="<<static_cast<unsigned>(result.output_validation.failure)
           <<" output_degenerate="<<result.output_validation.degenerate_tetrahedra
           <<" output_missing_outer="<<result.output_validation.missing_outer_faces
           <<" output_missing_core="<<result.output_validation.missing_core_tetrahedra
           <<" output_unexpected="<<result.output_validation.unexpected_boundary_faces
           <<" output_invalid_preserved="<<result.output_validation.invalid_preserved_subfaces
           <<" initial_min_six="<<result.initial_minimum_absolute_six_volume
           <<" initial_tiny="<<result.initial_publication_degenerate_tetrahedra
           <<" segment_min_six="<<result.segment_minimum_absolute_six_volume
           <<" segment_tiny="<<result.segment_publication_degenerate_tetrahedra
           <<" repair="<<result.publication_repair_initial_degenerate_tetrahedra
           <<'/'<<result.publication_repair_remaining_degenerate_tetrahedra
           <<" repair_mutations="<<result.publication_repair_accepted_mutations
           <<" bounded="<<result.publication_repair_bounded_cavity_attempts
           <<'/'<<result.publication_repair_bounded_cavity_incompatible_rejections
           <<'/'<<result.publication_repair_bounded_cavity_trial_limit_rejections
           <<'/'<<result.publication_repair_bounded_cavity_inspection_rejections
           <<'/'<<result.publication_repair_bounded_cavity_non_improving_rejections
           <<" repair_invalid="<<result.publication_repair_invalid_candidate_mesh_rejections
           <<'\n';
  if(result.publication_repair_has_first_unrepaired_tetrahedron)
    std::cout<<"repair_first_unrepaired_ids="
             <<result.publication_repair_first_unrepaired_vertex_ids[0]<<','
             <<result.publication_repair_first_unrepaired_vertex_ids[1]<<','
             <<result.publication_repair_first_unrepaired_vertex_ids[2]<<','
             <<result.publication_repair_first_unrepaired_vertex_ids[3]<<'\n';
  for(const auto& incident:result.publication_repair_first_unrepaired_incident_tetrahedra)
    std::cout<<"repair_incident_ids="<<incident[0]<<','<<incident[1]<<','
             <<incident[2]<<','<<incident[3]<<'\n';
  for(const auto& facet:result.publication_repair_first_unrepaired_constrained_facets)
    std::cout<<"repair_constrained_facet_ids="<<facet[0]<<','<<facet[1]<<','
             <<facet[2]<<'\n';
  for(const auto& tet:result.output_degenerate_tetrahedra) {
    std::cout<<"degenerate region="<<static_cast<unsigned>(tet.region)
             <<" six="<<tet.absolute_six_volume<<" ids="
             <<tet.vertex_ids[0]<<','<<tet.vertex_ids[1]<<','
             <<tet.vertex_ids[2]<<','<<tet.vertex_ids[3]<<" points=";
    for(const auto& point:tet.positions)
      std::cout<<'('<<std::setprecision(17)<<point.x<<','<<point.y<<','
               <<point.z<<')';
    std::cout<<'\n';
  }
  if(result.output_validation.valid)
    print_worst_transition_tetrahedra(request.request.contract,result.output,
                                      result.output_cell_regions);
  if(const auto* fixed=std::getenv("TETRA_TERRAIN_PROBE_FIXED_SCAFFOLD")) {
    tetra::Vec3 position{};
    unsigned long long id=std::uint64_t{1}<<63U;
    const auto parsed=std::sscanf(fixed,"%llu,%lf,%lf,%lf",&id,
                                  &position.x,&position.y,&position.z);
    if(parsed!=4&&std::sscanf(fixed,"%lf,%lf,%lf",&position.x,&position.y,&position.z)!=3) {
      std::cerr<<"fixed scaffold must be id,x,y,z or x,y,z\n";
      return 4;
    }
    const std::array<tetra::probes::FrozenFacetVertex,1> scaffold{{
        {static_cast<std::uint64_t>(id),position}}};
    const auto trial=tetra::probes::run_terrain_wang_viability_with_scaffold(
        request.request,options,scaffold);
    const auto quality=tetra::probes::measure_terrain_volume_quality(
        request.request.contract,trial.output,trial.output_cell_regions);
    std::cout<<"fixed_scaffold_recovery="<<trial.output_validation.valid
             <<" fixed_quality="<<quality.minimum_mean_ratio<<'/'
             <<quality.minimum_dihedral_degrees<<'/'<<quality.maximum_dihedral_degrees<<'/'
             <<quality.maximum_edge_ratio
             <<" fixed_violations="<<quality.elements_below_mean_ratio_001<<'/'
             <<quality.dihedrals_below_5_degrees<<'/'
             <<quality.dihedrals_above_175_degrees<<'\n';
  }
  if(std::getenv("TETRA_TERRAIN_PROBE_CONSTRUCT")!=nullptr) {
    const auto volume=tetra::probes::construct_terrain_volume(request.request,options);
    std::cout<<"construct_accepted="<<volume.accepted()
             <<" failure="<<static_cast<unsigned>(volume.failure)
             <<" quality="<<volume.quality.minimum_mean_ratio<<'/'
             <<volume.quality.minimum_dihedral_degrees<<'/'
             <<volume.quality.maximum_dihedral_degrees<<'/'
             <<volume.quality.maximum_edge_ratio
             <<" violations="<<volume.quality.elements_below_mean_ratio_001<<'/'
             <<volume.quality.dihedrals_below_5_degrees<<'/'
             <<volume.quality.dihedrals_above_175_degrees
             <<" scaffold="<<volume.quality_scaffold_selected<<'/'
             <<volume.quality_scaffold_recovery_valid<<'/'
             <<volume.quality_scaffold_candidates
             <<" mutations="<<volume.quality_repair_accepted<<'/'
             <<volume.quality_repair_candidates
             <<" cavity_search="<<volume.quality_cavity_fill_search_nodes<<'/'
             <<volume.quality_cavity_fill_completed_fills<<'/'
             <<volume.quality_cavity_fill_changed_fills<<'/'
             <<volume.quality_cavity_fill_steiner_fills<<'/'
             <<volume.quality_cavity_fill_geometry_valid_fills<<'/'
             <<volume.quality_cavity_fill_quality_improving_fills<<'/'
             <<volume.quality_cavity_fill_trial_limit_rejections<<'\n';
    for(std::size_t index=0U;index<volume.quality_scaffold_selected_positions.size();++index) {
      const auto& point=volume.quality_scaffold_selected_positions[index];
      std::cout<<"scaffold_point="<<volume.quality_scaffold_selected_ids[index]<<','
               <<std::setprecision(17)<<point.x<<','
               <<point.y<<','<<point.z<<'\n';
    }
  }
  if(std::getenv("TETRA_TERRAIN_PROBE_CAVITY_ORACLE")!=nullptr) {
    const auto oracle=tetra::probes::run_terrain_volume_cavity_oracle(request.request,options);
    std::cout<<"oracle_recovery="<<oracle.recovery_valid
             <<" oracle_gate="<<oracle.quality_gate_met
             <<" oracle_validation="<<oracle.validation.valid
             <<" oracle_quality="<<oracle.quality_after.minimum_mean_ratio<<'/'
             <<oracle.quality_after.minimum_dihedral_degrees<<'/'
             <<oracle.quality_after.maximum_dihedral_degrees<<'/'
             <<oracle.quality_after.maximum_edge_ratio
             <<" oracle_violations="<<oracle.quality_after.elements_below_mean_ratio_001<<'/'
             <<oracle.quality_after.dihedrals_below_5_degrees<<'/'
             <<oracle.quality_after.dihedrals_above_175_degrees
             <<" oracle_mutations="<<oracle.accepted_mutations<<'/'<<oracle.candidates
             <<" oracle_cavity="<<oracle.search_nodes<<'/'<<oracle.completed_fills<<'/'
             <<oracle.changed_fills<<'/'<<oracle.steiner_fills<<'/'
             <<oracle.geometry_valid_fills<<'/'<<oracle.quality_improving_fills<<'/'
             <<oracle.trial_limit_rejections<<'\n';
  }
  return 0;
}
