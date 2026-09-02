#include "tetra_viewer/preview_surface.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <utility>

namespace tetra_viewer {
namespace {

constexpr std::uint64_t fnv_offset=1469598103934665603ULL;
constexpr std::uint64_t fnv_prime=1099511628211ULL;

void hash_u64(std::uint64_t& hash,std::uint64_t value) noexcept {
  for(unsigned int byte=0;byte<8U;++byte){
    hash^=(value>>(byte*8U))&0xffU;
    hash*=fnv_prime;
  }
}

void hash_double(std::uint64_t& hash,double value) noexcept {
  hash_u64(hash,std::bit_cast<std::uint64_t>(value));
}

struct SampleKey {
  std::int64_t x{},z{};
  auto operator<=>(const SampleKey&) const=default;
};

struct EdgeKey {
  std::uint32_t first{},second{};
  auto operator<=>(const EdgeKey&) const=default;
};

void validate_configuration(PreviewSurfaceConfiguration configuration) {
  if(configuration.level_count<1U||configuration.level_count>16U)
    throw std::invalid_argument("preview level count must be between 1 and 16");
  if(configuration.cells_per_side<4U||
     configuration.cells_per_side>1024U||
     configuration.cells_per_side%4U!=0U)
    throw std::invalid_argument(
        "preview cells per side must be a multiple of four from 4 to 1024");
  if(!(configuration.finest_spacing>0.0)||
     !std::isfinite(configuration.finest_spacing)||
     !(configuration.finest_spacing*0.5>0.0)||
     !std::isfinite(std::ldexp(
         configuration.finest_spacing,
         static_cast<int>(configuration.level_count-1U))))
    throw std::invalid_argument(
        "preview spacing range must remain finite and positive");
}

struct ChartProjection {
  PreviewSupportReason reason{PreviewSupportReason::supported};
  double x{},z{};
};

ChartProjection preview_chart_projection(
    const tetra::Sphere& field,tetra::Vec3 position) noexcept {
  if(!std::isfinite(position.x)||!std::isfinite(position.y)||
     !std::isfinite(position.z))
    return {PreviewSupportReason::non_finite_projection};
  if(!(field.terrain.planet_radius>0.0))
    return {PreviewSupportReason::supported,position.x,position.z};
  const tetra::Vec3 planet_centre{
      field.centre.x,field.centre.y-field.terrain.planet_radius,field.centre.z};
  const auto offset=position-planet_centre;
  const double magnitude=std::hypot(offset.x,offset.y,offset.z);
  if(!std::isfinite(magnitude))
    return {PreviewSupportReason::non_finite_projection};
  if(!(magnitude>1.0e-15))
    return {PreviewSupportReason::outside_chart_hemisphere};
  const auto direction=offset/magnitude;
  if(direction.y<=1.0e-6)
    return {PreviewSupportReason::outside_chart_hemisphere};
  const double x=field.centre.x+
      field.terrain.planet_radius*direction.x/direction.y;
  const double z=field.centre.z+
      field.terrain.planet_radius*direction.z/direction.y;
  if(!std::isfinite(x)||!std::isfinite(z))
    return {PreviewSupportReason::non_finite_projection};
  return {PreviewSupportReason::supported,x,z};
}

std::optional<std::int64_t> snapped_half_lattice(
    double coordinate,double spacing) noexcept {
  const double value=std::floor(coordinate/spacing);
  constexpr auto minimum=std::numeric_limits<std::int64_t>::min()/2;
  constexpr auto maximum=std::numeric_limits<std::int64_t>::max()/2;
  const double safe_maximum=std::nextafter(
      static_cast<double>(maximum),0.0);
  if(!std::isfinite(value)||value<static_cast<double>(minimum)||
     value>safe_maximum)return std::nullopt;
  return static_cast<std::int64_t>(value)*2;
}

bool clipmap_extent_representable(
    std::int64_t origin,PreviewSurfaceConfiguration configuration) noexcept {
  const std::int64_t half_cells=
      static_cast<std::int64_t>(configuration.cells_per_side/2U);
  for(std::uint32_t level=0;level<configuration.level_count;++level){
    const std::int64_t step=std::int64_t{2}<<level;
    const std::int64_t extent=half_cells*step;
    if(origin<std::numeric_limits<std::int64_t>::min()+extent||
       origin>std::numeric_limits<std::int64_t>::max()-extent)return false;
  }
  return true;
}

struct PreviewResourceEstimate {
  std::size_t cells{};
  std::size_t vertices{};
  std::size_t indices{};
  std::size_t cpu_bytes{};
  std::size_t upload_bytes{};
};

PreviewResourceEstimate preview_resource_estimate(
    PreviewSurfaceConfiguration configuration) noexcept {
  const std::size_t cells=static_cast<std::size_t>(configuration.level_count)*
      configuration.cells_per_side*configuration.cells_per_side;
  const std::size_t vertices=cells*9U;
  const std::size_t indices=cells*24U;
  const std::size_t levels=configuration.level_count;
  const std::size_t geometry=vertices*sizeof(PreviewSurfaceVertex)+
      indices*sizeof(std::uint32_t);
  const std::size_t coverage=cells*sizeof(PreviewCoverageCell);
  const std::size_t metadata=levels*(sizeof(PreviewSurfaceDrawRange)+
      sizeof(PreviewSurfaceLevelOrigin)+sizeof(PreviewLevelOriginKey)+
      sizeof(PreviewLevelOriginGuard));
  const std::size_t map_scratch=vertices*(sizeof(SampleKey)+
      sizeof(std::uint32_t)+4U*sizeof(void*));
  return {cells,vertices,indices,
          geometry+coverage+metadata+map_scratch,geometry+coverage};
}

std::uint64_t configuration_signature(
    PreviewSurfaceConfiguration configuration) noexcept {
  std::uint64_t hash=fnv_offset;
  hash_u64(hash,configuration.level_count);
  hash_u64(hash,configuration.cells_per_side);
  hash_double(hash,configuration.finest_spacing);
  return hash;
}

PreviewSurfaceVertex sample_vertex(
    const tetra::Sphere& field,SampleKey key,double half_spacing) {
  const double chart_x=static_cast<double>(key.x)*half_spacing;
  const double chart_z=static_cast<double>(key.z)*half_spacing;
  const auto sample=tetra::terrain_surface_sample(field,chart_x,chart_z);
  return {sample.position,sample.normal,key.x,key.z};
}

std::uint64_t geometry_hash(
    std::span<const PreviewSurfaceVertex> vertices,
    std::span<const std::uint32_t> indices) noexcept {
  std::uint64_t hash=fnv_offset;
  hash_u64(hash,vertices.size());hash_u64(hash,indices.size());
  for(const auto& vertex:vertices){
    hash_u64(hash,static_cast<std::uint64_t>(vertex.sample_x));
    hash_u64(hash,static_cast<std::uint64_t>(vertex.sample_z));
    hash_double(hash,vertex.position.x);hash_double(hash,vertex.position.y);
    hash_double(hash,vertex.position.z);hash_double(hash,vertex.normal.x);
    hash_double(hash,vertex.normal.y);hash_double(hash,vertex.normal.z);
  }
  for(const auto index:indices)hash_u64(hash,index);
  return hash;
}

}  // namespace

bool preview_surface_supported(const tetra::Sphere& field) noexcept {
  return field.kind==tetra::ImplicitShapeKind::perlin_terrain;
}

std::uint64_t preview_surface_field_signature(
    const tetra::Sphere& field) noexcept {
  return terrain_field_signature(field);
}

PreviewSupportDecision plan_preview_surface(
    const TerrainViewIdentity& view,const tetra::Camera& camera,
    const tetra::Sphere& field,PreviewSurfaceConfiguration configuration) {
  validate_configuration(configuration);
  if(!view.valid())
    throw std::invalid_argument("preview request view identity is invalid");
  if(view.camera_signature!=terrain_camera_signature(camera))
    throw std::invalid_argument("preview request camera identity is stale");
  if(view.field_signature!=preview_surface_field_signature(field))
    throw std::invalid_argument("preview request field identity is stale");
  if(!preview_surface_supported(field))
    return {PreviewSupportReason::unsupported_field,std::nullopt};
  const auto chart=preview_chart_projection(field,camera.position);
  if(chart.reason!=PreviewSupportReason::supported)
    return {chart.reason,std::nullopt};
  const auto origin_x=snapped_half_lattice(
      chart.x,configuration.finest_spacing);
  const auto origin_z=snapped_half_lattice(
      chart.z,configuration.finest_spacing);
  if(!origin_x||!origin_z)
    return {PreviewSupportReason::lattice_range_exceeded,std::nullopt};
  if(!clipmap_extent_representable(*origin_x,configuration)||
     !clipmap_extent_representable(*origin_z,configuration))
    return {PreviewSupportReason::clipmap_extent_exceeded,std::nullopt};
  PreviewSpatialKey result{
      .field_revision=view.field_revision,
      .field_signature=view.field_signature,
      .chart=field.terrain.planet_radius>0.0
          ?PreviewChart::north_pole_gnomonic:PreviewChart::planar,
      .configuration_signature=configuration_signature(configuration)};
  result.level_origins.reserve(configuration.level_count);
  for(std::uint32_t level=0;level<configuration.level_count;++level)
    result.level_origins.push_back({level,*origin_x,*origin_z});
  return {PreviewSupportReason::supported,std::move(result)};
}

PreviewSurfaceBuildResult build_preview_surface(
    const PreviewSurfaceRequest& request,const tetra::Sphere& field,
    PreviewSurfaceConfiguration configuration,
    PreviewSurfaceResourceLimits resource_limits) {
  PreviewSupportDecision plan;
  try {
    plan=plan_preview_surface(
        request.requested_view,request.camera,field,configuration);
  }catch(const std::invalid_argument&){
    throw;
  }catch(const std::bad_alloc&){
    return {PreviewFrontOutcome::resource_rejected,nullptr};
  }catch(const std::length_error&){
    return {PreviewFrontOutcome::resource_rejected,nullptr};
  }catch(...){
    return {PreviewFrontOutcome::failed,nullptr};
  }
  if(!plan.supported())
    return {PreviewFrontOutcome::unsupported,nullptr};
  if(request.spatial_key!=*plan.spatial_key)
    throw std::invalid_argument("preview request spatial key is stale");
  const auto estimate=preview_resource_estimate(configuration);
  if(estimate.cpu_bytes>resource_limits.maximum_cpu_bytes||
     estimate.upload_bytes>resource_limits.maximum_upload_bytes)
    return {PreviewFrontOutcome::resource_rejected,nullptr};

  try {
  const auto started=std::chrono::steady_clock::now();
  const auto& spatial_key=*plan.spatial_key;
  auto front=std::shared_ptr<PreviewSurfaceFront>(new PreviewSurfaceFront);
  front->requested_view_=request.requested_view;
  front->coverage_.spatial_key=spatial_key;
  front->source_camera_=request.camera;
  front->vertices_.reserve(estimate.vertices);
  front->indices_.reserve(estimate.indices);
  front->draw_ranges_.reserve(configuration.level_count);
  front->level_origins_.reserve(configuration.level_count);
  front->coverage_.covered_cells.reserve(estimate.cells);
  const double half_spacing=configuration.finest_spacing*0.5;
  const SampleKey origin{spatial_key.level_origins.front().sample_x,
                         spatial_key.level_origins.front().sample_z};
  constexpr std::int64_t guard_lattice_cells=2;
  front->coverage_.guarded_level_origins.reserve(
      spatial_key.level_origins.size());
  for(const auto& level_origin:spatial_key.level_origins)
    front->coverage_.guarded_level_origins.push_back(
        {level_origin.level,
         level_origin.sample_x-guard_lattice_cells,
         level_origin.sample_z-guard_lattice_cells,
         level_origin.sample_x+guard_lattice_cells,
         level_origin.sample_z+guard_lattice_cells});

  std::map<SampleKey,std::uint32_t> vertex_directory;
  const auto vertex=[&](SampleKey key){
    if(const auto found=vertex_directory.find(key);
       found!=vertex_directory.end())return found->second;
    if(front->vertices_.size()>=std::numeric_limits<std::uint32_t>::max())
      throw std::length_error("preview vertex index exceeds 32-bit range");
    const auto index=static_cast<std::uint32_t>(front->vertices_.size());
    const auto sampled=sample_vertex(field,key,half_spacing);
    const double values[]{sampled.position.x,sampled.position.y,
        sampled.position.z,sampled.normal.x,sampled.normal.y,sampled.normal.z};
    if(!std::ranges::all_of(values,[](double value){
         return std::isfinite(value);
       }))
      throw std::runtime_error("preview terrain sample is non-finite");
    front->vertices_.push_back(sampled);
    vertex_directory.emplace(key,index);
    return index;
  };

  const std::int64_t half_cells=
      static_cast<std::int64_t>(configuration.cells_per_side/2U);
  for(std::uint32_t level=0;level<configuration.level_count;++level){
    const std::int64_t step=std::int64_t{2}<<level;
    front->level_origins_.push_back({level,origin.x,origin.z,
        std::ldexp(configuration.finest_spacing,static_cast<int>(level))});
    const auto first_index=static_cast<std::uint32_t>(front->indices_.size());
    for(std::int64_t row=-half_cells;row<half_cells;++row){
      for(std::int64_t column=-half_cells;column<half_cells;++column){
        if(level>0U){
          const std::int64_t inner=half_cells*step/2;
          const std::int64_t x0=column*step,x1=(column+1)*step;
          const std::int64_t z0=row*step,z1=(row+1)*step;
          if(x0>=-inner&&x1<=inner&&z0>=-inner&&z1<=inner)continue;
        }
        const SampleKey corners[]{
            {origin.x+column*step,origin.z+row*step},
            {origin.x+column*step,origin.z+(row+1)*step},
            {origin.x+(column+1)*step,origin.z+(row+1)*step},
            {origin.x+(column+1)*step,origin.z+row*step}};
        front->coverage_.covered_cells.push_back(
            {corners[0].x,corners[0].z,corners[2].x,corners[2].z});
        for(const auto key:corners)(void)vertex(key);
        std::vector<SampleKey> polygon;
        polygon.reserve(8U);
        for(std::size_t corner=0;corner<4U;++corner){
          const auto first=corners[corner];
          const auto second=corners[(corner+1U)%4U];
          polygon.push_back(first);
          const SampleKey midpoint{
              (first.x+second.x)/2,(first.z+second.z)/2};
          if(vertex_directory.contains(midpoint))polygon.push_back(midpoint);
        }
        const SampleKey centre{
            origin.x+column*step+step/2,
            origin.z+row*step+step/2};
        const auto centre_index=vertex(centre);
        for(std::size_t edge=0;edge<polygon.size();++edge){
          front->indices_.push_back(centre_index);
          front->indices_.push_back(vertex(polygon[edge]));
          front->indices_.push_back(vertex(polygon[(edge+1U)%polygon.size()]));
        }
      }
    }
    const auto index_count=static_cast<std::uint32_t>(
        front->indices_.size()-first_index);
    front->draw_ranges_.push_back(
        {level,first_index,index_count,
         std::ldexp(configuration.finest_spacing,static_cast<int>(level))});
  }

  if(front->vertices_.empty())throw std::logic_error("preview surface is empty");
  const double infinity=std::numeric_limits<double>::infinity();
  front->covered_world_bounds_.minimum={infinity,infinity,infinity};
  front->covered_world_bounds_.maximum={-infinity,-infinity,-infinity};
  for(const auto& item:front->vertices_){
    auto& minimum=front->covered_world_bounds_.minimum;
    auto& maximum=front->covered_world_bounds_.maximum;
    minimum.x=std::min(minimum.x,item.position.x);
    minimum.y=std::min(minimum.y,item.position.y);
    minimum.z=std::min(minimum.z,item.position.z);
    maximum.x=std::max(maximum.x,item.position.x);
    maximum.y=std::max(maximum.y,item.position.y);
    maximum.z=std::max(maximum.z,item.position.z);
  }
  std::map<EdgeKey,std::size_t> incidence;
  for(std::size_t index=0;index<front->indices_.size();index+=3U){
    for(std::size_t edge=0;edge<3U;++edge){
      auto first=front->indices_[index+edge];
      auto second=front->indices_[index+(edge+1U)%3U];
      if(second<first)std::swap(first,second);
      ++incidence[{first,second}];
    }
  }
  for(const auto& [edge,count]:incidence){
    (void)edge;
    front->diagnostics_.boundary_edge_count+=count==1U?1U:0U;
    front->diagnostics_.maximum_edge_incidence=std::max(
        front->diagnostics_.maximum_edge_incidence,count);
  }
  front->diagnostics_.vertex_count=front->vertices_.size();
  front->diagnostics_.triangle_count=front->indices_.size()/3U;
  front->diagnostics_.cpu_bytes=
      front->vertices_.size()*sizeof(PreviewSurfaceVertex)+
      front->indices_.size()*sizeof(std::uint32_t)+
      front->draw_ranges_.size()*sizeof(PreviewSurfaceDrawRange)+
      front->level_origins_.size()*sizeof(PreviewSurfaceLevelOrigin)+
      front->coverage_.spatial_key.level_origins.size()*
          sizeof(PreviewLevelOriginKey)+
      front->coverage_.covered_cells.size()*sizeof(PreviewCoverageCell)+
      front->coverage_.guarded_level_origins.size()*
          sizeof(PreviewLevelOriginGuard);
  front->diagnostics_.upload_bytes=
      front->vertices_.size()*sizeof(PreviewSurfaceVertex)+
      front->indices_.size()*sizeof(std::uint32_t)+
      front->coverage_.covered_cells.size()*sizeof(PreviewCoverageCell);
  front->diagnostics_.geometry_hash=geometry_hash(
      front->vertices_,front->indices_);
  front->diagnostics_.build_milliseconds=
      std::chrono::duration<double,std::milli>(
          std::chrono::steady_clock::now()-started).count();
  return {PreviewFrontOutcome::ready,std::move(front)};
  }catch(const std::bad_alloc&){
    return {PreviewFrontOutcome::resource_rejected,nullptr};
  }catch(const std::length_error&){
    return {PreviewFrontOutcome::resource_rejected,nullptr};
  }catch(...){
    return {PreviewFrontOutcome::failed,nullptr};
  }
}

}  // namespace tetra_viewer
