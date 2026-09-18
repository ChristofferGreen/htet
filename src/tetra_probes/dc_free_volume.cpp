#include "tetra_probes/dc_free_volume.hpp"

namespace tetra::probes {

DcFreeVolumeInput make_dc_free_volume_input(const AdvancingFrontFixture& fixture) {
  DcFreeVolumeInput result;
  result.vertices.reserve(fixture.dc_vertices.size());
  for(std::size_t index=0U;index<fixture.dc_vertices.size();++index)
    result.vertices.push_back({static_cast<std::uint64_t>(index)+1U,
                               fixture.dc_vertices[index]});
  result.faces.reserve(fixture.dc_triangles.size());
  for(const auto triangle:fixture.dc_triangles)
    result.faces.push_back({{
        static_cast<std::uint64_t>(triangle[0])+1U,
        static_cast<std::uint64_t>(triangle[1])+1U,
        static_cast<std::uint64_t>(triangle[2])+1U}});
  return result;
}

DcFreeVolumeResult construct_dc_free_volume(
    const AdvancingFrontFixture& fixture,
    const ClosedPlcTetrahedralizationOptions& options) {
  DcFreeVolumeResult result;
  if(!fixture.audit.dc_closed_two_manifold||
     !fixture.audit.dc_consistently_oriented||
     fixture.audit.dc_boundary_edges!=0U||
     fixture.audit.dc_nonmanifold_edges!=0U)
    return result;
  result.input=make_dc_free_volume_input(fixture);
  result.volume=tetrahedralize_closed_plc(result.input.vertices,
                                          result.input.faces,options);
  result.failure=result.volume.accepted()?DcFreeVolumeFailure::none:
      DcFreeVolumeFailure::tetrahedralization_failed;
  return result;
}

} // namespace tetra::probes
