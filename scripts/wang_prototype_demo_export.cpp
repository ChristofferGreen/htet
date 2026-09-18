#include "tetra_probes/advancing_front_fixture.hpp"
#include "tetra_probes/dc_free_volume.hpp"
#include "tetra_probes/terrain_volume_request.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {
using tetra::Vec3;
using tetra::probes::SurfaceCoreTransitionOutput;
using tetra::probes::TerrainVolumeCellRegion;

// FourHexahedra stores corners in cube-bit order
//   000, 100, 010, 110, 001, 101, 011, 111.
// VTK_HEXAHEDRON instead walks each z face around its perimeter, so convert
// explicitly at the file-format boundary.
constexpr std::array<unsigned,8> vtk_corner_from_cube_bit{{
    0U,1U,3U,2U,4U,5U,7U,6U}};

void write_points(std::ostream& out,const std::vector<Vec3>& points) {
  out<<"POINTS "<<points.size()<<" double\n"<<std::setprecision(17);
  for(const auto point:points)out<<point.x<<' '<<point.y<<' '<<point.z<<'\n';
}

template<std::size_t N>
void write_javascript_cells(std::ostream& out,
                            const std::vector<std::array<std::uint32_t,N>>& cells) {
  out<<'[';
  for(std::size_t cell=0;cell<cells.size();++cell) {
    if(cell)out<<',';
    out<<'[';
    for(std::size_t corner=0;corner<N;++corner) {
      if(corner)out<<',';
      out<<cells[cell][corner];
    }
    out<<']';
  }
  out<<']';
}

void write_javascript_points(std::ostream& out,const std::vector<Vec3>& points) {
  out<<'['<<std::setprecision(17);
  for(std::size_t index=0;index<points.size();++index) {
    if(index)out<<',';
    const auto point=points[index];
    out<<'['<<point.x<<','<<point.y<<','<<point.z<<']';
  }
  out<<']';
}

double tetrahedron_volume(const std::vector<Vec3>& points,
                          const std::array<std::uint32_t,4>& tetrahedron) {
  const auto a=points[tetrahedron[0]],b=points[tetrahedron[1]],
             c=points[tetrahedron[2]],d=points[tetrahedron[3]];
  const auto cross=[](Vec3 left,Vec3 right) {
    return Vec3{left.y*right.z-left.z*right.y,
                left.z*right.x-left.x*right.z,
                left.x*right.y-left.y*right.x};
  };
  const auto dot=[](Vec3 left,Vec3 right) {
    return left.x*right.x+left.y*right.y+left.z*right.z;
  };
  return std::abs(dot(b-a,cross(c-a,d-a)))/6.0;
}

bool write_tetrahedra(const std::filesystem::path& path,
                      const std::vector<Vec3>& points,
                      const std::vector<std::array<std::uint32_t,4>>& cells,
                      const std::vector<TerrainVolumeCellRegion>* regions=nullptr) {
  std::ofstream out(path);
  if(!out)return false;
  out<<"# vtk DataFile Version 2.0\nWang four-hexahedra prototype\nASCII\n"
     <<"DATASET UNSTRUCTURED_GRID\n";
  write_points(out,points);
  out<<"CELLS "<<cells.size()<<' '<<cells.size()*5U<<"\n";
  for(const auto cell:cells)
    out<<"4 "<<cell[0]<<' '<<cell[1]<<' '<<cell[2]<<' '<<cell[3]<<'\n';
  out<<"CELL_TYPES "<<cells.size()<<"\n";
  for(std::size_t i=0;i<cells.size();++i)out<<"10\n";
  if(regions) {
    out<<"CELL_DATA "<<cells.size()<<"\nSCALARS region int 1\nLOOKUP_TABLE default\n";
    for(const auto region:*regions)
      out<<(region==TerrainVolumeCellRegion::retained_core?1:0)<<'\n';
  }
  return static_cast<bool>(out);
}

bool write_transition(const std::filesystem::path& path,
                      const std::vector<Vec3>& points,
                      const SurfaceCoreTransitionOutput& output,
                      const std::vector<TerrainVolumeCellRegion>& regions) {
  std::vector<std::array<std::uint32_t,4>> cells;
  for(std::size_t i=0;i<output.tetrahedra.size();++i)
    if(regions[i]==TerrainVolumeCellRegion::transition)
      cells.push_back(output.tetrahedra[i]);
  return write_tetrahedra(path,points,cells);
}
} // namespace

int main(int argc,char** argv) {
  if(argc<2||argc>8) {
    std::cerr<<"usage: wang_prototype_demo_export OUTPUT_DIRECTORY [GRID_RESOLUTION] [uniform|adaptive] [MIN_CORE_LEVEL] [SURFACE_BAND] [FREE_RADIAL_LAYERS] [GENERIC_SAMPLE_BUDGET]\n";
    return 2;
  }
  unsigned grid_resolution=5U;
  if(argc>=3) try {
    grid_resolution=static_cast<unsigned>(std::stoul(argv[2]));
  } catch(...) {
    std::cerr<<"grid resolution must be an integer\n";
    return 2;
  }
  if(grid_resolution<4U||grid_resolution>12U) {
    std::cerr<<"grid resolution must be in [4,12]\n";
    return 2;
  }
  const std::string core_mode=argc>=4?argv[3]:"uniform";
  if(core_mode!="uniform"&&core_mode!="adaptive") {
    std::cerr<<"core mode must be uniform or adaptive\n";
    return 2;
  }
  const std::filesystem::path directory=argv[1];
  std::error_code error;
  std::filesystem::create_directories(directory,error);
  if(error)return 3;

  tetra::probes::AdvancingFrontFixtureConfig config;
  config.field_kind=tetra::probes::AdvancingFrontFieldKind::contained_noisy_sphere;
  config.grid_resolution=grid_resolution;
  const auto core_sizing=tetra::probes::advancing_front_core_sizing(grid_resolution);
  config.core_red_depth=core_sizing.red_depth;
  config.core_mode=core_mode=="adaptive"
      ?tetra::probes::AdvancingFrontCoreMode::surface_distance_adaptive
      :tetra::probes::AdvancingFrontCoreMode::uniform;
  if(argc>=5) try {
    config.core_min_red_depth=static_cast<unsigned>(std::stoul(argv[4]));
  } catch(...) { std::cerr<<"minimum core level must be an integer\n"; return 2; }
  if(argc>=6) try {
    config.core_surface_band_multiplier=std::stod(argv[5]);
  } catch(...) { std::cerr<<"surface band must be a number\n"; return 2; }
  config.core_min_red_depth=std::min(config.core_min_red_depth,config.core_red_depth);
  config.sphere_radius=0.23;
  config.noise_amplitude=0.02;
  config.noise_frequency=4.0;
  config.core_clearance=core_sizing.clearance;

  tetra::probes::WangConstrainedTetrahedralizationOptions options;
  options.recovery.maximum_vertices=16384U;
  options.recovery.maximum_facets=32768U;
  options.recovery.maximum_tetrahedra=262144U;
  options.recovery.maximum_fhc_steiner_insertions=512U;
  options.recovery.maximum_fhc_steiner_attempts_per_segment=32U;
  options.recovery.maximum_edge_splits=128U;
  options.recovery.maximum_facet_splits=128U;
  const auto prototype=tetra::probes::construct_four_hexahedra_wang_prototype(
      config,options);
  const auto& fixture=prototype.fixture;
  if(!fixture.audit.accepted||!prototype.accepted()) {
    std::cerr<<"prototype rejected: fixture="<<fixture.audit.accepted
             <<" dc_boundary_edges="<<fixture.audit.dc_boundary_edges
             <<" dc_nonmanifold_edges="<<fixture.audit.dc_nonmanifold_edges
             <<" dc_closed="<<fixture.audit.dc_closed_two_manifold
             <<" outer_closed="<<fixture.audit.outer_closed_two_manifold
             <<" outer_oriented="<<fixture.audit.outer_consistently_oriented
             <<" outer_self_intersection_free="<<fixture.audit.outer_no_self_intersections
             <<" core_nested="<<fixture.audit.core_strictly_nested
             <<" surface_core_disjoint="<<fixture.audit.surface_core_disjoint
             <<" positive_cavity="<<fixture.audit.positive_cavity_volume
             <<" artificial_closure_faces="<<fixture.audit.artificial_closure_faces
             <<" prototype_failure="<<static_cast<unsigned>(prototype.failure)
             <<" volume_failure="<<static_cast<unsigned>(prototype.volume.failure)
             <<" wang_failure="<<static_cast<unsigned>(prototype.volume.viability.wang_failure)
             <<'\n';
    return 4;
  }
  tetra::probes::ClosedPlcTetrahedralizationOptions free_options;
  if(argc>=7) try {
    free_options.common_kernel_radial_layers=static_cast<unsigned>(std::stoul(argv[6]));
  } catch(...) { std::cerr<<"free radial layers must be an integer\n"; return 2; }
  const auto free_started=std::chrono::steady_clock::now();
  const auto free_volume=tetra::probes::construct_dc_free_volume(fixture,free_options);
  const auto free_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-free_started).count();
  if(!free_volume.accepted()) {
    std::cerr<<"exact DC volume fill rejected: failure="
             <<static_cast<unsigned>(free_volume.failure)
             <<" volume_failure="<<static_cast<unsigned>(free_volume.volume.failure)<<'\n';
    return 15;
  }
  const auto free_boundary_matches_dc=[&] {
    if(free_volume.input.vertices.size()!=fixture.dc_vertices.size()||
       free_volume.input.faces.size()!=fixture.dc_triangles.size())return false;
    for(std::size_t index=0U;index<fixture.dc_vertices.size();++index) {
      const auto& frozen=free_volume.input.vertices[index];
      const auto& source=fixture.dc_vertices[index];
      if(frozen.id!=index+1U||frozen.position.x!=source.x||
         frozen.position.y!=source.y||frozen.position.z!=source.z)return false;
    }
    for(std::size_t index=0U;index<fixture.dc_triangles.size();++index) {
      const auto triangle=fixture.dc_triangles[index];
      if(free_volume.input.faces[index]!=
          std::array<std::uint64_t,3>{{static_cast<std::uint64_t>(triangle[0])+1U,
                                       static_cast<std::uint64_t>(triangle[1])+1U,
                                       static_cast<std::uint64_t>(triangle[2])+1U}})
        return false;
    }
    return true;
  }();
  if(!free_boundary_matches_dc)return 18;
  tetra::probes::DcSurfaceDistanceSamplingOptions generic_sampling;
  generic_sampling.maximum_points=32U;
  generic_sampling.maximum_refinement_passes=1U;
  generic_sampling.maximum_refinement_points_per_pass=32U;
  if(argc>=8) try {
    generic_sampling.maximum_points=static_cast<std::size_t>(std::stoul(argv[7]));
  } catch(...) { std::cerr<<"generic sample budget must be an integer\n"; return 2; }
  const auto generic_started=std::chrono::steady_clock::now();
  const auto generic_volume=tetra::probes::construct_dc_surface_conforming_volume(
      free_volume.input,generic_sampling,options);
  const auto generic_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-generic_started).count();
  if(!generic_volume.accepted()) {
    std::cerr<<"generic DC volume fill rejected: failure="
             <<static_cast<unsigned>(generic_volume.failure)
             <<" wang_failure="<<static_cast<unsigned>(generic_volume.volume.failure)<<'\n';
    return 19;
  }
  std::vector<Vec3> generic_points;
  std::map<std::uint64_t,std::uint32_t> generic_point_index;
  generic_points.reserve(generic_volume.volume.vertices.size());
  for(const auto& vertex:generic_volume.volume.vertices) {
    const auto index=static_cast<std::uint32_t>(generic_points.size());
    if(!generic_point_index.emplace(vertex.id,index).second)return 20;
    generic_points.push_back(vertex.position);
  }
  std::vector<std::array<std::uint32_t,4>> generic_tetrahedra;
  generic_tetrahedra.reserve(generic_volume.volume.tetrahedra.size());
  for(const auto& tet:generic_volume.volume.tetrahedra) {
    std::array<std::uint32_t,4> compact{};
    for(std::size_t corner=0U;corner<compact.size();++corner)
      compact[corner]=generic_point_index.at(tet[corner]);
    generic_tetrahedra.push_back(compact);
  }
  std::vector<Vec3> free_points;
  std::map<std::uint64_t,std::uint32_t> free_point_index;
  free_points.reserve(free_volume.volume.vertices.size());
  for(const auto& vertex:free_volume.volume.vertices) {
    const auto index=static_cast<std::uint32_t>(free_points.size());
    if(!free_point_index.emplace(vertex.id,index).second)return 16;
    free_points.push_back(vertex.position);
  }
  std::vector<std::array<std::uint32_t,4>> free_tetrahedra;
  free_tetrahedra.reserve(free_volume.volume.tetrahedra.size());
  for(const auto& tet:free_volume.volume.tetrahedra) {
    std::array<std::uint32_t,4> compact{};
    for(std::size_t corner=0U;corner<compact.size();++corner)
      compact[corner]=free_point_index.at(tet[corner]);
    free_tetrahedra.push_back(compact);
  }

  {
    std::vector<Vec3> points;
    points.reserve(32U);
    for(const auto& hexahedron:fixture.hexahedra)
      points.insert(points.end(),hexahedron.begin(),hexahedron.end());
    std::ofstream out(directory/"01-four-hexahedra.vtk");
    if(!out)return 5;
    out<<"# vtk DataFile Version 2.0\nTetrahedron subdivided into four hexahedra\n"
       <<"ASCII\nDATASET UNSTRUCTURED_GRID\n";
    write_points(out,points);
    out<<"CELLS 4 36\n";
    for(unsigned cell=0U;cell<4U;++cell) {
      out<<"8";
      for(const auto corner:vtk_corner_from_cube_bit)
        out<<' '<<cell*8U+corner;
      out<<'\n';
    }
    out<<"CELL_TYPES 4\n12\n12\n12\n12\nCELL_DATA 4\n"
       <<"SCALARS hexahedron int 1\nLOOKUP_TABLE default\n0\n1\n2\n3\n";
  }
  {
    std::ofstream out(directory/"02-dual-contour-surface.vtk");
    if(!out)return 6;
    out<<"# vtk DataFile Version 2.0\nFrozen dual-contouring surface\nASCII\n"
       <<"DATASET POLYDATA\n";
    write_points(out,fixture.outer_vertices);
    out<<"POLYGONS "<<fixture.dc_triangles.size()<<' '
       <<fixture.dc_triangles.size()*4U<<"\n";
    for(const auto face:fixture.dc_triangles)
      out<<"3 "<<face[0]<<' '<<face[1]<<' '<<face[2]<<'\n';
  }
  if(!write_tetrahedra(directory/"03-implicit-tetrahedral-core.vtk",
                       fixture.core_vertices,fixture.core_tetrahedra))return 7;

  auto output_points=prototype.request.request.contract.vertices;
  output_points.insert(output_points.end(),prototype.volume.output.owned_vertices.begin(),
                       prototype.volume.output.owned_vertices.end());
  if(prototype.volume.cell_regions.size()!=prototype.volume.output.tetrahedra.size())
    return 8;
  double transition_volume{},published_core_volume{};
  for(std::size_t index=0U;index<prototype.volume.output.tetrahedra.size();++index) {
    const auto volume=tetrahedron_volume(output_points,
                                         prototype.volume.output.tetrahedra[index]);
    if(prototype.volume.cell_regions[index]==TerrainVolumeCellRegion::transition)
      transition_volume+=volume;
    else
      published_core_volume+=volume;
  }
  if(!write_transition(directory/"04-wang-transition.vtk",output_points,
                       prototype.volume.output,prototype.volume.cell_regions))return 9;
  if(!write_tetrahedra(directory/"05-complete-prototype.vtk",output_points,
                       prototype.volume.output.tetrahedra,
                       &prototype.volume.cell_regions))return 10;
  if(!write_tetrahedra(directory/"06-exact-dc-volume-fill.vtk",free_points,
                       free_tetrahedra))return 17;
  if(!write_tetrahedra(directory/"07-generic-dc-volume-fill.vtk",generic_points,
                       generic_tetrahedra))return 21;

  {
    std::vector<Vec3> hexahedron_points;
    hexahedron_points.reserve(32U);
    for(const auto& hexahedron:fixture.hexahedra)
      hexahedron_points.insert(hexahedron_points.end(),hexahedron.begin(),hexahedron.end());
    std::vector<std::array<std::uint32_t,8>> hexahedra;
    hexahedra.reserve(4U);
    for(std::uint32_t cell=0U;cell<4U;++cell) {
      std::array<std::uint32_t,8> vtk_cell{};
      for(std::size_t corner=0;corner<vtk_cell.size();++corner)
        vtk_cell[corner]=cell*8U+vtk_corner_from_cube_bit[corner];
      hexahedra.push_back(vtk_cell);
    }
    std::vector<std::array<std::uint32_t,4>> transition;
    for(std::size_t index=0;index<prototype.volume.output.tetrahedra.size();++index)
      if(prototype.volume.cell_regions[index]==TerrainVolumeCellRegion::transition)
        transition.push_back(prototype.volume.output.tetrahedra[index]);
    std::ofstream data(directory/"prototype-data.js");
    if(!data)return 11;
    data<<"window.WANG_PROTOTYPE_DATA={field:'contained-noisy-sphere',"
        <<"hexahedra:{points:";
    write_javascript_points(data,hexahedron_points);
    data<<",cells:";write_javascript_cells(data,hexahedra);
    data<<"},dc:{points:";write_javascript_points(data,fixture.dc_vertices);
    data<<",triangles:";write_javascript_cells(data,fixture.dc_triangles);
    data<<"},core:{points:";write_javascript_points(data,fixture.core_vertices);
    data<<",tetrahedra:";write_javascript_cells(data,fixture.core_tetrahedra);
    data<<"},transition:{points:";write_javascript_points(data,output_points);
    data<<",tetrahedra:";write_javascript_cells(data,transition);
    data<<"},freeVolume:{points:";write_javascript_points(data,free_points);
    data<<",tetrahedra:";write_javascript_cells(data,free_tetrahedra);
    data<<"},genericVolume:{points:";write_javascript_points(data,generic_points);
    data<<",tetrahedra:";write_javascript_cells(data,generic_tetrahedra);
    data<<"},summary:{gridResolution:"<<grid_resolution
        <<",coreMode:'"<<core_mode<<"'"
        <<",coreRedDepth:"<<config.core_red_depth
        <<",coreMinRedDepth:"<<config.core_min_red_depth
        <<",coreSurfaceBand:"<<config.core_surface_band_multiplier
        <<",coreTetEdge:"<<fixture.audit.maximum_core_tetrahedron_edge_length
        <<",coreClearance:"<<config.core_clearance
        <<",dcTriangles:"<<fixture.dc_triangles.size()
        <<",coreTetrahedra:"<<fixture.core_tetrahedra.size()
        <<",transitionTetrahedra:"<<transition.size()
        <<",freeVolumeTetrahedra:"<<free_tetrahedra.size()
        <<",freeVolumeVertices:"<<free_points.size()
        <<",freeRadialLayers:"<<free_options.common_kernel_radial_layers
        <<",freeVolumeMilliseconds:"<<free_milliseconds
        <<",freeOverlapPairsTested:"<<free_volume.volume.overlap_pairs_tested
        <<",freeExhaustivePairs:"<<free_tetrahedra.size()*(free_tetrahedra.size()-1U)/2U
        <<",freeExactBoundary:"<<(free_volume.volume.exact_boundary?"true":"false")
        <<",freePositive:"<<(free_volume.volume.positive?"true":"false")
        <<",freeNoStrictOverlap:"<<(free_volume.volume.no_strict_overlap?"true":"false")
        <<",freeExactVolume:"<<(free_volume.volume.exact_volume?"true":"false")
        <<",freeBoundaryMatchesDc:"<<(free_boundary_matches_dc?"true":"false")
        <<",genericVolumeTetrahedra:"<<generic_tetrahedra.size()
        <<",genericVolumeVertices:"<<generic_points.size()
        <<",genericSamples:"<<generic_volume.interior_samples.size()
        <<",genericVolumeMilliseconds:"<<generic_milliseconds
        <<",genericExactBoundary:"<<(generic_volume.volume.boundary_audit.accepted()?"true":"false")
        <<",genericMinimumEdge:"<<generic_volume.quality.minimum_edge_length
        <<",genericMaximumEdge:"<<generic_volume.quality.maximum_edge_length
        <<",genericMinimumVolume:"<<generic_volume.quality.minimum_volume
        <<",genericMinimumDihedral:"<<generic_volume.quality.minimum_dihedral_degrees
        <<",genericMaximumDihedral:"<<generic_volume.quality.maximum_dihedral_degrees
        <<",genericMinimumMeanRatio:"<<generic_volume.quality.minimum_mean_ratio
        <<",genericBoundaryTetrahedra:"<<generic_volume.quality.boundary_tetrahedra
        <<",genericInteriorTetrahedra:"<<generic_volume.quality.interior_tetrahedra
        <<",genericOversizedTetrahedra:"<<generic_volume.quality.oversized_tetrahedra
        <<",genericMaximumEdgeTargetRatio:"<<generic_volume.quality.maximum_edge_target_ratio
        <<",genericMaximumVertexValence:"<<generic_volume.quality.maximum_vertex_valence
        <<",genericHighValenceVertices:"<<generic_volume.quality.high_valence_vertices
        <<",genericRefinementPasses:"<<generic_volume.quality.refinement_passes
        <<",genericRefinementPointsAdded:"<<generic_volume.quality.refinement_points_added
        <<",coreVolume:"<<published_core_volume
        <<",transitionVolume:"<<transition_volume
        <<",dcBoundaryEdges:"<<fixture.audit.dc_boundary_edges
        <<",dcNonmanifoldEdges:"<<fixture.audit.dc_nonmanifold_edges
        <<",artificialClosureFaces:"<<fixture.audit.artificial_closure_faces
        <<",coreHierarchyNodesVisited:"<<fixture.audit.core_hierarchy_nodes_visited
        <<",coreRedLeavesSelected:"<<fixture.audit.core_red_leaves_selected
        <<",coreGreenTransitionCells:"<<fixture.audit.core_green_transition_cells
        <<",minimumRetainedCoreRedDepth:"<<fixture.audit.minimum_retained_core_red_depth
        <<",maximumRetainedCoreRedDepth:"<<fixture.audit.maximum_retained_core_red_depth
        <<",fixtureMilliseconds:"<<prototype.fixture_milliseconds
        <<",requestMilliseconds:"<<prototype.request_milliseconds
        <<",wangMilliseconds:"<<prototype.wang_transaction_milliseconds
        <<",wangRecoveryMilliseconds:"<<prototype.volume.wang_recovery_milliseconds
        <<",seedMilliseconds:"<<prototype.volume.viability.seed_milliseconds
        <<",segmentRecoveryMilliseconds:"<<prototype.volume.viability.segment_recovery_milliseconds
        <<",facetRecoveryMilliseconds:"<<prototype.volume.viability.facet_recovery_milliseconds
        <<",recoveryFinalizationMilliseconds:"<<prototype.volume.viability.recovery_finalization_milliseconds
        <<",cleanupMilliseconds:"<<prototype.volume.viability.cleanup_milliseconds
        <<",regionClassificationMilliseconds:"<<prototype.volume.viability.region_classification_milliseconds
        <<",outputValidationMilliseconds:"<<prototype.volume.viability.output_validation_milliseconds
        <<",qualityMilliseconds:"<<prototype.volume.quality_measurement_milliseconds
        <<",totalMilliseconds:"<<prototype.total_milliseconds
        <<"}};\n";
    if(!data)return 12;
  }

  std::ofstream summary(directory/"summary.json");
  if(!summary)return 13;
  summary<<"{\n"
         <<"  \"field\": \"contained-noisy-sphere\",\n"
         <<"  \"grid_resolution\": "<<grid_resolution<<",\n"
         <<"  \"core_mode\": \""<<core_mode<<"\",\n"
         <<"  \"core_red_depth\": "<<config.core_red_depth<<",\n"
         <<"  \"core_min_red_depth\": "<<config.core_min_red_depth<<",\n"
         <<"  \"core_surface_band\": "<<config.core_surface_band_multiplier<<",\n"
         <<"  \"core_tetrahedron_edge_length\": "
         <<fixture.audit.maximum_core_tetrahedron_edge_length<<",\n"
         <<"  \"core_clearance\": "<<config.core_clearance<<",\n"
         <<"  \"sphere_radius\": 0.23,\n"
         <<"  \"noise_amplitude\": 0.02,\n"
         <<"  \"hexahedra\": 4,\n"
         <<"  \"dual_contour_triangles\": "<<fixture.dc_triangles.size()<<",\n"
         <<"  \"dc_closed_two_manifold\": "
         <<(fixture.audit.dc_closed_two_manifold?"true":"false")<<",\n"
         <<"  \"dc_boundary_edges\": "<<fixture.audit.dc_boundary_edges<<",\n"
         <<"  \"dc_nonmanifold_edges\": "<<fixture.audit.dc_nonmanifold_edges<<",\n"
         <<"  \"artificial_closure_faces\": "
         <<fixture.audit.artificial_closure_faces<<",\n"
         <<"  \"core_tetrahedra\": "<<fixture.core_tetrahedra.size()<<",\n"
         <<"  \"transition_tetrahedra\": "<<prototype.volume.quality.transition.tetrahedra<<",\n"
         <<"  \"exact_dc_free_volume_tetrahedra\": "<<free_tetrahedra.size()<<",\n"
         <<"  \"exact_dc_free_volume_vertices\": "<<free_points.size()<<",\n"
         <<"  \"exact_dc_free_volume_radial_layers\": "<<free_options.common_kernel_radial_layers<<",\n"
         <<"  \"exact_dc_free_volume_milliseconds\": "<<free_milliseconds<<",\n"
         <<"  \"exact_dc_free_volume_overlap_pairs_tested\": "<<free_volume.volume.overlap_pairs_tested<<",\n"
         <<"  \"exact_dc_free_volume_accepted\": "<<(free_volume.accepted()?"true":"false")<<",\n"
         <<"  \"exact_dc_free_volume_boundary_matches_dc\": "<<(free_boundary_matches_dc?"true":"false")<<",\n"
         <<"  \"generic_dc_volume_tetrahedra\": "<<generic_tetrahedra.size()<<",\n"
         <<"  \"generic_dc_volume_samples\": "<<generic_volume.interior_samples.size()<<",\n"
         <<"  \"generic_dc_volume_milliseconds\": "<<generic_milliseconds<<",\n"
         <<"  \"generic_dc_volume_literal_boundary\": "<<(generic_volume.volume.boundary_audit.accepted()?"true":"false")<<",\n"
         <<"  \"core_hierarchy_nodes_visited\": "<<fixture.audit.core_hierarchy_nodes_visited<<",\n"
         <<"  \"core_red_leaves_selected\": "<<fixture.audit.core_red_leaves_selected<<",\n"
         <<"  \"core_green_transition_cells\": "<<fixture.audit.core_green_transition_cells<<",\n"
         <<"  \"minimum_retained_core_red_depth\": "<<fixture.audit.minimum_retained_core_red_depth<<",\n"
         <<"  \"maximum_retained_core_red_depth\": "<<fixture.audit.maximum_retained_core_red_depth<<",\n"
         <<"  \"fixture_milliseconds\": "<<prototype.fixture_milliseconds<<",\n"
         <<"  \"request_milliseconds\": "<<prototype.request_milliseconds<<",\n"
         <<"  \"wang_transaction_milliseconds\": "<<prototype.wang_transaction_milliseconds<<",\n"
         <<"  \"wang_recovery_milliseconds\": "<<prototype.volume.wang_recovery_milliseconds<<",\n"
         <<"  \"seed_milliseconds\": "<<prototype.volume.viability.seed_milliseconds<<",\n"
         <<"  \"segment_recovery_milliseconds\": "<<prototype.volume.viability.segment_recovery_milliseconds<<",\n"
         <<"  \"facet_recovery_milliseconds\": "<<prototype.volume.viability.facet_recovery_milliseconds<<",\n"
         <<"  \"recovery_finalization_milliseconds\": "<<prototype.volume.viability.recovery_finalization_milliseconds<<",\n"
         <<"  \"cleanup_milliseconds\": "<<prototype.volume.viability.cleanup_milliseconds<<",\n"
         <<"  \"region_classification_milliseconds\": "<<prototype.volume.viability.region_classification_milliseconds<<",\n"
         <<"  \"output_validation_milliseconds\": "<<prototype.volume.viability.output_validation_milliseconds<<",\n"
         <<"  \"quality_measurement_milliseconds\": "<<prototype.volume.quality_measurement_milliseconds<<",\n"
         <<"  \"total_milliseconds\": "<<prototype.total_milliseconds<<",\n"
         <<"  \"core_volume\": "<<published_core_volume<<",\n"
         <<"  \"transition_volume\": "<<transition_volume<<",\n"
         <<"  \"published_tetrahedra\": "<<prototype.volume.output.tetrahedra.size()<<",\n"
         <<"  \"frozen_outer_faces_preserved\": "
         <<(prototype.volume.validation.frozen_outer_faces_preserved?"true":"false")<<",\n"
         <<"  \"retained_core_preserved\": "
         <<(prototype.volume.validation.retained_core_preserved?"true":"false")<<",\n"
         <<"  \"no_strict_tetrahedron_overlap\": "
         <<(prototype.volume.validation.no_strict_tetrahedron_overlap?"true":"false")<<",\n"
         <<"  \"quality_is_diagnostic_only\": true\n}\n";
  return summary?0:14;
}
