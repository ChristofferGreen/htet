#include "tetra_viewer/terrain_display_front.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace tetra_viewer {
namespace {

struct ChartPoint {
  double x{},z{};
  bool valid{};
};

PreviewCoverageCell coverage_bounds(const PreviewSurfaceFront& preview) {
  if(preview.coverage().covered_cells.empty())return {};
  return {
      std::ranges::min(preview.coverage().covered_cells,{},
          &PreviewCoverageCell::minimum_x).minimum_x,
      std::ranges::min(preview.coverage().covered_cells,{},
          &PreviewCoverageCell::minimum_z).minimum_z,
      std::ranges::max(preview.coverage().covered_cells,{},
          &PreviewCoverageCell::maximum_x).maximum_x,
      std::ranges::max(preview.coverage().covered_cells,{},
          &PreviewCoverageCell::maximum_z).maximum_z};
}

bool finite(tetra::Vec3 value) noexcept {
  return std::isfinite(value.x)&&std::isfinite(value.y)&&
      std::isfinite(value.z);
}

ChartPoint project_to_preview_chart(
    const PreviewSurfaceFront& preview,const tetra::Sphere& field,
    tetra::Vec3 point) noexcept {
  if(!finite(point))return {};
  if(preview.spatial_key().chart==PreviewChart::planar)
    return {point.x,point.z,true};
  if(!(field.terrain.planet_radius>0.0))return {};
  const tetra::Vec3 centre{
      field.centre.x,field.centre.y-field.terrain.planet_radius,field.centre.z};
  const auto offset=point-centre;
  const double magnitude=std::hypot(offset.x,offset.y,offset.z);
  if(!(magnitude>1.0e-15)||!std::isfinite(magnitude))return {};
  const auto direction=offset/magnitude;
  if(!(direction.y>1.0e-6))return {};
  const double x=field.centre.x+
      field.terrain.planet_radius*direction.x/direction.y;
  const double z=field.centre.z+
      field.terrain.planet_radius*direction.z/direction.y;
  return {x,z,std::isfinite(x)&&std::isfinite(z)};
}

tetra::Vec3 world_position(
    const SceneVertex& vertex,tetra::Vec3 origin) noexcept {
  return {origin.x+vertex.position[0],origin.y+vertex.position[1],
          origin.z+vertex.position[2]};
}

bool triangle_intersects_cell(
    const std::array<ChartPoint,3>& triangle,
    const PreviewCoverageCell& cell) noexcept {
  const long double minimum_x=static_cast<long double>(cell.minimum_x);
  const long double maximum_x=static_cast<long double>(cell.maximum_x);
  const long double minimum_z=static_cast<long double>(cell.minimum_z);
  const long double maximum_z=static_cast<long double>(cell.maximum_z);
  const auto separated=[&](long double axis_x,long double axis_z){
    std::array<long double,3> triangle_projection{};
    for(std::size_t index=0;index<3U;++index)
      triangle_projection[index]=axis_x*triangle[index].x+
          axis_z*triangle[index].z;
    const std::array<long double,4> cell_projection{
        axis_x*minimum_x+axis_z*minimum_z,
        axis_x*minimum_x+axis_z*maximum_z,
        axis_x*maximum_x+axis_z*minimum_z,
        axis_x*maximum_x+axis_z*maximum_z};
    const auto [triangle_minimum,triangle_maximum]=
        std::ranges::minmax(triangle_projection);
    const auto [cell_minimum,cell_maximum]=std::ranges::minmax(cell_projection);
    return triangle_maximum<cell_minimum||cell_maximum<triangle_minimum;
  };
  if(separated(1.0L,0.0L)||separated(0.0L,1.0L))return false;
  for(std::size_t edge=0;edge<3U;++edge){
    const auto& first=triangle[edge];
    const auto& second=triangle[(edge+1U)%3U];
    if(separated(-(second.z-first.z),second.x-first.x))return false;
  }
  return true;
}

SceneVertex make_preview_vertex(
    const PreviewSurfaceVertex& source,tetra::Vec3 geometric_normal,
    tetra::Vec3 render_origin,std::size_t corner) {
  SceneVertex result{};
  const auto relative=source.position-render_origin;
  const double values[]{relative.x,relative.y,relative.z,source.normal.x,
                        source.normal.y,source.normal.z};
  if(!std::ranges::all_of(values,[](double value){
       return std::isfinite(value)&&
           value>=-std::numeric_limits<float>::max()&&
           value<=std::numeric_limits<float>::max();
     }))
    throw std::overflow_error("preview vertex exceeds Metal float range");
  result.position[0]=static_cast<float>(relative.x);
  result.position[1]=static_cast<float>(relative.y);
  result.position[2]=static_cast<float>(relative.z);
  result.colour[0]=0.43F;result.colour[1]=0.45F;result.colour[2]=0.47F;
  // Select the production connected-surface shader path. This is render
  // classification only; preview geometry never enters the exact directory.
  result.diagnostics[0]=-2.0F;
  result.normal[0]=static_cast<float>(geometric_normal.x);
  result.normal[1]=static_cast<float>(geometric_normal.y);
  result.normal[2]=static_cast<float>(geometric_normal.z);
  result.smooth_normal[0]=static_cast<float>(source.normal.x);
  result.smooth_normal[1]=static_cast<float>(source.normal.y);
  result.smooth_normal[2]=static_cast<float>(source.normal.z);
  result.barycentric[corner]=1.0F;
  return result;
}

tetra::Vec3 geometric_normal(
    tetra::Vec3 first,tetra::Vec3 second,tetra::Vec3 third) noexcept {
  const auto a=second-first;
  const auto b=third-first;
  const tetra::Vec3 cross{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,
                          a.x*b.y-a.y*b.x};
  const double length=std::hypot(cross.x,cross.y,cross.z);
  return length>1.0e-30?cross/length:tetra::Vec3{0.0,1.0,0.0};
}

}  // namespace

bool preview_coverage_contains_world_point(
    const PreviewSurfaceFront& preview,const tetra::Sphere& field,
    tetra::Vec3 world_point) noexcept {
  if(preview.level_origins().empty())return false;
  const double half_spacing=preview.level_origins().front().sample_spacing*0.5;
  if(!(half_spacing>0.0)||!std::isfinite(half_spacing))return false;
  const auto projected=project_to_preview_chart(preview,field,world_point);
  if(!projected.valid)return false;
  const long double lattice_x=projected.x/half_spacing;
  const long double lattice_z=projected.z/half_spacing;
  return std::ranges::any_of(
      preview.coverage().covered_cells,[&](const PreviewCoverageCell& cell){
    return lattice_x>=static_cast<long double>(cell.minimum_x)&&
        lattice_x<static_cast<long double>(cell.maximum_x)&&
        lattice_z>=static_cast<long double>(cell.minimum_z)&&
        lattice_z<static_cast<long double>(cell.maximum_z);
  });
}

bool preview_coverage_intersects_world_triangle(
    const PreviewSurfaceFront& preview,const tetra::Sphere& field,
    std::array<tetra::Vec3,3> world_triangle) noexcept {
  if(preview.level_origins().empty()||
     preview.coverage().covered_cells.empty())return false;
  const double half_spacing=preview.level_origins().front().sample_spacing*0.5;
  if(!(half_spacing>0.0)||!std::isfinite(half_spacing))return false;
  std::array<ChartPoint,3> projected{};
  for(std::size_t index=0;index<3U;++index){
    projected[index]=project_to_preview_chart(preview,field,world_triangle[index]);
    if(!projected[index].valid)return false;
    projected[index].x/=half_spacing;
    projected[index].z/=half_spacing;
  }
  const auto outer=coverage_bounds(preview);
  if(!triangle_intersects_cell(projected,outer))return false;
  return std::ranges::any_of(
      preview.coverage().covered_cells,[&](const PreviewCoverageCell& cell){
    return triangle_intersects_cell(projected,cell);
  });
}

TerrainDisplayComposition compose_terrain_display(
    std::span<const SceneVertex> exact_vertices,tetra::Vec3 render_origin,
    const tetra::Sphere& field,const PreviewSurfaceFront& preview) {
  if(exact_vertices.size()%3U!=0U)
    throw std::invalid_argument("exact terrain vertex count is not triangular");
  if(!finite(render_origin))
    throw std::invalid_argument("terrain display render origin is non-finite");
  if(preview.field_signature()!=terrain_field_signature(field))
    throw std::invalid_argument("preview and exact fields do not match");

  TerrainDisplayComposition result;
  result.metrics.suppressed_minimum_world_y=
      std::numeric_limits<double>::infinity();
  result.metrics.suppressed_maximum_world_y=
      -std::numeric_limits<double>::infinity();
  result.metrics.exact_input_triangles=exact_vertices.size()/3U;
  result.exact_indices.reserve(exact_vertices.size());
  if(preview.level_origins().empty()||
     preview.coverage().covered_cells.empty())
    throw std::invalid_argument("preview coverage is empty");
  for(std::size_t begin=0;begin<exact_vertices.size();begin+=3U){
    const std::array<tetra::Vec3,3> triangle{
        world_position(exact_vertices[begin],render_origin),
        world_position(exact_vertices[begin+1U],render_origin),
        world_position(exact_vertices[begin+2U],render_origin)};
    // Exact triangles may be much larger than the preview cells.  Keeping a
    // triangle simply because one of its vertices lies outside the preview
    // leaves its interior drawn over the replacement surface, which produces
    // the visibly floating coarse sheets during an interactive update.  The
    // preview has a welded guard ring, so it owns every triangle that touches
    // its covered cells; the exact front owns only disjoint triangles.
    if(preview_coverage_intersects_world_triangle(preview,field,triangle)){
      ++result.metrics.exact_suppressed_triangles;
      for(const auto point:triangle){
        result.metrics.suppressed_minimum_world_y=std::min(
            result.metrics.suppressed_minimum_world_y,point.y);
        result.metrics.suppressed_maximum_world_y=std::max(
            result.metrics.suppressed_maximum_world_y,point.y);
      }
      continue;
    }
    if(begin>std::numeric_limits<std::uint32_t>::max()-2U)
      throw std::overflow_error("exact terrain index exceeds 32-bit range");
    result.exact_indices.push_back(static_cast<std::uint32_t>(begin));
    result.exact_indices.push_back(static_cast<std::uint32_t>(begin+1U));
    result.exact_indices.push_back(static_cast<std::uint32_t>(begin+2U));
    ++result.metrics.exact_selected_triangles;
  }

  if(preview.indices().size()%3U!=0U)
    throw std::invalid_argument("preview index count is not triangular");
  if(std::ranges::any_of(preview.indices(),[&](std::uint32_t index){
       return index>=preview.vertices().size();
     }))
      throw std::invalid_argument("preview index is out of range");
  result.preview_vertices.reserve(preview.indices().size());
  result.preview_indices.reserve(preview.indices().size());
  for(std::size_t begin=0;begin<preview.indices().size();begin+=3U){
    const std::array<std::uint32_t,3> indices{
        preview.indices()[begin],preview.indices()[begin+1U],
        preview.indices()[begin+2U]};
    const auto normal=geometric_normal(
        preview.vertices()[indices[0]].position,
        preview.vertices()[indices[1]].position,
        preview.vertices()[indices[2]].position);
    for(std::size_t corner=0;corner<3U;++corner){
      if(result.preview_vertices.size()>
         std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("expanded preview index exceeds 32-bit range");
      result.preview_indices.push_back(static_cast<std::uint32_t>(
          result.preview_vertices.size()));
      result.preview_vertices.push_back(make_preview_vertex(
          preview.vertices()[indices[corner]],normal,render_origin,corner));
    }
  }
  result.metrics.preview_triangles=result.preview_indices.size()/3U;
  if(result.metrics.exact_suppressed_triangles==0U){
    result.metrics.suppressed_minimum_world_y=0.0;
    result.metrics.suppressed_maximum_world_y=0.0;
  }
  result.metrics.upload_bytes=result.exact_indices.size()*sizeof(std::uint32_t)+
      result.preview_vertices.size()*sizeof(SceneVertex)+
      result.preview_indices.size()*sizeof(std::uint32_t);
  return result;
}

bool TerrainDisplayPublicationPlanner::prepare(
    TerrainDisplayIdentity identity,std::size_t upload_bytes,
    std::size_t maximum_upload_bytes) {
  if(!identity.valid()||!finite(identity.render_origin))
    throw std::invalid_argument("terrain display identity is invalid");
  state_.candidate.reset();
  state_.candidate_upload_bytes=0U;
  if(upload_bytes>maximum_upload_bytes){
    state_.last_outcome=TerrainDisplayPublicationOutcome::resource_rejected;
    return false;
  }
  state_.candidate=std::move(identity);
  state_.candidate_upload_bytes=upload_bytes;
  state_.last_outcome=TerrainDisplayPublicationOutcome::upload_pending;
  return true;
}

bool TerrainDisplayPublicationPlanner::complete(
    const TerrainDisplayIdentity& identity,bool upload_succeeded,
    const TerrainDisplayIdentity& current_identity) {
  if(!state_.candidate||*state_.candidate!=identity||identity!=current_identity){
    state_.candidate.reset();
    state_.candidate_upload_bytes=0U;
    state_.last_outcome=TerrainDisplayPublicationOutcome::stale;
    return false;
  }
  if(!upload_succeeded){
    state_.candidate.reset();
    state_.candidate_upload_bytes=0U;
    state_.last_outcome=TerrainDisplayPublicationOutcome::upload_failed;
    return false;
  }
  state_.published=identity;
  state_.candidate.reset();
  state_.candidate_upload_bytes=0U;
  state_.last_outcome=TerrainDisplayPublicationOutcome::published;
  return true;
}

void TerrainDisplayPublicationPlanner::cancel() noexcept {
  state_.candidate.reset();
  state_.candidate_upload_bytes=0U;
  if(state_.last_outcome==TerrainDisplayPublicationOutcome::upload_pending)
    state_.last_outcome=TerrainDisplayPublicationOutcome::stale;
}

}  // namespace tetra_viewer
