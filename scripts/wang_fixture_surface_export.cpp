#include "tetra_probes/advancing_front_fixture.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

int main(int argc,char** argv) {
  if(argc!=4) {
    std::cerr<<"usage: wang_fixture_surface_export OUTPUT GRID_RESOLUTION NOISE\n";
    return 2;
  }
  tetra::probes::AdvancingFrontFixtureConfig config;
  config.grid_resolution=static_cast<unsigned>(std::stoul(argv[2]));
  config.noise_amplitude=std::stod(argv[3]);
  const auto fixture=tetra::probes::build_advancing_front_fixture(config);
  if(!fixture.audit.accepted)return 3;
  std::ofstream out(argv[1]);
  if(!out)return 4;
  const auto point_count=fixture.outer_vertices.size()+fixture.core_vertices.size();
  const auto face_count=fixture.outer_triangles.size()+fixture.core_boundary_triangles.size();
  out<<"# vtk DataFile Version 2.0\nWang DC/core seed differential\nASCII\n"
     <<"DATASET POLYDATA\nPOINTS "<<point_count<<" double\n"
     <<std::setprecision(17);
  for(const auto point:fixture.outer_vertices)
    out<<point.x<<' '<<point.y<<' '<<point.z<<'\n';
  for(const auto point:fixture.core_vertices)
    out<<point.x<<' '<<point.y<<' '<<point.z<<'\n';
  out<<"POLYGONS "<<face_count<<' '<<face_count*4U<<'\n';
  for(const auto face:fixture.outer_triangles)
    out<<"3 "<<face[0]<<' '<<face[1]<<' '<<face[2]<<'\n';
  const auto offset=static_cast<unsigned>(fixture.outer_vertices.size());
  for(const auto face:fixture.core_boundary_triangles)
    out<<"3 "<<offset+face[0]<<' '<<offset+face[1]<<' '<<offset+face[2]<<'\n';
  return out?0:5;
}
