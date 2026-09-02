#include "tetra_viewer/preview_surface.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
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

double length(tetra::Vec3 value) {
  return std::sqrt(value.x*value.x+value.y*value.y+value.z*value.z);
}

tetra::Vec3 normalized(tetra::Vec3 value,tetra::Vec3 fallback={0.0,1.0,0.0}) {
  const double magnitude=length(value);
  return magnitude>1.0e-15?value/magnitude:fallback;
}

struct SampleKey {
  std::int64_t x{},z{};
  auto operator<=>(const SampleKey&) const=default;
};

struct EdgeKey {
  std::uint32_t first{},second{};
  auto operator<=>(const EdgeKey&) const=default;
};

tetra::Vec3 preview_chart_coordinates(
    const tetra::Sphere& field,tetra::Vec3 position) {
  if(!(field.terrain.planet_radius>0.0))return position;
  const tetra::Vec3 planet_centre{
      field.centre.x,field.centre.y-field.terrain.planet_radius,field.centre.z};
  const auto direction=normalized(position-planet_centre);
  if(direction.y<=1.0e-6)
    throw std::invalid_argument(
        "preview camera is outside the planetary north-pole height chart");
  return {field.centre.x+field.terrain.planet_radius*direction.x/direction.y,
          position.y,
          field.centre.z+field.terrain.planet_radius*direction.z/direction.y};
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

std::shared_ptr<const PreviewSurfaceFront> build_preview_surface_front(
    const PreviewSurfaceRequest& request,const tetra::Sphere& field,
    PreviewSurfaceConfiguration configuration) {
  const auto started=std::chrono::steady_clock::now();
  if(!preview_surface_supported(field))
    throw std::invalid_argument("preview surface requires height-field terrain");
  if(!request.source.valid())
    throw std::invalid_argument("preview request source identity is invalid");
  if(request.source.camera_signature!=terrain_camera_signature(request.camera))
    throw std::invalid_argument("preview request camera identity is stale");
  if(request.source.field_signature!=preview_surface_field_signature(field))
    throw std::invalid_argument("preview request field identity is stale");
  if(configuration.level_count<1U||configuration.level_count>16U)
    throw std::invalid_argument("preview level count must be between 1 and 16");
  if(configuration.cells_per_side<4U||
     configuration.cells_per_side>1024U||
     configuration.cells_per_side%4U!=0U)
    throw std::invalid_argument(
        "preview cells per side must be a multiple of four from 4 to 1024");
  if(!(configuration.finest_spacing>0.0)||
     !std::isfinite(configuration.finest_spacing))
    throw std::invalid_argument("preview finest spacing must be finite and positive");

  auto front=std::shared_ptr<PreviewSurfaceFront>(new PreviewSurfaceFront);
  front->source_identity_=request.source;
  front->source_camera_=request.camera;
  const double half_spacing=configuration.finest_spacing*0.5;
  const auto chart=preview_chart_coordinates(field,request.camera.position);
  const auto snap=[](double coordinate,double spacing){
    const double value=std::floor(coordinate/spacing);
    if(value<static_cast<double>(std::numeric_limits<std::int64_t>::min())||
       value>static_cast<double>(std::numeric_limits<std::int64_t>::max()))
      throw std::overflow_error("preview clipmap origin exceeds integer lattice");
    return static_cast<std::int64_t>(value);
  };
  const SampleKey origin{
      snap(chart.x,configuration.finest_spacing)*2,
      snap(chart.z,configuration.finest_spacing)*2};

  std::map<SampleKey,std::uint32_t> vertex_directory;
  const auto vertex=[&](SampleKey key){
    if(const auto found=vertex_directory.find(key);
       found!=vertex_directory.end())return found->second;
    if(front->vertices_.size()>=std::numeric_limits<std::uint32_t>::max())
      throw std::length_error("preview vertex index exceeds 32-bit range");
    const auto index=static_cast<std::uint32_t>(front->vertices_.size());
    front->vertices_.push_back(sample_vertex(field,key,half_spacing));
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
      front->level_origins_.size()*sizeof(PreviewSurfaceLevelOrigin);
  front->diagnostics_.upload_bytes=
      front->vertices_.size()*sizeof(PreviewSurfaceVertex)+
      front->indices_.size()*sizeof(std::uint32_t);
  front->diagnostics_.geometry_hash=geometry_hash(
      front->vertices_,front->indices_);
  front->diagnostics_.build_milliseconds=
      std::chrono::duration<double,std::milli>(
          std::chrono::steady_clock::now()-started).count();
  return front;
}

}  // namespace tetra_viewer
