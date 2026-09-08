#include "tetra_viewer/atmosphere.hpp"
#include "tetra_viewer/atmosphere_shadow_front.hpp"
#include "tetra_viewer/first_person_controller.hpp"
#include "tetra_viewer/image_oracle.hpp"
#include "tetra_viewer/projection.hpp"
#include "tetra_viewer/shadow_cascades.hpp"
#include "tetra_viewer/terrain_display_front.hpp"
#include "tetra_viewer/preview_surface_worker.hpp"
#include "tetra_viewer/terrain_runtime.hpp"
#include "tetra_viewer/viewer_scene.hpp"
#include "tetra_viewer/world_script.hpp"
#include "tetra_core/gpu_hierarchy_snapshot.hpp"
#include "tetra_core/tet_mesh.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_metal.h"

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <Metal/MTLCounters.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/CAMetalLayer.h>
#include <mach/mach_time.h>
#include <simd/simd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <numbers>
#include <numeric>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr const char* metal_shader_source=R"METAL(
#include <metal_stdlib>
using namespace metal;

struct SceneVertexIn {
  float3 position [[attribute(0)]];
  float3 colour [[attribute(1)]];
  float3 normal [[attribute(2)]];
  float3 smooth_normal [[attribute(3)]];
  float3 barycentric [[attribute(4)]];
  float edge_flags [[attribute(5)]];
};

struct CameraUniforms {
  float4x4 view_projection;
  float4 sun_direction;
  float4 rendering;
};

struct SceneVertexOut {
  float4 position [[position]];
  float3 colour;
  float3 normal;
  float3 world_position;
  float3 barycentric [[center_no_perspective]];
  float edge_flags [[flat]];
};

struct ShadowUniforms {
  float4x4 matrices[4];
  float4 splits;
  float4 depth_spans;
  float4 camera_position;
};

struct ShadowVertexOut {
  float4 position [[position]];
};

vertex ShadowVertexOut shadow_vertex(
    SceneVertexIn input [[stage_in]],
    constant float4x4& matrix [[buffer(1)]]) {
  ShadowVertexOut output;
  output.position=matrix*float4(input.position,1.0);
  output.position.y=-output.position.y;
  return output;
}

vertex SceneVertexOut scene_vertex(
    SceneVertexIn input [[stage_in]],
    constant CameraUniforms& camera [[buffer(1)]]) {
  SceneVertexOut output;
  output.position=camera.view_projection*float4(input.position,1.0);
  // The shared projection deliberately uses the established positive-height
  // Vulkan framebuffer basis. Metal's viewport maps clip-space Y in the
  // opposite direction, so perform the backend conversion exactly once here.
  output.position.y=-output.position.y;
  output.colour=input.colour;
  output.normal=camera.rendering.y>0.5&&
      length_squared(input.smooth_normal)>1.0e-8?
      input.smooth_normal:input.normal;
  output.world_position=input.position;
  output.barycentric=input.barycentric;
  output.edge_flags=input.edge_flags;
  return output;
}

float cascade_visibility(float3 position,float n_dot_l,uint selected,
    constant ShadowUniforms& shadows,depth2d_array<float> shadow_maps) {
  const float3 projected=(shadows.matrices[selected]*
      float4(position,1.0)).xyz;
  if(any(abs(projected.xy)>1.0)||projected.z<0.0||projected.z>1.0)
    return 1.0;
  const float2 uv=projected.xy*0.5+0.5;
  const float world_bias=mix(0.00144,0.0096,1.0-n_dot_l);
  const float bias=world_bias/max(shadows.depth_spans[selected],1.0e-6);
  const uint2 extent=uint2(shadow_maps.get_width(),shadow_maps.get_height());
  const int2 centre=int2(clamp(uv*float2(extent),float2(0.0),
      float2(extent-1u)));
  float visibility=0.0;
  for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){
    const uint2 coordinate=uint2(clamp(centre+int2(x,y),int2(0),
        int2(extent)-1));
    const float blocker=shadow_maps.read(coordinate,selected);
    visibility+=projected.z-bias<=blocker?1.0:0.0;
  }
  return visibility/9.0;
}

fragment float4 scene_fragment(
    SceneVertexOut input [[stage_in]],
    constant CameraUniforms& camera [[buffer(1)]],
    constant ShadowUniforms& shadows [[buffer(2)]],
    depth2d_array<float> shadow_maps [[texture(0)]]) {
  const float3 normal=normalize(input.normal);
  const float3 sun=normalize(camera.sun_direction.xyz);
  const float diffuse=max(dot(normal,sun),0.0);
  const float distance_from_camera=distance(
      input.world_position,shadows.camera_position.xyz);
  uint cascade=3u;
  if(distance_from_camera<shadows.splits.x)cascade=0u;
  else if(distance_from_camera<shadows.splits.y)cascade=1u;
  else if(distance_from_camera<shadows.splits.z)cascade=2u;
  float shadow=camera.rendering.w>0.5?cascade_visibility(
      input.world_position,diffuse,cascade,shadows,shadow_maps):1.0;
  if(camera.rendering.w>0.5&&cascade<3u){
    const float previous=cascade==0u?0.0:shadows.splits[cascade-1u];
    const float blend=smoothstep(mix(previous,shadows.splits[cascade],0.85),
                                 shadows.splits[cascade],distance_from_camera);
    shadow=mix(shadow,min(shadow,cascade_visibility(
        input.world_position,diffuse,cascade+1u,shadows,shadow_maps)),blend);
  }
  const float sky=0.22+0.18*max(normal.y,0.0);
  const float3 stone=float3(0.43,0.45,0.47);
  const float colour_energy=max(input.colour.r,max(input.colour.g,input.colour.b));
  const float3 albedo=colour_energy>0.02?mix(stone,input.colour,0.20):stone;
  const float3 lit=albedo*(sky+1.35*diffuse*shadow)*camera.rendering.z;
  float3 display_colour=lit;
  if(camera.rendering.x>0.5){
    const uint flags=uint(round(input.edge_flags))&7u;
    const float3 transition=smoothstep(
        float3(0.0),fwidth(input.barycentric)*1.15,input.barycentric);
    float interior=1.0;
    if((flags&1u)!=0u)interior=min(interior,transition.x);
    if((flags&2u)!=0u)interior=min(interior,transition.y);
    if((flags&4u)!=0u)interior=min(interior,transition.z);
    display_colour=mix(float3(0.055,0.065,0.075),display_colour,interior);
  }
  return float4(display_colour,1.0);
}

fragment float4 overlay_fragment(SceneVertexOut input [[stage_in]]) {
  return float4(input.colour,1.0);
}

struct CompositeVertexOut {
  float4 position [[position]];
  float2 uv;
};

vertex CompositeVertexOut composite_vertex(uint vertex_id [[vertex_id]]) {
  const float2 coordinate=float2((vertex_id<<1)&2,vertex_id&2);
  CompositeVertexOut output;
  output.position=float4(coordinate*2.0-1.0,0.0,1.0);
  output.uv=float2(coordinate.x,1.0-coordinate.y);
  return output;
}

fragment float4 composite_fragment(
    CompositeVertexOut input [[stage_in]],
    texture2d<float> source [[texture(0)]],
    constant float4& settings [[buffer(0)]]) {
  constexpr sampler linear_sampler(filter::linear,address::clamp_to_edge);
  const float2 texel=1.0/settings.xy;
  const float3 centre=source.sample(linear_sampler,input.uv).rgb;
  const float amount=0.15*settings.z;
  const float3 neighbours=
      source.sample(linear_sampler,input.uv+float2(texel.x,0.0)).rgb+
      source.sample(linear_sampler,input.uv-float2(texel.x,0.0)).rgb+
      source.sample(linear_sampler,input.uv+float2(0.0,texel.y)).rgb+
      source.sample(linear_sampler,input.uv-float2(0.0,texel.y)).rgb;
  return float4(clamp(centre*(1.0+4.0*amount)-neighbours*amount,
                      0.0,1.0),1.0);
}

struct TemporalMotionUniforms {
  float4 current_camera_near;
  float4 current_forward_tangent;
  float4 current_right_aspect;
  float4 current_down_jitter_x;
  float4 current_jitter_y_extent;
  float4 previous_camera_tangent;
  float4 previous_forward_tangent;
  float4 previous_right_aspect;
  float4 previous_down;
  float4 origin_delta;
};

struct TemporalMotionOut {
  float2 motion [[color(0)]];
  float reactive [[color(1)]];
};

vertex CompositeVertexOut temporal_vertex(uint vertex_id [[vertex_id]]) {
  const float2 coordinate=float2((vertex_id<<1)&2,vertex_id&2);
  CompositeVertexOut output;
  output.position=float4(coordinate.x*2.0-1.0,
                         1.0-coordinate.y*2.0,0.0,1.0);
  output.uv=coordinate;
  return output;
}

fragment TemporalMotionOut temporal_motion_fragment(
    CompositeVertexOut input [[stage_in]],
    constant TemporalMotionUniforms& state [[buffer(0)]],
    depth2d<float> scene_depth [[texture(0)]]) {
  const uint2 extent=uint2(scene_depth.get_width(),scene_depth.get_height());
  const uint2 coordinate=uint2(clamp(input.position.xy,float2(0.0),
      float2(extent-1u)));
  const float depth=scene_depth.read(coordinate);
  const float2 jitter_ndc=float2(state.current_down_jitter_x.w,
                                 state.current_jitter_y_extent.x);
  const float2 current_ndc=input.uv*2.0-1.0-jitter_ndc;
  const float3 direction=normalize(state.current_forward_tangent.xyz+
      state.current_right_aspect.xyz*current_ndc.x*
          state.current_forward_tangent.w*state.current_right_aspect.w+
      state.current_down_jitter_x.xyz*current_ndc.y*
          state.current_forward_tangent.w);
  float2 previous_ndc=current_ndc;
  if(depth>1.0e-8){
    const float view_distance=state.current_camera_near.w/depth;
    const float ray_distance=view_distance/max(
        dot(direction,state.current_forward_tangent.xyz),1.0e-6);
    const float3 current_position=state.current_camera_near.xyz+
        direction*ray_distance;
    const float3 previous_offset=current_position+state.origin_delta.xyz-
        state.previous_camera_tangent.xyz;
    const float previous_distance=dot(
        previous_offset,state.previous_forward_tangent.xyz);
    if(previous_distance>1.0e-6){
      previous_ndc=float2(
          dot(previous_offset,state.previous_right_aspect.xyz)/
              (previous_distance*state.previous_forward_tangent.w*
               state.previous_right_aspect.w),
          dot(previous_offset,state.previous_down.xyz)/
              (previous_distance*state.previous_forward_tangent.w));
    }
  }else{
    const float previous_distance=dot(
        direction,state.previous_forward_tangent.xyz);
    if(previous_distance>1.0e-6){
      previous_ndc=float2(
          dot(direction,state.previous_right_aspect.xyz)/
              (previous_distance*state.previous_forward_tangent.w*
               state.previous_right_aspect.w),
          dot(direction,state.previous_down.xyz)/
              (previous_distance*state.previous_forward_tangent.w));
    }
  }
  const float2 extent_pixels=state.current_jitter_y_extent.yz;
  TemporalMotionOut output;
  output.motion=(previous_ndc-current_ndc)*0.5*extent_pixels;
  float discontinuity=0.0;
  // MetalFX reconstructs several output samples from every low-resolution
  // input pixel.  Reject history across a two-pixel footprint so bright sky
  // cannot be pulled over a terrain silhouette as a false rim.
  for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x){
    const uint2 neighbour=uint2(clamp(int2(coordinate)+int2(x,y),
        int2(0),int2(extent)-1));
    const float other=scene_depth.read(neighbour);
    if((depth>1.0e-8)!=(other>1.0e-8)||
       (depth>1.0e-8&&abs(depth-other)>max(depth,other)*0.08))
      discontinuity=1.0;
  }
  output.reactive=max(discontinuity,state.current_jitter_y_extent.w);
  return output;
}

fragment float4 temporal_present_fragment(
    CompositeVertexOut input [[stage_in]],
    texture2d<float> source [[texture(0)]]) {
  constexpr sampler linear_sampler(filter::linear,address::clamp_to_edge);
  return float4(source.sample(linear_sampler,input.uv).rgb,1.0);
}
)METAL";

constexpr const char* ray_visibility_shader_source=R"METAL(
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace metal::raytracing;

struct RayInput {
  float3 origin;
  float3 direction;
  float minimum_distance;
  float maximum_distance;
};

kernel void terrain_visibility_probe(
    constant RayInput* rays [[buffer(0)]],
    device uint* visibility [[buffer(1)]],
    primitive_acceleration_structure terrain [[buffer(2)]],
    uint index [[thread_position_in_grid]]) {
  ray query;
  query.origin=rays[index].origin;
  query.direction=normalize(rays[index].direction);
  query.min_distance=max(rays[index].minimum_distance,1.0e-5f);
  query.max_distance=max(query.min_distance,rays[index].maximum_distance);
  intersector<triangle_data> trace;
  const auto hit=trace.intersect(query,terrain);
  visibility[index]=hit.type==intersection_type::none?1u:0u;
}
)METAL";

// This pass deliberately mirrors atmosphere.comp's 32 radial quadrature
// midpoints.  It is not a screen-space mask: each texel is a physical
// sample-to-sun ray against the terrain AS, consumed only by the direct
// Rayleigh/Mie source term in the following atmosphere integration dispatch.
constexpr const char* atmosphere_ray_visibility_shader_source=R"METAL(
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace metal::raytracing;

struct AtmosphereRayVisibilityUniforms {
  float4 camera_world_metres_per_unit;
  float4 camera_from_planet_ground;
  float4 camera_right_tangent;
  float4 camera_down_tangent;
  float4 camera_forward_top;
  float4 sun_direction_maximum_world_distance;
  float4 mie_scale_padding;
};

float2 ray_sphere_roots(float3 origin,float3 direction,float radius) {
  const float b=dot(origin,direction);
  const float radial=length(origin);
  const float c=(radial-radius)*(radial+radius);
  const float discriminant=b*b-c;
  if(discriminant<0.0f)return float2(1.0f,-1.0f);
  const float root=sqrt(max(discriminant,0.0f));
  return float2(-b-root,-b+root);
}

bool ray_medium_segment(float3 origin,float3 direction,float ground,float top,
                        thread float& begin,thread float& end) {
  const float2 outer=ray_sphere_roots(origin,direction,top);
  if(outer.y<0.0f)return false;
  begin=max(0.0f,outer.x); end=outer.y;
  const float radial=length(origin);
  if(radial<ground){
    const float2 hit=ray_sphere_roots(origin,direction,ground);
    if(hit.y>=0.0f)begin=max(begin,hit.y);
  } else {
    const float altitude=radial-ground;
    const float radial_cosine=dot(origin/radial,direction);
    const float horizon2=max(0.0f,altitude*(radial+ground)/(radial*radial));
    if(radial_cosine<0.0f&&radial_cosine*radial_cosine>
       horizon2*(1.0f+2.0e-6f)) {
      const float root=radial*(-radial_cosine-
          sqrt(max(radial_cosine*radial_cosine-horizon2,0.0f)));
      if(root>begin+0.01f)end=min(end,root);
    }
  }
  return end>begin;
}

float3 ray_view_direction(float2 uv,constant AtmosphereRayVisibilityUniforms& u) {
  const float2 ndc=uv*2.0f-1.0f;
  return normalize(u.camera_forward_top.xyz+
      u.camera_right_tangent.xyz*ndc.x*u.camera_right_tangent.w+
      u.camera_down_tangent.xyz*ndc.y*u.camera_down_tangent.w);
}

void ray_reconstructed_interval(float3 origin,float3 direction,float begin,
    float end,float closest,float ground,float closest_radius,
    float closest_altitude,float altitude_power,uint index,
    thread float& distance_begin,thread float& distance_end) {
  const float segment_epsilon=max((end-begin)*1.0e-6f,1.0e-3f);
  const bool has_before=closest>begin+segment_epsilon;
  const bool has_after=end>closest+segment_epsilon;
  const bool split=has_before&&has_after;
  const bool before=split?index<16u:has_before;
  const uint interval_count=split?16u:32u;
  const uint local=split?(before?index:index-16u):index;
  const float u0=float(local)/float(interval_count);
  const float u1=float(local+1u)/float(interval_count);
  const float side_distance=before?begin:end;
  const float side_altitude=max(length(origin+direction*side_distance)-ground,
                                  closest_altitude);
  const float altitude_begin=mix(closest_altitude,side_altitude,
      pow(before?1.0f-u0:u0,altitude_power));
  const float altitude_end=mix(closest_altitude,side_altitude,
      pow(before?1.0f-u1:u1,altitude_power));
  const float offset_begin=sqrt(max((ground+altitude_begin)*(ground+altitude_begin)-
                                    closest_radius*closest_radius,0.0f));
  const float offset_end=sqrt(max((ground+altitude_end)*(ground+altitude_end)-
                                  closest_radius*closest_radius,0.0f));
  distance_begin=closest+(before?-offset_begin:offset_begin);
  distance_end=closest+(before?-offset_end:offset_end);
}

kernel void terrain_atmosphere_visibility(
    constant AtmosphereRayVisibilityUniforms& u [[buffer(0)]],
    texture2d<float,access::read> endpoint [[texture(0)]],
    texture3d<uint,access::write> visibility [[texture(1)]],
    primitive_acceleration_structure terrain [[buffer(1)]],
    uint3 index [[thread_position_in_grid]]) {
  const uint width=visibility.get_width(), height=visibility.get_height();
  const uint query_count=clamp(uint(u.mie_scale_padding.y),1u,32u);
  if(index.x>=width||index.y>=height||index.z>=query_count)return;
  const uint interval=query_count==32u?index.z:
      index.z*(32u/query_count)+(uint(u.mie_scale_padding.z)&
                                  (32u/query_count-1u));
  const float endpoint_distance=endpoint.read(uint2(index.xy)).x;
  const float2 uv=(float2(index.xy)+0.5f)/float2(width,height);
  const float3 direction=ray_view_direction(uv,u);
  const float3 atmosphere_origin=u.camera_from_planet_ground.xyz;
  float begin,end;
  uint result=255u;
  if(ray_medium_segment(atmosphere_origin,direction,
      u.camera_from_planet_ground.w,u.camera_forward_top.w,begin,end)) {
    end=min(end,endpoint_distance>0.0f?endpoint_distance:1.0e9f);
    if(end>begin){
      const float closest=clamp(-dot(atmosphere_origin,direction),begin,end);
      const float closest_radius=length(atmosphere_origin+direction*closest);
      const float closest_altitude=max(closest_radius-u.camera_from_planet_ground.w,0.0f);
      const float altitude_power=mix(2.0f,5.0f,exp(-closest_altitude/
          max(u.mie_scale_padding.x*4.0f,1.0f)));
      float first,last;
      ray_reconstructed_interval(atmosphere_origin,direction,begin,end,closest,
          u.camera_from_planet_ground.w,closest_radius,closest_altitude,
          altitude_power,interval,first,last);
      const float metres_per_unit=max(u.camera_world_metres_per_unit.w,1.0e-6f);
      uint hash=index.x*0x9e3779b9u^index.y*0x85ebca6bu^
          interval*0xc2b2ae35u^(uint(u.mie_scale_padding.z)*0x27d4eb2du);
      hash^=hash>>16u; hash*=0x7feb352du;
      hash^=hash>>15u; hash*=0x846ca68bu; hash^=hash>>16u;
      const float jitter=(float(hash&0xffffu)+0.5f)/65536.0f;
      uint visible_samples=0u;
      intersector<triangle_data> trace;
      // Four stratified sample-to-sun queries per integration interval follow
      // the qualified Hillaire reference path. A single interval midpoint
      // stamps a displaced copy of a mountain silhouette into the atmosphere.
      const float3 sun_centre=normalize(
          u.sun_direction_maximum_world_distance.xyz);
      const float3 seed=abs(sun_centre.y)<0.9f?float3(0.0f,1.0f,0.0f):
                                                   float3(1.0f,0.0f,0.0f);
      const float3 sun_right=normalize(cross(seed,sun_centre));
      const float3 sun_up=cross(sun_centre,sun_right);
      const float solar_radius=max(u.mie_scale_padding.w,0.0f);
      // High-frequency terrain visibility matters in the forward solar/Mie
      // cone. Use four finite-disc samples there and one centre sample
      // elsewhere. Direct Rayleigh/Mie scattering is never allowed to jump
      // back to fully sunlit at an angular optimization boundary.
      const uint stratified_count=dot(direction,sun_centre)>
          0.97814760073f?4u:1u; // cos(12 degrees)
      for(uint sample=0u;sample<stratified_count;++sample){
        const float fraction=stratified_count==4u?
            (float(sample)+jitter)*0.25f:0.5f;
        const float distance=mix(first,last,fraction);
        const float disc_angle=6.28318530718f*fract(
            jitter+float(sample)*0.61803398875f);
        const float disc_radius=stratified_count==4u?
            tan(solar_radius)*sqrt((float(sample)+0.5f)*0.25f):0.0f;
        const float3 sun_direction=normalize(sun_centre+disc_radius*(
            sun_right*cos(disc_angle)+sun_up*sin(disc_angle)));
        ray query; query.origin=u.camera_world_metres_per_unit.xyz+
            direction*(distance/metres_per_unit)+
            sun_direction*0.002f;
        query.direction=sun_direction;
        query.min_distance=0.001f;
        query.max_distance=u.sun_direction_maximum_world_distance.w;
        visible_samples+=trace.intersect(query,terrain).type==
            intersection_type::none?1u:0u;
      }
      // R8Uint exactly preserves the four possible averaged visibility steps.
      result=stratified_count==4u?visible_samples*85u:
          (visible_samples!=0u?255u:0u);
    }
  }
  visibility.write(uint4(result,0u,0u,0u),uint3(index.xy,interval));
}
)METAL";

struct CameraUniforms {
  std::array<float,16> view_projection{};
  std::array<float,4> sun_direction{};
  std::array<float,4> rendering{};
};

struct ShadowUniforms {
  std::array<std::array<float,16>,tetra_viewer::shadow_cascade_count> matrices{};
  std::array<float,4> splits{};
  std::array<float,4> depth_spans{};
  std::array<float,4> camera_position{};
};

struct ProductionCameraUniforms {
  std::array<float,16> view_projection{};
  std::array<float,4> light_direction{};
  std::array<float,4> rendering{};
  std::array<float,4> view_position{};
};

struct ProductionShadowUniforms {
  std::array<std::array<float,16>,5> matrices{};
  std::array<float,4> splits{};
  std::array<float,4> local_depth_spans{};
  std::array<float,4> atmosphere_metadata{};
  std::array<float,4> epipolar_metadata{};
};

struct TemporalMotionUniforms {
  std::array<float,4> current_camera_near{};
  std::array<float,4> current_forward_tangent{};
  std::array<float,4> current_right_aspect{};
  std::array<float,4> current_down_jitter_x{};
  std::array<float,4> current_jitter_y_extent{};
  std::array<float,4> previous_camera_tangent{};
  std::array<float,4> previous_forward_tangent{};
  std::array<float,4> previous_right_aspect{};
  std::array<float,4> previous_down{};
  std::array<float,4> origin_delta{};
};

struct MetalFxTemporalResources {
  id<MTLFXTemporalScaler> scaler=nil;
  id<MTLTexture> input_colour=nil;
  id<MTLTexture> motion=nil;
  id<MTLTexture> reactive=nil;
  id<MTLTexture> output_colour=nil;
  id<MTLTexture> exposure=nil;
  int input_width{};
  int input_height{};
  int output_width{};
  int output_height{};
  bool direct_output{};
  bool history_valid{};
  std::uint64_t encoded_frames{};
  std::uint64_t history_resets{};
  std::string failure;
};

void glfw_error_callback(int error,const char* description) {
  std::fprintf(stderr,"GLFW error %d: %s\n",error,description);
}

id<MTLLibrary> make_shader_library(id<MTLDevice> device) {
  NSError* error=nil;
  MTLCompileOptions* options=[MTLCompileOptions new];
  options.languageVersion=MTLLanguageVersion2_4;
  NSString* source=[NSString stringWithUTF8String:metal_shader_source];
  id<MTLLibrary> library=[device newLibraryWithSource:source
                                             options:options
                                               error:&error];
  if(library==nil)
    std::fprintf(stderr,"Metal shader compilation failed: %s\n",
                 error.localizedDescription.UTF8String);
  return library;
}

struct RayVisibilityInput {
  simd_float3 origin{};
  simd_float3 direction{};
  float minimum_distance{};
  float maximum_distance{};
};
static_assert(sizeof(RayVisibilityInput)==48U);

// Independent host-side reference for the exact same opaque, first-hit
// contract used by the Metal query.  It deliberately operates on the
// published SceneVertex triangles, rather than a simplified fixture.
bool cpu_terrain_visibility(std::span<const tetra_viewer::SceneVertex> vertices,
                            const RayVisibilityInput& ray) {
  const simd_float3 direction=simd_normalize(ray.direction);
  // Hardware traversal retains valid shallow intersections in the published
  // terrain; the former 1e-7 cutoff discarded seven such hits in the native
  // oracle. Keep only a near-degenerate rejection threshold.
  constexpr float determinant_epsilon=1.0e-12F;
  for(std::size_t index=0U;index+2U<vertices.size();index+=3U){
    const auto point=[](const tetra_viewer::SceneVertex& vertex){
      return simd_make_float3(vertex.position[0],vertex.position[1],
                              vertex.position[2]);
    };
    const simd_float3 a=point(vertices[index]);
    const simd_float3 b=point(vertices[index+1U]);
    const simd_float3 c=point(vertices[index+2U]);
    const simd_float3 edge_ab=b-a,edge_ac=c-a;
    const simd_float3 perpendicular=simd_cross(direction,edge_ac);
    const float determinant=simd_dot(edge_ab,perpendicular);
    if(std::abs(determinant)<determinant_epsilon)continue;
    const float inverse=1.0F/determinant;
    const simd_float3 relative=ray.origin-a;
    const float u=simd_dot(relative,perpendicular)*inverse;
    if(u<0.0F||u>1.0F)continue;
    const simd_float3 cross_relative=simd_cross(relative,edge_ab);
    const float v=simd_dot(direction,cross_relative)*inverse;
    if(v<0.0F||u+v>1.0F)continue;
    const float distance=simd_dot(edge_ac,cross_relative)*inverse;
    if(distance>=ray.minimum_distance&&distance<=ray.maximum_distance)
      return false;
  }
  return true;
}

id<MTLComputePipelineState> make_ray_visibility_pipeline(id<MTLDevice> device) {
  NSError* error=nil;
  MTLCompileOptions* options=[MTLCompileOptions new];
  options.languageVersion=MTLLanguageVersion2_4;
  id<MTLLibrary> library=[device newLibraryWithSource:
      [NSString stringWithUTF8String:ray_visibility_shader_source]
      options:options error:&error];
  if(library==nil){
    std::fprintf(stderr,"Metal ray-visibility shader compilation failed: %s\n",
                 error.localizedDescription.UTF8String);
    return nil;
  }
  id<MTLFunction> function=[library newFunctionWithName:@"terrain_visibility_probe"];
  id<MTLComputePipelineState> pipeline=function==nil?nil:
      [device newComputePipelineStateWithFunction:function error:&error];
  if(pipeline==nil)
    std::fprintf(stderr,"Metal ray-visibility pipeline failed: %s\n",
                 error.localizedDescription.UTF8String);
  return pipeline;
}

id<MTLComputePipelineState> make_atmosphere_ray_visibility_pipeline(
    id<MTLDevice> device) {
  NSError* error=nil;
  MTLCompileOptions* options=[MTLCompileOptions new];
  options.languageVersion=MTLLanguageVersion2_4;
  id<MTLLibrary> library=[device newLibraryWithSource:
      [NSString stringWithUTF8String:atmosphere_ray_visibility_shader_source]
      options:options error:&error];
  if(library==nil){
    std::fprintf(stderr,"Metal atmosphere ray-visibility shader compilation failed: %s\n",
                 error.localizedDescription.UTF8String);
    return nil;
  }
  id<MTLFunction> function=[library newFunctionWithName:
      @"terrain_atmosphere_visibility"];
  id<MTLComputePipelineState> pipeline=function==nil?nil:
      [device newComputePipelineStateWithFunction:function error:&error];
  if(pipeline==nil)
    std::fprintf(stderr,"Metal atmosphere ray-visibility pipeline failed: %s\n",
                 error.localizedDescription.UTF8String);
  return pipeline;
}

bool run_ray_visibility_smoke_test(id<MTLDevice> device) {
  if(!device.supportsRaytracing){
    std::fprintf(stderr,"Metal ray tracing is unsupported by this device.\n");
    return 1;
  }
  const auto vertices=std::array<simd_float3,3>{
      simd_make_float3(-1.0F,0.0F,-1.0F),
      simd_make_float3(1.0F,0.0F,-1.0F),
      simd_make_float3(0.0F,0.0F,1.0F)};
  id<MTLBuffer> vertex_buffer=[device newBufferWithBytes:vertices.data()
      length:sizeof(vertices) options:MTLResourceStorageModeShared];
  MTLAccelerationStructureTriangleGeometryDescriptor* geometry=
      [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
  geometry.vertexBuffer=vertex_buffer;
  geometry.vertexFormat=MTLAttributeFormatFloat3;
  geometry.vertexStride=sizeof(simd_float3);
  geometry.triangleCount=1U;
  MTLPrimitiveAccelerationStructureDescriptor* descriptor=
      [MTLPrimitiveAccelerationStructureDescriptor descriptor];
  descriptor.geometryDescriptors=@[geometry];
  const auto sizes=[device accelerationStructureSizesWithDescriptor:descriptor];
  id<MTLAccelerationStructure> structure=
      [device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
  id<MTLBuffer> scratch=[device newBufferWithLength:sizes.buildScratchBufferSize
      options:MTLResourceStorageModePrivate];
  id<MTLComputePipelineState> pipeline=make_ray_visibility_pipeline(device);
  id<MTLComputePipelineState> atmosphere_pipeline=
      make_atmosphere_ray_visibility_pipeline(device);
  std::vector<RayVisibilityInput> rays{
      RayVisibilityInput{simd_make_float3(0.0F,1.0F,0.0F),
                         simd_make_float3(0.0F,-1.0F,0.0F),0.001F,10.0F},
      RayVisibilityInput{simd_make_float3(2.0F,1.0F,0.0F),
                         simd_make_float3(0.0F,-1.0F,0.0F),0.001F,10.0F}};
  std::vector<std::uint32_t> expected{0U,1U};
  // Dense, deterministic front/back and grazing-edge coverage against the
  // CPU's analytic triangle half-space oracle.  The first two remain the
  // easy-to-read hit/miss probes printed below.
  for(int row=0;row<32;++row)for(int column=0;column<32;++column){
    const float x=(static_cast<float>(column)-15.5F)/8.0F;
    const float z=(static_cast<float>(row)-15.5F)/8.0F;
    rays.push_back({simd_make_float3(x,1.0F,z),
                    simd_make_float3(0.0F,-1.0F,0.0F),0.001F,10.0F});
    const bool blocked=2.0F*(z+1.0F)>=0.0F&&
        1.0F-2.0F*x-z>=0.0F&&1.0F+2.0F*x-z>=0.0F;
    expected.push_back(blocked?0U:1U);
    rays.push_back({simd_make_float3(x,-1.0F,z),
                    simd_make_float3(0.0F,1.0F,0.0F),0.001F,10.0F});
    expected.push_back(blocked?0U:1U);
  }
  id<MTLBuffer> input=[device newBufferWithBytes:rays.data()
      length:rays.size()*sizeof(RayVisibilityInput)
      options:MTLResourceStorageModeShared];
  id<MTLBuffer> output=[device newBufferWithLength:
      rays.size()*sizeof(std::uint32_t)
      options:MTLResourceStorageModeShared];
  MTLTextureDescriptor* endpoint_descriptor=[MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                  width:1U height:1U mipmapped:NO];
  endpoint_descriptor.storageMode=MTLStorageModeShared;
  endpoint_descriptor.usage=MTLTextureUsageShaderRead;
  id<MTLTexture> endpoint=[device newTextureWithDescriptor:endpoint_descriptor];
  const std::array<float,4> endpoint_value{500.0F,0.0F,1.0F,0.0F};
  if(endpoint!=nil)[endpoint replaceRegion:MTLRegionMake2D(0U,0U,1U,1U)
                             mipmapLevel:0U withBytes:endpoint_value.data()
                           bytesPerRow:sizeof(endpoint_value)];
  MTLTextureDescriptor* visibility_descriptor=[MTLTextureDescriptor new];
  visibility_descriptor.textureType=MTLTextureType3D;
  visibility_descriptor.pixelFormat=MTLPixelFormatR8Uint;
  visibility_descriptor.width=1U;
  visibility_descriptor.height=1U;
  visibility_descriptor.depth=32U;
  visibility_descriptor.mipmapLevelCount=1U;
  visibility_descriptor.storageMode=MTLStorageModePrivate;
  visibility_descriptor.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
  id<MTLTexture> atmosphere_visibility=
      [device newTextureWithDescriptor:visibility_descriptor];
  id<MTLBuffer> atmosphere_readback=[device newBufferWithLength:256U
      options:MTLResourceStorageModeShared];
  id<MTLCommandQueue> queue=[device newCommandQueue];
  if(vertex_buffer==nil||structure==nil||scratch==nil||pipeline==nil||
     atmosphere_pipeline==nil||
     input==nil||output==nil||endpoint==nil||atmosphere_visibility==nil||
     atmosphere_readback==nil||queue==nil)return 1;
  id<MTLCommandBuffer> command=[queue commandBuffer];
  id<MTLAccelerationStructureCommandEncoder> build=
      [command accelerationStructureCommandEncoder];
  [build buildAccelerationStructure:structure descriptor:descriptor
                      scratchBuffer:scratch scratchBufferOffset:0U];
  [build endEncoding];
  id<MTLComputeCommandEncoder> compute=[command computeCommandEncoder];
  [compute setComputePipelineState:pipeline];
  [compute setBuffer:input offset:0U atIndex:0U];
  [compute setBuffer:output offset:0U atIndex:1U];
  [compute setAccelerationStructure:structure atBufferIndex:2U];
  [compute dispatchThreads:MTLSizeMake(rays.size(),1U,1U)
      threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
  [compute endEncoding];
  const std::array<float,28> atmosphere_uniform{
      0.0F,100.0F,0.0F,10.0F, 0.0F,100.0F,0.0F,10.0F,
      1.0F,0.0F,0.0F,1.0F, 0.0F,0.0F,1.0F,1.0F,
      0.0F,-1.0F,0.0F,1000.0F, 0.0F,-1.0F,0.0F,1000.0F,
      800.0F,0.0F,0.0F,0.0F};
  id<MTLComputeCommandEncoder> atmosphere_compute=[command computeCommandEncoder];
  [atmosphere_compute setComputePipelineState:atmosphere_pipeline];
  [atmosphere_compute setBytes:atmosphere_uniform.data()
                         length:atmosphere_uniform.size()*sizeof(float)
                       atIndex:0U];
  [atmosphere_compute setTexture:endpoint atIndex:0U];
  [atmosphere_compute setTexture:atmosphere_visibility atIndex:1U];
  [atmosphere_compute setAccelerationStructure:structure atBufferIndex:1U];
  [atmosphere_compute dispatchThreads:MTLSizeMake(1U,1U,32U)
             threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
  [atmosphere_compute endEncoding];
  id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
  [blit copyFromTexture:atmosphere_visibility sourceSlice:0U sourceLevel:0U
            sourceOrigin:MTLOriginMake(0U,0U,0U)
              sourceSize:MTLSizeMake(1U,1U,1U)
                toBuffer:atmosphere_readback destinationOffset:0U
   destinationBytesPerRow:256U destinationBytesPerImage:256U];
  [blit endEncoding];
  [command commit];
  [command waitUntilCompleted];
  const auto* values=static_cast<const std::uint32_t*>(output.contents);
  const auto* atmosphere_values=static_cast<const std::uint8_t*>(
      atmosphere_readback.contents);
  const std::size_t mismatches=values==nullptr?rays.size():
      static_cast<std::size_t>(std::count_if(expected.begin(),expected.end(),
          [values,index=std::size_t{}](std::uint32_t value)mutable{
            return values[index++]!=value;
          }));
  const bool passed=command.status==MTLCommandBufferStatusCompleted&&
      values!=nullptr&&values[0]==0U&&values[1]==1U&&
      atmosphere_values!=nullptr&&atmosphere_values[0]==0U&&mismatches==0U;
  std::printf("{\"event\":\"metal_ray_visibility_smoke\","
              "\"triangle_count\":1,\"oracle_queries\":%zu,"
              "\"oracle_mismatches\":%zu,\"blocked_visibility\":%u,"
              "\"clear_visibility\":%u,"
              "\"atmosphere_sample_visibility\":%u,\"passed\":%s}\n",
              rays.size(),mismatches,values==nullptr?99U:values[0],
              values==nullptr?99U:values[1],
              atmosphere_values==nullptr?99U:atmosphere_values[0],
              passed?"true":"false");
  return passed?0:1;
}

// Surface triangles are immutable for a published terrain generation.  Build
// a primitive AS from that exact rendering buffer and promote it only after
// Metal has finished the build; the prior complete generation stays active.
struct MetalTerrainAccelerationStructure {
  id<MTLAccelerationStructure> active=nil;
  id<MTLBuffer> active_vertices=nil;
  id<MTLAccelerationStructure> pending=nil;
  id<MTLBuffer> pending_vertices=nil;
  id<MTLBuffer> active_exact_indices=nil;
  id<MTLBuffer> active_preview_vertices=nil;
  id<MTLBuffer> active_preview_indices=nil;
  id<MTLBuffer> pending_exact_indices=nil;
  id<MTLBuffer> pending_preview_vertices=nil;
  id<MTLBuffer> pending_preview_indices=nil;
  id<MTLBuffer> pending_scratch=nil;
  std::uint64_t active_generation{};
  std::uint64_t pending_generation{};
  std::uint64_t build_count{};
  std::size_t resident_bytes{};
  float maximum_vertex_radius_world{};
  std::shared_ptr<std::atomic<double>> last_build_milliseconds=
      std::make_shared<std::atomic<double>>(0.0);
  std::shared_ptr<std::atomic<bool>> last_build_timing_valid=
      std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<std::uint64_t>> completed_generation=
      std::make_shared<std::atomic<std::uint64_t>>(0U);
};

// One immutable set of buffers and identities is consumed by every terrain
// pass in a frame. Replacing this value is the Metal-side publication point.
struct MetalTerrainDisplayFront {
  tetra_viewer::TerrainDisplayIdentity identity;
  std::shared_ptr<const tetra_viewer::PreviewSurfaceFront> preview_cpu;
  id<MTLBuffer> exact_vertices=nil;
  id<MTLBuffer> exact_indices=nil;
  id<MTLBuffer> preview_vertices=nil;
  id<MTLBuffer> preview_indices=nil;
  id<MTLBuffer> exact_indirect_arguments=nil;
  bool indexed_exact_selection{};
  std::size_t exact_vertex_count{};
  std::size_t exact_index_count{};
  std::size_t preview_vertex_count{};
  std::size_t preview_index_count{};
  std::size_t upload_bytes{};
  std::uint64_t render_generation{};

  [[nodiscard]] bool ready() const noexcept {
    const bool exact_ready=exact_vertices!=nil&&
        (exact_vertex_count!=0U||exact_indirect_arguments!=nil);
    const bool preview_ready=preview_cpu!=nullptr&&preview_vertices!=nil&&
        preview_indices!=nil&&preview_vertex_count!=0U&&
        preview_index_count!=0U;
    return identity.valid()&&(exact_ready||preview_ready)&&render_generation!=0U;
  }
  [[nodiscard]] std::size_t triangle_count() const noexcept {
    return (indexed_exact_selection?exact_index_count:exact_vertex_count)/3U+
        preview_index_count/3U;
  }
};

// A commutative two-lane fingerprint lets a diagnostic completion validate the
// complete CPU-captured geometry without depending on the extractor's workgroup
// order.  It intentionally covers positions, normals, triangle membership and
// the linear index stream, rather than treating a vertex count as parity.
struct MetalGpuTerrainFingerprint {
  std::uint64_t sum{};
  std::uint64_t xor_sum{};

  [[nodiscard]] bool operator==(const MetalGpuTerrainFingerprint&) const=
      default;
};

[[nodiscard]] std::uint64_t metal_terrain_mix(std::uint64_t value) noexcept {
  value^=value>>30U;value*=0xbf58476d1ce4e5b9ULL;
  value^=value>>27U;value*=0x94d049bb133111ebULL;
  return value^(value>>31U);
}

[[nodiscard]] MetalGpuTerrainFingerprint metal_terrain_fingerprint(
    std::span<const tetra_viewer::SceneVertex> vertices) {
  MetalGpuTerrainFingerprint result;
  const auto include=[&](std::uint64_t value){
    const auto mixed=metal_terrain_mix(value);
    result.sum+=mixed;
    result.xor_sum^=std::rotl(mixed,static_cast<int>(mixed&63U));
  };
  const auto vertex_key=[](const tetra_viewer::SceneVertex& vertex){
    std::array<std::uint32_t,6> words{};
    for(std::size_t lane=0U;lane<3U;++lane){
      words[lane]=std::bit_cast<std::uint32_t>(vertex.position[lane]);
      words[lane+3U]=std::bit_cast<std::uint32_t>(vertex.normal[lane]);
    }
    std::uint64_t key=0x6a09e667f3bcc909ULL;
    for(const auto word:words)key=metal_terrain_mix(key^word);
    return key;
  };
  for(const auto& vertex:vertices)include(vertex_key(vertex));
  for(std::size_t first=0U;first+2U<vertices.size();first+=3U){
    std::array<std::uint64_t,3> triangle{
        vertex_key(vertices[first]),vertex_key(vertices[first+1U]),
        vertex_key(vertices[first+2U])};
    std::ranges::sort(triangle);
    include(metal_terrain_mix(triangle[0])^
            std::rotl(metal_terrain_mix(triangle[1]),21)^
            std::rotl(metal_terrain_mix(triangle[2]),42));
  }
  return result;
}

struct MetalGpuTerrainDiagnosticCounters {
  std::atomic<std::uint64_t> dispatched{};
  std::atomic<std::uint64_t> completed{};
  std::atomic<std::uint64_t> accepted{};
  std::atomic<std::uint64_t> stale_rejected{};
  std::atomic<std::uint64_t> failed{};
  std::atomic<std::uint64_t> overflow{};
  std::atomic<std::uint64_t> cpu_front_frames{};
  std::atomic<std::uint64_t> cpu_front_violations{};
  std::atomic<std::uint64_t> device_closure_submitted{};
  std::atomic<std::uint64_t> device_owner_submitted{};
  std::atomic<std::uint64_t> p6_requests{};
  std::atomic<std::uint64_t> cpu_surface_build_requests{};
  std::atomic<std::uint64_t> immutable_snapshot_builds{};
  std::atomic<std::uint64_t> candidate_payload_readback_requests{};
  std::atomic<std::uint64_t> device_front_rejections{};
  std::atomic<std::uint64_t> device_front_private_commits{};
  std::atomic<std::uint64_t> device_front_injections{};
  std::atomic<std::uint64_t> post_bootstrap_seed_attempts{};
  // These are pointer-identity observations taken at actual draw encoding.
  // They prove the device-front P8 publication reached a raster consumer
  // without mapping candidate geometry back to the CPU.
  std::atomic<std::uint64_t> device_front_display_promotions{};
  std::atomic<std::uint64_t> device_front_display_frames{};
  std::atomic<std::uint64_t> device_front_display_binding_violations{};
  std::atomic<std::uint64_t> device_front_bootstrap_fallback_frames{};
};

// P5c2 diagnostic slots are deliberately separate from the immutable CPU
// display front. A result may be inspected only when its command completion,
// immutable payload fingerprint, scene generation, and render origin all still
// match; it has no draw handle.
struct MetalGpuTerrainDiagnosticSlot {
  id<MTLBuffer> cells=nil;
  id<MTLBuffer> output=nil;
  id<MTLBuffer> indices=nil;
  std::uint64_t scene_generation{};
  tetra::Vec3 render_origin{};
  std::uint32_t expected_vertices{};
  MetalGpuTerrainFingerprint expected_fingerprint{};
  bool pending{};
  std::shared_ptr<std::atomic<bool>> completed=
      std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<bool>> succeeded=
      std::make_shared<std::atomic<bool>>(false);

  [[nodiscard]] bool matches(std::uint64_t generation,
                             tetra::Vec3 origin) const noexcept {
    return scene_generation==generation&&render_origin.x==origin.x&&
        render_origin.y==origin.y&&render_origin.z==origin.z;
  }
};

// P7c2b1b retains the P6 packet rather than the superseded pre-expanded cell
// ABI.  These buffers are diagnostic-only: `vertices` is private and has no
// renderer handle until the later atomic consumer-promotion leaf.
struct MetalGpuTerrainNativeDiagnosticSlot {
  id<MTLBuffer> field=nil;
  id<MTLBuffer> owners=nil;
  // P8's CPU-source route supplies a trivial all-active header; P7e4a binds
  // P7e3c's device-written closure header at this same ABI boundary.
  id<MTLBuffer> owner_header=nil;
  id<MTLBuffer> templates=nil;
  id<MTLBuffer> roots=nil;
  id<MTLBuffer> counts=nil;
  // Packed owner-cell signs are retained in private scratch for the
  // owner-direct route; they never cross to the CPU.
  id<MTLBuffer> signs=nil;
  id<MTLBuffer> offsets=nil;
  id<MTLBuffer> added_offsets=nil;
  id<MTLBuffer> block_totals=nil;
  id<MTLBuffer> block_offsets=nil;
  id<MTLBuffer> block_totals2=nil;
  id<MTLBuffer> block_offsets2=nil;
  id<MTLBuffer> compaction_status=nil;
  id<MTLBuffer> triangles=nil;
  id<MTLBuffer> projected=nil;
  id<MTLBuffer> vertices=nil;
  id<MTLBuffer> commit_control=nil;
  id<MTLBuffer> readback=nil;
  // P7e4a audits only this two-word private commit control, never an owner or
  // terrain-vertex payload. It proves both successful private publication and
  // injected rejection without making a candidate CPU-visible.
  id<MTLBuffer> control_audit=nil;
  tetra::GpuTerrainFieldTuple tuple{};
  std::uint64_t scene_generation{};
  std::uint64_t source_revision{};
  std::uint64_t field_revision{};
  std::uint64_t candidate_identity{};
  tetra::Vec3 render_origin{};
  std::uint32_t vertex_capacity{};
  bool expected_rejection{};
  bool pending{};
  std::shared_ptr<std::atomic<bool>> completed=
      std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<bool>> succeeded=
      std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<std::uint32_t>> completed_vertex_count=
      std::make_shared<std::atomic<std::uint32_t>>(0U);

  [[nodiscard]] bool matches(std::uint64_t generation,
                             std::uint64_t source,
                             std::uint64_t field_revision_value,
                             tetra::Vec3 origin) const noexcept {
    return scene_generation==generation&&source_revision==source&&
        field_revision==field_revision_value&&
        render_origin.x==origin.x&&render_origin.y==origin.y&&
        render_origin.z==origin.z;
  }
};

// Immutable P6 inputs are shared by all three flights for one published
// candidate.  Only the compact field tuple changes per submission.
struct MetalGpuTerrainPacketUpload {
  id<MTLBuffer> owners=nil;
  id<MTLBuffer> templates=nil;
  std::uint64_t source_revision{};
  std::uint64_t candidate_identity{};
  std::size_t owner_count{};
};

// The P8 active surface is always private.  CPU geometry may seed it after a
// new exact publication, but candidate validation and replacement never map
// it: invalid work simply leaves these prior complete contents untouched.
struct MetalGpuTerrainActiveFront {
  id<MTLBuffer> vertices=nil;
  id<MTLBuffer> indirect_arguments=nil;
  id<MTLBuffer> seed_vertices=nil;
  id<MTLBuffer> seed_arguments=nil;
  tetra_viewer::TerrainDisplayIdentity identity;
  std::uint32_t vertex_capacity{};
  bool seed_pending{};
  bool promoted{};
  // The compact P8 workspace is reused across flights.  This stamp lets the
  // renderer publish a new completed private front once per successful P8
  // audit, rather than repeatedly mutating one display generation.
  std::uint64_t displayed_p8_commits{};
  std::shared_ptr<std::atomic<bool>> completed=
      std::make_shared<std::atomic<bool>>(false);
};

// P7e2 keeps immutable hierarchy data and the resulting selection marks in
// device storage across camera motion.  Its mark buffer deliberately has no
// CPU readback or terrain draw consumer yet: P7e3 owns conforming closure and
// P7e4 owns render-front promotion.
struct MetalGpuHierarchyLiveSelectionSlot {
  id<MTLBuffer> tuple=nil;
  id<MTLBuffer> marks=nil;
  std::uint64_t tuple_identity{};
  bool pending{};
  std::shared_ptr<std::atomic<bool>> completed=
      std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<bool>> succeeded=
      std::make_shared<std::atomic<bool>>(false);
};

struct MetalGpuHierarchyLiveSelection {
  id<MTLBuffer> hierarchy=nil;
  id<MTLBuffer> children=nil;
  id<MTLBuffer> roots=nil;
  id<MTLBuffer> canonical=nil;
  id<MTLBuffer> parents=nil;
  id<MTLBuffer> face_incidence=nil;
  id<MTLBuffer> edge_topology=nil;
  id<MTLBuffer> edge_ranges=nil;
  id<MTLBuffer> edge_incidence=nil;
  id<MTLBuffer> ancestor_edge_ranges=nil;
  id<MTLBuffer> orientations=nil;
  id<MTLBuffer> vertex_topology=nil;
  id<MTLBuffer> vertex_ranges=nil;
  id<MTLBuffer> vertex_incidence=nil;
  id<MTLBuffer> inputs=nil;
  // One bounded closure workspace is deliberately serialized.  P7e4a's
  // compact owners and scalar header never leave device memory; a subsequent
  // flight waits for this command buffer to complete before reusing it.
  id<MTLBuffer> closure_owners=nil;
  id<MTLBuffer> closure_counts=nil;
  id<MTLBuffer> closure_offsets=nil;
  id<MTLBuffer> closure_added_offsets=nil;
  id<MTLBuffer> closure_block_totals=nil;
  id<MTLBuffer> closure_block_offsets=nil;
  id<MTLBuffer> closure_scan_total=nil;
  id<MTLBuffer> closure_edge_marks=nil;
  id<MTLBuffer> closure_red_promotions=nil;
  id<MTLBuffer> closure_status=nil;
  // Two aligned MTLDispatchThreadgroupsIndirectArguments records: repair and
  // green.  They stay private; only a
  // scalar audit copy is exposed to the P7e4a smoke.
  id<MTLBuffer> closure_dispatch_args=nil;
  id<MTLBuffer> closure_control_audit=nil;
  // P7e4a1 compact-list ABI: each private ping/pong allocation starts with
  // four uints (count, capacity, failure bits, reserved), followed by compact
  // record IDs. The closure queues share this header shape. The three padded
  // indirect records are selected-copy, closure, and P8 respectively.
  id<MTLBuffer> compact_selected_ping=nil;
  id<MTLBuffer> compact_selected_pong=nil;
  id<MTLBuffer> compact_closure_queue_ping=nil;
  id<MTLBuffer> compact_closure_queue_pong=nil;
  id<MTLBuffer> compact_dispatch_args=nil;
  id<MTLBuffer> compact_canonical_ranks=nil;
  id<MTLBuffer> compact_histogram=nil;
  id<MTLBuffer> compact_histogram_offsets=nil;
  id<MTLBuffer> compact_bin_bases=nil;
  // P7e4a1 compact closure sidecars.  These are capacity-sized allocations,
  // but every data-parallel use is driven by compact_dispatch_args, which is
  // armed from the device-written compact-list header.
  id<MTLBuffer> compact_edge_marks=nil;
  id<MTLBuffer> compact_green_masks=nil;
  id<MTLBuffer> compact_green_control=nil;
  id<MTLBuffer> compact_red_status=nil;
  id<MTLBuffer> compact_expand_counts=nil;
  id<MTLBuffer> compact_expand_offsets=nil;
  id<MTLBuffer> compact_block_totals=nil;
  id<MTLBuffer> compact_block_offsets=nil;
  id<MTLBuffer> compact_level_totals=nil;
  id<MTLBuffer> compact_level_offsets=nil;
  id<MTLBuffer> compact_scan_total=nil;
  // Device-latched final compact result. It removes ping/pong identity from
  // the future P8 boundary without asking the CPU which repair round won.
  id<MTLBuffer> compact_final_active=nil;
  id<MTLBuffer> compact_final_masks=nil;
  // P7e4a1's staged 12-word owner stream.  It has no terrain/P8 consumer in
  // this leaf; validity is published only in compact_owner_header after the
  // indirect materializer has completed without a device failure.
  id<MTLBuffer> compact_owner_stream=nil;
  id<MTLBuffer> compact_owner_header=nil;
  id<MTLBuffer> compact_owner_dispatch_args=nil;
  // Dedicated compact P8 workspace.  Unlike the legacy P8c slot, all work
  // bounds come from compact_owner_header and private indirect records.
  id<MTLBuffer> compact_p8_field=nil;
  id<MTLBuffer> compact_p8_templates=nil;
  id<MTLBuffer> compact_p8_counts=nil;
  id<MTLBuffer> compact_p8_offsets=nil;
  id<MTLBuffer> compact_p8_block_totals=nil;
  id<MTLBuffer> compact_p8_block_offsets=nil;
  id<MTLBuffer> compact_p8_level_totals=nil;
  id<MTLBuffer> compact_p8_level_offsets=nil;
  id<MTLBuffer> compact_p8_signs=nil;
  id<MTLBuffer> compact_p8_candidate=nil;
  id<MTLBuffer> compact_p8_status=nil;
  id<MTLBuffer> compact_p8_dispatches=nil;
  id<MTLBuffer> compact_p8_microbatch_copy_dispatch=nil;
  // P7e4k's device-produced three-word indirect grid. The hybrid prefix
  // admission pass arms it; CPU neither supplies nor observes its count.
  id<MTLBuffer> compact_p8_triangle_dispatch=nil;
  id<MTLBuffer> compact_p8_audit=nil;
  std::uint32_t compact_p8_vertex_capacity{};
  // Completion-only scalar audit.  It contains stage status, never a
  // candidate owner or terrain payload, and is inspected only after the
  // command buffer completes by the live smoke.
  id<MTLBuffer> compact_closure_audit=nil;
  std::array<std::uint32_t,13> compact_last_closure_audit{};
  std::array<std::uint32_t,4> compact_last_selected_header{};
  std::array<std::uint32_t,4> compact_last_final_active_header{};
  std::array<std::uint32_t,4> compact_last_final_masks_header{};
  bool closure_pending{};
  std::shared_ptr<std::atomic<bool>> closure_completed=
      std::make_shared<std::atomic<bool>>(false);
  std::uint32_t closure_slot_index{std::numeric_limits<std::uint32_t>::max()};
  std::array<MetalGpuHierarchyLiveSelectionSlot,3> slots;
  std::uint64_t source_revision{};
  std::uint64_t field_revision{};
  std::uint64_t bootstrap_scene_generation{};
  std::uint32_t record_count{};
  std::uint32_t root_count{};
  std::uint32_t output_capacity{};
  std::uint32_t mark_word_count{};
  // The ordinary device display front must reproduce the complete published
  // CPU cut.  Generic GPU-LOD diagnostics retain view-local selection.
  bool require_complete_front{};
  std::uint64_t submitted{};
  std::uint64_t completed{};
  std::uint64_t accepted{};
  std::uint64_t stale_rejected{};
  std::uint64_t failed{};
  std::uint64_t cpu_generation_violations{};
  std::uint64_t indirect_zero_grid_observations{};
  std::uint64_t compact_closure_encoded{};
  std::uint64_t compact_closure_completed{};
  std::uint64_t compact_red_encoded{};
  std::uint64_t compact_closure_rejected{};
  std::uint64_t compact_quiescent{};
  std::uint64_t compact_p8_encoded{};
  std::uint64_t compact_p8_completed{};
  std::uint64_t compact_p8_private_commits{};
  std::uint64_t compact_p8_rejected{};
  std::array<std::uint32_t,6> compact_p8_last_audit{};
  std::array<std::uint32_t,4> compact_p8_last_owner_header{};
  // Device-front progress is host-side timing of submitted boundaries and
  // completed scalar audits.  It deliberately contains no owner or terrain
  // payload and makes an automated-launch timeout diagnostically useful.
  std::uint64_t compact_owner_materialization_encoded{};
  std::chrono::steady_clock::time_point device_front_bootstrap_at{};
  std::chrono::steady_clock::time_point device_front_selector_at{};
  std::chrono::steady_clock::time_point device_front_closure_at{};
  std::chrono::steady_clock::time_point device_front_materializer_at{};
  std::chrono::steady_clock::time_point device_front_p8_at{};
  std::chrono::steady_clock::time_point device_front_p8_completed_at{};
  double device_front_last_p8_completion_milliseconds{-1.0};
  std::uint64_t cursor{};

  [[nodiscard]] bool ready() const noexcept {
    return hierarchy!=nil&&children!=nil&&roots!=nil&&parents!=nil&&face_incidence!=nil&&edge_topology!=nil&&edge_ranges!=nil&&edge_incidence!=nil&&ancestor_edge_ranges!=nil&&
        inputs!=nil&&record_count!=0U&&root_count!=0U&&root_count<=12U&&
        mark_word_count!=0U;
  }
  [[nodiscard]] bool closure_ready() const noexcept {
    return ready()&&canonical!=nil&&orientations!=nil&&vertex_topology!=nil&&
        vertex_ranges!=nil&&vertex_incidence!=nil&&closure_owners!=nil&&
        closure_counts!=nil&&closure_offsets!=nil&&closure_added_offsets!=nil&&
        closure_block_totals!=nil&&closure_block_offsets!=nil&&
        closure_scan_total!=nil&&closure_edge_marks!=nil&&
        closure_red_promotions!=nil&&closure_status!=nil&&
        closure_dispatch_args!=nil&&closure_control_audit!=nil;
  }
  [[nodiscard]] bool compact_worklist_ready() const noexcept {
    return ready()&&compact_selected_ping!=nil&&compact_selected_pong!=nil&&
        compact_closure_queue_ping!=nil&&compact_closure_queue_pong!=nil&&
        compact_dispatch_args!=nil&&compact_canonical_ranks!=nil&&
        compact_histogram!=nil&&compact_histogram_offsets!=nil&&
        compact_bin_bases!=nil;
  }
  [[nodiscard]] bool compact_closure_ready() const noexcept {
    return compact_worklist_ready()&&orientations!=nil&&vertex_topology!=nil&&
        vertex_ranges!=nil&&vertex_incidence!=nil&&compact_edge_marks!=nil&&
        compact_green_masks!=nil&&compact_green_control!=nil&&
        compact_red_status!=nil&&compact_expand_counts!=nil&&
        compact_expand_offsets!=nil&&compact_block_totals!=nil&&
        compact_block_offsets!=nil&&compact_level_totals!=nil&&
        compact_level_offsets!=nil&&compact_scan_total!=nil&&
        compact_final_active!=nil&&compact_final_masks!=nil&&
        compact_closure_audit!=nil;
  }
  [[nodiscard]] bool compact_owner_ready() const noexcept {
    return compact_closure_ready()&&compact_owner_stream!=nil&&
        compact_owner_header!=nil&&compact_owner_dispatch_args!=nil;
  }
  [[nodiscard]] bool compact_p8_ready() const noexcept {
    return compact_owner_ready()&&compact_p8_field!=nil&&
        compact_p8_templates!=nil&&compact_p8_counts!=nil&&
        compact_p8_offsets!=nil&&compact_p8_block_totals!=nil&&
        compact_p8_block_offsets!=nil&&compact_p8_level_totals!=nil&&
        compact_p8_level_offsets!=nil&&compact_p8_signs!=nil&&
        compact_p8_candidate!=nil&&compact_p8_status!=nil&&
        compact_p8_dispatches!=nil&&compact_p8_microbatch_copy_dispatch!=nil&&
        compact_p8_triangle_dispatch!=nil&&compact_p8_audit!=nil&&
        compact_p8_vertex_capacity!=0U;
  }
};

bool ensure_metal_gpu_hierarchy_compact_p8_workspace(
    id<MTLDevice> device,MetalGpuHierarchyLiveSelection& selection,
    std::uint32_t vertex_capacity) {
  if(device==nil||!selection.compact_owner_ready()||vertex_capacity==0U)return false;
  if(selection.compact_p8_ready()&&
     selection.compact_p8_vertex_capacity==vertex_capacity)return true;
  const auto owner_capacity=selection.output_capacity;
  const auto blocks=(owner_capacity+255U)/256U;
  const auto levels=std::max(1U,(blocks+255U)/256U);
  const auto words=[](std::size_t count)->std::optional<NSUInteger>{
    if(count>std::numeric_limits<NSUInteger>::max()/sizeof(std::uint32_t))
      return std::nullopt;
    return static_cast<NSUInteger>(count*sizeof(std::uint32_t));
  };
  const auto count_words=words(owner_capacity),block_words=words(blocks),
      level_words=words(levels),sign_words=words(std::size_t(owner_capacity)*3U),
      candidate_words=words(4U+std::size_t(vertex_capacity)*18U);
  if(!count_words||!block_words||!level_words||!sign_words||!candidate_words)return false;
  selection.compact_p8_counts=[device newBufferWithLength:*count_words options:MTLResourceStorageModePrivate];
  selection.compact_p8_offsets=[device newBufferWithLength:*count_words options:MTLResourceStorageModePrivate];
  selection.compact_p8_block_totals=[device newBufferWithLength:*block_words options:MTLResourceStorageModePrivate];
  selection.compact_p8_block_offsets=[device newBufferWithLength:*block_words options:MTLResourceStorageModePrivate];
  selection.compact_p8_level_totals=[device newBufferWithLength:*level_words options:MTLResourceStorageModePrivate];
  selection.compact_p8_level_offsets=[device newBufferWithLength:*level_words options:MTLResourceStorageModePrivate];
  selection.compact_p8_signs=[device newBufferWithLength:*sign_words options:MTLResourceStorageModePrivate];
  selection.compact_p8_candidate=[device newBufferWithLength:*candidate_words options:MTLResourceStorageModePrivate];
  selection.compact_p8_status=[device newBufferWithLength:2U*sizeof(std::uint32_t) options:MTLResourceStorageModePrivate];
  selection.compact_p8_dispatches=[device newBufferWithLength:9U*4U*sizeof(std::uint32_t) options:MTLResourceStorageModePrivate];
  selection.compact_p8_microbatch_copy_dispatch=[device newBufferWithLength:
      4U*sizeof(std::uint32_t) options:MTLResourceStorageModePrivate];
  selection.compact_p8_triangle_dispatch=[device newBufferWithLength:
      3U*sizeof(std::uint32_t) options:MTLResourceStorageModePrivate];
  // Six P8 result words plus the four-word staged owner header are the
  // bounded failure audit for a live flight.  Neither is terrain payload.
  selection.compact_p8_audit=[device newBufferWithLength:10U*sizeof(std::uint32_t) options:MTLResourceStorageModeShared];
  selection.compact_p8_vertex_capacity=vertex_capacity;
  return selection.compact_p8_ready();
}

bool configure_metal_gpu_hierarchy_live_selection(
    id<MTLDevice> device,MetalGpuHierarchyLiveSelection& selection,
    const tetra::GpuHierarchySnapshot& snapshot,std::uint64_t field_revision,
    std::uint64_t bootstrap_scene_generation) {
  tetra::validate_gpu_hierarchy_snapshot(snapshot);
  if(snapshot.header.source_world_revision==0U||field_revision==0U||
     // Production directories legitimately contain more than one million
     // immutable ancestry records.  The old fixed diagnostic cap rejected
     // such a front before a single GPU stage ran, guaranteeing that the
     // device renderer could never reproduce the production CPU front.
     // Every downstream dispatch and byte computation is checked below.
     snapshot.records.empty()||
     snapshot.records.size()>(std::size_t{1U}<<24U)||
     snapshot.records.size()>std::numeric_limits<std::uint32_t>::max())return false;
  if(selection.ready()&&selection.source_revision==
         snapshot.header.source_world_revision&&
     selection.field_revision==field_revision&&selection.record_count==
         snapshot.records.size())return true;
  const auto make_shared=[device](const void* bytes,NSUInteger length){
    return [device newBufferWithBytes:bytes length:std::max<NSUInteger>(length,4U)
                              options:MTLResourceStorageModeShared];
  };
  const auto mark_words=(snapshot.records.size()+31U)/32U;
  const auto output_words=4U+snapshot.records.size()+mark_words;
  if(output_words>std::numeric_limits<NSUInteger>::max()/sizeof(std::uint32_t))
    return false;
  MetalGpuHierarchyLiveSelection replacement;
  replacement.hierarchy=make_shared(snapshot.records.data(),
      snapshot.records.size()*sizeof(snapshot.records.front()));
  replacement.children=make_shared(snapshot.child_indices.data(),
      snapshot.child_indices.size()*sizeof(std::uint32_t));
  std::vector<std::uint32_t> root_indices;
  for(std::uint32_t index=0U;index<snapshot.records.size();++index)
    if((snapshot.records[index].child_mask_flags&0x800U)!=0U)
      root_indices.push_back(index);
  if(root_indices.empty()||root_indices.size()>12U)return false;
  replacement.roots=make_shared(root_indices.data(),
      root_indices.size()*sizeof(std::uint32_t));
  replacement.canonical=make_shared(snapshot.canonical_record_indices.data(),
      snapshot.canonical_record_indices.size()*sizeof(std::uint32_t));
  replacement.parents=make_shared(snapshot.parent_records.data(),
      snapshot.parent_records.size()*sizeof(std::uint32_t));
  replacement.face_incidence=make_shared(snapshot.face_incidence.data(),
      snapshot.face_incidence.size()*sizeof(snapshot.face_incidence.front()));
  replacement.edge_topology=make_shared(snapshot.edge_topology.data(),snapshot.edge_topology.size()*sizeof(snapshot.edge_topology.front()));
  replacement.edge_ranges=make_shared(snapshot.edge_ranges.data(),snapshot.edge_ranges.size()*sizeof(snapshot.edge_ranges.front()));
  replacement.edge_incidence=make_shared(snapshot.edge_incidence.data(),snapshot.edge_incidence.size()*sizeof(snapshot.edge_incidence.front()));
  replacement.ancestor_edge_ranges=make_shared(snapshot.ancestor_edge_ranges.data(),snapshot.ancestor_edge_ranges.size()*sizeof(std::uint32_t));
  if(!snapshot.vertex_topology.empty()){
    replacement.orientations=make_shared(snapshot.orientation_flags.data(),
        snapshot.orientation_flags.size()*sizeof(std::uint32_t));
    replacement.vertex_topology=make_shared(snapshot.vertex_topology.data(),
        snapshot.vertex_topology.size()*sizeof(snapshot.vertex_topology.front()));
    replacement.vertex_ranges=make_shared(snapshot.vertex_ranges.data(),
        snapshot.vertex_ranges.size()*sizeof(snapshot.vertex_ranges.front()));
    replacement.vertex_incidence=make_shared(snapshot.vertex_incidence.data(),
        snapshot.vertex_incidence.size()*sizeof(snapshot.vertex_incidence.front()));
  }
  replacement.inputs=make_shared(snapshot.selection_records.data(),
      snapshot.selection_records.size()*sizeof(snapshot.selection_records.front()));
  replacement.source_revision=snapshot.header.source_world_revision;
  replacement.field_revision=field_revision;
  replacement.bootstrap_scene_generation=bootstrap_scene_generation;
  replacement.record_count=static_cast<std::uint32_t>(snapshot.records.size());
  replacement.root_count=static_cast<std::uint32_t>(root_indices.size());
  replacement.output_capacity=replacement.record_count;
  replacement.mark_word_count=static_cast<std::uint32_t>(mark_words);
  // This allocation establishes P7e4a1's compact selected-list and closure
  // queue ABI. Capacity is an allocation guard, never a normal dispatch
  // bound: each later stage takes its grid from a private produced count.
  const auto compact_words=static_cast<std::size_t>(4U)+snapshot.records.size();
  if(compact_words>std::numeric_limits<NSUInteger>::max()/sizeof(std::uint32_t))
    return false;
  const auto compact_bytes=static_cast<NSUInteger>(compact_words*sizeof(std::uint32_t));
  replacement.compact_selected_ping=[device newBufferWithLength:compact_bytes
      options:MTLResourceStorageModePrivate];
  replacement.compact_selected_pong=[device newBufferWithLength:compact_bytes
      options:MTLResourceStorageModePrivate];
  replacement.compact_closure_queue_ping=[device newBufferWithLength:compact_bytes
      options:MTLResourceStorageModePrivate];
  replacement.compact_closure_queue_pong=[device newBufferWithLength:compact_bytes
      options:MTLResourceStorageModePrivate];
  replacement.compact_dispatch_args=[device newBufferWithLength:12U*sizeof(std::uint32_t)
      options:MTLResourceStorageModePrivate];
  std::vector<std::uint32_t> canonical_ranks(snapshot.records.size());
  for(std::uint32_t rank=0U;rank<snapshot.canonical_record_indices.size();++rank)
    canonical_ranks[snapshot.canonical_record_indices[rank]]=rank;
  replacement.compact_canonical_ranks=make_shared(canonical_ranks.data(),
      canonical_ranks.size()*sizeof(std::uint32_t));
  const auto histogram_words=((snapshot.records.size()+255U)/256U)*16U;
  replacement.compact_histogram=[device newBufferWithLength:
      histogram_words*sizeof(std::uint32_t) options:MTLResourceStorageModePrivate];
  replacement.compact_histogram_offsets=[device newBufferWithLength:
      histogram_words*sizeof(std::uint32_t) options:MTLResourceStorageModePrivate];
  replacement.compact_bin_bases=[device newBufferWithLength:16U*sizeof(std::uint32_t)
      options:MTLResourceStorageModePrivate];
  if(!replacement.compact_worklist_ready())return false;
  if(!snapshot.vertex_topology.empty()){
    const auto words=[](std::size_t count)->std::optional<NSUInteger>{
      if(count>std::numeric_limits<NSUInteger>::max()/sizeof(std::uint32_t))
        return std::nullopt;
      return static_cast<NSUInteger>(count*sizeof(std::uint32_t));
    };
    const auto blocks=(snapshot.records.size()+255U)/256U;
    const auto owner_words=words(snapshot.records.size()*12U);
    const auto record_words=words(snapshot.records.size());
    const auto block_words=words(blocks);
    const auto edge_words=words(snapshot.edge_ranges.size());
    const auto promotion_words=words(mark_words);
    if(!owner_words||!record_words||!block_words||!edge_words||!promotion_words)
      return false;
    replacement.closure_owners=[device newBufferWithLength:*owner_words
        options:MTLResourceStorageModePrivate];
    replacement.closure_counts=[device newBufferWithLength:*record_words
        options:MTLResourceStorageModePrivate];
    replacement.closure_offsets=[device newBufferWithLength:*record_words
        options:MTLResourceStorageModePrivate];
    replacement.closure_added_offsets=[device newBufferWithLength:*record_words
        options:MTLResourceStorageModePrivate];
    replacement.closure_block_totals=[device newBufferWithLength:*block_words
        options:MTLResourceStorageModePrivate];
    replacement.closure_block_offsets=[device newBufferWithLength:*block_words
        options:MTLResourceStorageModePrivate];
    replacement.closure_scan_total=[device newBufferWithLength:4U*sizeof(std::uint32_t)
        options:MTLResourceStorageModePrivate];
    replacement.closure_edge_marks=[device newBufferWithLength:*edge_words
        options:MTLResourceStorageModePrivate];
    replacement.closure_red_promotions=[device newBufferWithLength:*promotion_words
        options:MTLResourceStorageModePrivate];
    replacement.closure_status=[device newBufferWithLength:5U*sizeof(std::uint32_t)
        options:MTLResourceStorageModePrivate];
    replacement.closure_dispatch_args=[device newBufferWithLength:8U*sizeof(std::uint32_t)
        options:MTLResourceStorageModePrivate];
    replacement.closure_control_audit=[device newBufferWithLength:8U*sizeof(std::uint32_t)
        options:MTLResourceStorageModeShared];
    // The compact path deliberately has distinct sidecars from P7e4a's
    // fenced full-snapshot prototype above.  A normal P7e4a1 flight must not
    // accidentally bind a record-count owner stream from that prototype.
    const auto compact_edge_words=words(std::max<std::size_t>(snapshot.edge_ranges.size(),1U));
    const auto compact_list_words=words(compact_words);
    const auto compact_record_words=words(snapshot.records.size());
    const auto compact_block_words=words(std::max<std::size_t>(blocks,1U));
    const auto compact_level_words=words(std::max<std::size_t>((blocks+255U)/256U,1U));
    if(!compact_edge_words||!compact_list_words||!compact_record_words||
       !compact_block_words||!compact_level_words)return false;
    replacement.compact_edge_marks=[device newBufferWithLength:*compact_edge_words
        options:MTLResourceStorageModePrivate];
    replacement.compact_green_masks=[device newBufferWithLength:*compact_list_words
        options:MTLResourceStorageModePrivate];
    replacement.compact_green_control=[device newBufferWithLength:5U*sizeof(std::uint32_t)
        options:MTLResourceStorageModePrivate];
    replacement.compact_red_status=[device newBufferWithLength:4U*sizeof(std::uint32_t)
        options:MTLResourceStorageModePrivate];
    replacement.compact_expand_counts=[device newBufferWithLength:*compact_record_words
        options:MTLResourceStorageModePrivate];
    replacement.compact_expand_offsets=[device newBufferWithLength:*compact_record_words
        options:MTLResourceStorageModePrivate];
    replacement.compact_block_totals=[device newBufferWithLength:*compact_block_words
        options:MTLResourceStorageModePrivate];
    replacement.compact_block_offsets=[device newBufferWithLength:*compact_block_words
        options:MTLResourceStorageModePrivate];
    replacement.compact_level_totals=[device newBufferWithLength:*compact_level_words
        options:MTLResourceStorageModePrivate];
    replacement.compact_level_offsets=[device newBufferWithLength:*compact_level_words
        options:MTLResourceStorageModePrivate];
    replacement.compact_scan_total=[device newBufferWithLength:sizeof(std::uint32_t)
        options:MTLResourceStorageModePrivate];
    replacement.compact_final_active=[device newBufferWithLength:*compact_list_words
        options:MTLResourceStorageModePrivate];
    replacement.compact_final_masks=[device newBufferWithLength:*compact_list_words
        options:MTLResourceStorageModePrivate];
    replacement.compact_owner_stream=[device newBufferWithLength:*owner_words
        options:MTLResourceStorageModePrivate];
    replacement.compact_owner_header=[device newBufferWithLength:4U*sizeof(std::uint32_t)
        options:MTLResourceStorageModePrivate];
    replacement.compact_owner_dispatch_args=[device newBufferWithLength:4U*sizeof(std::uint32_t)
        options:MTLResourceStorageModePrivate];
    // Scalar only: the selected/final compact-list headers localize a
    // handoff failure without exposing record IDs or terrain payload.
    replacement.compact_closure_audit=[device newBufferWithLength:25U*sizeof(std::uint32_t)
        options:MTLResourceStorageModeShared];
  }
  for(auto& slot:replacement.slots){
    slot.tuple=[device newBufferWithLength:sizeof(tetra::GpuHierarchySelectionTuple)
        options:MTLResourceStorageModeShared];
    slot.marks=[device newBufferWithLength:output_words*sizeof(std::uint32_t)
        options:MTLResourceStorageModePrivate];
    if(slot.tuple==nil||slot.marks==nil)return false;
  }
  if(replacement.hierarchy==nil||replacement.children==nil||replacement.roots==nil||replacement.canonical==nil||
     replacement.parents==nil||replacement.face_incidence==nil||replacement.edge_topology==nil||replacement.edge_ranges==nil||replacement.edge_incidence==nil||replacement.ancestor_edge_ranges==nil||
     replacement.inputs==nil)
    return false;
  if(!snapshot.vertex_topology.empty()&&
     (!replacement.closure_ready()||!replacement.compact_closure_ready()))return false;
  selection=std::move(replacement);
  selection.device_front_bootstrap_at=std::chrono::steady_clock::now();
  return true;
}

const char* metal_gpu_hierarchy_device_front_phase(
    const MetalGpuHierarchyLiveSelection& selection) {
  if(!selection.ready())return "bootstrap";
  if(selection.compact_p8_encoded>selection.compact_p8_completed)
    return "p8_in_flight";
  if(selection.compact_p8_private_commits!=0U)return "private_front_committed";
  if(selection.compact_p8_rejected!=0U)return "p8_rejected";
  if(selection.compact_owner_materialization_encoded!=0U)return "p8_pending";
  if(selection.compact_closure_encoded!=0U)return "owner_materializer_pending";
  if(selection.submitted!=0U)return "closure_pending";
  return "selector_pending";
}

void retire_metal_gpu_hierarchy_live_selection(
    MetalGpuHierarchyLiveSelection& selection) {
  if(selection.closure_pending&&
     selection.closure_completed->load(std::memory_order_acquire)) {
    const auto* audit=static_cast<const std::uint32_t*>(
        selection.compact_closure_audit.contents);
    // The audit is copied only after the final compact green pass.  It is a
    // completion diagnostic, not an intermediate count readback: words 2
    // and 5 are respectively final-green and red failure latches.
    if(audit!=nullptr)std::copy_n(audit,selection.compact_last_closure_audit.size(),
                                  selection.compact_last_closure_audit.begin());
    if(audit!=nullptr&&audit[2U]==0U&&audit[5U]==0U) {
      ++selection.compact_closure_completed;
      if(audit[4U]!=0U&&audit[12U]!=0U)++selection.compact_quiescent;
    } else ++selection.compact_closure_rejected;
    if(audit!=nullptr) {
      std::copy_n(audit+13U,selection.compact_last_selected_header.size(),
                  selection.compact_last_selected_header.begin());
      std::copy_n(audit+17U,selection.compact_last_final_active_header.size(),
                  selection.compact_last_final_active_header.begin());
      std::copy_n(audit+21U,selection.compact_last_final_masks_header.size(),
                  selection.compact_last_final_masks_header.begin());
    }
    if(selection.compact_p8_encoded>selection.compact_p8_completed) {
      const auto* p8=static_cast<const std::uint32_t*>(
          selection.compact_p8_audit.contents);
      ++selection.compact_p8_completed;
      if(p8!=nullptr)std::copy_n(p8,selection.compact_p8_last_audit.size(),
                                 selection.compact_p8_last_audit.begin());
      if(p8!=nullptr)std::copy_n(p8+selection.compact_p8_last_audit.size(),
                                 selection.compact_p8_last_owner_header.size(),
                                 selection.compact_p8_last_owner_header.begin());
      if(p8!=nullptr&&p8[0U]==1U&&p8[1U]==0U&&p8[2U]!=0U&&
         p8[3U]==0U&&p8[4U]!=0U)++selection.compact_p8_private_commits;
      else ++selection.compact_p8_rejected;
      selection.device_front_p8_completed_at=std::chrono::steady_clock::now();
      if(selection.device_front_p8_at.time_since_epoch().count()!=0)
        selection.device_front_last_p8_completion_milliseconds=
            std::chrono::duration<double,std::milli>(
                selection.device_front_p8_completed_at-
                selection.device_front_p8_at).count();
    }
    selection.closure_pending=false;
  }
  for(auto& slot:selection.slots)if(slot.pending&&
      slot.completed->load(std::memory_order_acquire)){
    slot.pending=false;
    ++selection.completed;
    if(slot.succeeded->load(std::memory_order_acquire))++selection.accepted;
    else ++selection.failed;
  }
}

// P7e4a1's real live closure schedule.  It starts from P7e2's appended
// selection list, never from the full immutable record array.  All candidate
// work grids come from the private compact-list header; scalar controllers
// only arm/validate those grids and are intentionally fixed in number.
//
// The final canonical list and green masks remain private in `selection` for
// P8's future owner materializer.  This function deliberately stops there:
// the old P8 owner shaders take a CPU-sized owner count and cannot consume
// this ABI without reintroducing the record-count dispatch this path removes.
bool encode_metal_gpu_hierarchy_live_compact_closure(
    id<MTLCommandBuffer> command,id<MTLComputePipelineState> canonicalize,
    id<MTLComputePipelineState> green,id<MTLComputePipelineState> red,
    id<MTLComputePipelineState> red_scan,
    MetalGpuHierarchyLiveSelection& selection,bool inject_green_budget_failure,
    std::uint32_t diagnostic_stop=std::numeric_limits<std::uint32_t>::max()) {
  if(!selection.compact_closure_ready()||selection.closure_pending||
     canonicalize==nil||green==nil||red==nil||red_scan==nil)return false;
  constexpr NSUInteger input_grid_offset=0U;
  constexpr NSUInteger output_grid_offset=4U*sizeof(std::uint32_t);
  constexpr std::uint32_t green_round_limit=8U;
  constexpr std::uint32_t red_repair_budget=8U;
  const auto records=selection.record_count;
  id<MTLBlitCommandEncoder> clear=[command blitCommandEncoder];
  // `compact_selected_ping` was just filled by the worklist pass.  Every
  // other capacity-sized sidecar is either completely overwritten before a
  // successful consumer reads it or is fail-closed by its compact four-word
  // header.  Resetting whole allocations here needlessly dominated the
  // no-red flight, especially as the snapshot capacity grows.  Clear just
  // those headers, so a malformed/failed command cannot reuse a previous
  // result while the valid data-parallel stages retain their exact writes.
  constexpr NSUInteger compact_header_bytes=4U*sizeof(std::uint32_t);
  for(id<MTLBuffer> buffer:{selection.compact_selected_pong,
      selection.compact_closure_queue_ping,selection.compact_closure_queue_pong,
      selection.compact_final_active,selection.compact_final_masks})
    [clear fillBuffer:buffer range:NSMakeRange(0U,compact_header_bytes) value:0U];
  // Red status carries a per-flight failure/round latch, so its complete tiny
  // control record (not a capacity-sized payload) must start cleared.
  [clear fillBuffer:selection.compact_red_status
              range:NSMakeRange(0U,compact_header_bytes) value:0U];
  [clear endEncoding];
  // Diagnostic-only cumulative stop points let the benchmark place command
  // boundaries at valid dependencies without counters or CPU readback. Normal
  // closure uses the default sentinel and retains its original one-buffer ABI.
  if(diagnostic_stop==0U)return true;

  const auto encode_canonical=[&](id<MTLBuffer> first,id<MTLBuffer> second,
                                   NSUInteger grid_offset)->id<MTLBuffer>{
    id<MTLBuffer> input=first,output=second;
    // The radix passes depend on the preceding pass's private buffers, but
    // do not need a command-encoder boundary.  Keeping them in one compute
    // encoder avoids a large number of tiny Metal encoder submissions while
    // retaining the same ordered dispatch schedule and an explicit buffer
    // visibility barrier between every dependent pass.
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    for(std::uint32_t shift=0U;shift<24U;shift+=4U)
      for(std::uint32_t phase=0U;phase<3U;++phase) {
        const std::array<std::uint32_t,4> parameters{records,phase,shift,0U};
        [encoder setComputePipelineState:canonicalize];
        // Generated MSL ABI: device quiescence gate, input, parameters,
        // output, ranks, histogram, bin bases, histogram offsets.
        [encoder setBuffer:selection.compact_dispatch_args offset:0U atIndex:0U];
        [encoder setBuffer:input offset:0U atIndex:1U];
        [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:2U];
        [encoder setBuffer:output offset:0U atIndex:3U];
        [encoder setBuffer:selection.compact_canonical_ranks offset:0U atIndex:4U];
        [encoder setBuffer:selection.compact_histogram offset:0U atIndex:5U];
        [encoder setBuffer:selection.compact_bin_bases offset:0U atIndex:6U];
        [encoder setBuffer:selection.compact_histogram_offsets offset:0U atIndex:7U];
        if(phase==1U)
          [encoder dispatchThreads:MTLSizeMake(1U,1U,1U)
               threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
        else
          [encoder dispatchThreadgroupsWithIndirectBuffer:selection.compact_dispatch_args
              indirectBufferOffset:grid_offset
              threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];
        [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
        if(phase==2U)std::swap(input,output);
      }
    [encoder endEncoding];
    return input;
  };
  const auto encode_green=[&](id<MTLBuffer> active,bool inject_failure){
    // Phase 0 itself initializes the mutable sparse sidecars.  Do not clear
    // them on the host before testing the red quiescence gate: a gated final
    // green pass must preserve the already latched masks and zero-grid count.
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    const auto dispatch=[&](std::uint32_t phase,bool indirect){
      const std::array<std::uint32_t,5> parameters{records,
          static_cast<std::uint32_t>(selection.edge_ranges.length/sizeof(std::uint32_t)/2U),
          static_cast<std::uint32_t>(selection.ancestor_edge_ranges.length/sizeof(std::uint32_t)),
          green_round_limit,phase};
      [encoder setComputePipelineState:green];
      // Generated MSL ABI: queue, dispatch, ranks, active, parameters,
      // topology, masks, marks, ancestors.
      [encoder setBuffer:selection.compact_green_control offset:0U atIndex:0U];
      [encoder setBuffer:selection.compact_dispatch_args offset:0U atIndex:1U];
      [encoder setBuffer:selection.compact_canonical_ranks offset:0U atIndex:2U];
      [encoder setBuffer:active offset:0U atIndex:3U];
      [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:4U];
      [encoder setBuffer:selection.edge_topology offset:0U atIndex:5U];
      [encoder setBuffer:selection.compact_green_masks offset:0U atIndex:6U];
      [encoder setBuffer:selection.compact_edge_marks offset:0U atIndex:7U];
      [encoder setBuffer:selection.ancestor_edge_ranges offset:0U atIndex:8U];
      if(indirect)
        [encoder dispatchThreadgroupsWithIndirectBuffer:selection.compact_dispatch_args
            indirectBufferOffset:input_grid_offset
            threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];
      else [encoder dispatchThreads:MTLSizeMake(1U,1U,1U)
            threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
      [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
    };
    dispatch(0U,false); dispatch(1U,true); dispatch(2U,true);
    if(inject_failure)dispatch(7U,false);
    else for(std::uint32_t round=0U;round<green_round_limit;++round) {
      dispatch(3U,false);dispatch(4U,true);dispatch(5U,false);
    }
    dispatch(8U,false);
    dispatch(6U,true);
    [encoder endEncoding];
  };
  const auto encode_red=[&](id<MTLBuffer> active,id<MTLBuffer> output){
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    const auto red_dispatch=[&](std::uint32_t phase,bool indirect){
      const std::array<std::uint32_t,6> parameters{records,
          static_cast<std::uint32_t>(selection.children.length/sizeof(std::uint32_t)),
          static_cast<std::uint32_t>(selection.vertex_ranges.length/(2U*sizeof(std::uint32_t))),
          static_cast<std::uint32_t>(selection.vertex_incidence.length/(2U*sizeof(std::uint32_t))),
          red_repair_budget,phase};
      [encoder setComputePipelineState:red];
      // Generated MSL ABI mirrors the compact-red fixture.
      [encoder setBuffer:selection.compact_red_status offset:0U atIndex:0U];
      [encoder setBuffer:selection.compact_dispatch_args offset:0U atIndex:1U];
      [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:2U];
      [encoder setBuffer:selection.compact_canonical_ranks offset:0U atIndex:3U];
      [encoder setBuffer:active offset:0U atIndex:4U];
      [encoder setBuffer:selection.parents offset:0U atIndex:5U];
      [encoder setBuffer:selection.hierarchy offset:0U atIndex:6U];
      [encoder setBuffer:selection.vertex_topology offset:0U atIndex:7U];
      [encoder setBuffer:selection.vertex_ranges offset:0U atIndex:8U];
      [encoder setBuffer:selection.vertex_incidence offset:0U atIndex:9U];
      [encoder setBuffer:output offset:0U atIndex:10U];
      [encoder setBuffer:selection.compact_green_masks offset:0U atIndex:11U];
      [encoder setBuffer:selection.compact_scan_total offset:0U atIndex:12U];
      [encoder setBuffer:selection.compact_final_active offset:0U atIndex:13U];
      [encoder setBuffer:selection.compact_final_masks offset:0U atIndex:14U];
      [encoder setBuffer:selection.compact_expand_counts offset:0U atIndex:15U];
      [encoder setBuffer:selection.compact_expand_offsets offset:0U atIndex:16U];
      [encoder setBuffer:selection.children offset:0U atIndex:17U];
      if(indirect)[encoder dispatchThreadgroupsWithIndirectBuffer:selection.compact_dispatch_args
          indirectBufferOffset:(phase==3U||phase==7U)?output_grid_offset:input_grid_offset
          threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];
      else [encoder dispatchThreads:MTLSizeMake(1U,1U,1U)
          threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
      [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
    };
    const auto scan_dispatch=[&](std::uint32_t phase,bool indirect){
      [encoder setComputePipelineState:red_scan];
      [encoder setBuffer:active offset:0U atIndex:0U];
      [encoder setBytes:&phase length:sizeof(phase) atIndex:1U];
      [encoder setBuffer:selection.compact_expand_counts offset:0U atIndex:2U];
      [encoder setBuffer:selection.compact_expand_offsets offset:0U atIndex:3U];
      [encoder setBuffer:selection.compact_block_totals offset:0U atIndex:4U];
      [encoder setBuffer:selection.compact_block_offsets offset:0U atIndex:5U];
      [encoder setBuffer:selection.compact_level_totals offset:0U atIndex:6U];
      [encoder setBuffer:selection.compact_level_offsets offset:0U atIndex:7U];
      [encoder setBuffer:selection.compact_scan_total offset:0U atIndex:8U];
      if(indirect)[encoder dispatchThreadgroupsWithIndirectBuffer:selection.compact_dispatch_args
          indirectBufferOffset:phase==7U?output_grid_offset:input_grid_offset
          threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];
      // Phase 2 scans the tiny superblock stream with the same 256-lane
      // prefix primitive.  It has one workgroup but not one thread: with
      // more than one superblock, a scalar dispatch reads uninitialized
      // threadgroup lanes and can falsely report capacity exhaustion.
      else if(phase==2U)[encoder dispatchThreads:MTLSizeMake(256U,1U,1U)
          threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];
      else [encoder dispatchThreads:MTLSizeMake(1U,1U,1U)
          threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
      [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
    };
    red_dispatch(0U,false);red_dispatch(1U,true);
    scan_dispatch(0U,true);scan_dispatch(1U,true);scan_dispatch(2U,false);
    scan_dispatch(3U,true);red_dispatch(2U,false);red_dispatch(3U,true);
    red_dispatch(4U,false);
    [encoder endEncoding];
  };

  // P7e2 append -> canonical compact list -> bounded green/red fixed point.
  // The red output grid is wholly device-produced and becomes the next
  // canonicalizer's input grid.  We intentionally execute the bounded
  // schedule even after quiescence: no CPU observes a red count between
  // rounds.  The red terminal phase latches a failure if repair remains at
  // the end of the fixed budget.
  id<MTLBuffer> canonical_active=encode_canonical(selection.compact_selected_ping,
      selection.compact_selected_pong,input_grid_offset);
  if(diagnostic_stop==1U)return true;
  if(inject_green_budget_failure)encode_green(canonical_active,true);
  else {
    for(std::uint32_t round=0U;round<red_repair_budget;++round) {
      encode_green(canonical_active,false);
      if(diagnostic_stop==2U)return true;
      id<MTLBuffer> red_output=(round&1U)==0U?
          selection.compact_closure_queue_pong:
          selection.compact_closure_queue_ping;
      // A red round writes every live entry before consuming it: phase 0
      // overwrites the compact header, phase 1 overwrites every live count,
      // the four scan phases overwrite their complete live block hierarchy,
      // and phase 3 scatters every output position below the newly written
      // total.  The initial closure clear already initializes failure state.
      // Clearing these capacity-sized sidecars again was therefore pure work;
      // after device quiescence it occurred eight times despite all indirect
      // grids being zero.  Retaining the fixed repair schedule but removing
      // this redundant blit preserves the device-only fail-closed boundary.
      if(diagnostic_stop==3U)return true;
      encode_red(canonical_active,red_output);
      if(diagnostic_stop==4U)return true;
      ++selection.compact_red_encoded;
      id<MTLBuffer> canonical_destination=(round&1U)==0U?
          selection.compact_selected_ping:selection.compact_selected_pong;
      canonical_active=encode_canonical(red_output,canonical_destination,
          output_grid_offset);
      if(diagnostic_stop==5U)return true;
      // Only after the just-produced canonical list exists may red mark the
      // next scheduled round as quiescent.  From then on green/red grids and
      // every radix phase are gated on device; the host never reads a count
      // or decides whether another repair is needed.
      const std::uint32_t quiesce_phase=6U;
      const std::array<std::uint32_t,6> quiesce_parameters{records,
          static_cast<std::uint32_t>(selection.children.length/sizeof(std::uint32_t)),
          static_cast<std::uint32_t>(selection.vertex_ranges.length/(2U*sizeof(std::uint32_t))),
          static_cast<std::uint32_t>(selection.vertex_incidence.length/(2U*sizeof(std::uint32_t))),
          red_repair_budget,quiesce_phase};
      id<MTLComputeCommandEncoder> quiesce=[command computeCommandEncoder];
      [quiesce setComputePipelineState:red];
      [quiesce setBuffer:selection.compact_red_status offset:0U atIndex:0U];
      [quiesce setBuffer:selection.compact_dispatch_args offset:0U atIndex:1U];
      [quiesce setBytes:quiesce_parameters.data() length:sizeof(quiesce_parameters) atIndex:2U];
      [quiesce setBuffer:selection.compact_final_active offset:0U atIndex:13U];
      [quiesce setBuffer:selection.compact_final_masks offset:0U atIndex:14U];
      [quiesce dispatchThreads:MTLSizeMake(1U,1U,1U)
         threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
      [quiesce endEncoding];
      id<MTLComputeCommandEncoder> final_copy=[command computeCommandEncoder];
      [final_copy setComputePipelineState:red];
      [final_copy setBuffer:selection.compact_red_status offset:0U atIndex:0U];
      [final_copy setBuffer:selection.compact_dispatch_args offset:0U atIndex:1U];
      const std::array<std::uint32_t,6> final_copy_parameters{records,
          static_cast<std::uint32_t>(selection.children.length/sizeof(std::uint32_t)),
          static_cast<std::uint32_t>(selection.vertex_ranges.length/(2U*sizeof(std::uint32_t))),
          static_cast<std::uint32_t>(selection.vertex_incidence.length/(2U*sizeof(std::uint32_t))),
          red_repair_budget,7U};
      [final_copy setBytes:final_copy_parameters.data() length:sizeof(final_copy_parameters) atIndex:2U];
      [final_copy setBuffer:canonical_active offset:0U atIndex:4U];
      [final_copy setBuffer:selection.compact_green_masks offset:0U atIndex:11U];
      [final_copy setBuffer:selection.compact_final_active offset:0U atIndex:13U];
      [final_copy setBuffer:selection.compact_final_masks offset:0U atIndex:14U];
      [final_copy dispatchThreadgroupsWithIndirectBuffer:selection.compact_dispatch_args
          indirectBufferOffset:output_grid_offset
          threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];
      [final_copy endEncoding];
      const std::array<std::uint32_t,6> close_copy_parameters{records,
          static_cast<std::uint32_t>(selection.children.length/sizeof(std::uint32_t)),
          static_cast<std::uint32_t>(selection.vertex_ranges.length/(2U*sizeof(std::uint32_t))),
          static_cast<std::uint32_t>(selection.vertex_incidence.length/(2U*sizeof(std::uint32_t))),
          red_repair_budget,8U};
      id<MTLComputeCommandEncoder> close_copy=[command computeCommandEncoder];
      [close_copy setComputePipelineState:red];
      [close_copy setBuffer:selection.compact_dispatch_args offset:0U atIndex:1U];
      [close_copy setBytes:close_copy_parameters.data() length:sizeof(close_copy_parameters) atIndex:2U];
      [close_copy dispatchThreads:MTLSizeMake(1U,1U,1U)
          threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
      [close_copy endEncoding];
    }
    // Probe the post-eighth-repair canonical list, rather than the predicate
    // that caused that last repair.  A final allowed repair may itself reach
    // the fixed point.  This extra green/predicate-only round makes that
    // distinction on device without scanning, scattering, or CPU polling.
    encode_green(canonical_active,false);
    const auto red_probe=[&](std::uint32_t phase,bool indirect){
      const std::array<std::uint32_t,6> parameters{records,
          static_cast<std::uint32_t>(selection.children.length/sizeof(std::uint32_t)),
          static_cast<std::uint32_t>(selection.vertex_ranges.length/(2U*sizeof(std::uint32_t))),
          static_cast<std::uint32_t>(selection.vertex_incidence.length/(2U*sizeof(std::uint32_t))),
          red_repair_budget,phase};
      id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
      [encoder setComputePipelineState:red];
      [encoder setBuffer:selection.compact_red_status offset:0U atIndex:0U];
      [encoder setBuffer:selection.compact_dispatch_args offset:0U atIndex:1U];
      [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:2U];
      [encoder setBuffer:selection.compact_canonical_ranks offset:0U atIndex:3U];
      [encoder setBuffer:canonical_active offset:0U atIndex:4U];
      [encoder setBuffer:selection.parents offset:0U atIndex:5U];
      [encoder setBuffer:selection.hierarchy offset:0U atIndex:6U];
      [encoder setBuffer:selection.vertex_topology offset:0U atIndex:7U];
      [encoder setBuffer:selection.vertex_ranges offset:0U atIndex:8U];
      [encoder setBuffer:selection.vertex_incidence offset:0U atIndex:9U];
      // Predicate and terminal do not dereference the output/scan sidecars,
      // but binding the complete ABI keeps this controller safely reusable.
      [encoder setBuffer:selection.compact_closure_queue_ping offset:0U atIndex:10U];
      [encoder setBuffer:selection.compact_green_masks offset:0U atIndex:11U];
      [encoder setBuffer:selection.compact_scan_total offset:0U atIndex:12U];
      [encoder setBuffer:selection.compact_final_active offset:0U atIndex:13U];
      [encoder setBuffer:selection.compact_final_masks offset:0U atIndex:14U];
      [encoder setBuffer:selection.compact_expand_counts offset:0U atIndex:15U];
      [encoder setBuffer:selection.compact_expand_offsets offset:0U atIndex:16U];
      [encoder setBuffer:selection.children offset:0U atIndex:17U];
      if(indirect)[encoder dispatchThreadgroupsWithIndirectBuffer:selection.compact_dispatch_args
          indirectBufferOffset:phase==7U?output_grid_offset:input_grid_offset
          threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];
      else [encoder dispatchThreads:MTLSizeMake(1U,1U,1U)
          threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
      [encoder endEncoding];
    };
    // A nonzero fresh predicate latches terminal failure before P8 can
    // materialize its future owner stream.
    red_probe(0U,false);red_probe(1U,true);red_probe(5U,false);
    // If the post-budget probe is clean it is the first authoritative
    // quiescent list, so latch/copy it too.  If it still has red work, phase
    // 5 has poisoned status and phases 6/7 become harmless no-ops.
    red_probe(6U,false);red_probe(7U,true);red_probe(8U,false);
  }
  // The final green queue and the red status are copied only for completion
  // evidence.  No count, record ID, owner, or vertex payload is read back.
  id<MTLBlitCommandEncoder> audit=[command blitCommandEncoder];
  [audit copyFromBuffer:selection.compact_green_control sourceOffset:0U
               toBuffer:selection.compact_closure_audit destinationOffset:0U
                   size:5U*sizeof(std::uint32_t)];
  [audit copyFromBuffer:selection.compact_red_status sourceOffset:0U
               toBuffer:selection.compact_closure_audit
      destinationOffset:5U*sizeof(std::uint32_t) size:4U*sizeof(std::uint32_t)];
  [audit copyFromBuffer:selection.compact_dispatch_args sourceOffset:0U
               toBuffer:selection.compact_closure_audit
      destinationOffset:9U*sizeof(std::uint32_t) size:4U*sizeof(std::uint32_t)];
  [audit copyFromBuffer:selection.compact_selected_ping sourceOffset:0U
               toBuffer:selection.compact_closure_audit
      destinationOffset:13U*sizeof(std::uint32_t) size:4U*sizeof(std::uint32_t)];
  [audit copyFromBuffer:selection.compact_final_active sourceOffset:0U
               toBuffer:selection.compact_closure_audit
      destinationOffset:17U*sizeof(std::uint32_t) size:4U*sizeof(std::uint32_t)];
  [audit copyFromBuffer:selection.compact_final_masks sourceOffset:0U
               toBuffer:selection.compact_closure_audit
      destinationOffset:21U*sizeof(std::uint32_t) size:4U*sizeof(std::uint32_t)];
  [audit endEncoding];
  selection.closure_completed->store(false,std::memory_order_release);
  selection.closure_pending=true;
  const auto completed=selection.closure_completed;
  [command addCompletedHandler:^(id<MTLCommandBuffer> finished){
    completed->store(finished.status==MTLCommandBufferStatusCompleted,
                     std::memory_order_release);
  }];
  ++selection.compact_closure_encoded;
  selection.device_front_closure_at=std::chrono::steady_clock::now();
  return true;
}

// P7e4a1's sole production-facing operation.  The controller consumes the
// device-latched compact pair and writes an indirect grid; the host supplies
// neither a live owner count nor a record-count-sized dispatch.  This leaf
// deliberately stops at the staged owner stream, before P8's count/emit ABI.
bool encode_metal_gpu_hierarchy_compact_owner_materialize(
    id<MTLCommandBuffer> command,id<MTLComputePipelineState> materialize,
    MetalGpuHierarchyLiveSelection& selection) {
  if(command==nil||materialize==nil||!selection.compact_owner_ready())return false;
  const std::array<std::uint32_t,4> parameters{selection.record_count,
      selection.output_capacity,0U,0U};
  const auto encode=[&](std::uint32_t phase,bool indirect) {
    auto phase_parameters=parameters;
    phase_parameters[2U]=phase;
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:materialize];
    // Generated MSL ABI is audited by the owner-materializer smoke.  The
    // input headers, immutable topology, staged stream/header, and indirect
    // grid are all explicit private buffers.
    // SPIRV-Cross groups writable buffers first: dispatch, header,
    // parameters, then compact inputs and immutable topology.
    [encoder setBuffer:selection.compact_owner_dispatch_args offset:0U atIndex:0U];
    [encoder setBuffer:selection.compact_owner_header offset:0U atIndex:1U];
    [encoder setBytes:phase_parameters.data() length:sizeof(phase_parameters)
          atIndex:2U];
    [encoder setBuffer:selection.compact_final_active offset:0U atIndex:3U];
    [encoder setBuffer:selection.compact_final_masks offset:0U atIndex:4U];
    [encoder setBuffer:selection.orientations offset:0U atIndex:5U];
    [encoder setBuffer:selection.compact_owner_stream offset:0U atIndex:6U];
    [encoder setBuffer:selection.hierarchy offset:0U atIndex:7U];
    [encoder setBuffer:selection.edge_topology offset:0U atIndex:8U];
    if(indirect)
      [encoder dispatchThreadgroupsWithIndirectBuffer:selection.compact_owner_dispatch_args
          indirectBufferOffset:0U threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];
    else [encoder dispatchThreads:MTLSizeMake(1U,1U,1U)
          threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
    [encoder endEncoding];
  };
  encode(0U,false);
  encode(1U,true);
  encode(2U,true);
  encode(3U,false);
  ++selection.compact_owner_materialization_encoded;
  selection.device_front_materializer_at=std::chrono::steady_clock::now();
  return true;
}

// P7e4a1 P8 is deliberately a separate ABI from the legacy P8c owner route:
// every data-parallel grid comes from the private owner/result headers.  The
// host supplies immutable field data, render origin, and allocation capacity,
// never a current owner count or candidate payload.
struct MetalCompactOwnerP8Pipelines {
  id<MTLComputePipelineState> control=nil,count=nil,scan=nil,emit=nil,triangle_emit=nil;
  id<MTLComputePipelineState> validate=nil,copy=nil,publish=nil;
  id<MTLComputePipelineState> microbatch=nil,microbatch_validate=nil;
  id<MTLComputePipelineState> hybrid_scan=nil,hybrid_finalize=nil;
};
struct alignas(16) MetalCompactOwnerEmitParameters {
  std::uint32_t capacity{},unused{},padding0{},padding1{};
  std::array<float,4> origin{};
  std::uint32_t source_low{},source_high{},pad0{},pad1{};
};
static_assert(sizeof(MetalCompactOwnerEmitParameters)==48U);
static_assert(offsetof(MetalCompactOwnerEmitParameters,origin)==16U);
static_assert(offsetof(MetalCompactOwnerEmitParameters,source_low)==32U);

id<MTLLibrary> make_file_shader_library(id<MTLDevice> device,
                                        const char* path);

bool encode_metal_gpu_hierarchy_compact_owner_p8(
    id<MTLCommandBuffer> command,const MetalCompactOwnerP8Pipelines& p,
    id<MTLBuffer> owners,id<MTLBuffer> owner_header,id<MTLBuffer> field,
    id<MTLBuffer> templates,id<MTLBuffer> counts,id<MTLBuffer> offsets,
    id<MTLBuffer> block_totals,id<MTLBuffer> block_offsets,
    id<MTLBuffer> level_totals,id<MTLBuffer> level_offsets,
    id<MTLBuffer> signs,id<MTLBuffer> candidate,id<MTLBuffer> status,
    id<MTLBuffer> dispatches,id<MTLBuffer> retained,id<MTLBuffer> arguments,
    std::uint32_t vertex_capacity,tetra::Vec3 origin,std::uint64_t source,
    std::uint32_t stop_after=6U) {
  if(command==nil||p.control==nil||p.count==nil||p.scan==nil||p.emit==nil||
     p.validate==nil||p.copy==nil||p.publish==nil||owners==nil||
     owner_header==nil||field==nil||templates==nil||counts==nil||offsets==nil||
     block_totals==nil||block_offsets==nil||level_totals==nil||level_offsets==nil||
     signs==nil||candidate==nil||status==nil||dispatches==nil||retained==nil||
     arguments==nil)return false;
  const std::array<std::uint32_t,1> cap{vertex_capacity};
  const std::array<std::uint32_t,4> count_parameters{
      static_cast<std::uint32_t>(source),static_cast<std::uint32_t>(source>>32U),0U,0U};
  const MetalCompactOwnerEmitParameters emit_parameters{vertex_capacity,0U,0U,0U,
      {static_cast<float>(origin.x),static_cast<float>(origin.y),
       static_cast<float>(origin.z),0.0F},static_cast<std::uint32_t>(source),
      static_cast<std::uint32_t>(source>>32U),0U,0U};
  id<MTLComputeCommandEncoder> e=[command computeCommandEncoder];
  // Generated ABI: dispatches, status, candidate, parameters, owner-header.
  [e setComputePipelineState:p.control];[e setBuffer:dispatches offset:0 atIndex:0];
  [e setBuffer:status offset:0 atIndex:1];[e setBuffer:candidate offset:0 atIndex:2];
  [e setBytes:cap.data() length:sizeof(cap) atIndex:3];[e setBuffer:owner_header offset:0 atIndex:4];
  [e dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[e endEncoding];
  if(stop_after==0U)return true;
  e=[command computeCommandEncoder];[e setComputePipelineState:p.count];
  [e setBuffer:field offset:0 atIndex:0];[e setBytes:count_parameters.data() length:sizeof(count_parameters) atIndex:1];
  // Generated MSL ABI: field, parameters, owners, counts, status, header,
  // signs, templates. Keep this explicit: a shifted count buffer makes every
  // later prefix/candidate result look like a valid zero-output rejection.
  [e setBuffer:owners offset:0 atIndex:2];[e setBuffer:counts offset:0 atIndex:3];
  [e setBuffer:status offset:0 atIndex:4];[e setBuffer:owner_header offset:0 atIndex:5];
  [e setBuffer:signs offset:0 atIndex:6];[e setBuffer:templates offset:0 atIndex:7];
  [e dispatchThreadgroupsWithIndirectBuffer:dispatches indirectBufferOffset:0 threadsPerThreadgroup:MTLSizeMake(64,1,1)];[e endEncoding];
  if(stop_after==1U)return true;
  const std::array<std::uint32_t,6> scan_phases{0U,1U,2U,4U,5U,6U};
  for(const auto phase:scan_phases) {
    const std::array<std::uint32_t,2> scan_parameters{phase,vertex_capacity};
    const NSUInteger grid_offset=(phase==0U?16U:phase==1U?32U:phase==2U?48U:
        phase==4U?64U:phase==5U?80U:96U);
    e=[command computeCommandEncoder];[e setComputePipelineState:p.scan];
    // Generated ABI: owner header, parameters, counts, ascending totals,
    // output offsets, descending offsets, dispatches, status, candidate.
    [e setBuffer:owner_header offset:0 atIndex:0];
    [e setBytes:scan_parameters.data() length:sizeof(scan_parameters) atIndex:1];
    [e setBuffer:counts offset:0 atIndex:2];[e setBuffer:block_totals offset:0 atIndex:3];
    [e setBuffer:level_totals offset:0 atIndex:4];[e setBuffer:offsets offset:0 atIndex:5];
    [e setBuffer:block_offsets offset:0 atIndex:6];[e setBuffer:level_offsets offset:0 atIndex:7];
    [e setBuffer:dispatches offset:0 atIndex:8];[e setBuffer:status offset:0 atIndex:9];
    [e setBuffer:candidate offset:0 atIndex:10];
    [e dispatchThreadgroupsWithIndirectBuffer:dispatches indirectBufferOffset:grid_offset threadsPerThreadgroup:MTLSizeMake(256,1,1)];[e endEncoding];
  }
  if(stop_after==2U)return true;
  e=[command computeCommandEncoder];[e setComputePipelineState:p.emit];
  [e setBuffer:field offset:0 atIndex:0];[e setBuffer:owners offset:0 atIndex:1];
  [e setBuffer:candidate offset:0 atIndex:2];[e setBuffer:owner_header offset:0 atIndex:3];
  // Generated ABI: field, owners, candidate, header, templates, packed signs,
  // parameters, offsets, counts. Emission must consume count's sign evidence.
  [e setBuffer:templates offset:0 atIndex:4];[e setBuffer:signs offset:0 atIndex:5];
  [e setBytes:&emit_parameters length:sizeof(emit_parameters) atIndex:6];
  [e setBuffer:offsets offset:0 atIndex:7];[e setBuffer:counts offset:0 atIndex:8];
  [e dispatchThreadgroupsWithIndirectBuffer:dispatches indirectBufferOffset:112U threadsPerThreadgroup:MTLSizeMake(64,1,1)];[e endEncoding];
  if(stop_after==3U)return true;
  e=[command computeCommandEncoder];[e setComputePipelineState:p.validate];
  [e setBuffer:status offset:0 atIndex:0];[e setBuffer:candidate offset:0 atIndex:1];[e setBytes:cap.data() length:sizeof(cap) atIndex:2];
  [e dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[e endEncoding];
  if(stop_after==4U)return true;
  e=[command computeCommandEncoder];[e setComputePipelineState:p.copy];
  [e setBuffer:status offset:0 atIndex:0];[e setBuffer:candidate offset:0 atIndex:1];[e setBuffer:retained offset:0 atIndex:2];
  [e dispatchThreadgroupsWithIndirectBuffer:dispatches indirectBufferOffset:128U threadsPerThreadgroup:MTLSizeMake(256,1,1)];[e endEncoding];
  if(stop_after==5U)return true;
  e=[command computeCommandEncoder];[e setComputePipelineState:p.publish];
  [e setBuffer:status offset:0 atIndex:0];[e setBuffer:arguments offset:0 atIndex:1];[e setBuffer:candidate offset:0 atIndex:2];
  [e dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[e endEncoding];
  return true;
}

// P7e4d comparison route. The microbatch kernel itself enforces the
// 1,024-owner tier from the private owner header; this host code never reads
// or branches on that count. Validation remains before retained-front copy.
bool encode_metal_gpu_hierarchy_compact_owner_p8_microbatch(
    id<MTLCommandBuffer> command,const MetalCompactOwnerP8Pipelines& p,
    id<MTLBuffer> owners,id<MTLBuffer> owner_header,id<MTLBuffer> field,
    id<MTLBuffer> templates,id<MTLBuffer> candidate,id<MTLBuffer> status,
    id<MTLBuffer> copy_dispatch,id<MTLBuffer> counts,id<MTLBuffer> offsets,
    id<MTLBuffer> retained,id<MTLBuffer> arguments,std::uint32_t vertex_capacity,
    tetra::Vec3 origin,std::uint64_t source) {
  if(command==nil||p.microbatch==nil||p.microbatch_validate==nil||p.copy==nil||
     p.publish==nil||owners==nil||owner_header==nil||field==nil||
     templates==nil||candidate==nil||status==nil||copy_dispatch==nil||
     counts==nil||offsets==nil||retained==nil||arguments==nil)return false;
  const std::array<std::uint32_t,1> cap{vertex_capacity};
  const MetalCompactOwnerEmitParameters parameters{vertex_capacity,0U,0U,0U,
      {static_cast<float>(origin.x),static_cast<float>(origin.y),
       static_cast<float>(origin.z),0.0F},static_cast<std::uint32_t>(source),
      static_cast<std::uint32_t>(source>>32U),0U,0U};
  id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
  // Generated MSL ABI: field, owners, candidate, parameters, owner header,
  // status, templates. One 1,024-thread group owns count/prefix/emission.
  [encoder setComputePipelineState:p.microbatch];
  [encoder setBuffer:field offset:0U atIndex:0U];
  [encoder setBuffer:owners offset:0U atIndex:1U];
  [encoder setBuffer:candidate offset:0U atIndex:2U];
  [encoder setBytes:&parameters length:sizeof(parameters) atIndex:3U];
  [encoder setBuffer:owner_header offset:0U atIndex:4U];
  [encoder setBuffer:status offset:0U atIndex:5U];
  [encoder setBuffer:copy_dispatch offset:0U atIndex:6U];
  [encoder setBuffer:templates offset:0U atIndex:7U];
  [encoder setBuffer:counts offset:0U atIndex:8U];
  [encoder setBuffer:offsets offset:0U atIndex:9U];
  [encoder dispatchThreadgroups:MTLSizeMake(1U,1U,1U)
         threadsPerThreadgroup:MTLSizeMake(1024U,1U,1U)];
  [encoder endEncoding];
  encoder=[command computeCommandEncoder];
  [encoder setComputePipelineState:p.microbatch_validate];
  // Generated ABI: status, private copy grid, candidate, capacity.
  [encoder setBuffer:status offset:0U atIndex:0U];
  [encoder setBuffer:copy_dispatch offset:0U atIndex:1U];
  [encoder setBuffer:candidate offset:0U atIndex:2U];
  [encoder setBytes:cap.data() length:sizeof(cap) atIndex:3U];
  [encoder dispatchThreads:MTLSizeMake(1U,1U,1U)
         threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
  [encoder endEncoding];
  // Both microbatch construction and validation leave this device-produced
  // grid zero on failure, so an invalid candidate cannot copy its payload.
  encoder=[command computeCommandEncoder];
  [encoder setComputePipelineState:p.copy];
  [encoder setBuffer:status offset:0U atIndex:0U];
  [encoder setBuffer:candidate offset:0U atIndex:1U];
  [encoder setBuffer:retained offset:0U atIndex:2U];
  [encoder dispatchThreadgroupsWithIndirectBuffer:copy_dispatch
      indirectBufferOffset:0U
         threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];
  [encoder endEncoding];
  encoder=[command computeCommandEncoder];
  [encoder setComputePipelineState:p.publish];
  [encoder setBuffer:status offset:0U atIndex:0U];
  [encoder setBuffer:arguments offset:0U atIndex:1U];
  [encoder setBuffer:candidate offset:0U atIndex:2U];
  [encoder dispatchThreads:MTLSizeMake(1U,1U,1U)
         threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
  [encoder endEncoding];
  return true;
}

// P7e4e comparison route: only the bounded prefix/header step is serialised
// into one workgroup. Count/sign and emission retain their parallel kernels.
bool encode_metal_gpu_hierarchy_compact_owner_p8_hybrid(
    id<MTLCommandBuffer> command,const MetalCompactOwnerP8Pipelines& p,
    id<MTLBuffer> owners,id<MTLBuffer> owner_header,id<MTLBuffer> field,
    id<MTLBuffer> templates,id<MTLBuffer> candidate,id<MTLBuffer> status,
    id<MTLBuffer> copy_dispatch,id<MTLBuffer> triangle_dispatch,
    id<MTLBuffer> counts,id<MTLBuffer> offsets,
    id<MTLBuffer> signs,id<MTLBuffer> retained,id<MTLBuffer> arguments,
    std::uint32_t vertex_capacity,tetra::Vec3 origin,std::uint64_t source,
    std::uint32_t first_stage=0U,std::uint32_t last_stage=4U) {
  if(command==nil||p.count==nil||p.hybrid_scan==nil||p.triangle_emit==nil||
     p.hybrid_finalize==nil||p.copy==nil||owners==nil||owner_header==nil||
     field==nil||templates==nil||candidate==nil||status==nil||
     copy_dispatch==nil||triangle_dispatch==nil||counts==nil||offsets==nil||signs==nil||
     retained==nil||arguments==nil||first_stage>last_stage||last_stage>4U)return false;
  const std::array<std::uint32_t,1> cap{vertex_capacity};
  // reserved0 selects per-owner failure sentinels. The scan folds those after
  // parallel count work, avoiding the old scalar setup/control dispatch.
  const std::array<std::uint32_t,4> count_parameters{
      static_cast<std::uint32_t>(source),static_cast<std::uint32_t>(source>>32U),1U,0U};
  const MetalCompactOwnerEmitParameters emit_parameters{vertex_capacity,0U,0U,0U,
      {static_cast<float>(origin.x),static_cast<float>(origin.y),
       static_cast<float>(origin.z),0.0F},static_cast<std::uint32_t>(source),
      static_cast<std::uint32_t>(source>>32U),0U,0U};
  id<MTLComputeCommandEncoder> encoder=nil;
  // This fixed 1,024-thread grid is bounded by the count kernel's header
  // guards. It deliberately remains parallel (16 x 64-thread groups).
  if(first_stage<=0U&&last_stage>=0U){encoder=[command computeCommandEncoder];
  [encoder setComputePipelineState:p.count];
  [encoder setBuffer:field offset:0U atIndex:0U];
  [encoder setBytes:count_parameters.data() length:sizeof(count_parameters) atIndex:1U];
  [encoder setBuffer:owners offset:0U atIndex:2U];
  [encoder setBuffer:counts offset:0U atIndex:3U];
  [encoder setBuffer:status offset:0U atIndex:4U];
  [encoder setBuffer:owner_header offset:0U atIndex:5U];
  [encoder setBuffer:signs offset:0U atIndex:6U];
  [encoder setBuffer:templates offset:0U atIndex:7U];
  [encoder dispatchThreads:MTLSizeMake(1024U,1U,1U)
         threadsPerThreadgroup:MTLSizeMake(64U,1U,1U)];
  [encoder endEncoding];
  }
  if(first_stage<=1U&&last_stage>=1U){encoder=[command computeCommandEncoder];
  // Generated MSL ABI: owner header, status, candidate, triangle dispatch,
  // parameters, counts, offsets. This is the sole one-workgroup prefix/header
  // operation; it also arms the private triangle-emission grid.
  [encoder setComputePipelineState:p.hybrid_scan];
  [encoder setBuffer:owner_header offset:0U atIndex:0U];
  [encoder setBuffer:status offset:0U atIndex:1U];
  [encoder setBuffer:candidate offset:0U atIndex:2U];
  [encoder setBuffer:triangle_dispatch offset:0U atIndex:3U];
  [encoder setBytes:cap.data() length:sizeof(cap) atIndex:4U];
  [encoder setBuffer:counts offset:0U atIndex:5U];
  [encoder setBuffer:offsets offset:0U atIndex:6U];
  [encoder dispatchThreadgroups:MTLSizeMake(1U,1U,1U)
         threadsPerThreadgroup:MTLSizeMake(1024U,1U,1U)];
  [encoder endEncoding];
  }
  if(first_stage<=2U&&last_stage>=2U){encoder=[command computeCommandEncoder];
  [encoder setComputePipelineState:p.triangle_emit];
  [encoder setBuffer:field offset:0U atIndex:0U];
  [encoder setBuffer:owners offset:0U atIndex:1U];
  [encoder setBuffer:candidate offset:0U atIndex:2U];
  [encoder setBuffer:owner_header offset:0U atIndex:3U];
  [encoder setBuffer:offsets offset:0U atIndex:4U];
  [encoder setBuffer:counts offset:0U atIndex:5U];
  [encoder setBuffer:templates offset:0U atIndex:6U];
  [encoder setBuffer:signs offset:0U atIndex:7U];
  [encoder setBytes:&emit_parameters length:sizeof(emit_parameters) atIndex:8U];
  [encoder dispatchThreadgroupsWithIndirectBuffer:triangle_dispatch
      indirectBufferOffset:0U threadsPerThreadgroup:MTLSizeMake(64U,1U,1U)];
  [encoder endEncoding];
  }
  if(first_stage<=3U&&last_stage>=3U){encoder=[command computeCommandEncoder];
  // Generated ABI: status, actual copy grid, candidate, capacity, arguments.
  [encoder setComputePipelineState:p.hybrid_finalize];
  [encoder setBuffer:status offset:0U atIndex:0U];
  [encoder setBuffer:copy_dispatch offset:0U atIndex:1U];
  [encoder setBuffer:candidate offset:0U atIndex:2U];
  [encoder setBytes:cap.data() length:sizeof(cap) atIndex:3U];
  [encoder setBuffer:arguments offset:0U atIndex:4U];
  [encoder dispatchThreads:MTLSizeMake(1U,1U,1U)
         threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
  [encoder endEncoding];
  }
  if(first_stage<=4U&&last_stage>=4U){encoder=[command computeCommandEncoder];
  [encoder setComputePipelineState:p.copy];
  [encoder setBuffer:status offset:0U atIndex:0U];
  [encoder setBuffer:candidate offset:0U atIndex:1U];
  [encoder setBuffer:retained offset:0U atIndex:2U];
  [encoder dispatchThreadgroupsWithIndirectBuffer:copy_dispatch indirectBufferOffset:0U
         threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];
  [encoder endEncoding];
  }
  return true;
}

// Hardware fixture for the dedicated compact P8 ABI.  The owner input is a
// bounded test stand-in for the preceding private materializer; geometry and
// retained-front arguments are copied back only after completion for oracle
// comparison.
bool run_metal_gpu_compact_owner_p8_smoke_test(id<MTLDevice> device) {
  const auto directory=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR);
  const auto pipeline=[&](const char* name)->id<MTLComputePipelineState>{
    id<MTLLibrary> library=make_file_shader_library(device,(directory/name).string().c_str());
    NSError* error=nil;id<MTLFunction> function=library==nil?nil:[library newFunctionWithName:@"main0"];
    return function==nil?nil:[device newComputePipelineStateWithFunction:function error:&error];
  };
  MetalCompactOwnerP8Pipelines p{pipeline("gpu_terrain_compact_owner_control.comp.metal"),
      pipeline("gpu_terrain_compact_owner_count.comp.metal"),pipeline("gpu_terrain_compact_owner_scan.comp.metal"),
      pipeline("gpu_terrain_compact_owner_emit.comp.metal"),pipeline("gpu_terrain_compact_owner_triangle_emit.comp.metal"),pipeline("gpu_terrain_compact_owner_validate.comp.metal"),
      pipeline("gpu_terrain_compact_owner_copy.comp.metal"),pipeline("gpu_terrain_compact_owner_publish.comp.metal"),
      pipeline("gpu_terrain_compact_owner_microbatch.comp.metal"),
      pipeline("gpu_terrain_compact_owner_microbatch_validate.comp.metal"),
      pipeline("gpu_terrain_compact_owner_hybrid_scan.comp.metal"),
      pipeline("gpu_terrain_compact_owner_hybrid_finalize.comp.metal")};
  if(p.control==nil||p.count==nil||p.scan==nil||p.emit==nil||p.triangle_emit==nil||p.validate==nil||
     p.copy==nil||p.publish==nil||p.microbatch==nil||p.microbatch_validate==nil||
     p.hybrid_scan==nil||p.hybrid_finalize==nil){
    std::fprintf(stderr,"compact P8 fixture pipeline creation failed (microbatch=%s)\n",
        p.microbatch==nil?"false":"true");return false;
  }
  tetra::GpuTerrainFieldTupleParameters fp;fp.source_revision=719U;fp.field_revision=3U;
  fp.domain.world_extent=1.0;fp.field.kind=tetra::ImplicitShapeKind::perlin_terrain;
  fp.field.centre={.5,.52,.5};fp.field.radius=.37;fp.field.terrain.planet_radius=.37;
  const auto tuple=tetra::make_gpu_terrain_field_tuple(fp);
  std::vector<tetra::WorldTetAddress> addresses;
  for(std::uint8_t root=0;root<tetra::bcc_root_tetrahedron_count;++root)addresses.push_back(tetra::WorldTetAddress::root(root));
  const auto packet=tetra::make_gpu_green_mask_packet(addresses,fp.source_revision);
  const auto templates=tetra::make_gpu_green_template_table();
  const auto expected_roots=tetra::gpu_terrain_root_packet(packet,tuple,100000U);
  auto expected_base=tetra::gpu_terrain_base_triangles(expected_roots,100000U);
  for(auto& triangle:expected_base)for(auto& root:triangle.roots){root.x=float(root.x);root.y=float(root.y);root.z=float(root.z);}
  const auto expected=tetra::gpu_terrain_project_base_triangles(expected_base,
      tetra::gpu_terrain_field_tuple_sphere(tuple),{},100000U);
  const std::uint32_t owners=static_cast<std::uint32_t>(packet.owners.size());
  const std::uint32_t vertices=static_cast<std::uint32_t>(expected.size()*12U);
  if(owners==0U||vertices==0U)return false;
  const auto shared=[&](const void* data,NSUInteger bytes){return [device newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];};
  const auto private_buffer=[&](NSUInteger bytes){return [device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];};
  const std::array<std::uint32_t,4> owner_words{owners,owners,0U,1U};
  id<MTLBuffer> field=shared(&tuple,sizeof(tuple)),owner=shared(packet.owners.data(),packet.owners.size()*sizeof(packet.owners.front()));
  id<MTLBuffer> stencil=shared(templates.data(),sizeof(templates)),header=shared(owner_words.data(),sizeof(owner_words));
  id<MTLBuffer> counts=private_buffer(owners*4U),offsets=private_buffer(owners*4U),block_totals=private_buffer(((owners+255U)/256U)*4U),block_offsets=private_buffer(((owners+255U)/256U)*4U);
  id<MTLBuffer> level_totals=private_buffer(std::max(1U,(owners+65535U)/65536U)*4U),level_offsets=private_buffer(std::max(1U,(owners+65535U)/65536U)*4U),signs=private_buffer(owners*12U);
  id<MTLBuffer> candidate=private_buffer((4U+vertices*18U)*4U),status=private_buffer(8U),dispatches=private_buffer(9U*16U),microbatch_copy_dispatch=private_buffer(16U),triangle_dispatch=private_buffer(12U);
  id<MTLBuffer> retained=private_buffer(vertices*18U*4U),arguments=private_buffer(16U);
  id<MTLBuffer> readback=[device newBufferWithLength:vertices*18U*4U
      options:MTLResourceStorageModeShared];
  id<MTLBuffer> read_arguments=[device newBufferWithLength:16U
      options:MTLResourceStorageModeShared];
  id<MTLBuffer> failed_readback=[device newBufferWithLength:vertices*18U*4U
      options:MTLResourceStorageModeShared];
  id<MTLBuffer> failed_arguments=[device newBufferWithLength:16U
      options:MTLResourceStorageModeShared];
  id<MTLBuffer> stage_audit=[device newBufferWithLength:42U*sizeof(std::uint32_t)
      options:MTLResourceStorageModeShared];
  id<MTLCommandQueue> queue=[device newCommandQueue];
  if(field==nil||owner==nil||stencil==nil||header==nil||counts==nil||offsets==nil||block_totals==nil||block_offsets==nil||level_totals==nil||level_offsets==nil||signs==nil||candidate==nil||status==nil||dispatches==nil||microbatch_copy_dispatch==nil||triangle_dispatch==nil||retained==nil||arguments==nil||readback==nil||read_arguments==nil||failed_readback==nil||failed_arguments==nil||stage_audit==nil||queue==nil)return false;
  // Keep stage isolation in this fixture: a translated shader fault must be
  // attributable before this route can be composed into the live command
  // buffer.  No stage payload is read back here.
  for(std::uint32_t stage=0U;stage<=6U;++stage) {
    id<MTLCommandBuffer> probe=[queue commandBuffer];
    id<MTLBlitCommandEncoder> probe_clear=[probe blitCommandEncoder];
    for(id<MTLBuffer> b:{counts,offsets,block_totals,block_offsets,level_totals,
                         level_offsets,signs,candidate,status,dispatches,retained})
      [probe_clear fillBuffer:b range:NSMakeRange(0,b.length) value:0U];
    [probe_clear endEncoding];
    if(!encode_metal_gpu_hierarchy_compact_owner_p8(probe,p,owner,header,field,
         stencil,counts,offsets,block_totals,block_offsets,level_totals,
         level_offsets,signs,candidate,status,dispatches,retained,arguments,
         vertices,{},fp.source_revision,stage))return false;
    [probe commit];[probe waitUntilCompleted];
    if(probe.status!=MTLCommandBufferStatusCompleted) {
      std::fprintf(stderr,"compact P8 fixture failed at stage %u\n",stage);
      return false;
    }
    id<MTLCommandBuffer> audit_command=[queue commandBuffer];
    id<MTLBlitCommandEncoder> audit=[audit_command blitCommandEncoder];
    [audit copyFromBuffer:status sourceOffset:0U toBuffer:stage_audit
          destinationOffset:0U size:2U*sizeof(std::uint32_t)];
    [audit copyFromBuffer:candidate sourceOffset:0U toBuffer:stage_audit
          destinationOffset:2U*sizeof(std::uint32_t) size:4U*sizeof(std::uint32_t)];
    [audit copyFromBuffer:dispatches sourceOffset:0U toBuffer:stage_audit
          destinationOffset:6U*sizeof(std::uint32_t) size:36U*sizeof(std::uint32_t)];
    [audit endEncoding];[audit_command commit];[audit_command waitUntilCompleted];
    const auto* audit_words=static_cast<const std::uint32_t*>(stage_audit.contents);
    if(audit_command.status!=MTLCommandBufferStatusCompleted||audit_words==nullptr) {
      std::fprintf(stderr,"compact P8 fixture audit transfer failed after stage %u\n",stage);
      return false;
    }
    std::fprintf(stderr,"compact P8 stage %u: status=%u/%u candidate=%u/%u/%u grids=%u,%u,%u\n",
        stage,audit_words[0U],audit_words[1U],audit_words[2U],audit_words[3U],
        audit_words[4U],audit_words[6U],audit_words[10U],audit_words[34U]);
  }
  id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLBlitCommandEncoder> clear=[command blitCommandEncoder];
  for(id<MTLBuffer> b:{counts,offsets,block_totals,block_offsets,level_totals,level_offsets,signs,candidate,status,dispatches,retained})[clear fillBuffer:b range:NSMakeRange(0,b.length) value:0U];
  [clear endEncoding];
  if(!encode_metal_gpu_hierarchy_compact_owner_p8(command,p,owner,header,field,stencil,counts,offsets,block_totals,block_offsets,level_totals,level_offsets,signs,candidate,status,dispatches,retained,arguments,vertices,{},fp.source_revision))return false;
  [command commit];[command waitUntilCompleted];
  // Readback is deliberately a separate completed-command probe: it keeps
  // Metal's compute-to-private-front dependency distinct from the fixture's
  // diagnostic transfer and mirrors the live path's no-payload-readback rule.
  if(command.status!=MTLCommandBufferStatusCompleted) {
    std::fprintf(stderr,"compact P8 fixture generation command failed: status=%ld\n",
        static_cast<long>(command.status));return false;
  }
  id<MTLCommandBuffer> read_command=[queue commandBuffer];
  id<MTLBlitCommandEncoder> copy=[read_command blitCommandEncoder];
  [copy copyFromBuffer:retained sourceOffset:0 toBuffer:readback destinationOffset:0 size:readback.length];
  [copy copyFromBuffer:arguments sourceOffset:0 toBuffer:read_arguments destinationOffset:0 size:16U];
  [copy endEncoding];[read_command commit];[read_command waitUntilCompleted];
  const auto* words=static_cast<const std::uint32_t*>(readback.contents);const auto* args=static_cast<const std::uint32_t*>(read_arguments.contents);
  if(read_command.status!=MTLCommandBufferStatusCompleted||words==nullptr||args==nullptr||args[0]!=vertices||args[1]!=1U){
    std::fprintf(stderr,"compact P8 fixture commit failed: status=%ld args=%u,%u expected=%u error=%s\n",
        static_cast<long>(read_command.status),args==nullptr?0U:args[0U],args==nullptr?0U:args[1U],vertices,
        read_command.error==nil?"none":read_command.error.localizedDescription.UTF8String);return false;
  }
  constexpr std::array<std::array<std::uint32_t,3>,4> faces{{{{0U,1U,2U}},{{1U,3U,4U}},{{2U,4U,5U}},{{1U,4U,2U}}}};
  for(std::size_t triangle=0;triangle<expected.size();++triangle)for(std::size_t face=0;face<4;++face)for(std::size_t corner=0;corner<3;++corner){
    const auto base=(triangle*12U+face*3U+corner)*18U;const auto& point=expected[triangle].vertices[faces[face][corner]];
    for(std::size_t axis=0;axis<3;++axis){const float got=std::bit_cast<float>(words[base+axis]);const double want=axis==0?point.x:axis==1?point.y:point.z;if(!std::isfinite(got)||std::abs(double(got)-want)>2.e-3)return false;}
  }
  // The small root fixture is admitted by the 1,024-owner tier. Compare the
  // complete retained payload and published draw arguments to the full P8
  // route before exercising its fail-closed cases.
  std::memcpy(header.contents,owner_words.data(),sizeof(owner_words));
  id<MTLCommandBuffer> micro=[queue commandBuffer];
  if(!encode_metal_gpu_hierarchy_compact_owner_p8_hybrid(
       micro,p,owner,header,field,stencil,candidate,status,microbatch_copy_dispatch,triangle_dispatch,counts,offsets,signs,retained,arguments,
       vertices,{},fp.source_revision)) {
    std::fprintf(stderr,"compact P8 hybrid encoder rejected fixture\n");
    return false;
  }
  [micro commit];[micro waitUntilCompleted];
  if(micro.status!=MTLCommandBufferStatusCompleted) {
    std::fprintf(stderr,"compact P8 hybrid failed: status=%ld error=%s\n",
        static_cast<long>(micro.status),micro.error==nil?"none":
        micro.error.localizedDescription.UTF8String);
    return false;
  }
  id<MTLCommandBuffer> micro_read=[queue commandBuffer];
  id<MTLBlitCommandEncoder> micro_copy=[micro_read blitCommandEncoder];
  [micro_copy copyFromBuffer:retained sourceOffset:0U toBuffer:failed_readback
             destinationOffset:0U size:failed_readback.length];
  [micro_copy copyFromBuffer:arguments sourceOffset:0U toBuffer:failed_arguments
             destinationOffset:0U size:failed_arguments.length];
  [micro_copy endEncoding];[micro_read commit];[micro_read waitUntilCompleted];
  if(micro_read.status!=MTLCommandBufferStatusCompleted)return false;
  if(std::memcmp(readback.contents,failed_readback.contents,readback.length)!=0) {
    const auto* full=static_cast<const std::uint32_t*>(readback.contents);
    const auto* compact=static_cast<const std::uint32_t*>(failed_readback.contents);
    std::size_t first=0U;
    while(first<readback.length/sizeof(std::uint32_t)&&full[first]==compact[first])++first;
    std::fprintf(stderr,"compact P8 hybrid payload mismatch at word %zu: %u != %u\n",
        first,full[first],compact[first]);
    return false;
  }
  if(std::memcmp(read_arguments.contents,failed_arguments.contents,
                 read_arguments.length)!=0) {
    const auto* full=static_cast<const std::uint32_t*>(read_arguments.contents);
    const auto* compact=static_cast<const std::uint32_t*>(failed_arguments.contents);
    std::fprintf(stderr,"compact P8 hybrid arguments mismatch: %u/%u != %u/%u\n",
        full[0U],full[1U],compact[0U],compact[1U]);
    return false;
  }
  const auto retains_prior=[&](const std::array<std::uint32_t,4>& bad_header,
                               std::uint64_t source,std::uint32_t capacity)->bool {
    std::memcpy(header.contents,bad_header.data(),sizeof(bad_header));
    id<MTLCommandBuffer> failed=[queue commandBuffer];
    if(!encode_metal_gpu_hierarchy_compact_owner_p8(failed,p,owner,header,field,
         stencil,counts,offsets,block_totals,block_offsets,level_totals,
         level_offsets,signs,candidate,status,dispatches,retained,arguments,
         capacity,{},source))return false;
    [failed commit];[failed waitUntilCompleted];
    if(failed.status!=MTLCommandBufferStatusCompleted)return false;
    id<MTLCommandBuffer> audit=[queue commandBuffer];
    id<MTLBlitCommandEncoder> blit=[audit blitCommandEncoder];
    [blit copyFromBuffer:retained sourceOffset:0U toBuffer:failed_readback
          destinationOffset:0U size:failed_readback.length];
    [blit copyFromBuffer:arguments sourceOffset:0U toBuffer:failed_arguments
          destinationOffset:0U size:failed_arguments.length];
    [blit endEncoding];[audit commit];[audit waitUntilCompleted];
    return audit.status==MTLCommandBufferStatusCompleted&&
        std::memcmp(readback.contents,failed_readback.contents,readback.length)==0&&
        std::memcmp(read_arguments.contents,failed_arguments.contents,
                    read_arguments.length)==0;
  };
  const std::array<std::uint32_t,4> malformed_header{owners,owners,1U,1U};
  const std::array<std::uint32_t,4> capacity_header{owners,owners-1U,0U,1U};
  if(!retains_prior(malformed_header,fp.source_revision,vertices)||
     !retains_prior(capacity_header,fp.source_revision,vertices)||
     !retains_prior(owner_words,fp.source_revision+1U,vertices)||
     !retains_prior(owner_words,fp.source_revision,0U)) {
    std::fprintf(stderr,"compact P8 full route failed a retained-front check\n");
    return false;
  }
  const auto micro_retains_prior=[&](const std::array<std::uint32_t,4>& bad_header,
                                     std::uint64_t source,std::uint32_t capacity)->bool {
    std::memcpy(header.contents,bad_header.data(),sizeof(bad_header));
    id<MTLCommandBuffer> failed=[queue commandBuffer];
    if(!encode_metal_gpu_hierarchy_compact_owner_p8_hybrid(
         failed,p,owner,header,field,stencil,candidate,status,microbatch_copy_dispatch,triangle_dispatch,counts,offsets,signs,retained,arguments,
         capacity,{},source))return false;
    [failed commit];[failed waitUntilCompleted];
    if(failed.status!=MTLCommandBufferStatusCompleted)return false;
    id<MTLCommandBuffer> audit=[queue commandBuffer];
    id<MTLBlitCommandEncoder> blit=[audit blitCommandEncoder];
    [blit copyFromBuffer:retained sourceOffset:0U toBuffer:failed_readback
          destinationOffset:0U size:failed_readback.length];
    [blit copyFromBuffer:arguments sourceOffset:0U toBuffer:failed_arguments
          destinationOffset:0U size:failed_arguments.length];
    [blit endEncoding];[audit commit];[audit waitUntilCompleted];
    return audit.status==MTLCommandBufferStatusCompleted&&
        std::memcmp(readback.contents,failed_readback.contents,readback.length)==0&&
        std::memcmp(read_arguments.contents,failed_arguments.contents,
                    read_arguments.length)==0;
  };
  const std::array<std::uint32_t,4> oversized_header{1025U,1025U,0U,1U};
  const bool micro_malformed=micro_retains_prior(malformed_header,fp.source_revision,vertices);
  const bool micro_capacity=micro_retains_prior(capacity_header,fp.source_revision,vertices);
  const bool micro_stale=micro_retains_prior(owner_words,fp.source_revision+1U,vertices);
  const bool micro_zero_capacity=micro_retains_prior(owner_words,fp.source_revision,0U);
  const bool micro_oversized=micro_retains_prior(oversized_header,fp.source_revision,vertices);
  if(!micro_malformed||!micro_capacity||!micro_stale||!micro_zero_capacity||
     !micro_oversized) {
    std::fprintf(stderr,"compact P8 hybrid retained checks malformed=%d capacity=%d stale=%d zero=%d oversized=%d\n",
        micro_malformed,micro_capacity,micro_stale,micro_zero_capacity,micro_oversized);
    return false;
  }
  std::printf("{\"event\":\"metal_gpu_compact_owner_p8\",\"owners\":%u,\"vertices\":%u,\"private_commit\":true,\"hybrid_exact\":true,\"hybrid_retains\":true,\"hybrid_capacity_rejected\":true,\"passed\":true}\n",owners,vertices);
  return true;
}

// Regression fixture for P7e4a1's second scan level. A production compact
// list can exceed 256 blocks, while its input-derived dispatch is larger than
// the tiny superblock stream. Execute that larger grid deliberately and prove
// the private scan returns its exact sum without overrunning level storage.
bool run_metal_gpu_hierarchy_compact_red_scan_large_smoke_test(
    id<MTLDevice> device) {
  const auto path=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR)/
      "gpu_hierarchy_compact_red_scan.comp.metal";
  id<MTLLibrary> library=make_file_shader_library(device,path.string().c_str());
  NSError* error=nil;
  id<MTLComputePipelineState> pipeline=library==nil?nil:
      [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"main0"] error:&error];
  constexpr std::uint32_t count=66952U;
  constexpr std::uint32_t blocks=(count+255U)/256U;
  constexpr std::uint32_t levels=(blocks+255U)/256U;
  std::vector<std::uint32_t> counts(count);
  std::uint32_t expected{};
  for(std::uint32_t index=0U;index<count;++index) {
    counts[index]=(index%17U)==0U?8U:1U;
    expected+=counts[index];
  }
  const std::array<std::uint32_t,4> header{count,count,0U,0U};
  const auto shared=[&](const void* data,NSUInteger bytes){return [device newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];};
  const auto private_buffer=[&](NSUInteger bytes){return [device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];};
  id<MTLBuffer> active=shared(header.data(),sizeof(header)),values=shared(counts.data(),counts.size()*sizeof(std::uint32_t));
  id<MTLBuffer> offsets=private_buffer(count*sizeof(std::uint32_t)),block_totals=private_buffer(blocks*sizeof(std::uint32_t)),block_offsets=private_buffer(blocks*sizeof(std::uint32_t)),scan_total=private_buffer(sizeof(std::uint32_t)),level_totals=private_buffer(levels*sizeof(std::uint32_t)),level_offsets=private_buffer(levels*sizeof(std::uint32_t));
  id<MTLBuffer> audit=[device newBufferWithLength:3U*sizeof(std::uint32_t) options:MTLResourceStorageModeShared];
  id<MTLCommandQueue> queue=[device newCommandQueue];
  if(pipeline==nil||active==nil||values==nil||offsets==nil||block_totals==nil||block_offsets==nil||scan_total==nil||level_totals==nil||level_offsets==nil||audit==nil||queue==nil)return false;
  const auto encode=[&](id<MTLCommandBuffer> command,std::uint32_t phase,std::uint32_t groups){
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];[encoder setBuffer:active offset:0U atIndex:0U];[encoder setBytes:&phase length:sizeof(phase) atIndex:1U];[encoder setBuffer:values offset:0U atIndex:2U];[encoder setBuffer:offsets offset:0U atIndex:3U];[encoder setBuffer:block_totals offset:0U atIndex:4U];[encoder setBuffer:block_offsets offset:0U atIndex:5U];[encoder setBuffer:level_totals offset:0U atIndex:6U];[encoder setBuffer:level_offsets offset:0U atIndex:7U];[encoder setBuffer:scan_total offset:0U atIndex:8U];
    [encoder dispatchThreadgroups:MTLSizeMake(groups,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[encoder endEncoding];
  };
  id<MTLCommandBuffer> command=[queue commandBuffer];
  encode(command,0U,blocks);
  // Mimic the larger live input grid. Phase 1 itself must reject the surplus.
  encode(command,1U,blocks);encode(command,2U,1U);encode(command,3U,blocks);
  id<MTLBlitCommandEncoder> copy=[command blitCommandEncoder];
  [copy copyFromBuffer:scan_total sourceOffset:0U toBuffer:audit destinationOffset:0U size:sizeof(std::uint32_t)];
  [copy copyFromBuffer:level_totals sourceOffset:0U toBuffer:audit destinationOffset:sizeof(std::uint32_t) size:2U*sizeof(std::uint32_t)];
  [copy endEncoding];[command commit];[command waitUntilCompleted];
  const auto* words=static_cast<const std::uint32_t*>(audit.contents);
  const bool passed=command.status==MTLCommandBufferStatusCompleted&&words!=nullptr&&words[0U]==expected&&words[1U]+words[2U]==expected;
  std::printf("{\"event\":\"metal_gpu_compact_red_scan_large\",\"count\":%u,\"blocks\":%u,\"levels\":%u,\"total\":%u,\"expected\":%u,\"passed\":%s}\\n",count,blocks,levels,words==nullptr?0U:words[0U],expected,passed?"true":"false");
  return passed;
}

// P7e4a's live closure is a fixed, fully device-resident schedule.  The host
// does not inspect green-round or red-promotion counters: all bounded repair
// rounds execute in one command buffer, and the final phase poisons the
// closure header if any red work remains.  P8 consumes that header directly.
bool encode_metal_gpu_hierarchy_live_closure(
    id<MTLCommandBuffer> command,id<MTLComputePipelineState> frontier,
    id<MTLComputePipelineState> scan,id<MTLComputePipelineState> closure,
    id<MTLComputePipelineState> control,
    MetalGpuHierarchyLiveSelection& selection,bool inject_green_budget_failure) {
  if(!selection.closure_ready()||selection.closure_pending||frontier==nil||
     scan==nil||closure==nil||control==nil||selection.closure_slot_index>=selection.slots.size())
    return false;
  const auto records=selection.record_count;
  const auto blocks=(records+255U)/256U;
  constexpr NSUInteger repair_indirect_offset=0U;
  constexpr NSUInteger green_indirect_offset=4U*sizeof(std::uint32_t);
  const auto closure_slot=&selection.slots[selection.closure_slot_index];
  id<MTLBlitCommandEncoder> clear=[command blitCommandEncoder];
  for(id<MTLBuffer> buffer:{selection.closure_owners,selection.closure_counts,
      selection.closure_offsets,selection.closure_added_offsets,
      selection.closure_block_totals,selection.closure_block_offsets,
      selection.closure_scan_total,selection.closure_edge_marks,
      selection.closure_red_promotions,selection.closure_status,
      selection.closure_dispatch_args})
    [clear fillBuffer:buffer range:NSMakeRange(0U,buffer.length) value:0U];
  [clear endEncoding];
  std::memset(selection.closure_control_audit.contents,0,
      selection.closure_control_audit.length);
  const auto encode_control=[&](std::uint32_t action){
    const std::array<std::uint32_t,2> parameters{blocks,action};
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:control];
    // SPIRV-Cross orders the writable indirect arguments before parameters
    // and the read-only status sidecar; keep this in lockstep with MSL.
    [encoder setBuffer:selection.closure_dispatch_args offset:0U atIndex:0U];
    [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:1U];
    [encoder setBuffer:selection.closure_status offset:0U atIndex:2U];
    [encoder dispatchThreads:MTLSizeMake(1U,1U,1U)
         threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)]; [encoder endEncoding];
  };
  const std::array<std::uint32_t,5> frontier_parameters{
      records,selection.output_capacity,selection.mark_word_count,records,0U};
  const auto encode_frontier=[&](NSUInteger indirect_offset){
    auto parameters=frontier_parameters;
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:frontier];
    [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:0U];
    [encoder setBuffer:closure_slot->marks offset:0U atIndex:1U];
    [encoder setBuffer:selection.hierarchy offset:0U atIndex:2U];
    [encoder setBuffer:selection.closure_status offset:0U atIndex:3U];
    [encoder setBuffer:selection.closure_offsets offset:0U atIndex:4U];
    [encoder setBuffer:selection.closure_counts offset:0U atIndex:5U];
    [encoder setBuffer:selection.canonical offset:0U atIndex:6U];
    [encoder setBuffer:selection.closure_owners offset:0U atIndex:7U];
    [encoder dispatchThreadgroupsWithIndirectBuffer:selection.closure_dispatch_args
        indirectBufferOffset:indirect_offset
        threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[encoder endEncoding];
    const std::array<std::uint32_t,2> scan_parameters{records,0U};
    encoder=[command computeCommandEncoder];[encoder setComputePipelineState:scan];
    [encoder setBytes:scan_parameters.data() length:sizeof(scan_parameters) atIndex:0U];
    [encoder setBuffer:selection.closure_offsets offset:0U atIndex:1U];
    [encoder setBuffer:selection.closure_counts offset:0U atIndex:2U];
    [encoder setBuffer:selection.closure_block_totals offset:0U atIndex:3U];
    [encoder dispatchThreadgroupsWithIndirectBuffer:selection.closure_dispatch_args
        indirectBufferOffset:indirect_offset
        threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[encoder endEncoding];
    const std::array<std::uint32_t,2> block_parameters{blocks,0U};
    encoder=[command computeCommandEncoder];[encoder setComputePipelineState:scan];
    [encoder setBytes:block_parameters.data() length:sizeof(block_parameters) atIndex:0U];
    [encoder setBuffer:selection.closure_block_offsets offset:0U atIndex:1U];
    [encoder setBuffer:selection.closure_block_totals offset:0U atIndex:2U];
    [encoder setBuffer:selection.closure_scan_total offset:0U atIndex:3U];
    [encoder dispatchThreadgroupsWithIndirectBuffer:selection.closure_dispatch_args
        indirectBufferOffset:indirect_offset
        threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[encoder endEncoding];
    const std::array<std::uint32_t,2> add_parameters{records,1U};
    encoder=[command computeCommandEncoder];[encoder setComputePipelineState:scan];
    [encoder setBytes:add_parameters.data() length:sizeof(add_parameters) atIndex:0U];
    [encoder setBuffer:selection.closure_added_offsets offset:0U atIndex:1U];
    [encoder setBuffer:selection.closure_offsets offset:0U atIndex:2U];
    [encoder setBuffer:selection.closure_block_offsets offset:0U atIndex:3U];
    [encoder dispatchThreadgroupsWithIndirectBuffer:selection.closure_dispatch_args
        indirectBufferOffset:indirect_offset
        threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[encoder endEncoding];
    parameters[4]=1U;encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:frontier];
    [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:0U];
    [encoder setBuffer:closure_slot->marks offset:0U atIndex:1U];
    [encoder setBuffer:selection.hierarchy offset:0U atIndex:2U];
    [encoder setBuffer:selection.closure_status offset:0U atIndex:3U];
    [encoder setBuffer:selection.closure_added_offsets offset:0U atIndex:4U];
    [encoder setBuffer:selection.closure_counts offset:0U atIndex:5U];
    [encoder setBuffer:selection.canonical offset:0U atIndex:6U];
    [encoder setBuffer:selection.closure_owners offset:0U atIndex:7U];
    [encoder dispatchThreadgroupsWithIndirectBuffer:selection.closure_dispatch_args
        indirectBufferOffset:indirect_offset
        threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];[encoder endEncoding];
  };
  const std::array<std::uint32_t,12> closure_parameters{records,
      selection.output_capacity,selection.mark_word_count,records,
      static_cast<std::uint32_t>(selection.edge_ranges.length/sizeof(tetra::GpuHierarchyEdgeRange)),
      static_cast<std::uint32_t>(selection.ancestor_edge_ranges.length/sizeof(std::uint32_t)),0U,
      static_cast<std::uint32_t>(selection.children.length/sizeof(std::uint32_t)),
      static_cast<std::uint32_t>(selection.vertex_ranges.length/sizeof(tetra::GpuHierarchyVertexRange)),
      static_cast<std::uint32_t>(selection.vertex_incidence.length/sizeof(tetra::GpuHierarchyVertexIncidence)),0U,0U};
  const auto encode_closure=[&](std::uint32_t phase,NSUInteger indirect_offset){
    auto parameters=closure_parameters;parameters[6]=phase;
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:closure];
    [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:0U];
    [encoder setBuffer:closure_slot->marks offset:0U atIndex:1U];
    [encoder setBuffer:selection.closure_status offset:0U atIndex:2U];
    [encoder setBuffer:selection.hierarchy offset:0U atIndex:3U];
    [encoder setBuffer:selection.face_incidence offset:0U atIndex:4U];
    [encoder setBuffer:selection.edge_topology offset:0U atIndex:5U];
    [encoder setBuffer:selection.vertex_topology offset:0U atIndex:6U];
    [encoder setBuffer:selection.vertex_ranges offset:0U atIndex:7U];
    [encoder setBuffer:selection.vertex_incidence offset:0U atIndex:8U];
    [encoder setBuffer:selection.closure_edge_marks offset:0U atIndex:9U];
    [encoder setBuffer:selection.canonical offset:0U atIndex:10U];
    // SPIRV-Cross orders these four active SSBOs by their first use in the
    // closure shader: red promotions precede counts/orientation/ancestors.
    // Keep this explicit Metal ABI in lockstep with the generated signature.
    [encoder setBuffer:selection.closure_red_promotions offset:0U atIndex:11U];
    [encoder setBuffer:selection.closure_counts offset:0U atIndex:12U];
    [encoder setBuffer:selection.orientations offset:0U atIndex:13U];
    [encoder setBuffer:selection.ancestor_edge_ranges offset:0U atIndex:14U];
    [encoder setBuffer:selection.children offset:0U atIndex:15U];
    [encoder setBuffer:selection.closure_added_offsets offset:0U atIndex:16U];
    [encoder setBuffer:selection.closure_owners offset:0U atIndex:17U];
    [encoder dispatchThreadgroupsWithIndirectBuffer:selection.closure_dispatch_args
        indirectBufferOffset:indirect_offset
        threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[encoder endEncoding];
  };
  // This is a live rendering gate, not the exhaustive P7e3c fixture: bound
  // its command-buffer footprint deliberately.  Any cut needing more work
  // fails device-only in phase 8 (green) or the control red-budget check and P8 retains the
  // complete bootstrap front.  The fixture below still exhaustively proves
  // the 48-round/max-depth closure schedule.
  constexpr std::uint32_t live_repair_rounds=1U;
  constexpr std::uint32_t live_green_rounds=1U;
  const auto green_rounds=inject_green_budget_failure?0U:live_green_rounds;
  encode_control(0U);
  for(std::uint32_t repair=0U;repair<live_repair_rounds;++repair){
    clear=[command blitCommandEncoder];
    [clear fillBuffer:selection.closure_edge_marks range:NSMakeRange(0U,selection.closure_edge_marks.length) value:0U];
    [clear fillBuffer:selection.closure_red_promotions range:NSMakeRange(0U,selection.closure_red_promotions.length) value:0U];
    // Preserve emitted-owner count (word 3): once a no-red repair has
    // produced the final stream, subsequent device-zeroed repair iterations
    // must not erase P8's private header.
    [clear fillBuffer:selection.closure_status range:NSMakeRange(sizeof(std::uint32_t),2U*sizeof(std::uint32_t)) value:0U];
    [clear fillBuffer:selection.closure_status range:NSMakeRange(4U*sizeof(std::uint32_t),sizeof(std::uint32_t)) value:0U];
    [clear endEncoding];
    encode_frontier(repair_indirect_offset);
    encode_closure(0U,repair_indirect_offset);
    encode_control(1U);
    for(std::uint32_t green=0U;green<green_rounds;++green){
      clear=[command blitCommandEncoder];
      [clear fillBuffer:selection.closure_status range:NSMakeRange(sizeof(std::uint32_t),sizeof(std::uint32_t)) value:0U];[clear endEncoding];
      encode_closure(1U,green_indirect_offset);
      encode_control(2U);
    }
    encode_closure(8U,repair_indirect_offset);
    encode_closure(2U,repair_indirect_offset);
    encode_closure(6U,repair_indirect_offset);
    encode_closure(4U,repair_indirect_offset);
    encode_closure(5U,repair_indirect_offset);
    encode_control(3U);
  }
  // The last active repair already compacted the exact selected marks.  This
  // scalar device control is the live red-budget terminal: it preserves that
  // first no-red owner stream, or latches failure before P8 can consume it.
  encode_control(6U);
  clear=[command blitCommandEncoder];
  [clear copyFromBuffer:selection.closure_dispatch_args sourceOffset:0U
      toBuffer:selection.closure_control_audit destinationOffset:0U
      size:selection.closure_control_audit.length];
  [clear endEncoding];
  selection.closure_completed->store(false,std::memory_order_release);
  selection.closure_pending=true;
  const auto completed=selection.closure_completed;
  [command addCompletedHandler:^(id<MTLCommandBuffer> finished){
    completed->store(finished.status==MTLCommandBufferStatusCompleted,
                     std::memory_order_release);
  }];
  return true;
}

bool encode_metal_gpu_hierarchy_live_selection(
    id<MTLCommandBuffer> command,id<MTLComputePipelineState> pipeline,
    id<MTLComputePipelineState> compact_worklist,
    MetalGpuHierarchyLiveSelection& selection,
    const tetra::GpuHierarchySelectionTuple& tuple) {
  if(!selection.ready()||pipeline==nil)return false;
  const auto source_revision=static_cast<std::uint64_t>(tuple.revision_lanes[0])|
      (static_cast<std::uint64_t>(tuple.revision_lanes[1])<<32U);
  const auto field_revision=static_cast<std::uint64_t>(tuple.revision_lanes[2])|
      (static_cast<std::uint64_t>(tuple.revision_lanes[3])<<32U);
  if(source_revision!=selection.source_revision||field_revision!=selection.field_revision){
    ++selection.stale_rejected;
    return false;
  }
  MetalGpuHierarchyLiveSelectionSlot* selected=nullptr;
  for(std::size_t attempt=0U;attempt<selection.slots.size();++attempt){
    auto& candidate=selection.slots[(selection.cursor+attempt)%selection.slots.size()];
    if(!candidate.pending){selected=&candidate;selection.cursor+=attempt+1U;break;}
  }
  if(selected==nullptr)return false;
  std::memcpy(selected->tuple.contents,&tuple,sizeof(tuple));
  id<MTLBlitCommandEncoder> clear=[command blitCommandEncoder];
  [clear fillBuffer:selected->marks range:NSMakeRange(0U,selected->marks.length)
               value:0U];
  [clear endEncoding];
  const std::array<std::uint32_t,5> parameters{selection.record_count,
      selection.output_capacity,selection.mark_word_count,selection.root_count,
      selection.require_complete_front?1U:0U};
  id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
  [encoder setComputePipelineState:pipeline];
  [encoder setBuffer:selection.hierarchy offset:0U atIndex:0U];
  [encoder setBuffer:selection.children offset:0U atIndex:1U];
  [encoder setBuffer:selection.inputs offset:0U atIndex:2U];
  [encoder setBuffer:selected->tuple offset:0U atIndex:3U];
  [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:4U];
  [encoder setBuffer:selection.roots offset:0U atIndex:5U];
  [encoder setBuffer:selected->marks offset:0U atIndex:6U];
  // The root-index list is immutable snapshot metadata and contains no more
  // than the twelve BCC roots. It replaces the former record-count grid.
  [encoder dispatchThreads:MTLSizeMake(selection.root_count,1U,1U)
       threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
  [encoder endEncoding];
  if(compact_worklist!=nil) {
    if(!selection.compact_worklist_ready())return false;
    // P7e4a1's vertical seed: arm a private indirect grid from P7e2's
    // appended count, then copy that many entries into the distinct compact
    // active list. No host observes or supplies the selected count.
    const std::array<std::uint32_t,5> worklist_arm{selection.record_count,
        selection.output_capacity,selection.output_capacity,0U,0U};
    encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:compact_worklist];
    [encoder setBuffer:selection.compact_dispatch_args offset:0U atIndex:0U];
    [encoder setBytes:worklist_arm.data() length:sizeof(worklist_arm) atIndex:1U];
    [encoder setBuffer:selected->marks offset:0U atIndex:2U];
    [encoder setBuffer:selection.compact_selected_ping offset:0U atIndex:3U];
    [encoder dispatchThreads:MTLSizeMake(1U,1U,1U)
         threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
    [encoder endEncoding];
    auto worklist_copy=worklist_arm;worklist_copy[3U]=1U;
    encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:compact_worklist];
    [encoder setBuffer:selection.compact_dispatch_args offset:0U atIndex:0U];
    [encoder setBytes:worklist_copy.data() length:sizeof(worklist_copy) atIndex:1U];
    [encoder setBuffer:selected->marks offset:0U atIndex:2U];
    [encoder setBuffer:selection.compact_selected_ping offset:0U atIndex:3U];
    [encoder dispatchThreadgroupsWithIndirectBuffer:selection.compact_dispatch_args
        indirectBufferOffset:0U threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];
    [encoder endEncoding];
  }
  selected->tuple_identity=tetra::gpu_hierarchy_selection_tuple_identity(tuple);
  selection.closure_slot_index=static_cast<std::uint32_t>(
      selected-selection.slots.data());
  selected->completed->store(false,std::memory_order_release);
  selected->succeeded->store(false,std::memory_order_release);
  selected->pending=true;
  const auto completed=selected->completed,success=selected->succeeded;
  [command addCompletedHandler:^(id<MTLCommandBuffer> finished){
    success->store(finished.status==MTLCommandBufferStatusCompleted,
                   std::memory_order_release);
    completed->store(true,std::memory_order_release);
  }];
  ++selection.submitted;
  selection.device_front_selector_at=std::chrono::steady_clock::now();
  return true;
}

struct alignas(16) MetalGpuTerrainGeometryParameters {
  std::uint32_t count{},capacity{},reserved0{},reserved1{};
  std::array<float,4> origin{};
  std::uint32_t source_low{},source_high{},padding0{},padding1{};
};
static_assert(sizeof(MetalGpuTerrainGeometryParameters)==48U);

void promote_completed_terrain_acceleration_structure(
    MetalTerrainAccelerationStructure& structures) {
  if(structures.pending==nil||structures.pending_generation==0U||
     structures.completed_generation->load(std::memory_order_acquire)!=
         structures.pending_generation)return;
  structures.active=structures.pending;
  structures.active_vertices=structures.pending_vertices;
  structures.active_exact_indices=structures.pending_exact_indices;
  structures.active_preview_vertices=structures.pending_preview_vertices;
  structures.active_preview_indices=structures.pending_preview_indices;
  structures.active_generation=structures.pending_generation;
  structures.pending=nil;
  structures.pending_vertices=nil;
  structures.pending_exact_indices=nil;
  structures.pending_preview_vertices=nil;
  structures.pending_preview_indices=nil;
  structures.pending_scratch=nil;
  structures.pending_generation=0U;
}

bool encode_terrain_acceleration_structure_build(
    id<MTLDevice> device,id<MTLCommandBuffer> command,
    MetalTerrainAccelerationStructure& structures,id<MTLBuffer> vertices,
    std::size_t vertex_count,bool indexed_exact_selection,
    id<MTLBuffer> exact_indices,
    std::size_t exact_index_count,id<MTLBuffer> preview_vertices,
    std::size_t preview_vertex_count,id<MTLBuffer> preview_indices,
    std::size_t preview_index_count,std::uint64_t generation,
    id<MTLCounterSampleBuffer> timestamp_samples,bool& build_encoded) {
  build_encoded=false;
  promote_completed_terrain_acceleration_structure(structures);
  const bool exact_ready=vertices!=nil&&vertex_count!=0U&&vertex_count%3U==0U;
  const bool preview_ready=preview_vertices!=nil&&preview_vertex_count!=0U&&
      preview_indices!=nil&&preview_index_count!=0U&&preview_index_count%3U==0U;
  if((!exact_ready&&!preview_ready)||generation==0U||
     (indexed_exact_selection&&exact_index_count%3U!=0U)||
     (indexed_exact_selection&&exact_index_count!=0U&&exact_indices==nil)||
     (preview_vertices!=nil&&
      (preview_vertex_count==0U||preview_indices==nil||
       preview_index_count%3U!=0U)))
    return false;
  if(structures.active_generation==generation)return true;
  if(structures.pending!=nil)return false;
  NSMutableArray<MTLAccelerationStructureGeometryDescriptor*>* geometries=
      [NSMutableArray array];
  if(!indexed_exact_selection||exact_index_count!=0U){
    MTLAccelerationStructureTriangleGeometryDescriptor* exact_geometry=
        [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
    exact_geometry.vertexBuffer=vertices;
    exact_geometry.vertexFormat=MTLAttributeFormatFloat3;
    exact_geometry.vertexStride=sizeof(tetra_viewer::SceneVertex);
    exact_geometry.triangleCount=indexed_exact_selection?
        exact_index_count/3U:vertex_count/3U;
    if(indexed_exact_selection){
      exact_geometry.indexBuffer=exact_indices;
      exact_geometry.indexType=MTLIndexTypeUInt32;
    }
    [geometries addObject:exact_geometry];
  }
  MTLAccelerationStructureTriangleGeometryDescriptor* preview_geometry=nil;
  if(preview_vertices!=nil){
    preview_geometry=[MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
    preview_geometry.vertexBuffer=preview_vertices;
    preview_geometry.vertexFormat=MTLAttributeFormatFloat3;
    preview_geometry.vertexStride=sizeof(tetra_viewer::SceneVertex);
    preview_geometry.indexBuffer=preview_indices;
    preview_geometry.indexType=MTLIndexTypeUInt32;
    preview_geometry.triangleCount=preview_index_count/3U;
    [geometries addObject:preview_geometry];
  }
  if(geometries.count==0U)return false;
  MTLPrimitiveAccelerationStructureDescriptor* descriptor=
      [MTLPrimitiveAccelerationStructureDescriptor descriptor];
  descriptor.geometryDescriptors=geometries;
  const auto sizes=[device accelerationStructureSizesWithDescriptor:descriptor];
  id<MTLAccelerationStructure> candidate=
      [device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
  id<MTLBuffer> scratch=[device newBufferWithLength:sizes.buildScratchBufferSize
      options:MTLResourceStorageModePrivate];
  if(candidate==nil||scratch==nil)return false;
  id<MTLAccelerationStructureCommandEncoder> encoder=nil;
  if(timestamp_samples!=nil){
    if(@available(macOS 13.0,*)){
      MTLAccelerationStructurePassDescriptor* pass=
          [MTLAccelerationStructurePassDescriptor
              accelerationStructurePassDescriptor];
      auto* attachment=pass.sampleBufferAttachments[0];
      attachment.sampleBuffer=timestamp_samples;
      attachment.startOfEncoderSampleIndex=15U;
      attachment.endOfEncoderSampleIndex=16U;
      encoder=[command accelerationStructureCommandEncoderWithDescriptor:pass];
    }
  }
  if(encoder==nil)encoder=[command accelerationStructureCommandEncoder];
  [encoder buildAccelerationStructure:candidate descriptor:descriptor
                       scratchBuffer:scratch scratchBufferOffset:0U];
  [encoder endEncoding];
  structures.pending=candidate;
  structures.pending_vertices=vertices;
  structures.pending_exact_indices=exact_indices;
  structures.pending_preview_vertices=preview_vertices;
  structures.pending_preview_indices=preview_indices;
  structures.pending_scratch=scratch;
  structures.pending_generation=generation;
  structures.resident_bytes=sizes.accelerationStructureSize;
  ++structures.build_count;
  build_encoded=true;
  structures.last_build_timing_valid->store(false,std::memory_order_relaxed);
  const auto completed=structures.completed_generation;
  [command addCompletedHandler:^(id<MTLCommandBuffer> finished){
    if(finished.status==MTLCommandBufferStatusCompleted){
      completed->store(generation,std::memory_order_release);
    }
  }];
  return true;
}

id<MTLLibrary> make_file_shader_library(id<MTLDevice> device,
                                        const char* path) {
  std::ifstream input(path,std::ios::binary);
  if(!input){
    std::fprintf(stderr,"Metal shader source is missing: %s\n",path);
    return nil;
  }
  const std::string contents{
      std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
  NSError* error=nil;
  MTLCompileOptions* options=[MTLCompileOptions new];
  options.languageVersion=MTLLanguageVersion2_4;
  id<MTLLibrary> library=[device newLibraryWithSource:
      [NSString stringWithUTF8String:contents.c_str()]
                                             options:options error:&error];
  if(library==nil)
    std::fprintf(stderr,"Metal shader compilation failed for %s: %s\n",path,
                 error.localizedDescription.UTF8String);
  return library;
}

// P4c2's backend qualification fixture deliberately constructs the same
// immutable BCC snapshot and exact float tuple consumed by the Vulkan path.
// It does not touch the interactive terrain front: Metal output is read back
// only here and compared to the shared CPU shader-ABI oracle.
bool run_metal_gpu_lod_selector_smoke_test(id<MTLDevice> device) {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  for(unsigned int generation=0U;generation<3U;++generation)
    mesh.refine_all_binary();
  std::vector<tetra::WorldTetAddress> owners;
  for(const auto owner:mesh.logical_red_owners())
    owners.push_back(tetra::world_tet_address(owner));
  const tetra::WorldCutDirectory directory(
      tetra::make_sparse_world_cut_checkpoint(
          owners,1U,41U,tetra::HierarchyResidencyTier::surface));
  const auto snapshot=tetra::make_gpu_hierarchy_snapshot(directory,43U);
  try { tetra::validate_gpu_hierarchy_snapshot(snapshot); }
  catch(const std::exception& error) {
    std::fprintf(stderr,"Metal GPU LOD fixture is invalid: %s\n",error.what());
    return false;
  }
  if(snapshot.records.empty()||snapshot.selection_records.size()!=
     snapshot.records.size())return false;

  const auto shader_path=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR)/
      "gpu_lod.comp.metal";
  id<MTLLibrary> library=make_file_shader_library(
      device,shader_path.string().c_str());
  id<MTLFunction> function=[library newFunctionWithName:@"main0"];
  NSError* error=nil;
  id<MTLComputePipelineState> pipeline=function==nil?nil:
      [device newComputePipelineStateWithFunction:function error:&error];
  if(pipeline==nil){
    std::fprintf(stderr,"Metal GPU LOD pipeline creation failed: %s\n",
        error==nil?"missing translated entry point":
        error.localizedDescription.UTF8String);
    return false;
  }
  const auto worklist_path=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR)/
      "gpu_hierarchy_compact_worklist.comp.metal";
  id<MTLLibrary> worklist_library=make_file_shader_library(
      device,worklist_path.string().c_str());
  id<MTLComputePipelineState> worklist=worklist_library==nil?nil:
      [device newComputePipelineStateWithFunction:
          [worklist_library newFunctionWithName:@"main0"] error:&error];
  if(worklist==nil)return false;
  const auto canonicalize_path=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR)/
      "gpu_hierarchy_canonicalize.comp.metal";
  id<MTLLibrary> canonicalize_library=make_file_shader_library(
      device,canonicalize_path.string().c_str());
  id<MTLComputePipelineState> canonicalize=canonicalize_library==nil?nil:
      [device newComputePipelineStateWithFunction:
          [canonicalize_library newFunctionWithName:@"main0"] error:&error];
  if(canonicalize==nil)return false;
  const auto make_buffer=[&](const void* bytes,NSUInteger length){
    return [device newBufferWithBytes:bytes length:length
        options:MTLResourceStorageModeShared];
  };
  id<MTLBuffer> hierarchy=make_buffer(snapshot.records.data(),
      snapshot.records.size()*sizeof(snapshot.records.front()));
  id<MTLBuffer> children=make_buffer(snapshot.child_indices.data(),
      snapshot.child_indices.size()*sizeof(snapshot.child_indices.front()));
  id<MTLBuffer> inputs=make_buffer(snapshot.selection_records.data(),
      snapshot.selection_records.size()*sizeof(snapshot.selection_records.front()));
  std::vector<std::uint32_t> root_indices;
  for(std::uint32_t index=0U;index<snapshot.records.size();++index)
    if((snapshot.records[index].child_mask_flags&0x800U)!=0U)
      root_indices.push_back(index);
  id<MTLBuffer> roots=root_indices.empty()||root_indices.size()>12U?nil:
      make_buffer(root_indices.data(),root_indices.size()*sizeof(std::uint32_t));
  if(hierarchy==nil||children==nil||inputs==nil||roots==nil)return false;
  id<MTLCommandQueue> queue=[device newCommandQueue];
  if(queue==nil)return false;

  constexpr std::uint32_t output_capacity=65536U;
  const auto make_tuple=[](tetra::Vec3 position,tetra::Vec3 forward,
                           float edge,float field,float limb){
    tetra::GpuHierarchySelectionTupleParameters p;
    p.camera.position=position;p.camera.forward=forward;
    p.camera.up={0.0,1.0,0.0};p.camera.viewport_height_pixels=800.0;
    p.camera.aspect_ratio=1.0;p.render_origin={};p.field_centre={0.5,0.5,0.5};
    p.planet_radius=2.0;p.terrain_height_bound=0.1;p.field_lipschitz=1.0;
    p.edge_threshold=edge;p.field_threshold=field;p.limb_threshold=limb;
    p.merge_ratio=0.5;p.source_revision=41U;p.field_revision=43U;
    return tetra::make_gpu_hierarchy_selection_tuple(p);
  };
  struct SelectorCase {
    const char* name;
    tetra::Vec3 position,forward;
    float edge,field,limb;
    std::uint32_t capacity;
  };
  constexpr std::array cases{
      SelectorCase{"fixed",{0.5,0.5,3.0},{0.0,0.0,-1.0},
                   1.0e6F,1.0e6F,1.0e6F,output_capacity},
      SelectorCase{"walk",{0.7,0.5,2.8},{0.0,0.0,-1.0},
                   0.5F,1.0e6F,1.0e6F,output_capacity},
      SelectorCase{"orbit",{3.0,0.5,0.5},{-1.0,0.0,0.0},
                   1.0e6F,0.05F,1.0e6F,output_capacity},
      SelectorCase{"limb",{0.5,0.5,3.0},{0.0,0.0,-1.0},
                   1.0e6F,1.0e6F,0.02F,output_capacity},
      // A full selector result with zero writable entries is the hardware
      // overflow/fail-closed contract; the interactive Metal renderer still
      // draws its CPU front because this diagnostic can never be promoted.
      SelectorCase{"overflow",{0.5,0.5,3.0},{0.0,0.0,-1.0},
                   1.0e6F,1.0e6F,1.0e6F,0U}};
  std::size_t completed{};
  for(const auto& selector_case:cases){
    const auto tuple=make_tuple(selector_case.position,selector_case.forward,
                                selector_case.edge,selector_case.field,
                                selector_case.limb);
    const auto oracle=tetra::gpu_hierarchy_traverse(snapshot,
        tetra::gpu_hierarchy_traversal_parameters(tuple));
    if(oracle.selected_records.size()>output_capacity)return false;
    id<MTLBuffer> tuple_buffer=make_buffer(&tuple,sizeof(tuple));
    const std::size_t mark_word_count=(snapshot.records.size()+31U)/32U;
    std::vector<std::uint32_t> zeroed(4U+selector_case.capacity+
                                      mark_word_count,0U);
    id<MTLBuffer> output=make_buffer(zeroed.data(),
        zeroed.size()*sizeof(zeroed.front()));
    const std::array<std::uint32_t,4> parameters{
        static_cast<std::uint32_t>(snapshot.records.size()),selector_case.capacity,
        static_cast<std::uint32_t>(mark_word_count),
        static_cast<std::uint32_t>(root_indices.size())};
    if(tuple_buffer==nil||output==nil)return false;
    id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:hierarchy offset:0U atIndex:0U];
    [encoder setBuffer:children offset:0U atIndex:1U];
    [encoder setBuffer:inputs offset:0U atIndex:2U];
    [encoder setBuffer:tuple_buffer offset:0U atIndex:3U];
    [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:4U];
    [encoder setBuffer:roots offset:0U atIndex:5U];
    [encoder setBuffer:output offset:0U atIndex:6U];
    [encoder dispatchThreads:MTLSizeMake(root_indices.size(),1U,1U)
         threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
    [encoder endEncoding];[command commit];[command waitUntilCompleted];
    if(command.status!=MTLCommandBufferStatusCompleted){std::fprintf(stderr,"Metal terrain project command status %ld\n",static_cast<long>(command.status));return false;}
    const auto* words=static_cast<const std::uint32_t*>(output.contents);
    const std::uint32_t count=words[0];
    if(count!=oracle.selected_records.size()||
       words[1]!=oracle.metrics.visited||words[2]!=oracle.metrics.frustum_rejected)
      return false;
    const bool expects_overflow=selector_case.capacity==0U;
    if((words[3]!=0U)!=expects_overflow||
       (!expects_overflow&&count>selector_case.capacity))return false;
    if(expects_overflow){ ++completed; continue; }
    std::vector<std::uint32_t> device(words+4U,words+4U+count);
    auto canonical=[](std::vector<std::uint32_t> values){
      std::ranges::sort(values);return values;
    };
    if(canonical(std::move(device))!=canonical(oracle.selected_records))return false;
    std::vector<std::uint32_t> marked;
    for(std::size_t word=0U;word<mark_word_count;++word){
      std::uint32_t bits=words[4U+selector_case.capacity+word];
      while(bits!=0U){
        const auto lane=static_cast<std::uint32_t>(std::countr_zero(bits));
        marked.push_back(static_cast<std::uint32_t>(word*32U+lane));
        bits&=bits-1U;
      }
    }
    if(canonical(std::move(marked))!=canonical(oracle.selected_records))return false;
    // P7e4a1's vertical compact-list seed consumes P7e2's append stream
    // exactly as written, but derives its dispatch grid from that private
    // count. Fixture memory is shared solely to prove the ABI; the live route
    // keeps both buffers private and performs no count/payload readback.
    std::vector<std::uint32_t> compact_words(4U+selector_case.capacity,0U);
    std::array<std::uint32_t,4> dispatch_words{99U,99U,99U,99U};
    id<MTLBuffer> compact=make_buffer(compact_words.data(),
        compact_words.size()*sizeof(std::uint32_t));
    id<MTLBuffer> dispatch_args=make_buffer(dispatch_words.data(),
        sizeof(dispatch_words));
    if(compact==nil||dispatch_args==nil)return false;
    std::array<std::uint32_t,5> worklist_parameters{
        static_cast<std::uint32_t>(snapshot.records.size()),selector_case.capacity,
        selector_case.capacity,0U,0U};
    id<MTLCommandBuffer> worklist_command=[queue commandBuffer];
    id<MTLComputeCommandEncoder> worklist_encoder=[worklist_command computeCommandEncoder];
    [worklist_encoder setComputePipelineState:worklist];
    [worklist_encoder setBuffer:dispatch_args offset:0U atIndex:0U];
    [worklist_encoder setBytes:worklist_parameters.data()
                  length:sizeof(worklist_parameters) atIndex:1U];
    [worklist_encoder setBuffer:output offset:0U atIndex:2U];
    [worklist_encoder setBuffer:compact offset:0U atIndex:3U];
    [worklist_encoder dispatchThreads:MTLSizeMake(1U,1U,1U)
          threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
    [worklist_encoder endEncoding];
    worklist_parameters[3U]=1U;
    worklist_encoder=[worklist_command computeCommandEncoder];
    [worklist_encoder setComputePipelineState:worklist];
    [worklist_encoder setBuffer:dispatch_args offset:0U atIndex:0U];
    [worklist_encoder setBytes:worklist_parameters.data()
                  length:sizeof(worklist_parameters) atIndex:1U];
    [worklist_encoder setBuffer:output offset:0U atIndex:2U];
    [worklist_encoder setBuffer:compact offset:0U atIndex:3U];
    [worklist_encoder dispatchThreadgroupsWithIndirectBuffer:dispatch_args
        indirectBufferOffset:0U threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];
    [worklist_encoder endEncoding];[worklist_command commit];
    [worklist_command waitUntilCompleted];
    const auto* compact_result=static_cast<const std::uint32_t*>(compact.contents);
    const auto* indirect_result=static_cast<const std::uint32_t*>(dispatch_args.contents);
    if(worklist_command.status!=MTLCommandBufferStatusCompleted||
       compact_result==nullptr||indirect_result==nullptr||
       compact_result[0U]!=count||compact_result[1U]!=selector_case.capacity||
       compact_result[2U]!=0U||indirect_result[0U]!=(count+255U)/256U||
       indirect_result[1U]!=1U||indirect_result[2U]!=1U||
       !std::equal(device.begin(),device.end(),compact_result+4U))return false;
    // Canonical ranks are immutable snapshot metadata. Five stable radix
    // passes turn the nondeterministic root-append order into the exact
    // address-canonical owner order required before ping/pong closure.
    std::vector<std::uint32_t> ranks(snapshot.records.size());
    for(std::uint32_t rank=0U;rank<snapshot.canonical_record_indices.size();++rank)
      ranks[snapshot.canonical_record_indices[rank]]=rank;
    const auto groups=(count+255U)/256U;
    std::vector<std::uint32_t> histogram(std::max<std::uint32_t>(groups*16U,1U));
    std::vector<std::uint32_t> histogram_offsets(histogram.size());
    std::array<std::uint32_t,16> bin_bases{};
    std::vector<std::uint32_t> canonical_words(4U+selector_case.capacity,0U);
    id<MTLBuffer> rank_buffer=make_buffer(ranks.data(),ranks.size()*sizeof(std::uint32_t));
    id<MTLBuffer> histogram_buffer=make_buffer(histogram.data(),histogram.size()*sizeof(std::uint32_t));
    id<MTLBuffer> histogram_offsets_buffer=make_buffer(histogram_offsets.data(),histogram_offsets.size()*sizeof(std::uint32_t));
    id<MTLBuffer> bin_bases_buffer=make_buffer(bin_bases.data(),sizeof(bin_bases));
    id<MTLBuffer> canonical_buffer=make_buffer(canonical_words.data(),canonical_words.size()*sizeof(std::uint32_t));
    if(rank_buffer==nil||histogram_buffer==nil||histogram_offsets_buffer==nil||
       bin_bases_buffer==nil||canonical_buffer==nil)return false;
    id<MTLBuffer> canonical_input=compact,canonical_output=canonical_buffer;
    id<MTLCommandBuffer> canonical_command=[queue commandBuffer];
    for(std::uint32_t shift=0U;shift<20U;shift+=4U) {
      const auto encode_canonical=[&](std::uint32_t phase,bool indirect){
        const std::array<std::uint32_t,4> parameters{
            static_cast<std::uint32_t>(snapshot.records.size()),phase,shift,0U};
        id<MTLComputeCommandEncoder> e=[canonical_command computeCommandEncoder];
        [e setComputePipelineState:canonicalize];
        // Generated MSL ABI: gate/input/parameters/output/ranks/histogram/bases/offsets.
        [e setBuffer:dispatch_args offset:0U atIndex:0U];
        [e setBuffer:canonical_input offset:0U atIndex:1U];
        [e setBytes:parameters.data() length:sizeof(parameters) atIndex:2U];
        [e setBuffer:canonical_output offset:0U atIndex:3U];
        [e setBuffer:rank_buffer offset:0U atIndex:4U];
        [e setBuffer:histogram_buffer offset:0U atIndex:5U];
        [e setBuffer:bin_bases_buffer offset:0U atIndex:6U];
        [e setBuffer:histogram_offsets_buffer offset:0U atIndex:7U];
        if(indirect)[e dispatchThreadgroupsWithIndirectBuffer:dispatch_args
            indirectBufferOffset:0U threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];
        else [e dispatchThreads:MTLSizeMake(1U,1U,1U)
            threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
        [e endEncoding];
      };
      encode_canonical(0U,true);encode_canonical(1U,false);encode_canonical(2U,true);
      std::swap(canonical_input,canonical_output);
    }
    [canonical_command commit];[canonical_command waitUntilCompleted];
    const auto* canonical_result=static_cast<const std::uint32_t*>(canonical_input.contents);
    std::vector<std::uint32_t> expected=oracle.selected_records;
    std::ranges::sort(expected,{},[&](std::uint32_t record){return ranks[record];});
    if(canonical_command.status!=MTLCommandBufferStatusCompleted||canonical_result==nullptr||
       canonical_result[0U]!=count||canonical_result[2U]!=0U||
       !std::equal(expected.begin(),expected.end(),canonical_result+4U)) {
      std::fprintf(stderr,"Metal compact canonicalize failed: status=%ld count=%u/%u failure=%u first=%u expected=%u\n",
          static_cast<long>(canonical_command.status),canonical_result==nullptr?0U:canonical_result[0U],count,
          canonical_result==nullptr?0U:canonical_result[2U],canonical_result==nullptr?0U:canonical_result[4U],
          expected.empty()?0U:expected.front());
      return false;
    }
    std::vector<std::uint32_t> oversized_words(4U+selector_case.capacity,0U);
    id<MTLBuffer> oversized=make_buffer(oversized_words.data(),
        oversized_words.size()*sizeof(std::uint32_t));
    if(oversized==nil)return false;
    const std::array<std::uint32_t,4> oversized_parameters{
        (1U<<20U)+1U,0U,0U,0U};
    id<MTLCommandBuffer> oversized_command=[queue commandBuffer];
    id<MTLComputeCommandEncoder> oversized_encoder=[oversized_command computeCommandEncoder];
    [oversized_encoder setComputePipelineState:canonicalize];
    [oversized_encoder setBuffer:dispatch_args offset:0U atIndex:0U];
    [oversized_encoder setBuffer:compact offset:0U atIndex:1U];
    [oversized_encoder setBytes:oversized_parameters.data()
                        length:sizeof(oversized_parameters) atIndex:2U];
    [oversized_encoder setBuffer:oversized offset:0U atIndex:3U];
    [oversized_encoder setBuffer:rank_buffer offset:0U atIndex:4U];
    [oversized_encoder setBuffer:histogram_buffer offset:0U atIndex:5U];
    [oversized_encoder setBuffer:bin_bases_buffer offset:0U atIndex:6U];
    [oversized_encoder setBuffer:histogram_offsets_buffer offset:0U atIndex:7U];
    [oversized_encoder dispatchThreadgroupsWithIndirectBuffer:dispatch_args
        indirectBufferOffset:0U threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];
    [oversized_encoder endEncoding];[oversized_command commit];[oversized_command waitUntilCompleted];
    const auto* oversized_result=static_cast<const std::uint32_t*>(oversized.contents);
    if(oversized_command.status!=MTLCommandBufferStatusCompleted||
       oversized_result==nullptr||oversized_result[2U]!=16U)return false;
    ++completed;
  }
  // Fail-closed worklist ABI coverage: a bad append header arms no indirect
  // work; a malformed record is detected by the indirect copy and retires its
  // grid before any later consumer can run.
  const auto worklist_failure=[&](std::vector<std::uint32_t> source,
                                  std::uint32_t selection_capacity,
                                  std::uint32_t active_capacity,
                                  std::uint32_t expected_failure){
    source.resize(std::max<std::size_t>(source.size(),4U),0U);
    std::vector<std::uint32_t> destination(
        4U+std::max<std::uint32_t>(active_capacity,1U),0U);
    std::array<std::uint32_t,4> args{99U,99U,99U,99U};
    id<MTLBuffer> source_buffer=make_buffer(source.data(),
        source.size()*sizeof(std::uint32_t));
    id<MTLBuffer> destination_buffer=make_buffer(destination.data(),
        destination.size()*sizeof(std::uint32_t));
    id<MTLBuffer> args_buffer=make_buffer(args.data(),sizeof(args));
    if(source_buffer==nil||destination_buffer==nil||args_buffer==nil)return false;
    std::array<std::uint32_t,5> parameters{
        static_cast<std::uint32_t>(snapshot.records.size()),selection_capacity,
        active_capacity,0U,0U};
    id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:worklist];[encoder setBuffer:args_buffer offset:0U atIndex:0U];
    [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:1U];
    [encoder setBuffer:source_buffer offset:0U atIndex:2U];
    [encoder setBuffer:destination_buffer offset:0U atIndex:3U];
    [encoder dispatchThreads:MTLSizeMake(1U,1U,1U)
         threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];[encoder endEncoding];
    parameters[3U]=1U;encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:worklist];[encoder setBuffer:args_buffer offset:0U atIndex:0U];
    [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:1U];
    [encoder setBuffer:source_buffer offset:0U atIndex:2U];
    [encoder setBuffer:destination_buffer offset:0U atIndex:3U];
    [encoder dispatchThreadgroupsWithIndirectBuffer:args_buffer indirectBufferOffset:0U
        threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[encoder endEncoding];
    [command commit];[command waitUntilCompleted];
    const auto* header=static_cast<const std::uint32_t*>(destination_buffer.contents);
    const auto* grid=static_cast<const std::uint32_t*>(args_buffer.contents);
    return command.status==MTLCommandBufferStatusCompleted&&header!=nullptr&&
        grid!=nullptr&&header[2U]==expected_failure&&grid[0U]==0U&&
        grid[1U]==1U&&grid[2U]==1U;
  };
  if(!worklist_failure({0U,0U,0U,0U},1U,1U,2U)||
     !worklist_failure({2U,0U,0U,0U,0U,1U},1U,1U,1U)||
     !worklist_failure({1U,0U,0U,0U,
                         static_cast<std::uint32_t>(snapshot.records.size())},
                        1U,1U,4U))return false;
  std::printf("{\"event\":\"metal_gpu_lod_selector\","
              "\"cases\":%zu,\"records\":%zu,\"passed\":true}\n",
              completed,snapshot.records.size());
  return true;
}

// P7e2 exercises the production-state lifetime separately from the readback
// parity fixture above.  It proves immutable buffers survive camera tuples,
// a source revision replaces every old flight, and a stale tuple cannot be
// encoded into the current mark front.
bool run_metal_gpu_live_selection_state_smoke_test(id<MTLDevice> device) {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  for(unsigned int generation=0U;generation<3U;++generation)
    mesh.refine_all_binary();
  std::vector<tetra::WorldTetAddress> owners;
  for(const auto owner:mesh.logical_red_owners())
    owners.push_back(tetra::world_tet_address(owner));
  const auto snapshot_for=[&](std::uint64_t source,std::uint64_t field){
    const tetra::WorldCutDirectory directory(
        tetra::make_sparse_world_cut_checkpoint(
            owners,1U,source,tetra::HierarchyResidencyTier::surface));
    return tetra::make_gpu_hierarchy_snapshot(directory,field);
  };
  const auto first=snapshot_for(41U,43U);
  const auto second=snapshot_for(47U,53U);
  const auto shader_path=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR)/
      "gpu_lod.comp.metal";
  id<MTLLibrary> library=make_file_shader_library(device,shader_path.string().c_str());
  NSError* error=nil;
  id<MTLComputePipelineState> pipeline=library==nil?nil:
      [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"main0"]
                                             error:&error];
  id<MTLCommandQueue> queue=[device newCommandQueue];
  if(pipeline==nil||queue==nil)return false;
  MetalGpuHierarchyLiveSelection selection;
  if(!configure_metal_gpu_hierarchy_live_selection(device,selection,first,43U,7U))
    return false;
  const auto first_hierarchy=selection.hierarchy;
  const auto first_parents=selection.parents;
  const auto first_faces=selection.face_incidence;
  const auto first_edges=selection.edge_topology;
  if(first_parents.length!=first.parent_records.size()*sizeof(std::uint32_t)||
     first_faces.length!=first.face_incidence.size()*sizeof(first.face_incidence.front())||
     std::memcmp(first_parents.contents,first.parent_records.data(),
                 first_parents.length)!=0||
     std::memcmp(first_faces.contents,first.face_incidence.data(),
                 first_faces.length)!=0||first_edges.length!=first.edge_topology.size()*sizeof(first.edge_topology.front())||
     std::memcmp(first_edges.contents,first.edge_topology.data(),first_edges.length)!=0)return false;
  const auto tuple_for=[](std::uint64_t source,std::uint64_t field,
                          tetra::Vec3 position){
    tetra::Camera camera;
    camera.position=position;camera.forward={0.0,0.0,-1.0};
    camera.up={0.0,1.0,0.0};camera.viewport_height_pixels=800.0;
    camera.aspect_ratio=1.0;
    return tetra::make_gpu_hierarchy_selection_tuple({
        .camera=camera,.render_origin={},.field_centre={0.5,0.5,0.5},
        .planet_radius=2.0,.terrain_height_bound=0.1,.field_lipschitz=1.0,
        .edge_threshold=0.5,.field_threshold=0.05,.limb_threshold=0.02,
        .merge_ratio=0.5,.source_revision=source,.field_revision=field});
  };
  const auto dispatch=[&](const tetra::GpuHierarchySelectionTuple& tuple){
    id<MTLCommandBuffer> command=[queue commandBuffer];
    if(!encode_metal_gpu_hierarchy_live_selection(command,pipeline,nil,selection,tuple))
      return false;
    [command commit];[command waitUntilCompleted];
    retire_metal_gpu_hierarchy_live_selection(selection);
    return command.status==MTLCommandBufferStatusCompleted;
  };
  if(!dispatch(tuple_for(41U,43U,{0.5,0.5,3.0}))||
     !configure_metal_gpu_hierarchy_live_selection(device,selection,first,43U,7U)||
     selection.hierarchy!=first_hierarchy||selection.parents!=first_parents||
     selection.face_incidence!=first_faces||selection.edge_topology!=first_edges||
     !dispatch(tuple_for(41U,43U,{0.7,0.5,2.8})))return false;
  if(encode_metal_gpu_hierarchy_live_selection([queue commandBuffer],pipeline,nil,
      selection,tuple_for(47U,43U,{0.5,0.5,3.0}))||
     selection.stale_rejected!=1U)return false;
  if(!configure_metal_gpu_hierarchy_live_selection(device,selection,second,53U,9U)||
     selection.source_revision!=47U||selection.field_revision!=53U||
     selection.bootstrap_scene_generation!=9U||selection.submitted!=0U||
     selection.parents==first_parents||selection.face_incidence==first_faces||selection.edge_topology==first_edges||
     !dispatch(tuple_for(47U,53U,{1.1,0.5,2.6})))return false;
  const bool passed=selection.accepted==1U&&selection.completed==1U&&
      selection.failed==0U&&selection.stale_rejected==0U;
  std::printf("{\"event\":\"metal_gpu_live_selection_state\","
              "\"revision_replaced\":true,\"persistent\":true,"
              "\"accepted\":%llu,\"failed\":%llu,\"passed\":%s}\n",
      static_cast<unsigned long long>(selection.accepted),
      static_cast<unsigned long long>(selection.failed),passed?"true":"false");
  return passed;
}

// Isolate the production encoder from renderer/world bootstrap timing.  This
// builds one modest immutable snapshot, runs the exact live selector and
// compact closure command buffer, then reads completion latches only.
bool run_metal_gpu_compact_live_closure_smoke_test(id<MTLDevice> device) {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  for(unsigned generation=0U;generation<3U;++generation)mesh.refine_all_binary();
  std::vector<tetra::WorldTetAddress> owners;
  for(const auto owner:mesh.logical_red_owners())owners.push_back(tetra::world_tet_address(owner));
  const tetra::WorldCutDirectory directory(tetra::make_sparse_world_cut_checkpoint(
      owners,1U,79U,tetra::HierarchyResidencyTier::surface));
  const auto snapshot=tetra::make_gpu_hierarchy_snapshot(directory,83U);
  const auto pipeline_for=[&](const char* name)->id<MTLComputePipelineState>{
    const auto path=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR)/name;
    id<MTLLibrary> library=make_file_shader_library(device,path.string().c_str());
    NSError* error=nil;
    return library==nil?nil:[device newComputePipelineStateWithFunction:
        [library newFunctionWithName:@"main0"] error:&error];
  };
  id<MTLComputePipelineState> selector=pipeline_for("gpu_lod.comp.metal");
  id<MTLComputePipelineState> worklist=pipeline_for("gpu_hierarchy_compact_worklist.comp.metal");
  id<MTLComputePipelineState> canonicalize=pipeline_for("gpu_hierarchy_canonicalize.comp.metal");
  id<MTLComputePipelineState> green=pipeline_for("gpu_hierarchy_compact_green_closure.comp.metal");
  id<MTLComputePipelineState> red=pipeline_for("gpu_hierarchy_compact_red_repair.comp.metal");
  id<MTLComputePipelineState> scan=pipeline_for("gpu_hierarchy_compact_red_scan.comp.metal");
  id<MTLComputePipelineState> materialize=pipeline_for("gpu_hierarchy_compact_owner_materialize.comp.metal");
  MetalCompactOwnerP8Pipelines p8{pipeline_for("gpu_terrain_compact_owner_control.comp.metal"),
      pipeline_for("gpu_terrain_compact_owner_count.comp.metal"),pipeline_for("gpu_terrain_compact_owner_scan.comp.metal"),
      pipeline_for("gpu_terrain_compact_owner_emit.comp.metal"),pipeline_for("gpu_terrain_compact_owner_triangle_emit.comp.metal"),pipeline_for("gpu_terrain_compact_owner_validate.comp.metal"),
      pipeline_for("gpu_terrain_compact_owner_copy.comp.metal"),pipeline_for("gpu_terrain_compact_owner_publish.comp.metal"),
      pipeline_for("gpu_terrain_compact_owner_microbatch.comp.metal"),
      pipeline_for("gpu_terrain_compact_owner_microbatch_validate.comp.metal"),
      pipeline_for("gpu_terrain_compact_owner_hybrid_scan.comp.metal"),
      pipeline_for("gpu_terrain_compact_owner_hybrid_finalize.comp.metal")};
  id<MTLCommandQueue> queue=[device newCommandQueue];
  if(selector==nil||worklist==nil||canonicalize==nil||green==nil||red==nil||scan==nil||materialize==nil||
     p8.control==nil||p8.count==nil||p8.scan==nil||p8.emit==nil||p8.triangle_emit==nil||p8.validate==nil||p8.copy==nil||p8.publish==nil||p8.hybrid_scan==nil||p8.hybrid_finalize==nil||queue==nil)return false;
  MetalGpuHierarchyLiveSelection selection;
  if(!configure_metal_gpu_hierarchy_live_selection(device,selection,snapshot,83U,11U))return false;
  tetra::Camera camera;
  camera.position={0.5,0.5,3.0};camera.forward={0.0,0.0,-1.0};camera.up={0.0,1.0,0.0};
  camera.viewport_height_pixels=800.0;camera.aspect_ratio=1.0;
  const auto tuple=tetra::make_gpu_hierarchy_selection_tuple({
      .camera=camera,.render_origin={},.field_centre={0.5,0.5,0.5},
      .planet_radius=2.0,.terrain_height_bound=0.1,.field_lipschitz=1.0,
      .edge_threshold=0.5,.field_threshold=0.05,.limb_threshold=0.02,
      .merge_ratio=0.5,.source_revision=79U,.field_revision=83U});
  tetra::GpuTerrainFieldTupleParameters terrain_parameters;
  terrain_parameters.source_revision=79U;terrain_parameters.field_revision=83U;
  terrain_parameters.domain.world_extent=1.0;terrain_parameters.field.kind=tetra::ImplicitShapeKind::perlin_terrain;
  terrain_parameters.field.centre={.5,.52,.5};terrain_parameters.field.radius=.37;
  terrain_parameters.field.terrain.planet_radius=.37;
  const auto terrain_tuple=tetra::make_gpu_terrain_field_tuple(terrain_parameters);
  const auto templates=tetra::make_gpu_green_template_table();
  selection.compact_p8_field=[device newBufferWithBytes:&terrain_tuple length:sizeof(terrain_tuple)
      options:MTLResourceStorageModeShared];
  selection.compact_p8_templates=[device newBufferWithBytes:templates.data() length:sizeof(templates)
      options:MTLResourceStorageModeShared];
  constexpr std::uint32_t p8_vertex_capacity=65536U;
  id<MTLBuffer> retained=[device newBufferWithLength:p8_vertex_capacity*
      sizeof(tetra_viewer::SceneVertex) options:MTLResourceStorageModePrivate];
  id<MTLBuffer> arguments=[device newBufferWithLength:4U*sizeof(std::uint32_t)
      options:MTLResourceStorageModePrivate];
  if(retained==nil||arguments==nil||
     !ensure_metal_gpu_hierarchy_compact_p8_workspace(device,selection,p8_vertex_capacity))
    return false;
  id<MTLCommandBuffer> command=[queue commandBuffer];
  if(!encode_metal_gpu_hierarchy_live_selection(command,selector,worklist,selection,tuple)||
     !encode_metal_gpu_hierarchy_live_compact_closure(command,canonicalize,green,red,
         scan,selection,false)||
     !encode_metal_gpu_hierarchy_compact_owner_materialize(command,materialize,selection)||
     !encode_metal_gpu_hierarchy_compact_owner_p8(command,p8,
         selection.compact_owner_stream,selection.compact_owner_header,
         selection.compact_p8_field,selection.compact_p8_templates,
         selection.compact_p8_counts,selection.compact_p8_offsets,
         selection.compact_p8_block_totals,selection.compact_p8_block_offsets,
         selection.compact_p8_level_totals,selection.compact_p8_level_offsets,
         selection.compact_p8_signs,selection.compact_p8_candidate,
         selection.compact_p8_status,selection.compact_p8_dispatches,
         retained,arguments,p8_vertex_capacity,{},79U))return false;
  // Retire this only with the closure command that produced its owner
  // header.  The audit below is scalar-only: status plus candidate header,
  // never compact-owner or vertex payload.
  ++selection.compact_p8_encoded;
  id<MTLBlitCommandEncoder> p8_audit=[command blitCommandEncoder];
  [p8_audit copyFromBuffer:selection.compact_p8_status sourceOffset:0U
       toBuffer:selection.compact_p8_audit destinationOffset:0U
           size:2U*sizeof(std::uint32_t)];
  [p8_audit copyFromBuffer:selection.compact_p8_candidate sourceOffset:0U
       toBuffer:selection.compact_p8_audit destinationOffset:2U*sizeof(std::uint32_t)
           size:4U*sizeof(std::uint32_t)];
  [p8_audit copyFromBuffer:selection.compact_owner_header sourceOffset:0U
       toBuffer:selection.compact_p8_audit destinationOffset:6U*sizeof(std::uint32_t)
           size:4U*sizeof(std::uint32_t)];
  [p8_audit endEncoding];
  [command commit];[command waitUntilCompleted];
  retire_metal_gpu_hierarchy_live_selection(selection);
  const bool passed=command.status==MTLCommandBufferStatusCompleted&&
      selection.submitted==1U&&selection.completed==1U&&selection.accepted==1U&&
      selection.compact_closure_encoded==1U&&selection.compact_red_encoded==8U&&
      selection.compact_closure_completed==1U&&selection.compact_quiescent==1U&&
      selection.compact_closure_rejected==0U&&
      selection.compact_p8_encoded==1U&&selection.compact_p8_completed==1U&&
      selection.compact_p8_private_commits==1U&&selection.compact_p8_rejected==0U&&
      static_cast<const std::uint32_t*>(selection.compact_p8_audit.contents)[0U]==1U&&
      static_cast<const std::uint32_t*>(selection.compact_p8_audit.contents)[1U]==0U&&
      static_cast<const std::uint32_t*>(selection.compact_p8_audit.contents)[2U]!=0U&&
      static_cast<const std::uint32_t*>(selection.compact_p8_audit.contents)[3U]==0U;
  std::printf("{\"event\":\"metal_gpu_compact_live_closure\","
              "\"bootstrap_records\":%u,\"selector_submitted\":%llu,"
              "\"compact_encoded\":%llu,\"compact_completed\":%llu,"
              "\"direct_red_rounds\":%llu,\"final_latched\":%llu,"
              "\"p8_owner_materialization\":%s,\"p8_encoded\":%llu,"
              "\"p8_completed\":%llu,\"p8_rejected\":%llu,"
              "\"p8_private_commit\":%s,\"passed\":%s}\n",
      selection.record_count,static_cast<unsigned long long>(selection.submitted),
      static_cast<unsigned long long>(selection.compact_closure_encoded),
      static_cast<unsigned long long>(selection.compact_closure_completed),
      static_cast<unsigned long long>(selection.compact_red_encoded),
      static_cast<unsigned long long>(selection.compact_quiescent),
      selection.compact_p8_encoded!=0U?"true":"false",
      static_cast<unsigned long long>(selection.compact_p8_encoded),
      static_cast<unsigned long long>(selection.compact_p8_completed),
      static_cast<unsigned long long>(selection.compact_p8_rejected),
      selection.compact_p8_private_commits!=0U?"true":"false",
      passed?"true":"false");
  return passed;
}

// Qualification-only end-to-end oracle for the real compact device front.
// Unlike the scalar live-closure smoke above, this deliberately reads the
// completed private front in a separate command buffer and compares it to an
// independently closed CPU cut.  Nothing in the interactive route calls this
// function or allocates its payload readback buffers.
bool run_metal_gpu_compact_live_parity_smoke_test(id<MTLDevice> device,
                                                  bool run_p95_benchmark=false,
                                                  bool use_hybrid=false) {
  const auto shader_directory=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR);
  const auto pipeline_for=[&](const char* name)->id<MTLComputePipelineState>{
    id<MTLLibrary> library=make_file_shader_library(device,(shader_directory/name).string().c_str());
    NSError* error=nil;
    return library==nil?nil:[device newComputePipelineStateWithFunction:
        [library newFunctionWithName:@"main0"] error:&error];
  };
  id<MTLComputePipelineState> selector=pipeline_for("gpu_lod.comp.metal");
  id<MTLComputePipelineState> worklist=pipeline_for("gpu_hierarchy_compact_worklist.comp.metal");
  id<MTLComputePipelineState> canonicalize=pipeline_for("gpu_hierarchy_canonicalize.comp.metal");
  id<MTLComputePipelineState> green=pipeline_for("gpu_hierarchy_compact_green_closure.comp.metal");
  id<MTLComputePipelineState> red=pipeline_for("gpu_hierarchy_compact_red_repair.comp.metal");
  id<MTLComputePipelineState> scan=pipeline_for("gpu_hierarchy_compact_red_scan.comp.metal");
  id<MTLComputePipelineState> materialize=pipeline_for("gpu_hierarchy_compact_owner_materialize.comp.metal");
  id<MTLComputePipelineState> owner_trace=pipeline_for("gpu_terrain_compact_owner_trace.comp.metal");
  MetalCompactOwnerP8Pipelines p8{pipeline_for("gpu_terrain_compact_owner_control.comp.metal"),
      pipeline_for("gpu_terrain_compact_owner_count.comp.metal"),pipeline_for("gpu_terrain_compact_owner_scan.comp.metal"),
      pipeline_for("gpu_terrain_compact_owner_emit.comp.metal"),pipeline_for("gpu_terrain_compact_owner_triangle_emit.comp.metal"),pipeline_for("gpu_terrain_compact_owner_validate.comp.metal"),
      pipeline_for("gpu_terrain_compact_owner_copy.comp.metal"),pipeline_for("gpu_terrain_compact_owner_publish.comp.metal"),
      pipeline_for("gpu_terrain_compact_owner_microbatch.comp.metal"),
      pipeline_for("gpu_terrain_compact_owner_microbatch_validate.comp.metal"),
      pipeline_for("gpu_terrain_compact_owner_hybrid_scan.comp.metal"),
      pipeline_for("gpu_terrain_compact_owner_hybrid_finalize.comp.metal")};
  id<MTLCommandQueue> queue=[device newCommandQueue];
  if(selector==nil||worklist==nil||canonicalize==nil||green==nil||red==nil||
     scan==nil||materialize==nil||owner_trace==nil||p8.control==nil||p8.count==nil||p8.scan==nil||
     p8.emit==nil||p8.validate==nil||p8.copy==nil||p8.publish==nil||
     (use_hybrid&&(p8.triangle_emit==nil||p8.hybrid_scan==nil||p8.hybrid_finalize==nil))||queue==nil)
    return false;

  // This source is intentionally mixed depth.  The first root is split once
  // after a uniform two-level refinement, exercising root seams and the
  // closure's mixed-depth repair rather than a uniform leaf-only shortcut.
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  for(unsigned generation=0U;generation<2U;++generation)mesh.refine_all_binary();
  if(mesh.logical_red_owners().empty()||
     !mesh.refine_selected_binary({mesh.logical_red_owners().front()}))return false;
  std::vector<tetra::WorldTetAddress> owners;
  for(const auto owner:mesh.logical_red_owners())owners.push_back(tetra::world_tet_address(owner));
  constexpr std::uint64_t source_revision=719U,field_revision=83U;
  const tetra::WorldCutDirectory directory(tetra::make_sparse_world_cut_checkpoint(
      owners,1U,source_revision,tetra::HierarchyResidencyTier::surface));
  const auto snapshot=tetra::make_gpu_hierarchy_snapshot(directory,field_revision);
  try { tetra::validate_gpu_hierarchy_snapshot(snapshot); }
  catch(const std::exception& error) {
    std::fprintf(stderr,"compact live parity fixture snapshot is invalid: %s\n",error.what());
    return false;
  }
  MetalGpuHierarchyLiveSelection selection;
  if(!configure_metal_gpu_hierarchy_live_selection(device,selection,snapshot,
      field_revision,17U))return false;
  tetra::GpuTerrainFieldTupleParameters field_parameters;
  field_parameters.source_revision=source_revision;field_parameters.field_revision=field_revision;
  field_parameters.domain.world_extent=1.0;field_parameters.field.kind=tetra::ImplicitShapeKind::perlin_terrain;
  field_parameters.field.centre={.5,.52,.5};field_parameters.field.radius=.37;
  field_parameters.field.terrain.planet_radius=.37;
  const auto field_tuple=tetra::make_gpu_terrain_field_tuple(field_parameters);
  const auto templates=tetra::make_gpu_green_template_table();
  selection.compact_p8_field=[device newBufferWithBytes:&field_tuple length:sizeof(field_tuple)
      options:MTLResourceStorageModeShared];
  selection.compact_p8_templates=[device newBufferWithBytes:templates.data() length:sizeof(templates)
      options:MTLResourceStorageModeShared];
  constexpr std::uint32_t vertex_capacity=262144U;
  id<MTLBuffer> retained=[device newBufferWithLength:std::size_t(vertex_capacity)*18U*sizeof(std::uint32_t)
      options:MTLResourceStorageModePrivate];
  id<MTLBuffer> arguments=[device newBufferWithLength:4U*sizeof(std::uint32_t)
      options:MTLResourceStorageModePrivate];
  // These two shared buffers are test-oracle-only.  The successful command
  // has completed before the blit, so they cannot accidentally become a live
  // candidate transport path.
  id<MTLBuffer> retained_readback=[device newBufferWithLength:retained.length
      options:MTLResourceStorageModeShared];
  id<MTLBuffer> arguments_readback=[device newBufferWithLength:arguments.length
      options:MTLResourceStorageModeShared];
  id<MTLBuffer> candidate_readback=nil;
  id<MTLBuffer> owner_readback=[device newBufferWithLength:
      std::size_t(selection.output_capacity)*12U*sizeof(std::uint32_t)
      options:MTLResourceStorageModeShared];
  id<MTLBuffer> owner_header_readback=[device newBufferWithLength:4U*sizeof(std::uint32_t)
      options:MTLResourceStorageModeShared];
  // P8's per-owner count/offset is read only by this qualification fixture,
  // after completion.  It lets the diagnostic associate a retained vertex
  // mismatch with one canonical compact owner without making that payload a
  // live transport path.
  id<MTLBuffer> count_readback=[device newBufferWithLength:
      std::size_t(selection.output_capacity)*sizeof(std::uint32_t)
      options:MTLResourceStorageModeShared];
  id<MTLBuffer> offset_readback=[device newBufferWithLength:
      std::size_t(selection.output_capacity)*sizeof(std::uint32_t)
      options:MTLResourceStorageModeShared];
  // 24 header/owner/geometry words plus 24 fixed-size cell records.  This is
  // a deliberately test-only one-owner trace, never an input to rendering.
  id<MTLBuffer> owner_trace_readback=[device newBufferWithLength:1024U*sizeof(std::uint32_t)
      options:MTLResourceStorageModeShared];
  if(selection.compact_p8_field==nil||selection.compact_p8_templates==nil||retained==nil||
     arguments==nil||retained_readback==nil||arguments_readback==nil||owner_readback==nil||
     owner_header_readback==nil||count_readback==nil||offset_readback==nil||
     owner_trace_readback==nil||
     !ensure_metal_gpu_hierarchy_compact_p8_workspace(device,selection,vertex_capacity))return false;
  candidate_readback=[device newBufferWithLength:selection.compact_p8_candidate.length
      options:MTLResourceStorageModeShared];
  if(candidate_readback==nil)return false;

  const auto make_tuple=[](tetra::Vec3 position,tetra::Vec3 origin,
                           std::uint64_t tuple_field_revision) {
    tetra::Camera camera;
    camera.position=position;camera.forward={0.0,0.0,-1.0};camera.up={0.0,1.0,0.0};
    camera.viewport_height_pixels=800.0;camera.aspect_ratio=1.0;
    return tetra::make_gpu_hierarchy_selection_tuple({.camera=camera,.render_origin=origin,
        .field_centre={.5,.5,.5},.planet_radius=2.0,.terrain_height_bound=.1,
        .field_lipschitz=1.0,.edge_threshold=.5,.field_threshold=.05,
        .limb_threshold=.02,.merge_ratio=.5,.source_revision=source_revision,
        .field_revision=tuple_field_revision});
  };
  const auto oracle_geometry=[&](const tetra::GpuHierarchySelectionTuple& tuple,
                                 const tetra::GpuTerrainFieldTuple& terrain,
                                 tetra::Vec3 origin,
                                 std::vector<tetra::GpuTerrainProjectedTriangleRecord>& geometry) {
    const auto traversal=tetra::gpu_hierarchy_traverse(snapshot,
        tetra::gpu_hierarchy_traversal_parameters(tuple));
    std::vector<tetra::WorldTetAddress> candidates;
    candidates.reserve(traversal.selected_records.size());
    for(const auto record:traversal.selected_records) {
      if(record>=snapshot.records.size())return false;
      candidates.push_back(tetra::gpu_hierarchy_address_from_lanes(snapshot.records[record].address));
    }
    std::ranges::sort(candidates);
    if(candidates.empty()||std::ranges::adjacent_find(candidates)!=candidates.end())return false;
    const auto packet=tetra::make_gpu_green_mask_packet(candidates,source_revision);
    auto roots=tetra::gpu_terrain_root_packet(packet,terrain,vertex_capacity);
    auto base=tetra::gpu_terrain_base_triangles(roots,vertex_capacity);
    for(auto& triangle:base)for(auto& root:triangle.roots) {
      root.x=static_cast<float>(root.x);root.y=static_cast<float>(root.y);root.z=static_cast<float>(root.z);
    }
    geometry=tetra::gpu_terrain_project_base_triangles(base,
        tetra::gpu_terrain_field_tuple_sphere(terrain),origin,vertex_capacity);
    return !geometry.empty()&&geometry.size()<=vertex_capacity/12U;
  };
  const auto read_retained=[&]() {
    id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLBlitCommandEncoder> copy=[command blitCommandEncoder];
    [copy copyFromBuffer:retained sourceOffset:0U toBuffer:retained_readback
        destinationOffset:0U size:retained.length];
    [copy copyFromBuffer:arguments sourceOffset:0U toBuffer:arguments_readback
        destinationOffset:0U size:arguments.length];
    [copy copyFromBuffer:selection.compact_p8_candidate sourceOffset:0U toBuffer:candidate_readback
        destinationOffset:0U size:candidate_readback.length];
    [copy endEncoding];[command commit];[command waitUntilCompleted];
    return command.status==MTLCommandBufferStatusCompleted;
  };
  const auto read_owners=[&]() {
    id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLBlitCommandEncoder> copy=[command blitCommandEncoder];
    [copy copyFromBuffer:selection.compact_owner_header sourceOffset:0U
        toBuffer:owner_header_readback destinationOffset:0U size:owner_header_readback.length];
    [copy copyFromBuffer:selection.compact_owner_stream sourceOffset:0U
        toBuffer:owner_readback destinationOffset:0U size:owner_readback.length];
    [copy copyFromBuffer:selection.compact_p8_counts sourceOffset:0U
        toBuffer:count_readback destinationOffset:0U size:count_readback.length];
    [copy copyFromBuffer:selection.compact_p8_offsets sourceOffset:0U
        toBuffer:offset_readback destinationOffset:0U size:offset_readback.length];
    [copy endEncoding];[command commit];[command waitUntilCompleted];
    return command.status==MTLCommandBufferStatusCompleted;
  };
  const auto compare_owners=[&](const tetra::GpuHierarchySelectionTuple& tuple) {
    const auto traversal=tetra::gpu_hierarchy_traverse(snapshot,
        tetra::gpu_hierarchy_traversal_parameters(tuple));
    std::vector<tetra::WorldTetAddress> candidates;
    for(const auto record:traversal.selected_records)
      candidates.push_back(tetra::gpu_hierarchy_address_from_lanes(snapshot.records[record].address));
    std::ranges::sort(candidates);
    const auto packet=tetra::make_gpu_green_mask_packet(candidates,source_revision);
    const auto* header=static_cast<const std::uint32_t*>(owner_header_readback.contents);
    const auto* words=static_cast<const std::uint32_t*>(owner_readback.contents);
    if(header==nullptr||words==nullptr||header[0U]!=packet.owners.size()||header[2U]!=0U||header[3U]!=1U) {
      std::fprintf(stderr,"compact live owner header mismatch: got=%u/%u/%u/%u expected=%zu\n",
          header==nullptr?0U:header[0U],header==nullptr?0U:header[1U],
          header==nullptr?0U:header[2U],header==nullptr?0U:header[3U],packet.owners.size());
      return false;
    }
    // The six edge lanes intentionally name different immutable directories
    // on the two routes (P6's packet edge table vs compact snapshot ranges).
    // They are not geometry identity. Address, green mask, and orientation
    // are the independently comparable closed-owner contract.
    std::vector<std::array<std::uint32_t,6U>> expected,actual;
    expected.reserve(packet.owners.size());actual.reserve(packet.owners.size());
    for(const auto& owner:packet.owners) {
      std::array<std::uint32_t,6U> value{};
      std::copy(owner.address.begin(),owner.address.end(),value.begin());
      value[4U]=owner.mask;value[5U]=owner.reflected_orientation;expected.push_back(value);
    }
    for(std::size_t index=0U;index<packet.owners.size();++index) {
      std::array<std::uint32_t,6U> value{};
      std::copy_n(words+index*12U,4U,value.begin());
      value[4U]=words[index*12U+10U];value[5U]=words[index*12U+11U];actual.push_back(value);
    }
    std::ranges::sort(expected);std::ranges::sort(actual);
    if(expected!=actual) {
      for(std::size_t index=0U;index<expected.size();++index)if(expected[index]!=actual[index]) {
        std::fprintf(stderr,"compact live owner mismatch at %zu: CPU=%u/%u/%u/%u mask=%u GPU=%u/%u/%u/%u mask=%u\n",
            index,expected[index][0U],expected[index][1U],expected[index][2U],expected[index][3U],expected[index][4U],
            actual[index][0U],actual[index][1U],actual[index][2U],actual[index][3U],actual[index][4U]);
        break;
      }
      return false;
    }
    return true;
  };
  const auto trace_owner=[&](std::uint32_t owner_index,
                             const tetra::GpuTerrainFieldTuple& terrain,tetra::Vec3 origin) {
    auto* trace=static_cast<std::uint32_t*>(owner_trace_readback.contents);
    const auto* owner_words=static_cast<const std::uint32_t*>(owner_readback.contents);
    if(trace==nullptr||owner_words==nullptr)return;
    std::fill_n(trace,1024U,0U);
    const MetalCompactOwnerEmitParameters parameters{owner_index,0U,0U,0U,
        {static_cast<float>(origin.x),static_cast<float>(origin.y),static_cast<float>(origin.z),0.0F},0U,0U,0U,0U};
    id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:owner_trace];
    // SPIRV-Cross orders this test kernel's MSL resources by its translated
    // ABI (Field, Owners, Trace, parameters, header, templates), not by the
    // source declaration order.
    [encoder setBuffer:selection.compact_p8_field offset:0U atIndex:0U];
    [encoder setBuffer:selection.compact_owner_stream offset:0U atIndex:1U];
    [encoder setBuffer:owner_trace_readback offset:0U atIndex:2U];
    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:3U];
    [encoder setBuffer:selection.compact_owner_header offset:0U atIndex:4U];
    [encoder setBuffer:selection.compact_p8_templates offset:0U atIndex:5U];
    [encoder dispatchThreads:MTLSizeMake(1U,1U,1U)
        threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
    [encoder endEncoding];[command commit];[command waitUntilCompleted];
    if(command.status!=MTLCommandBufferStatusCompleted||trace[1U]!=1U) {
      std::fprintf(stderr,"compact live parity owner trace unavailable: command=%ld owner=%u header=%u/%u/%u/%u trace=%u/%u\\n",
          static_cast<long>(command.status),owner_index,
          static_cast<const std::uint32_t*>(owner_header_readback.contents)[0U],
          static_cast<const std::uint32_t*>(owner_header_readback.contents)[1U],
          static_cast<const std::uint32_t*>(owner_header_readback.contents)[2U],
          static_cast<const std::uint32_t*>(owner_header_readback.contents)[3U],trace[0U],trace[1U]);
      return;
    }
    std::array<std::uint32_t,4> lanes{};
    std::copy_n(owner_words+owner_index*12U,4U,lanes.begin());
    const auto address=tetra::gpu_hierarchy_address_from_lanes(lanes);
    const auto exact=tetra::world_tetrahedron_geometry(address);
    const auto quantized=[](tetra::Vec3 point) {
      return tetra::Vec3{static_cast<float>(point.x),static_cast<float>(point.y),
                         static_cast<float>(point.z)};
    };
    std::array<tetra::Vec3,4> geometry{};
    for(std::size_t i=0U;i<geometry.size();++i)geometry[i]=quantized(exact[i]);
    bool reconstruction_differs=false;
    std::fprintf(stderr,"compact live parity owner trace owner=%u address=%u/%u/%u/%u origin=(%.9g,%.9g,%.9g) mask=%u cells=%u\\n",
        owner_index,lanes[0U],lanes[1U],lanes[2U],lanes[3U],std::bit_cast<float>(trace[2U]),
        std::bit_cast<float>(trace[3U]),std::bit_cast<float>(trace[4U]),trace[21U],trace[22U]);
    for(std::size_t i=0U;i<geometry.size();++i) {
      const tetra::Vec3 gpu{std::bit_cast<float>(trace[9U+i*3U]),
          std::bit_cast<float>(trace[10U+i*3U]),std::bit_cast<float>(trace[11U+i*3U])};
      reconstruction_differs|=std::abs(geometry[i].x-gpu.x)>1.e-6||
          std::abs(geometry[i].y-gpu.y)>1.e-6||std::abs(geometry[i].z-gpu.z)>1.e-6;
      std::fprintf(stderr,"  G[%zu] cpu=(%.9g,%.9g,%.9g) gpu=(%.9g,%.9g,%.9g)\\n",i,
          geometry[i].x,geometry[i].y,geometry[i].z,gpu.x,gpu.y,gpu.z);
    }
    constexpr std::array<std::array<std::size_t,2>,6> edges{{{{0U,1U}},{{0U,2U}},{{0U,3U}},{{1U,2U}},{{1U,3U}},{{2U,3U}}}};
    constexpr std::array<std::array<std::uint8_t,3>,32> cuts{{
        {{0,0,0}},{{0,0,0}},{{0,1,2}},{{0,0,0}},{{0,3,4}},{{0,0,0}},{{1,2,4}},{{1,4,3}},
        {{1,3,5}},{{0,0,0}},{{0,2,5}},{{0,5,3}},{{0,1,5}},{{0,5,4}},{{2,4,5}},{{0,0,0}},
        {{2,4,5}},{{0,0,0}},{{0,1,5}},{{0,5,4}},{{0,2,5}},{{0,5,3}},{{1,3,5}},{{0,0,0}},
        {{1,2,4}},{{1,4,3}},{{0,3,4}},{{0,0,0}},{{0,1,2}},{{0,0,0}},{{0,0,0}},{{0,0,0}}}};
    std::array<tetra::Vec3,6> midpoints{};
    for(std::size_t e=0U;e<midpoints.size();++e)
      midpoints[e]=quantized((geometry[edges[e][0U]]+geometry[edges[e][1U]])/2.0);
    const std::array<tetra::Vec3,10> points{{midpoints[2U],midpoints[1U],midpoints[0U],geometry[0U],
        midpoints[4U],midpoints[3U],geometry[1U],midpoints[5U],geometry[2U],geometry[3U]}};
    const auto field=tetra::gpu_terrain_field_tuple_sphere(terrain);
    const tetra::WorldStreamingDemand::Domain domain{{terrain.domain_origin_extent[0U],
        terrain.domain_origin_extent[1U],terrain.domain_origin_extent[2U]},terrain.domain_origin_extent[3U]};
    const auto mask=trace[21U];
    const auto cell_count=std::min(trace[22U],24U);
    bool topology_differs=false,root_differs=false;
    for(std::uint32_t cell=0U;cell<cell_count;++cell) {
      const auto at=24U+cell*40U;const auto packed=trace[at];
      std::array<std::uint8_t,4> corner{};std::uint32_t signs{};
      for(std::size_t k=0U;k<corner.size();++k) {
        corner[k]=static_cast<std::uint8_t>(packed>>(k*8U));
        if(field.signed_distance(domain.to_world(points[corner[k]]))<0.0)signs|=1U<<k;
      }
      std::uint32_t edge_mask{};
      std::array<tetra::Vec3,6> roots{};
      for(std::size_t edge=0U;edge<edges.size();++edge)if(
          ((signs>>edges[edge][0U])&1U)!=((signs>>edges[edge][1U])&1U)) {
        edge_mask|=1U<<edge;
        roots[edge]=quantized(field.edge_intersection(domain.to_world(points[corner[edges[edge][0U]]]),
            domain.to_world(points[corner[edges[edge][1U]]])));
      }
      const auto cut=cuts[signs*2U];
      const bool cell_topology=signs!=trace[at+1U]||edge_mask!=trace[at+2U]||
          cut[0U]!=trace[at+4U]||cut[1U]!=trace[at+5U]||cut[2U]!=trace[at+6U];
      topology_differs|=cell_topology;
      std::fprintf(stderr,"  cell=%u packed=%u signs cpu=%u gpu=%u edges cpu=%u gpu=%u cut cpu=%u/%u/%u gpu=%u/%u/%u\\n",
          cell,packed,signs,trace[at+1U],edge_mask,trace[at+2U],cut[0U],cut[1U],cut[2U],
          trace[at+4U],trace[at+5U],trace[at+6U]);
      for(std::size_t k=0U;k<3U;++k) {
        const tetra::Vec3 gpu{std::bit_cast<float>(trace[at+7U+k*3U]),
            std::bit_cast<float>(trace[at+8U+k*3U]),std::bit_cast<float>(trace[at+9U+k*3U])};
        const auto& cpu=roots[cut[k]];
        root_differs|=std::abs(cpu.x-gpu.x)>2.e-4||std::abs(cpu.y-gpu.y)>2.e-4||
            std::abs(cpu.z-gpu.z)>2.e-4;
        std::fprintf(stderr,"    root edge=%u cpu=(%.9g,%.9g,%.9g) gpu=(%.9g,%.9g,%.9g)\\n",
            cut[k],cpu.x,cpu.y,cpu.z,gpu.x,gpu.y,gpu.z);
      }
      if(trace[at+3U]!=0U)for(std::size_t k=0U;k<6U;++k)
        std::fprintf(stderr,"    emit-point[%zu] gpu=(%.9g,%.9g,%.9g)\\n",k,
            std::bit_cast<float>(trace[at+16U+k*3U]),
            std::bit_cast<float>(trace[at+17U+k*3U]),
            std::bit_cast<float>(trace[at+18U+k*3U]));
    }
    std::fprintf(stderr,"  first divergent stage=%s\\n",reconstruction_differs?"reconstruction":
        topology_differs?"field_classification_or_template":root_differs?"root_intersection":
        "projection_or_retained_copy");
  };
  const auto compare_geometry=[&](const std::vector<tetra::GpuTerrainProjectedTriangleRecord>& expected,
                                  const tetra::GpuHierarchySelectionTuple& tuple,
                                  const tetra::GpuTerrainFieldTuple& terrain,tetra::Vec3 origin) {
    const auto* words=static_cast<const std::uint32_t*>(retained_readback.contents);
    const auto* args=static_cast<const std::uint32_t*>(arguments_readback.contents);
    const auto* candidate_words=static_cast<const std::uint32_t*>(candidate_readback.contents);
    const auto* owner_words=static_cast<const std::uint32_t*>(owner_readback.contents);
    const auto* owner_header=static_cast<const std::uint32_t*>(owner_header_readback.contents);
    const auto* counts=static_cast<const std::uint32_t*>(count_readback.contents);
    const auto* offsets=static_cast<const std::uint32_t*>(offset_readback.contents);
    const std::uint32_t expected_vertices=static_cast<std::uint32_t>(expected.size()*12U);
    if(words==nullptr||args==nullptr||candidate_words==nullptr||args[0U]!=expected_vertices||args[1U]!=1U) {
      std::fprintf(stderr,"compact live parity argument mismatch: got=%u/%u expected=%u/1\n",
          args==nullptr?0U:args[0U],args==nullptr?0U:args[1U],expected_vertices);
      return false;
    }
    constexpr std::array<std::array<std::uint32_t,3>,4> faces{{{{0U,1U,2U}},{{1U,3U,4U}},{{2U,4U,5U}},{{1U,4U,2U}}}};
    // The retained front is compact-owner ordered, unlike the CPU packet's
    // address order. Associate each compact range with its exact CPU owner
    // before doing the unordered-set check below; this gives the diagnostic a
    // concrete first owner/cell rather than an arbitrary sorted coordinate.
    const auto traversal=tetra::gpu_hierarchy_traverse(snapshot,
        tetra::gpu_hierarchy_traversal_parameters(tuple));
    std::vector<tetra::WorldTetAddress> candidates;
    candidates.reserve(traversal.selected_records.size());
    for(const auto record:traversal.selected_records) {
      if(record>=snapshot.records.size())return false;
      candidates.push_back(tetra::gpu_hierarchy_address_from_lanes(snapshot.records[record].address));
    }
    std::ranges::sort(candidates);
    const auto packet=tetra::make_gpu_green_mask_packet(candidates,source_revision);
    using OwnerKey=std::array<std::uint32_t,6U>;
    std::map<OwnerKey,std::vector<const tetra::GpuTerrainProjectedTriangleRecord*>> expected_by_owner;
    for(const auto& owner:packet.owners) {
      OwnerKey key{};std::copy(owner.address.begin(),owner.address.end(),key.begin());
      key[4U]=owner.mask;key[5U]=owner.reflected_orientation;
      expected_by_owner.try_emplace(key);
    }
    for(const auto& record:expected) {
      if(record.source.owner_index>=packet.owners.size())return false;
      const auto& owner=packet.owners[record.source.owner_index];
      OwnerKey key{};std::copy(owner.address.begin(),owner.address.end(),key.begin());
      key[4U]=owner.mask;key[5U]=owner.reflected_orientation;
      expected_by_owner[key].push_back(&record);
    }
    if(owner_words==nullptr||owner_header==nullptr||counts==nullptr||offsets==nullptr||
       owner_header[0U]>selection.output_capacity)return false;
    std::uint32_t expected_offset{};
    for(std::uint32_t owner_index=0U;owner_index<owner_header[0U];++owner_index) {
      OwnerKey key{};std::copy_n(owner_words+owner_index*12U,4U,key.begin());
      key[4U]=owner_words[owner_index*12U+10U];key[5U]=owner_words[owner_index*12U+11U];
      const auto found=expected_by_owner.find(key);
      if(found==expected_by_owner.end()||counts[owner_index]!=found->second.size()||
         offsets[owner_index]!=expected_offset) {
        std::fprintf(stderr,"compact live parity owner range differs at owner %u: GPU offset/triangles=%u/%u CPU offset/triangles=%u/%zu\\n",
            owner_index,offsets[owner_index],counts[owner_index],expected_offset,
            found==expected_by_owner.end()?0U:found->second.size());
        trace_owner(owner_index,terrain,origin);return false;
      }
      for(std::size_t local=0U;local<found->second.size()*12U;++local) {
        const auto triangle=local/12U,face=(local%12U)/3U,corner=local%3U;
        const auto& point=found->second[triangle]->vertices[faces[face][corner]];
        const auto base=(static_cast<std::size_t>(offsets[owner_index])*12U+local)*18U;
        for(std::size_t axis=0U;axis<3U;++axis) {
          const float value=std::bit_cast<float>(words[base+axis]);
          const float candidate_value=std::bit_cast<float>(candidate_words[4U+base+axis]);
          const double wanted=axis==0U?point.x:axis==1U?point.y:point.z;
          if(!std::isfinite(value)||std::abs(static_cast<double>(value)-wanted)>2.e-3) {
            std::fprintf(stderr,"compact live parity owner geometry mismatch owner=%u local=%zu axis=%zu candidate=%.9g retained=%.9g CPU=%.9g stage=%s\\n",
                owner_index,local,axis,static_cast<double>(candidate_value),static_cast<double>(value),wanted,
                std::abs(static_cast<double>(candidate_value)-wanted)>2.e-3?"emit_or_projection":"retained_copy");
            const auto first_triangle=(local/12U)*12U;
            for(std::size_t probe=0U;probe<6U;++probe) {
              const auto probe_face=probe==0U?0U:probe==1U?0U:probe==2U?0U:probe==3U?1U:probe==4U?1U:1U;
              const auto probe_corner=probe==0U?0U:probe==1U?1U:probe==2U?2U:probe==3U?1U:probe==4U?3U:4U;
              const auto& cpu=found->second[first_triangle/12U]->vertices[faces[probe_face][probe_corner]];
              const auto candidate_base=(static_cast<std::size_t>(offsets[owner_index])*12U+first_triangle+probe)*18U;
              std::fprintf(stderr,"  emitted[%zu] candidate=(%.9g,%.9g,%.9g) cpu=(%.9g,%.9g,%.9g)\\n",probe,
                  std::bit_cast<float>(candidate_words[4U+candidate_base]),
                  std::bit_cast<float>(candidate_words[5U+candidate_base]),
                  std::bit_cast<float>(candidate_words[6U+candidate_base]),cpu.x,cpu.y,cpu.z);
            }
            const auto& sought=found->second[first_triangle/12U]->vertices[0U];
            std::uint32_t found_vertex=std::numeric_limits<std::uint32_t>::max();
            for(std::uint32_t vertex=0U;vertex<args[0U];++vertex) {
              const auto probe=4U+static_cast<std::size_t>(vertex)*18U;
              if(std::abs(std::bit_cast<float>(candidate_words[probe])-sought.x)<2.e-3&&
                 std::abs(std::bit_cast<float>(candidate_words[probe+1U])-sought.y)<2.e-3&&
                 std::abs(std::bit_cast<float>(candidate_words[probe+2U])-sought.z)<2.e-3) {
                found_vertex=vertex;break;
              }
            }
            std::fprintf(stderr,"  expected first owner vertex expected-index=%u found-candidate-index=%u\\n",
                offsets[owner_index]*12U+static_cast<std::uint32_t>(first_triangle),found_vertex);
            trace_owner(owner_index,terrain,origin);return false;
          }
        }
      }
      expected_offset+=counts[owner_index];
    }
    // This keyed comparison is stronger than a globally sorted position set:
    // every emitted vertex is tied to its compact owner, cell and local face.
    return true;
  };
  const auto compare_capture=[&](const std::vector<tetra::GpuTerrainProjectedTriangleRecord>& expected) {
    // A deterministic 96x96 orthographic capture.  This is intentionally a
    // tiny independent image oracle: matching a count or an unordered set of
    // vertices would miss winding/order defects that create visual seams.
    constexpr int side=96;
    std::array<double,4> bounds{std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    for(const auto& triangle:expected)for(const auto& point:triangle.vertices) {
      bounds[0]=std::min(bounds[0],point.x);bounds[1]=std::max(bounds[1],point.x);
      bounds[2]=std::min(bounds[2],point.y);bounds[3]=std::max(bounds[3],point.y);
    }
    if(!(bounds[1]>bounds[0])||!(bounds[3]>bounds[2]))return false;
    const auto rasterize=[&](const auto& vertex_at) {
      std::array<std::uint8_t,side*side> pixels{};
      const auto point=[&](std::size_t vertex) {
        const auto value=vertex_at(vertex);
        // The oracle captures coverage on a fixed 1/64-pixel subgrid.  It
        // deliberately removes CPU-double versus device-float noise below a
        // display sample while still requiring bit-identical captured pixels.
        const auto quantize=[](double value){return std::round(value*64.0)/64.0;};
        return std::array<double,2>{quantize((value[0]-bounds[0])/(bounds[1]-bounds[0])*(side-1)),
            quantize((value[1]-bounds[2])/(bounds[3]-bounds[2])*(side-1))};
      };
      const auto edge=[](const auto& a,const auto& b,double x,double y) {
        return (x-a[0])*(b[1]-a[1])-(y-a[1])*(b[0]-a[0]);
      };
      const auto vertices=expected.size()*12U;
      for(std::size_t base=0U;base<vertices;base+=3U) {
        const auto a=point(base),b=point(base+1U),c=point(base+2U);
        const auto area=edge(a,b,c[0],c[1]);
        if(std::abs(area)<1.e-9)continue;
        for(int y=0;y<side;++y)for(int x=0;x<side;++x) {
          const auto ab=edge(a,b,x,y),bc=edge(b,c,x,y),ca=edge(c,a,x,y);
          if((ab>=0.0&&bc>=0.0&&ca>=0.0)||(ab<=0.0&&bc<=0.0&&ca<=0.0))
            pixels[static_cast<std::size_t>(y*side+x)]=1U;
        }
      }
      return pixels;
    };
    constexpr std::array<std::array<std::uint32_t,3>,4> faces{{{{0U,1U,2U}},{{1U,3U,4U}},{{2U,4U,5U}},{{1U,4U,2U}}}};
    const auto cpu=rasterize([&](std::size_t index) {
      const auto triangle=index/12U,corner=index%3U,face=(index%12U)/3U;
      const auto& value=expected[triangle].vertices[faces[face][corner]];
      return std::array<double,2>{value.x,value.y};
    });
    const auto* words=static_cast<const std::uint32_t*>(retained_readback.contents);
    if(words==nullptr)return false;
    const auto gpu=rasterize([&](std::size_t index) {
      return std::array<double,2>{std::bit_cast<float>(words[index*18U]),
          std::bit_cast<float>(words[index*18U+1U])};
    });
    if(cpu==gpu)return true;
    std::size_t mismatch{};
    for(std::size_t pixel=0U;pixel<cpu.size();++pixel)mismatch+=cpu[pixel]!=gpu[pixel];
    std::fprintf(stderr,"compact live parity image differs at %zu/%zu pixels\\n",mismatch,cpu.size());
    return false;
  };
  struct CompactExecutionTiming {
    double selector_encode_ms{},closure_encode_ms{},materialize_encode_ms{},
        p8_encode_ms{},audit_encode_ms{},commit_wait_ms{},total_ms{};
    double gpu_command_ms{};
    std::uint32_t owners{},triangles{};
  };
  const auto execute=[&](const tetra::GpuHierarchySelectionTuple& tuple,tetra::Vec3 origin,
                         bool inject_green_budget_failure,CompactExecutionTiming* timing=nullptr) {
    const auto total_begin=std::chrono::steady_clock::now();
    id<MTLCommandBuffer> command=[queue commandBuffer];
    const auto selector_begin=std::chrono::steady_clock::now();
    const bool selector_ok=encode_metal_gpu_hierarchy_live_selection(command,selector,worklist,selection,tuple);
    const auto selector_end=std::chrono::steady_clock::now();
    const bool closure_ok=selector_ok&&encode_metal_gpu_hierarchy_live_compact_closure(command,canonicalize,green,red,scan,
          selection,inject_green_budget_failure);
    const auto closure_end=std::chrono::steady_clock::now();
    const bool materialize_ok=closure_ok&&encode_metal_gpu_hierarchy_compact_owner_materialize(command,materialize,selection);
    const auto materialize_end=std::chrono::steady_clock::now();
    const bool p8_ok=materialize_ok&&(use_hybrid?
        encode_metal_gpu_hierarchy_compact_owner_p8_hybrid(command,p8,
          selection.compact_owner_stream,selection.compact_owner_header,
          selection.compact_p8_field,selection.compact_p8_templates,
          selection.compact_p8_candidate,selection.compact_p8_status,
          selection.compact_p8_microbatch_copy_dispatch,selection.compact_p8_triangle_dispatch,
          selection.compact_p8_counts,
          selection.compact_p8_offsets,selection.compact_p8_signs,retained,arguments,
          vertex_capacity,origin,source_revision):
        encode_metal_gpu_hierarchy_compact_owner_p8(command,p8,selection.compact_owner_stream,
          selection.compact_owner_header,selection.compact_p8_field,selection.compact_p8_templates,
          selection.compact_p8_counts,selection.compact_p8_offsets,selection.compact_p8_block_totals,
          selection.compact_p8_block_offsets,selection.compact_p8_level_totals,
          selection.compact_p8_level_offsets,selection.compact_p8_signs,selection.compact_p8_candidate,
          selection.compact_p8_status,selection.compact_p8_dispatches,retained,arguments,
          vertex_capacity,origin,source_revision));
    const auto p8_end=std::chrono::steady_clock::now();
    if(!p8_ok)return false;
    ++selection.compact_p8_encoded;
    const auto audit_begin=std::chrono::steady_clock::now();
    id<MTLBlitCommandEncoder> audit=[command blitCommandEncoder];
    [audit copyFromBuffer:selection.compact_p8_status sourceOffset:0U
        toBuffer:selection.compact_p8_audit destinationOffset:0U size:2U*sizeof(std::uint32_t)];
    [audit copyFromBuffer:selection.compact_p8_candidate sourceOffset:0U
        toBuffer:selection.compact_p8_audit destinationOffset:2U*sizeof(std::uint32_t) size:4U*sizeof(std::uint32_t)];
    [audit copyFromBuffer:selection.compact_owner_header sourceOffset:0U
        toBuffer:selection.compact_p8_audit destinationOffset:6U*sizeof(std::uint32_t) size:4U*sizeof(std::uint32_t)];
    [audit endEncoding];
    const auto audit_end=std::chrono::steady_clock::now();
    const auto wait_begin=std::chrono::steady_clock::now();
    [command commit];[command waitUntilCompleted];
    const auto wait_end=std::chrono::steady_clock::now();
    retire_metal_gpu_hierarchy_live_selection(selection);
    if(timing!=nullptr) {
      const auto elapsed=[](auto start,auto end) {
        return std::chrono::duration<double,std::milli>(end-start).count();
      };
      timing->selector_encode_ms=elapsed(selector_begin,selector_end);
      timing->closure_encode_ms=elapsed(selector_end,closure_end);
      timing->materialize_encode_ms=elapsed(closure_end,materialize_end);
      timing->p8_encode_ms=elapsed(materialize_end,p8_end);
      timing->audit_encode_ms=elapsed(audit_begin,audit_end);
      timing->commit_wait_ms=elapsed(wait_begin,wait_end);
      timing->total_ms=elapsed(total_begin,wait_end);
      timing->gpu_command_ms=command.GPUEndTime>=command.GPUStartTime?
          (command.GPUEndTime-command.GPUStartTime)*1000.0:0.0;
      timing->owners=selection.compact_p8_last_owner_header[0U];
      timing->triangles=selection.compact_p8_last_audit[4U];
    }
    return command.status==MTLCommandBufferStatusCompleted;
  };
  const auto first_tuple=make_tuple({0.0,.5,3.0},{.125,-.25,.375},field_revision);
  const auto second_tuple=make_tuple({.7,.5,2.8},{-.25,.125,.5},field_revision);
  std::vector<tetra::GpuTerrainProjectedTriangleRecord> first_expected,second_expected;
  const bool first_oracle=oracle_geometry(first_tuple,field_tuple,{.125,-.25,.375},first_expected);
  const bool first_encoded=first_oracle&&execute(first_tuple,{.125,-.25,.375},false);
  const bool first_owner_read=first_encoded&&read_owners();
  const bool first_owner_parity=first_owner_read&&compare_owners(first_tuple);
  const bool first_read=first_encoded&&read_retained();
  const bool first_committed=first_owner_parity&&first_read&&
      compare_geometry(first_expected,first_tuple,field_tuple,{.125,-.25,.375});
  const bool image_parity=first_committed&&compare_capture(first_expected);
  const bool second_oracle=oracle_geometry(second_tuple,field_tuple,{-.25,.125,.5},second_expected);
  const bool second_encoded=first_committed&&second_oracle&&
      execute(second_tuple,{-.25,.125,.5},false);
  const bool second_owner_parity=second_encoded&&read_owners()&&compare_owners(second_tuple);
  const bool moving_parity=second_owner_parity&&read_retained()&&
      compare_geometry(second_expected,second_tuple,field_tuple,{-.25,.125,.5});
  // A field revision replaces the immutable selector snapshot and P8 field
  // tuple as one unit.  The source world revision stays fixed, so this catches
  // an accidental reuse of a prior-field private front.
  const auto changed_snapshot=tetra::make_gpu_hierarchy_snapshot(directory,field_revision+1U);
  auto changed_field_parameters=field_parameters;
  changed_field_parameters.field_revision=field_revision+1U;
  changed_field_parameters.field.centre.x-=.03125;
  changed_field_parameters.field.radius=.31;
  const auto changed_field_tuple=tetra::make_gpu_terrain_field_tuple(changed_field_parameters);
  const bool field_configured=moving_parity&&configure_metal_gpu_hierarchy_live_selection(
      device,selection,changed_snapshot,field_revision+1U,19U);
  if(field_configured) {
    selection.compact_p8_field=[device newBufferWithBytes:&changed_field_tuple length:sizeof(changed_field_tuple)
        options:MTLResourceStorageModeShared];
    selection.compact_p8_templates=[device newBufferWithBytes:templates.data() length:sizeof(templates)
        options:MTLResourceStorageModeShared];
  }
  const auto field_tuple_camera=make_tuple({.2,.55,2.9},{-.125,.25,-.375},field_revision+1U);
  std::vector<tetra::GpuTerrainProjectedTriangleRecord> field_expected;
  const bool field_encoded=field_configured&&selection.compact_p8_field!=nil&&
      selection.compact_p8_templates!=nil&&ensure_metal_gpu_hierarchy_compact_p8_workspace(
          device,selection,vertex_capacity)&&oracle_geometry(field_tuple_camera,changed_field_tuple,
          {-.125,.25,-.375},field_expected)&&execute(field_tuple_camera,{-.125,.25,-.375},false)&&
      read_owners()&&compare_owners(field_tuple_camera);
  const bool field_parity=field_encoded&&read_retained()&&
      compare_geometry(field_expected,field_tuple_camera,changed_field_tuple,{-.125,.25,-.375});
  std::vector<std::uint32_t> retained_before(vertex_capacity*18U),arguments_before(4U);
  if(field_parity) {
    std::memcpy(retained_before.data(),retained_readback.contents,retained_readback.length);
    std::memcpy(arguments_before.data(),arguments_readback.contents,arguments_readback.length);
  }
  const bool failed_command=field_parity&&execute(field_tuple_camera,{-.125,.25,-.375},true)&&read_retained();
  const bool failure_retains=failed_command&&selection.compact_p8_rejected!=0U&&
      std::memcmp(retained_before.data(),retained_readback.contents,retained_readback.length)==0&&
      std::memcmp(arguments_before.data(),arguments_readback.contents,arguments_readback.length)==0;
  auto stale_tuple=field_tuple_camera;
  stale_tuple.revision_lanes[0U]=static_cast<std::uint32_t>(source_revision+1U);
  const bool stale_rejected=!encode_metal_gpu_hierarchy_live_selection([queue commandBuffer],selector,
      worklist,selection,stale_tuple)&&selection.stale_rejected!=0U;
  std::array<std::vector<double>,2U> cpu_profiles,gpu_profiles;
  std::array<std::vector<double>,2U> selector_encode_profiles,closure_encode_profiles,
      materialize_encode_profiles,p8_encode_profiles,wait_profiles,gpu_command_profiles;
  // This diagnostic runs after the normal qualification samples.  It splits
  // the same private-buffer route into stage command buffers solely to obtain
  // device timestamps; the production route remains a single command buffer
  // and no owner/vertex payload is read back here.
    std::array<std::vector<double>,2U> selector_device_profiles,closure_device_profiles,
      materialize_device_profiles,p8_device_profiles,p8_count_device_profiles,
      p8_scan_device_profiles,p8_emit_device_profiles,p8_finalize_copy_device_profiles;
    // P7e4m derives closure substage attribution from matched cumulative
    // command-buffer spans. These remain diagnostic-only: each prefix is
    // replayed from a fresh private selection input and no payload is copied.
    std::array<std::vector<double>,2U> closure_clear_device_profiles,
      closure_initial_canonical_device_profiles,closure_green_device_profiles,
      closure_red_clear_device_profiles,closure_red_work_device_profiles,
      closure_followup_canonical_device_profiles;
  bool benchmark_valid=!run_p95_benchmark;
  if(run_p95_benchmark&&moving_parity&&image_parity&&field_parity&&failure_retains&&stale_rejected) {
    // This is deliberately a matched, device-front-only timing loop.  The
    // CPU side independently performs the same selector/closure/root/project
    // oracle.  The GPU interval starts with the identical moving camera tuple
    // and ends only after its compact P8 private front has completed.  No
    // candidate or retained payload is read during these samples.
    constexpr std::uint32_t profiles=2U,warmup=4U,samples=30U;
    for(std::uint32_t profile=0U;profile<profiles;++profile)for(auto* values:{
        &cpu_profiles[profile],&gpu_profiles[profile],&selector_encode_profiles[profile],
        &closure_encode_profiles[profile],&materialize_encode_profiles[profile],
        &p8_encode_profiles[profile],&wait_profiles[profile],&gpu_command_profiles[profile]})
      values->reserve(samples);
    bool cpu_ok=true,gpu_ok=true;
    std::uint32_t minimum_owners=std::numeric_limits<std::uint32_t>::max(),maximum_owners{};
    std::uint32_t minimum_triangles=std::numeric_limits<std::uint32_t>::max(),maximum_triangles{};
    for(std::uint32_t profile=0U;profile<profiles;++profile) {
      for(std::uint32_t sample=0U;sample<warmup+samples;++sample) {
        // Each profile repeats precisely the same camera/origin sequence;
        // only post-warmup tuples contribute to its independently reported
        // percentile. The CPU routine is the exact fallback/reference work
        // used by the parity gate: traverse, close, root, base and project.
        const double t=static_cast<double>(sample);
        const tetra::Vec3 origin{-.125+t/1024.0,.25-t/2048.0,-.375+t/4096.0};
        const auto tuple=make_tuple({.2+t/640.0,.55,2.9-t/960.0},origin,field_revision+1U);
        std::vector<tetra::GpuTerrainProjectedTriangleRecord> cpu_geometry;
        const auto cpu_begin=std::chrono::steady_clock::now();
        cpu_ok&=oracle_geometry(tuple,changed_field_tuple,origin,cpu_geometry);
        const auto cpu_end=std::chrono::steady_clock::now();
        const auto gpu_begin=std::chrono::steady_clock::now();
        CompactExecutionTiming execution;
        gpu_ok&=execute(tuple,origin,false,&execution);
        const auto gpu_end=std::chrono::steady_clock::now();
        if(sample>=warmup) {
          cpu_profiles[profile].push_back(std::chrono::duration<double,std::milli>(cpu_end-cpu_begin).count());
          gpu_profiles[profile].push_back(std::chrono::duration<double,std::milli>(gpu_end-gpu_begin).count());
          selector_encode_profiles[profile].push_back(execution.selector_encode_ms);
          closure_encode_profiles[profile].push_back(execution.closure_encode_ms);
          materialize_encode_profiles[profile].push_back(execution.materialize_encode_ms);
          p8_encode_profiles[profile].push_back(execution.p8_encode_ms);
          wait_profiles[profile].push_back(execution.commit_wait_ms);
          gpu_command_profiles[profile].push_back(execution.gpu_command_ms);
          minimum_owners=std::min(minimum_owners,execution.owners);
          maximum_owners=std::max(maximum_owners,execution.owners);
          minimum_triangles=std::min(minimum_triangles,execution.triangles);
          maximum_triangles=std::max(maximum_triangles,execution.triangles);
        }
      }
    }
    constexpr std::uint32_t stage_samples=3U;
    bool stage_isolation_ok=true;
    bool closure_attribution_ok=true;
    std::uint32_t minimum_active=std::numeric_limits<std::uint32_t>::max(),maximum_active{};
    std::uint32_t minimum_red_rounds=std::numeric_limits<std::uint32_t>::max(),maximum_red_rounds{};
    const auto run_stage=[&](const auto& encode,double& device_ms) {
      id<MTLCommandBuffer> command=[queue commandBuffer];
      if(!encode(command))return false;
      [command commit];[command waitUntilCompleted];
      if(command.status!=MTLCommandBufferStatusCompleted||
         command.GPUEndTime<command.GPUStartTime)return false;
      device_ms=(command.GPUEndTime-command.GPUStartTime)*1000.0;
      return std::isfinite(device_ms)&&device_ms>=0.0;
    };
    for(std::uint32_t profile=0U;profile<profiles;++profile) {
      for(auto* values:{&selector_device_profiles[profile],&closure_device_profiles[profile],
          &materialize_device_profiles[profile],&p8_device_profiles[profile],
          &p8_count_device_profiles[profile],&p8_scan_device_profiles[profile],
          &p8_emit_device_profiles[profile],&p8_finalize_copy_device_profiles[profile],
          &closure_clear_device_profiles[profile],&closure_initial_canonical_device_profiles[profile],
          &closure_green_device_profiles[profile],&closure_red_clear_device_profiles[profile],
          &closure_red_work_device_profiles[profile],&closure_followup_canonical_device_profiles[profile]})values->reserve(stage_samples);
      for(std::uint32_t sample=0U;sample<stage_samples;++sample) {
        const double t=static_cast<double>(sample+warmup);
        const tetra::Vec3 origin{-.125+t/1024.0,.25-t/2048.0,-.375+t/4096.0};
        const auto tuple=make_tuple({.2+t/640.0,.55,2.9-t/960.0},origin,field_revision+1U);
        double selector_device{},closure_device{},materialize_device{},p8_device{};
        double p8_count_device{},p8_scan_device{},p8_emit_device{},p8_finalize_copy_device{};
        stage_isolation_ok&=run_stage([&](id<MTLCommandBuffer> command) {
          return encode_metal_gpu_hierarchy_live_selection(command,selector,worklist,selection,tuple);
        },selector_device);
        retire_metal_gpu_hierarchy_live_selection(selection);
        stage_isolation_ok&=run_stage([&](id<MTLCommandBuffer> command) {
          return encode_metal_gpu_hierarchy_live_compact_closure(command,canonicalize,green,red,scan,
              selection,false);
        },closure_device);
        retire_metal_gpu_hierarchy_live_selection(selection);
        if(stage_isolation_ok) {
          minimum_active=std::min(minimum_active,selection.compact_last_final_active_header[0U]);
          maximum_active=std::max(maximum_active,selection.compact_last_final_active_header[0U]);
          minimum_red_rounds=std::min(minimum_red_rounds,selection.compact_last_closure_audit[7U]);
          maximum_red_rounds=std::max(maximum_red_rounds,selection.compact_last_closure_audit[7U]);
        }
        // Replay six cumulative closure prefixes from a freshly selected
        // private input. Adjacent differences are computed per sample, not
        // from independent percentiles, so every attributed interval covers
        // the actual dependency chain that precedes it.
        std::array<double,6U> closure_cumulative{};
        bool sample_attribution_ok=stage_isolation_ok;
        for(std::uint32_t stop=0U;stop<closure_cumulative.size()&&sample_attribution_ok;++stop) {
          double ignored{};
          sample_attribution_ok&=run_stage([&](id<MTLCommandBuffer> command) {
            return encode_metal_gpu_hierarchy_live_selection(command,selector,worklist,selection,tuple);
          },ignored);
          retire_metal_gpu_hierarchy_live_selection(selection);
          sample_attribution_ok&=run_stage([&](id<MTLCommandBuffer> command) {
            return encode_metal_gpu_hierarchy_live_compact_closure(command,canonicalize,green,red,scan,
                selection,false,stop);
          },closure_cumulative[stop]);
        }
        // Device command timing is expected to grow monotonically for these
        // nested prefixes. Permit only 0.01ms timestamp quantization noise;
        // otherwise omit all substage attribution rather than report a
        // synthetic negative interval.
        constexpr double closure_timing_noise_ms=.01;
        std::array<double,6U> closure_stages{};
        if(sample_attribution_ok) {
          closure_stages[0U]=closure_cumulative[0U];
          for(std::size_t stage=1U;stage<closure_stages.size();++stage) {
            const double difference=closure_cumulative[stage]-closure_cumulative[stage-1U];
            if(!std::isfinite(difference)||difference< -closure_timing_noise_ms) {
              sample_attribution_ok=false;break;
            }
            closure_stages[stage]=std::max(0.0,difference);
          }
        }
        closure_attribution_ok&=sample_attribution_ok;
        if(sample_attribution_ok) {
          closure_clear_device_profiles[profile].push_back(closure_stages[0U]);
          closure_initial_canonical_device_profiles[profile].push_back(closure_stages[1U]);
          closure_green_device_profiles[profile].push_back(closure_stages[2U]);
          closure_red_clear_device_profiles[profile].push_back(closure_stages[3U]);
          closure_red_work_device_profiles[profile].push_back(closure_stages[4U]);
          closure_followup_canonical_device_profiles[profile].push_back(closure_stages[5U]);
        }
        // Prefix replay leaves the workspace at the final diagnostic stop,
        // not at the published closure result. Rebuild that complete private
        // dependency chain before timing the materializer/P8 stages below.
        double restored_selector{},restored_closure{};
        stage_isolation_ok&=run_stage([&](id<MTLCommandBuffer> command) {
          return encode_metal_gpu_hierarchy_live_selection(command,selector,worklist,selection,tuple);
        },restored_selector);
        retire_metal_gpu_hierarchy_live_selection(selection);
        stage_isolation_ok&=run_stage([&](id<MTLCommandBuffer> command) {
          return encode_metal_gpu_hierarchy_live_compact_closure(command,canonicalize,green,red,scan,
              selection,false);
        },restored_closure);
        retire_metal_gpu_hierarchy_live_selection(selection);
        stage_isolation_ok&=run_stage([&](id<MTLCommandBuffer> command) {
          return encode_metal_gpu_hierarchy_compact_owner_materialize(command,materialize,selection);
        },materialize_device);
        const auto hybrid_stage=[&](id<MTLCommandBuffer> command,
                                    std::uint32_t first,std::uint32_t last) {
          return encode_metal_gpu_hierarchy_compact_owner_p8_hybrid(command,p8,
              selection.compact_owner_stream,selection.compact_owner_header,
              selection.compact_p8_field,selection.compact_p8_templates,
              selection.compact_p8_candidate,selection.compact_p8_status,
              selection.compact_p8_microbatch_copy_dispatch,selection.compact_p8_triangle_dispatch,
              selection.compact_p8_counts,
              selection.compact_p8_offsets,selection.compact_p8_signs,retained,arguments,
              vertex_capacity,origin,source_revision,first,last);
        };
        if(use_hybrid) {
          stage_isolation_ok&=run_stage([&](id<MTLCommandBuffer> command) {
            return hybrid_stage(command,0U,0U);},p8_count_device);
          stage_isolation_ok&=run_stage([&](id<MTLCommandBuffer> command) {
            return hybrid_stage(command,1U,1U);},p8_scan_device);
          stage_isolation_ok&=run_stage([&](id<MTLCommandBuffer> command) {
            return hybrid_stage(command,2U,2U);},p8_emit_device);
          stage_isolation_ok&=run_stage([&](id<MTLCommandBuffer> command) {
            return hybrid_stage(command,3U,4U);},p8_finalize_copy_device);
          p8_device=p8_count_device+p8_scan_device+p8_emit_device+
              p8_finalize_copy_device;
        } else stage_isolation_ok&=run_stage([&](id<MTLCommandBuffer> command) {
          return encode_metal_gpu_hierarchy_compact_owner_p8(command,p8,
              selection.compact_owner_stream,selection.compact_owner_header,
              selection.compact_p8_field,selection.compact_p8_templates,
              selection.compact_p8_counts,selection.compact_p8_offsets,
              selection.compact_p8_block_totals,selection.compact_p8_block_offsets,
              selection.compact_p8_level_totals,selection.compact_p8_level_offsets,
              selection.compact_p8_signs,selection.compact_p8_candidate,
              selection.compact_p8_status,selection.compact_p8_dispatches,
              retained,arguments,vertex_capacity,origin,source_revision);
        },p8_device);
        if(stage_isolation_ok) {
          selector_device_profiles[profile].push_back(selector_device);
          closure_device_profiles[profile].push_back(closure_device);
          materialize_device_profiles[profile].push_back(materialize_device);
          p8_device_profiles[profile].push_back(p8_device);
          p8_count_device_profiles[profile].push_back(p8_count_device);
          p8_scan_device_profiles[profile].push_back(p8_scan_device);
          p8_emit_device_profiles[profile].push_back(p8_emit_device);
          p8_finalize_copy_device_profiles[profile].push_back(p8_finalize_copy_device);
        }
      }
    }
    for(auto& profile:cpu_profiles)std::ranges::sort(profile);
    for(auto& profile:gpu_profiles)std::ranges::sort(profile);
    for(auto* profiles:{&selector_encode_profiles,&closure_encode_profiles,&materialize_encode_profiles,
        &p8_encode_profiles,&wait_profiles,&gpu_command_profiles})
      for(auto& profile:*profiles)std::ranges::sort(profile);
    for(auto* profiles:{&selector_device_profiles,&closure_device_profiles,
        &materialize_device_profiles,&p8_device_profiles,&p8_count_device_profiles,
        &p8_scan_device_profiles,&p8_emit_device_profiles,
        &p8_finalize_copy_device_profiles})for(auto& profile:*profiles) {
      std::ranges::sort(profile);
      stage_isolation_ok&=profile.size()==stage_samples;
    }
    for(auto* profiles:{&closure_clear_device_profiles,
        &closure_initial_canonical_device_profiles,&closure_green_device_profiles,
        &closure_red_clear_device_profiles,&closure_red_work_device_profiles,
        &closure_followup_canonical_device_profiles})for(auto& profile:*profiles) {
      std::ranges::sort(profile);
      closure_attribution_ok&=profile.size()==stage_samples;
    }
    benchmark_valid=cpu_ok&&gpu_ok&&stage_isolation_ok&&selection.cpu_generation_violations==0U&&
        selection.compact_p8_encoded>=profiles*(warmup+samples)&&minimum_owners>0U&&minimum_triangles>0U;
    for(std::uint32_t profile=0U;profile<profiles;++profile)
      benchmark_valid&=cpu_profiles[profile].size()==samples&&gpu_profiles[profile].size()==samples&&
          cpu_profiles[profile].front()>0.0&&gpu_profiles[profile].front()>0.0&&
          std::isfinite(cpu_profiles[profile].back())&&std::isfinite(gpu_profiles[profile].back());
    const auto percentile=[](const std::vector<double>& values,double fraction) {
      const auto index=static_cast<std::size_t>(std::ceil(fraction*(values.size()-1U)));
      return values[std::min(index,values.size()-1U)];
    };
    const double cpu_p50_0=percentile(cpu_profiles[0U],.50),cpu_p95_0=percentile(cpu_profiles[0U],.95);
    const double gpu_p50_0=percentile(gpu_profiles[0U],.50),gpu_p95_0=percentile(gpu_profiles[0U],.95);
    const double cpu_p50_1=percentile(cpu_profiles[1U],.50),cpu_p95_1=percentile(cpu_profiles[1U],.95);
    const double gpu_p50_1=percentile(gpu_profiles[1U],.50),gpu_p95_1=percentile(gpu_profiles[1U],.95);
    const auto pair_p95=[&](const auto& profiles) {
      return std::array<double,2U>{percentile(profiles[0U],.95),percentile(profiles[1U],.95)};
    };
    const auto selector_p95=pair_p95(selector_encode_profiles),closure_p95=pair_p95(closure_encode_profiles),
        materialize_p95=pair_p95(materialize_encode_profiles),p8_p95=pair_p95(p8_encode_profiles),
        wait_p95=pair_p95(wait_profiles),gpu_command_p95=pair_p95(gpu_command_profiles),
        selector_device_p95=pair_p95(selector_device_profiles),
        closure_device_p95=pair_p95(closure_device_profiles),
        materialize_device_p95=pair_p95(materialize_device_profiles),
        p8_device_p95=pair_p95(p8_device_profiles),
        p8_count_device_p95=pair_p95(p8_count_device_profiles),
        p8_scan_device_p95=pair_p95(p8_scan_device_profiles),
        p8_emit_device_p95=pair_p95(p8_emit_device_profiles),
        p8_finalize_copy_device_p95=pair_p95(p8_finalize_copy_device_profiles);
    const auto closure_attribution_json=[&](const auto& values) {
      if(!closure_attribution_ok)return std::string("null");
      const auto p95=pair_p95(values);
      char result[64];
      std::snprintf(result,sizeof(result),"[%.4f,%.4f]",p95[0U],p95[1U]);
      return std::string(result);
    };
    const auto closure_clear_device_json=closure_attribution_json(closure_clear_device_profiles),
        closure_initial_canonical_device_json=closure_attribution_json(closure_initial_canonical_device_profiles),
        closure_green_device_json=closure_attribution_json(closure_green_device_profiles),
        closure_red_clear_device_json=closure_attribution_json(closure_red_clear_device_profiles),
        closure_red_work_device_json=closure_attribution_json(closure_red_work_device_profiles),
        closure_followup_canonical_device_json=closure_attribution_json(closure_followup_canonical_device_profiles);
    constexpr double material_p95_ratio=0.90;
    const bool materially_improved=gpu_p95_0<=cpu_p95_0*material_p95_ratio&&
        gpu_p95_1<=cpu_p95_1*material_p95_ratio;
    std::printf("{\"event\":\"metal_gpu_compact_camera_to_private_front\","
                "\"profiles\":%u,\"warmup\":%u,\"samples\":%zu,"
                "\"cpu_p50_ms\":[%.4f,%.4f],\"cpu_p95_ms\":[%.4f,%.4f],"
                "\"gpu_p50_ms\":[%.4f,%.4f],\"gpu_p95_ms\":[%.4f,%.4f],"
                "\"p95_ratio\":[%.4f,%.4f],\"material_p95_ratio\":%.2f,"
                "\"selector_encode_p95_ms\":[%.4f,%.4f],\"closure_encode_p95_ms\":[%.4f,%.4f],"
                "\"materialize_encode_p95_ms\":[%.4f,%.4f],\"p8_encode_p95_ms\":[%.4f,%.4f],"
                "\"host_commit_wait_p95_ms\":[%.4f,%.4f],\"gpu_command_p95_ms\":[%.4f,%.4f],"
                "\"stage_isolation_samples\":%u,\"selector_device_p95_ms\":[%.4f,%.4f],"
                "\"closure_device_p95_ms\":[%.4f,%.4f],\"materialize_device_p95_ms\":[%.4f,%.4f],"
                "\"closure_substage_attribution_available\":%s,"
                "\"closure_clear_device_p95_ms\":%s,\"closure_initial_canonical_device_p95_ms\":%s,"
                "\"closure_green_device_p95_ms\":%s,\"closure_red_clear_device_p95_ms\":%s,"
                "\"closure_red_work_device_p95_ms\":%s,\"closure_followup_canonical_device_p95_ms\":%s,"
                "\"p8_device_p95_ms\":[%.4f,%.4f],\"stage_isolation_payload_readback\":false,"
                "\"p8_count_device_p95_ms\":[%.4f,%.4f],\"p8_scan_device_p95_ms\":[%.4f,%.4f],"
                "\"p8_emit_device_p95_ms\":[%.4f,%.4f],\"p8_finalize_copy_device_p95_ms\":[%.4f,%.4f],"
                "\"closure_active_owners\":[%u,%u],\"closure_red_rounds\":[%u,%u],"
                "\"selected_owners\":[%u,%u],\"emitted_triangles\":[%u,%u],"
                "\"gpu_p95_materially_improved_both\":%s,"
                "\"outcome\":\"%s\",\"benchmark_payload_readback\":false,"
                "\"cpu_generation_violations\":%llu,\"passed\":%s}\n",
        profiles,warmup,cpu_profiles[0U].size(),cpu_p50_0,cpu_p50_1,cpu_p95_0,cpu_p95_1,
        gpu_p50_0,gpu_p50_1,gpu_p95_0,gpu_p95_1,cpu_p95_0>0.0?gpu_p95_0/cpu_p95_0:0.0,
        cpu_p95_1>0.0?gpu_p95_1/cpu_p95_1:0.0,material_p95_ratio,
        selector_p95[0U],selector_p95[1U],closure_p95[0U],closure_p95[1U],
        materialize_p95[0U],materialize_p95[1U],p8_p95[0U],p8_p95[1U],
        wait_p95[0U],wait_p95[1U],gpu_command_p95[0U],gpu_command_p95[1U],
        stage_samples,selector_device_p95[0U],selector_device_p95[1U],
        closure_device_p95[0U],closure_device_p95[1U],
        materialize_device_p95[0U],materialize_device_p95[1U],
        closure_attribution_ok?"true":"false",
        closure_clear_device_json.c_str(),closure_initial_canonical_device_json.c_str(),
        closure_green_device_json.c_str(),closure_red_clear_device_json.c_str(),
        closure_red_work_device_json.c_str(),closure_followup_canonical_device_json.c_str(),
        p8_device_p95[0U],p8_device_p95[1U],
        p8_count_device_p95[0U],p8_count_device_p95[1U],
        p8_scan_device_p95[0U],p8_scan_device_p95[1U],
        p8_emit_device_p95[0U],p8_emit_device_p95[1U],
        p8_finalize_copy_device_p95[0U],p8_finalize_copy_device_p95[1U],
        minimum_active,maximum_active,minimum_red_rounds,maximum_red_rounds,
        minimum_owners,maximum_owners,minimum_triangles,maximum_triangles,
        materially_improved?"true":"false",
        materially_improved?"promoted_gpu_default":"rejected_promotion_gate",
        static_cast<unsigned long long>(selection.cpu_generation_violations),
        benchmark_valid?"true":"false");
  }
  const bool passed=moving_parity&&image_parity&&field_parity&&failure_retains&&stale_rejected&&benchmark_valid;
  std::printf("{\"event\":\"metal_gpu_compact_live_parity\",\"first_oracle\":%s,"
              "\"first_encoded\":%s,\"first_owner_parity\":%s,\"first_read\":%s,"
              "\"root_seam\":%s,\"mixed_depth\":%s,\"moving_camera\":%s,"
              "\"field_change\":%s,\"image_parity\":%s,\"failure_retains\":%s,"
              "\"stale_revision\":%s,\"p8_hybrid\":%s,\"passed\":%s}\n",
      first_oracle?"true":"false",first_encoded?"true":"false",
      first_owner_parity?"true":"false",first_read?"true":"false",
      first_committed?"true":"false",first_oracle?"true":"false",
      moving_parity?"true":"false",field_parity?"true":"false",
      image_parity?"true":"false",failure_retains?"true":"false",
      stale_rejected?"true":"false",use_hybrid?"true":"false",passed?"true":"false");
  return passed;
}

// This is deliberately an integration qualification, rather than another
// compact-fixture oracle.  The reference is the exact `PreparedScene` that
// BlockedTerrainRuntime has published for the camera/field/revision under
// test.  The device result is generated only through the compact selector,
// closure, owner materializer, and P8 emitter; the two shared buffers below
// are post-completion test readback and are never inputs to that path.
bool run_metal_gpu_production_front_parity_smoke_test(id<MTLDevice> device) {
  const auto shaders=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR);
  const auto pipeline_for=[&](const char* name)->id<MTLComputePipelineState>{
    id<MTLLibrary> library=make_file_shader_library(device,(shaders/name).string().c_str());
    NSError* error=nil;id<MTLFunction> function=library==nil?nil:[library newFunctionWithName:@"main0"];
    return function==nil?nil:[device newComputePipelineStateWithFunction:function error:&error];
  };
  id<MTLComputePipelineState> selector=pipeline_for("gpu_lod.comp.metal");
  id<MTLComputePipelineState> worklist=pipeline_for("gpu_hierarchy_compact_worklist.comp.metal");
  id<MTLComputePipelineState> canonicalize=pipeline_for("gpu_hierarchy_canonicalize.comp.metal");
  id<MTLComputePipelineState> green=pipeline_for("gpu_hierarchy_compact_green_closure.comp.metal");
  id<MTLComputePipelineState> red=pipeline_for("gpu_hierarchy_compact_red_repair.comp.metal");
  id<MTLComputePipelineState> red_scan=pipeline_for("gpu_hierarchy_compact_red_scan.comp.metal");
  id<MTLComputePipelineState> materialize=pipeline_for("gpu_hierarchy_compact_owner_materialize.comp.metal");
  MetalCompactOwnerP8Pipelines p8{pipeline_for("gpu_terrain_compact_owner_control.comp.metal"),
      pipeline_for("gpu_terrain_compact_owner_count.comp.metal"),pipeline_for("gpu_terrain_compact_owner_scan.comp.metal"),
      pipeline_for("gpu_terrain_compact_owner_emit.comp.metal"),pipeline_for("gpu_terrain_compact_owner_triangle_emit.comp.metal"),pipeline_for("gpu_terrain_compact_owner_validate.comp.metal"),
      pipeline_for("gpu_terrain_compact_owner_copy.comp.metal"),pipeline_for("gpu_terrain_compact_owner_publish.comp.metal"),
      pipeline_for("gpu_terrain_compact_owner_microbatch.comp.metal"),pipeline_for("gpu_terrain_compact_owner_microbatch_validate.comp.metal"),
      pipeline_for("gpu_terrain_compact_owner_hybrid_scan.comp.metal"),pipeline_for("gpu_terrain_compact_owner_hybrid_finalize.comp.metal")};
  id<MTLCommandQueue> queue=[device newCommandQueue];
  if(selector==nil||worklist==nil||canonicalize==nil||green==nil||red==nil||
     red_scan==nil||materialize==nil||p8.control==nil||p8.count==nil||p8.scan==nil||
     p8.emit==nil||p8.validate==nil||p8.copy==nil||p8.publish==nil||queue==nil)return false;

  struct QuantizedVertex { std::array<std::int64_t,6> lanes{}; auto operator<=>(const QuantizedVertex&) const=default; };
  using QuantizedTriangle=std::array<QuantizedVertex,3>;
  const auto canonical_triangles=[](std::span<const tetra_viewer::SceneVertex> vertices) {
    std::vector<QuantizedTriangle> result;result.reserve(vertices.size()/3U);
    constexpr double position_unit=1.e-3,normal_unit=2.e-3;
    const auto quantize=[](float value,double unit) { return static_cast<std::int64_t>(std::llround(static_cast<double>(value)/unit)); };
    for(std::size_t index=0U;index+2U<vertices.size();index+=3U) {
      QuantizedTriangle triangle{};
      for(std::size_t corner=0U;corner<3U;++corner)for(std::size_t axis=0U;axis<3U;++axis) {
        triangle[corner].lanes[axis]=quantize(vertices[index+corner].position[axis],position_unit);
        triangle[corner].lanes[axis+3U]=quantize(vertices[index+corner].normal[axis],normal_unit);
      }
      std::ranges::sort(triangle);result.push_back(triangle);
    }
    std::ranges::sort(result);return result;
  };
  // A full camera-frame CPU rasterizer.  It records nearest reversed-Z depth
  // and coverage after the same camera projection as the renderer.  It is
  // independent from the P8 owner/cell ordering comparison above.
  const auto frame_capture=[](std::span<const tetra_viewer::SceneVertex> vertices,
                              const tetra::Camera& camera,tetra::Vec3 origin) {
    constexpr int width=160,height=90;
    struct Capture { std::array<std::uint8_t,width*height> coverage{}; std::array<std::uint16_t,width*height> depth{}; } out;
    const auto projection=tetra_viewer::make_infinite_reversed_projection(
        camera.position,origin,camera.forward,camera.up,camera.vertical_fov_radians,
        camera.aspect_ratio);
    const auto edge=[](const std::array<double,2>& a,const std::array<double,2>& b,double x,double y) {
      return (x-a[0])*(b[1]-a[1])-(y-a[1])*(b[0]-a[0]);
    };
    for(std::size_t base=0U;base+2U<vertices.size();base+=3U) {
      std::array<std::array<double,2>,3> p{};std::array<double,3> z{};bool usable=true;
      for(std::size_t corner=0U;corner<3U;++corner) {
        const auto q=projection.project({vertices[base+corner].position[0],vertices[base+corner].position[1],vertices[base+corner].position[2]});
        // The production front is already tessellated finely; excluding a
        // straddling edge rather than inventing a clipping path keeps this
        // oracle deterministic while still covering the entire visible frame.
        usable&=q.visible;p[corner]={(q.ndc_x*.5+.5)*(width-1),(q.ndc_y*.5+.5)*(height-1)};z[corner]=q.depth;
      }
      if(!usable)continue;
      const double area=edge(p[0],p[1],p[2][0],p[2][1]);if(std::abs(area)<1.e-12)continue;
      const int xmin=std::max(0,static_cast<int>(std::floor(std::min({p[0][0],p[1][0],p[2][0]}))));
      const int xmax=std::min(width-1,static_cast<int>(std::ceil(std::max({p[0][0],p[1][0],p[2][0]}))));
      const int ymin=std::max(0,static_cast<int>(std::floor(std::min({p[0][1],p[1][1],p[2][1]}))));
      const int ymax=std::min(height-1,static_cast<int>(std::ceil(std::max({p[0][1],p[1][1],p[2][1]}))));
      for(int y=ymin;y<=ymax;++y)for(int x=xmin;x<=xmax;++x) {
        const double a=edge(p[1],p[2],x+.5,y+.5)/area,b=edge(p[2],p[0],x+.5,y+.5)/area,c=1.0-a-b;
        if(a<0.0||b<0.0||c<0.0)continue;
        const auto index=static_cast<std::size_t>(y*width+x);const double depth=a*z[0]+b*z[1]+c*z[2];
        const auto packed=static_cast<std::uint16_t>(std::clamp(std::llround(depth*65535.0),0LL,65535LL));
        if(!out.coverage[index]||packed>out.depth[index]) { out.coverage[index]=1U;out.depth[index]=packed; }
      }
    }
    return out;
  };
  const auto wait_for_front=[](tetra_viewer::TerrainRuntime& runtime) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(120);
    while(std::chrono::steady_clock::now()<deadline) {
      static_cast<void>(runtime.update());const auto d=runtime.diagnostics();
      if(d.converged&&!d.busy&&runtime.world_cut_directory()!=nullptr&&
         !runtime.scene().triangle_vertices.empty())return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  };
  const auto run_case=[&](tetra_viewer::TerrainRuntime& runtime,const tetra::Camera& camera,
                          const char* name) {
    if(!wait_for_front(runtime)) { std::printf("production parity %s: runtime did not publish\n",name);return false; }
    const auto& cpu=runtime.scene().triangle_vertices;const auto* directory=runtime.world_cut_directory();
    const auto profile=runtime.profile();const auto field_revision=runtime.published_view_identity().field_revision;
    const auto source_revision=directory==nullptr?0U:directory->revision();const auto origin=runtime.render_origin();
    if(directory==nullptr||source_revision==0U||cpu.empty()||cpu.size()%3U!=0U||
       cpu.size()>std::numeric_limits<std::uint32_t>::max()) { std::printf("production parity %s: invalid CPU publication\n",name);return false; }
    std::size_t cpu_logical_owners{};
    directory->for_each_logical_owner([&](tetra::WorldTetAddress){++cpu_logical_owners;});
    const auto snapshot=tetra::make_gpu_hierarchy_snapshot(*directory,field_revision,true);
    MetalGpuHierarchyLiveSelection selection;
    if(!configure_metal_gpu_hierarchy_live_selection(device,selection,snapshot,field_revision,
        runtime.diagnostics().scene_generation)) { std::printf("production parity %s: GPU snapshot configuration failed (records=%zu)\n",name,snapshot.records.size());return false; }
    selection.require_complete_front=true;
    auto surface_field=runtime.field();surface_field.sampling_footprint=
        tetra_viewer::planetary_surface_sampling_footprint(runtime.field(),camera,profile.pixel_threshold);
    tetra::GpuTerrainFieldTupleParameters parameters{.field=surface_field,.domain=profile.domain,
        .source_revision=source_revision,.field_revision=field_revision};
    const auto tuple_field=tetra::make_gpu_terrain_field_tuple(parameters);
    const auto templates=tetra::make_gpu_green_template_table();
    selection.compact_p8_field=[device newBufferWithBytes:&tuple_field length:sizeof(tuple_field) options:MTLResourceStorageModeShared];
    selection.compact_p8_templates=[device newBufferWithBytes:templates.data() length:sizeof(templates) options:MTLResourceStorageModeShared];
    const auto capacity=static_cast<std::uint32_t>(cpu.size());
    id<MTLBuffer> gpu=[device newBufferWithLength:static_cast<NSUInteger>(capacity)*sizeof(tetra_viewer::SceneVertex) options:MTLResourceStorageModePrivate];
    id<MTLBuffer> args=[device newBufferWithLength:4U*sizeof(std::uint32_t) options:MTLResourceStorageModePrivate];
    id<MTLBuffer> gpu_readback=[device newBufferWithLength:gpu==nil?0U:gpu.length options:MTLResourceStorageModeShared];
    id<MTLBuffer> args_readback=[device newBufferWithLength:4U*sizeof(std::uint32_t) options:MTLResourceStorageModeShared];
    id<MTLBuffer> owners_readback=[device newBufferWithLength:4U*sizeof(std::uint32_t) options:MTLResourceStorageModeShared];
    id<MTLBuffer> closure_headers_readback=[device newBufferWithLength:24U*sizeof(std::uint32_t) options:MTLResourceStorageModeShared];
    if(selection.compact_p8_field==nil||selection.compact_p8_templates==nil||gpu==nil||args==nil||gpu_readback==nil||
       args_readback==nil||owners_readback==nil||closure_headers_readback==nil||!ensure_metal_gpu_hierarchy_compact_p8_workspace(device,selection,capacity)) { std::printf("production parity %s: GPU workspace allocation failed\n",name);return false; }
    auto selector_camera=camera;selector_camera.position=profile.domain.to_root(camera.position);
    const auto select_tuple=tetra::make_gpu_hierarchy_selection_tuple({.camera=selector_camera,.render_origin={},
      .field_centre=profile.domain.to_root(runtime.field().centre),.planet_radius=runtime.field().terrain.planet_radius/profile.domain.world_extent,
      .terrain_height_bound=tetra::terrain_height_magnitude_bound(runtime.field())/profile.domain.world_extent,
      .field_lipschitz=tetra::implicit_field_lipschitz_bound(runtime.field())*profile.domain.world_extent,
      .edge_threshold=profile.pixel_threshold,.field_threshold=profile.field_error_pixel_threshold,
      .limb_threshold=profile.limb_error_pixel_threshold,.merge_ratio=profile.lod_merge_threshold_ratio,
      .source_revision=source_revision,.field_revision=field_revision});
    id<MTLCommandBuffer> command=[queue commandBuffer];
    const bool encoded=encode_metal_gpu_hierarchy_live_selection(command,selector,worklist,selection,select_tuple)&&
      encode_metal_gpu_hierarchy_live_compact_closure(command,canonicalize,green,red,red_scan,selection,false)&&
      encode_metal_gpu_hierarchy_compact_owner_materialize(command,materialize,selection)&&
      encode_metal_gpu_hierarchy_compact_owner_p8(command,p8,selection.compact_owner_stream,selection.compact_owner_header,
        selection.compact_p8_field,selection.compact_p8_templates,selection.compact_p8_counts,selection.compact_p8_offsets,
        selection.compact_p8_block_totals,selection.compact_p8_block_offsets,selection.compact_p8_level_totals,
        selection.compact_p8_level_offsets,selection.compact_p8_signs,selection.compact_p8_candidate,selection.compact_p8_status,
        selection.compact_p8_dispatches,gpu,args,capacity,origin,source_revision);
    if(!encoded) { std::printf("production parity %s: GPU chain encode failed\n",name);return false; }++selection.compact_p8_encoded;
    id<MTLBlitCommandEncoder> copy=[command blitCommandEncoder];
    [copy copyFromBuffer:gpu sourceOffset:0U toBuffer:gpu_readback destinationOffset:0U size:gpu.length];
    [copy copyFromBuffer:args sourceOffset:0U toBuffer:args_readback destinationOffset:0U size:args.length];
    [copy copyFromBuffer:selection.compact_owner_header sourceOffset:0U toBuffer:owners_readback destinationOffset:0U size:owners_readback.length];
    [copy copyFromBuffer:selection.compact_selected_ping sourceOffset:0U toBuffer:closure_headers_readback destinationOffset:0U size:4U*sizeof(std::uint32_t)];
    [copy copyFromBuffer:selection.compact_selected_pong sourceOffset:0U toBuffer:closure_headers_readback destinationOffset:4U*sizeof(std::uint32_t) size:4U*sizeof(std::uint32_t)];
    [copy copyFromBuffer:selection.compact_green_masks sourceOffset:0U toBuffer:closure_headers_readback destinationOffset:8U*sizeof(std::uint32_t) size:4U*sizeof(std::uint32_t)];
    [copy copyFromBuffer:selection.slots[selection.closure_slot_index].marks sourceOffset:0U toBuffer:closure_headers_readback destinationOffset:12U*sizeof(std::uint32_t) size:4U*sizeof(std::uint32_t)];
    [copy copyFromBuffer:selection.compact_closure_queue_ping sourceOffset:0U toBuffer:closure_headers_readback destinationOffset:16U*sizeof(std::uint32_t) size:4U*sizeof(std::uint32_t)];
    [copy copyFromBuffer:selection.compact_closure_queue_pong sourceOffset:0U toBuffer:closure_headers_readback destinationOffset:20U*sizeof(std::uint32_t) size:4U*sizeof(std::uint32_t)];
    [copy endEncoding];[command commit];[command waitUntilCompleted];retire_metal_gpu_hierarchy_live_selection(selection);
    const auto* gpu_count=static_cast<const std::uint32_t*>(args_readback.contents);
    const auto* owners=static_cast<const std::uint32_t*>(owners_readback.contents);
    const auto* closure_headers=static_cast<const std::uint32_t*>(closure_headers_readback.contents);
    const auto gpu_vertices=gpu_count==nullptr?0U:gpu_count[0U];
    const auto gpu_triangles=gpu_vertices/3U;
    const auto cpu_triangles=cpu.size()/3U;
    std::printf("production parity %s: cpu_triangles=%zu cpu_logical_owners=%zu gpu_owners=%u gpu_triangles=%u selected=%u final=%u ping=%u/%u/%u/%u pong=%u/%u/%u/%u masks=%u/%u/%u/%u selector=%u/%u/%u/%u queue_ping=%u/%u/%u/%u queue_pong=%u/%u/%u/%u closure=%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u p8=%u/%u/%u/%u\n",
        name,cpu_triangles,cpu_logical_owners,owners==nullptr?0U:owners[0U],gpu_triangles,selection.compact_last_selected_header[0U],
        selection.compact_last_final_active_header[0U],
        closure_headers==nullptr?0U:closure_headers[0U],closure_headers==nullptr?0U:closure_headers[1U],closure_headers==nullptr?0U:closure_headers[2U],closure_headers==nullptr?0U:closure_headers[3U],
        closure_headers==nullptr?0U:closure_headers[4U],closure_headers==nullptr?0U:closure_headers[5U],closure_headers==nullptr?0U:closure_headers[6U],closure_headers==nullptr?0U:closure_headers[7U],
        closure_headers==nullptr?0U:closure_headers[8U],closure_headers==nullptr?0U:closure_headers[9U],closure_headers==nullptr?0U:closure_headers[10U],closure_headers==nullptr?0U:closure_headers[11U],
        closure_headers==nullptr?0U:closure_headers[12U],closure_headers==nullptr?0U:closure_headers[13U],closure_headers==nullptr?0U:closure_headers[14U],closure_headers==nullptr?0U:closure_headers[15U],
        closure_headers==nullptr?0U:closure_headers[16U],closure_headers==nullptr?0U:closure_headers[17U],closure_headers==nullptr?0U:closure_headers[18U],closure_headers==nullptr?0U:closure_headers[19U],
        closure_headers==nullptr?0U:closure_headers[20U],closure_headers==nullptr?0U:closure_headers[21U],closure_headers==nullptr?0U:closure_headers[22U],closure_headers==nullptr?0U:closure_headers[23U],
        selection.compact_last_closure_audit[0U],selection.compact_last_closure_audit[1U],selection.compact_last_closure_audit[2U],
        selection.compact_last_closure_audit[3U],selection.compact_last_closure_audit[4U],selection.compact_last_closure_audit[5U],
        selection.compact_last_closure_audit[6U],selection.compact_last_closure_audit[7U],selection.compact_last_closure_audit[8U],
        selection.compact_last_closure_audit[9U],selection.compact_last_closure_audit[10U],selection.compact_last_closure_audit[11U],
        selection.compact_last_closure_audit[12U],selection.compact_p8_last_audit[0U],selection.compact_p8_last_audit[1U],
        selection.compact_p8_last_audit[2U],selection.compact_p8_last_audit[4U]);
    if(command.status!=MTLCommandBufferStatusCompleted||gpu_count==nullptr||owners==nullptr||gpu_vertices!=cpu.size()||
       gpu_vertices%3U!=0U)return false;
    const auto* gpu_vertices_data=static_cast<const tetra_viewer::SceneVertex*>(gpu_readback.contents);
    if(gpu_vertices_data==nullptr)return false;
    const std::span gpu_span{gpu_vertices_data,static_cast<std::size_t>(gpu_vertices)};
    const auto cpu_topology=canonical_triangles(cpu);const auto gpu_topology=canonical_triangles(gpu_span);
    if(cpu_topology!=gpu_topology) { std::printf("production parity %s: canonical topology/payload differs\n",name);return false; }
    const auto cpu_frame=frame_capture(cpu,camera,origin),gpu_frame=frame_capture(gpu_span,camera,origin);
    std::size_t coverage_mismatch{},depth_mismatch{};
    for(std::size_t pixel=0U;pixel<cpu_frame.coverage.size();++pixel) {
      coverage_mismatch+=cpu_frame.coverage[pixel]!=gpu_frame.coverage[pixel];
      depth_mismatch+=cpu_frame.coverage[pixel]&&gpu_frame.coverage[pixel]&&
          std::abs(static_cast<int>(cpu_frame.depth[pixel])-static_cast<int>(gpu_frame.depth[pixel]))>2;
    }
    if(coverage_mismatch!=0U||depth_mismatch!=0U) {
      std::printf("production parity %s: full-frame coverage/depth mismatch=%zu/%zu\n",name,coverage_mismatch,depth_mismatch);return false;
    }
    return true;
  };
  tetra::Camera first;first.position={0.5,0.72,0.68};first.forward={0.0,-0.2,-1.0};first.up={0.0,1.0,0.0};
  first.viewport_height_pixels=800.0;first.aspect_ratio=16.0/9.0;
  auto runtime=tetra_viewer::make_production_terrain_runtime(tetra_viewer::production_world_profile(),first);
  const bool static_parity=run_case(*runtime,first,"static");
  tetra::Camera moved=first;moved.position.x+=0.45;moved.position.z-=0.3;moved.forward={-.15,-.2,-1.0};
  runtime->set_camera(moved,false);
  const bool motion_rebase=static_parity&&run_case(*runtime,moved,"motion_rebase");
  auto changed_profile=tetra_viewer::production_world_profile();changed_profile.terrain.height_offset+=0.125;
  auto changed=tetra_viewer::make_production_terrain_runtime(changed_profile,moved);
  const bool field_change=motion_rebase&&run_case(*changed,moved,"field_change");
  const bool passed=static_parity&&motion_rebase&&field_change;
  std::printf("{\"event\":\"metal_gpu_production_front_parity\",\"static\":%s,\"motion_rebase\":%s,\"field_change\":%s,\"passed\":%s}\n",
      static_parity?"true":"false",motion_rebase?"true":"false",field_change?"true":"false",passed?"true":"false");
  return passed;
}

// This deliberately bypasses selector/closure scheduling: P7e4a1's oracle is
// the lossless, word-for-word projection of a stable compact list through the
// immutable snapshot sidecars.  The test makes the private final pair visible
// only through a test-only blit readback; no owner payload reaches a renderer.
bool run_metal_gpu_compact_owner_materialize_smoke_test(id<MTLDevice> device) {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  for(unsigned generation=0U;generation<2U;++generation)mesh.refine_all_binary();
  std::vector<tetra::WorldTetAddress> owners;
  for(const auto owner:mesh.logical_red_owners())owners.push_back(tetra::world_tet_address(owner));
  const tetra::WorldCutDirectory directory(tetra::make_sparse_world_cut_checkpoint(
      owners,1U,97U,tetra::HierarchyResidencyTier::surface));
  const auto snapshot=tetra::make_gpu_hierarchy_snapshot(directory,101U);
  const auto shader_path=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR)/
      "gpu_hierarchy_compact_owner_materialize.comp.metal";
  id<MTLLibrary> library=make_file_shader_library(device,shader_path.string().c_str());
  NSError* error=nil;
  id<MTLComputePipelineState> pipeline=library==nil?nil:
      [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"main0"]
                                             error:&error];
  id<MTLCommandQueue> queue=[device newCommandQueue];
  MetalGpuHierarchyLiveSelection selection;
  if(pipeline==nil||queue==nil||
     !configure_metal_gpu_hierarchy_live_selection(device,selection,snapshot,101U,13U)||
     !selection.compact_owner_ready()||snapshot.records.size()<3U)return false;
  const std::array<std::uint32_t,3> selected{
      snapshot.canonical_record_indices[0U],
      snapshot.canonical_record_indices[snapshot.canonical_record_indices.size()/2U],
      snapshot.canonical_record_indices.back()};
  std::vector<std::uint32_t> expected(selected.size()*12U);
  for(std::size_t index=0U;index<selected.size();++index) {
    const auto record=selected[index];
    const auto output=index*12U;
    std::copy_n(snapshot.records[record].address.begin(),4U,expected.begin()+output);
    std::copy_n(snapshot.edge_topology[record].edge_ranges.begin(),6U,
                expected.begin()+output+4U);
    expected[output+10U]=static_cast<std::uint32_t>(index*9U);
    expected[output+11U]=snapshot.orientation_flags[record];
  }
  const auto read_words=[&](id<MTLBuffer> buffer,std::size_t words) {
    return [device newBufferWithLength:std::max<NSUInteger>(words*sizeof(std::uint32_t),4U)
                                options:MTLResourceStorageModeShared];
  };
  const auto run=[&](const std::vector<std::uint32_t>& active,
                     const std::vector<std::uint32_t>& masks,
                     const std::vector<std::uint32_t>& initial,
                     std::uint32_t owner_capacity,
                     std::vector<std::uint32_t>& result,
                     std::array<std::uint32_t,4>& header) {
    id<MTLBuffer> active_source=[device newBufferWithBytes:active.data()
        length:active.size()*sizeof(std::uint32_t) options:MTLResourceStorageModeShared];
    id<MTLBuffer> masks_source=[device newBufferWithBytes:masks.data()
        length:masks.size()*sizeof(std::uint32_t) options:MTLResourceStorageModeShared];
    id<MTLBuffer> initial_source=[device newBufferWithBytes:initial.data()
        length:initial.size()*sizeof(std::uint32_t) options:MTLResourceStorageModeShared];
    id<MTLBuffer> output=read_words(selection.compact_owner_stream,
        initial.size());
    id<MTLBuffer> header_output=read_words(selection.compact_owner_header,4U);
    if(active_source==nil||masks_source==nil||initial_source==nil||
       output==nil||header_output==nil)return false;
    id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLBlitCommandEncoder> seed=[command blitCommandEncoder];
    [seed copyFromBuffer:active_source sourceOffset:0U
                toBuffer:selection.compact_final_active destinationOffset:0U
                    size:active.size()*sizeof(std::uint32_t)];
    [seed copyFromBuffer:masks_source sourceOffset:0U
                toBuffer:selection.compact_final_masks destinationOffset:0U
                    size:masks.size()*sizeof(std::uint32_t)];
    [seed copyFromBuffer:initial_source sourceOffset:0U
                toBuffer:selection.compact_owner_stream destinationOffset:0U
                    size:initial.size()*sizeof(std::uint32_t)];
    [seed endEncoding];
    const auto saved_capacity=selection.output_capacity;
    selection.output_capacity=owner_capacity;
    const bool encoded=encode_metal_gpu_hierarchy_compact_owner_materialize(
        command,pipeline,selection);
    selection.output_capacity=saved_capacity;
    if(!encoded)return false;
    id<MTLBlitCommandEncoder> readback=[command blitCommandEncoder];
    [readback copyFromBuffer:selection.compact_owner_stream sourceOffset:0U
                   toBuffer:output destinationOffset:0U
                       size:initial.size()*sizeof(std::uint32_t)];
    [readback copyFromBuffer:selection.compact_owner_header sourceOffset:0U
                   toBuffer:header_output destinationOffset:0U
                       size:4U*sizeof(std::uint32_t)];
    [readback endEncoding]; [command commit]; [command waitUntilCompleted];
    const auto* output_words=static_cast<const std::uint32_t*>(output.contents);
    const auto* header_words=static_cast<const std::uint32_t*>(header_output.contents);
    if(command.status!=MTLCommandBufferStatusCompleted||output_words==nullptr||
       header_words==nullptr)return false;
    result.assign(output_words,output_words+initial.size());
    std::copy_n(header_words,4U,header.begin());
    return true;
  };
  std::vector<std::uint32_t> active(4U+snapshot.records.size(),0U),
      masks(4U+snapshot.records.size(),0U);
  active[0U]=static_cast<std::uint32_t>(selected.size());
  active[1U]=static_cast<std::uint32_t>(snapshot.records.size());
  active[3U]=1U;
  masks[0U]=active[0U]; masks[1U]=active[1U]; masks[3U]=1U;
  for(std::size_t index=0U;index<selected.size();++index) {
    active[4U+index]=selected[index];
    masks[4U+index]=expected[index*12U+10U];
  }
  const std::vector<std::uint32_t> sentinel(snapshot.records.size()*12U,0xdecafbadU);
  std::vector<std::uint32_t> result;
  std::array<std::uint32_t,4> header{};
  if(!run(active,masks,sentinel,static_cast<std::uint32_t>(snapshot.records.size()),
          result,header))return false;
  const bool word_parity=header==std::array<std::uint32_t,4>{
      static_cast<std::uint32_t>(selected.size()),
      static_cast<std::uint32_t>(snapshot.records.size()),0U,1U}&&
      std::equal(expected.begin(),expected.end(),result.begin());
  active[2U]=1U;
  std::vector<std::uint32_t> malformed_result;
  std::array<std::uint32_t,4> malformed_header{};
  const bool malformed_retained=run(active,masks,sentinel,
      static_cast<std::uint32_t>(snapshot.records.size()),malformed_result,
      malformed_header)&&malformed_result==sentinel&&malformed_header[0U]==0U&&
      malformed_header[2U]==1U&&malformed_header[3U]==0U;
  active[2U]=0U;
  std::vector<std::uint32_t> capacity_result;
  std::array<std::uint32_t,4> capacity_header{};
  const bool capacity_retained=run(active,masks,sentinel,
      static_cast<std::uint32_t>(selected.size()-1U),capacity_result,
      capacity_header)&&capacity_result==sentinel&&capacity_header[0U]==0U&&
      capacity_header[2U]==4U&&capacity_header[3U]==0U;
  active[4U]=static_cast<std::uint32_t>(snapshot.records.size());
  std::vector<std::uint32_t> record_result;
  std::array<std::uint32_t,4> record_header{};
  const bool record_retained=run(active,masks,sentinel,
      static_cast<std::uint32_t>(snapshot.records.size()),record_result,
      record_header)&&record_result==sentinel&&record_header[0U]==0U&&
      record_header[2U]==2U&&record_header[3U]==0U;
  active[4U]=selected[0U];
  masks[4U]=64U;
  std::vector<std::uint32_t> mask_result;
  std::array<std::uint32_t,4> mask_header{};
  const bool mask_failed=run(active,masks,sentinel,
      static_cast<std::uint32_t>(snapshot.records.size()),mask_result,
      mask_header)&&mask_result==sentinel&&mask_header[0U]==0U&&mask_header[2U]==8U&&
      mask_header[3U]==0U;
  const bool passed=word_parity&&malformed_retained&&capacity_retained&&
      record_retained&&mask_failed;
  std::printf("{\"event\":\"metal_gpu_compact_owner_materialize\","
              "\"word_parity\":%s,\"malformed_retained\":%s,"
              "\"capacity_retained\":%s,\"record_retained\":%s,"
              "\"mask_failed\":%s,\"passed\":%s}\n",
      word_parity?"true":"false",malformed_retained?"true":"false",
      capacity_retained?"true":"false",record_retained?"true":"false",
      mask_failed?"true":"false",
      passed?"true":"false");
  return passed;
}

// P7e3a chains the real selector marks through a deterministic count/scan/
// scatter frontier build.  The fixture permits shared readback solely for its
// oracle; the produced records are not a P6 packet and have no renderer
// consumer until the following incidence and closure leaves are complete.
bool run_metal_gpu_hierarchy_frontier_smoke_test(id<MTLDevice> device) {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  for(unsigned int generation=0U;generation<3U;++generation)
    mesh.refine_all_binary();
  std::vector<tetra::WorldTetAddress> leaves;
  for(const auto owner:mesh.logical_red_owners())
    leaves.push_back(tetra::world_tet_address(owner));
  const tetra::WorldCutDirectory directory(tetra::make_sparse_world_cut_checkpoint(
      leaves,1U,61U,tetra::HierarchyResidencyTier::surface));
  const auto snapshot=tetra::make_gpu_hierarchy_snapshot(directory,67U);
  if(snapshot.records.empty()||snapshot.canonical_record_indices.size()!=
      snapshot.records.size())return false;
  if(snapshot.orientation_flags.size()!=snapshot.records.size()||
     std::ranges::any_of(snapshot.orientation_flags,
        [](std::uint32_t value){return value>1U;})){
    std::fprintf(stderr,"Metal hierarchy fixture has invalid CPU orientations\\n");
    return false;
  }
  const auto shader=[&](const char* name)->id<MTLComputePipelineState>{
    const auto path=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR)/name;
    id<MTLLibrary> library=make_file_shader_library(device,path.string().c_str());
    NSError* error=nil;
    id<MTLFunction> function=library==nil?nil:[library newFunctionWithName:@"main0"];
    id<MTLComputePipelineState> result=function==nil?nil:
        [device newComputePipelineStateWithFunction:function error:&error];
    if(result==nil)std::fprintf(stderr,"Metal hierarchy-frontier pipeline %s failed: %s\n",
        name,error==nil?"missing translated entry point":error.localizedDescription.UTF8String);
    return result;
  };
  id<MTLComputePipelineState> select=shader("gpu_lod.comp.metal");
  id<MTLComputePipelineState> frontier=shader("gpu_hierarchy_frontier.comp.metal");
  id<MTLComputePipelineState> closure=shader("gpu_hierarchy_closure.comp.metal");
  id<MTLComputePipelineState> scan=shader("gpu_terrain_exclusive_scan.comp.metal");
  id<MTLCommandQueue> queue=[device newCommandQueue];
  if(select==nil||frontier==nil||closure==nil||scan==nil||queue==nil)return false;
  const auto make=[&](const void* bytes,NSUInteger length){
    return [device newBufferWithBytes:bytes length:std::max<NSUInteger>(length,4U)
        options:MTLResourceStorageModeShared];
  };
  id<MTLBuffer> hierarchy=make(snapshot.records.data(),
      snapshot.records.size()*sizeof(snapshot.records.front()));
  id<MTLBuffer> children=make(snapshot.child_indices.data(),
      snapshot.child_indices.size()*sizeof(snapshot.child_indices.front()));
  id<MTLBuffer> inputs=make(snapshot.selection_records.data(),
      snapshot.selection_records.size()*sizeof(snapshot.selection_records.front()));
  std::vector<std::uint32_t> root_indices;
  for(std::uint32_t index=0U;index<snapshot.records.size();++index)
    if((snapshot.records[index].child_mask_flags&0x800U)!=0U)
      root_indices.push_back(index);
  id<MTLBuffer> roots=root_indices.empty()||root_indices.size()>12U?nil:
      make(root_indices.data(),root_indices.size()*sizeof(std::uint32_t));
  id<MTLBuffer> canonical=make(snapshot.canonical_record_indices.data(),
      snapshot.canonical_record_indices.size()*sizeof(std::uint32_t));
  id<MTLBuffer> faces=make(snapshot.face_incidence.data(),
      snapshot.face_incidence.size()*sizeof(snapshot.face_incidence.front()));
  id<MTLBuffer> edge_topology=make(snapshot.edge_topology.data(),
      snapshot.edge_topology.size()*sizeof(snapshot.edge_topology.front()));
  id<MTLBuffer> ancestors=make(snapshot.ancestor_edge_ranges.data(),
      snapshot.ancestor_edge_ranges.size()*sizeof(std::uint32_t));
  id<MTLBuffer> orientations=make(snapshot.orientation_flags.data(),
      snapshot.orientation_flags.size()*sizeof(std::uint32_t));
  id<MTLBuffer> vertex_topology=make(snapshot.vertex_topology.data(),
      snapshot.vertex_topology.size()*sizeof(snapshot.vertex_topology.front()));
  id<MTLBuffer> vertex_ranges=make(snapshot.vertex_ranges.data(),
      snapshot.vertex_ranges.size()*sizeof(snapshot.vertex_ranges.front()));
  id<MTLBuffer> vertex_incidence=make(snapshot.vertex_incidence.data(),
      snapshot.vertex_incidence.size()*sizeof(snapshot.vertex_incidence.front()));
  if(hierarchy==nil||children==nil||inputs==nil||roots==nil||canonical==nil||faces==nil||
     edge_topology==nil||ancestors==nil||orientations==nil||vertex_topology==nil||
     vertex_ranges==nil||vertex_incidence==nil)return false;
  const auto make_tuple=[](tetra::Vec3 position,tetra::Vec3 forward,
                           float edge,float field,float limb){
    return tetra::make_gpu_hierarchy_selection_tuple({
        .camera={.position=position,.viewport_height_pixels=800.0,
                 .forward=forward,.up={0.0,1.0,0.0},.aspect_ratio=1.0},
        .render_origin={},.field_centre={0.5,0.5,0.5},.planet_radius=2.0,
        .terrain_height_bound=0.1,.field_lipschitz=1.0,.edge_threshold=edge,
        .field_threshold=field,.limb_threshold=limb,.merge_ratio=0.5,
        .source_revision=61U,.field_revision=67U});
  };
  enum class ManualCut : std::uint8_t { none,interior_mixed,root_seam,green_transition };
  enum class FailureCase : std::uint8_t { none,overflow,stale_selection,malformed_topology,malformed_orientation };
  struct Case { tetra::Vec3 position,forward; float edge,field,limb; FailureCase failure; ManualCut manual; };
  const std::array cases{
      Case{{.5,.5,3.},{0.,0.,-1.},1.e6F,1.e6F,1.e6F,FailureCase::none,ManualCut::none},
      Case{{.7,.5,2.8},{0.,0.,-1.},.5F,1.e6F,1.e6F,FailureCase::none,ManualCut::none},
      Case{{3.,.5,.5},{-1.,0.,0.},1.e6F,.05F,1.e6F,FailureCase::none,ManualCut::none},
      Case{{.5,.5,3.},{0.,0.,-1.},1.e6F,1.e6F,1.e6F,FailureCase::overflow,ManualCut::none},
      Case{{.5,.5,3.},{0.,0.,-1.},1.e6F,1.e6F,1.e6F,FailureCase::stale_selection,ManualCut::none},
      Case{{.5,.5,3.},{0.,0.,-1.},1.e6F,1.e6F,1.e6F,FailureCase::malformed_topology,ManualCut::none},
      Case{{.5,.5,3.},{0.,0.,-1.},1.e6F,1.e6F,1.e6F,FailureCase::malformed_orientation,ManualCut::none},
      Case{{.5,.5,3.},{0.,0.,-1.},1.e6F,1.e6F,1.e6F,FailureCase::none,ManualCut::interior_mixed},
      Case{{.5,.5,3.},{0.,0.,-1.},1.e6F,1.e6F,1.e6F,FailureCase::none,ManualCut::root_seam},
      Case{{.5,.5,3.},{0.,0.,-1.},1.e6F,1.e6F,1.e6F,FailureCase::none,ManualCut::green_transition}};
  const auto record_count=static_cast<std::uint32_t>(snapshot.records.size());
  const auto mark_words=(record_count+31U)/32U;
  const auto block_count=(record_count+255U)/256U;
  std::size_t completed{};bool saw_green_mask{};
  for(const auto& test:cases) {
    const auto tuple=make_tuple(test.position,test.forward,test.edge,test.field,test.limb);
    const auto oracle=tetra::gpu_hierarchy_traverse(snapshot,
        tetra::gpu_hierarchy_traversal_parameters(tuple));
    if(test.manual==ManualCut::none&&oracle.selected_records.empty())return false;
    const auto manually_selected=[&](tetra::WorldTetAddress address){
      const auto interior=tetra::WorldTetAddress::root(0U).child(0U);
      switch(test.manual) {
      case ManualCut::none:return false;
      case ManualCut::interior_mixed:
        return address==interior||(address.red_depth()==3U&&!(address.root_id()==0U&&
            address.ancestor(1U)==interior));
      case ManualCut::root_seam:
        return address.root_id()==0U?address.red_depth()==0U:address.red_depth()==3U;
      case ManualCut::green_transition:
        return address.root_id()==0U?address.red_depth()==1U:address.red_depth()==0U;
      }
      return false;
    };
    // The CPU closure is fixture-only oracle data. The GPU receives only its
    // requested P7e2 marks and immutable hierarchy sidecars; none of these
    // closed owners or masks is ever uploaded back into the device flow.
    std::vector<tetra::WorldTetAddress> requested;
    if(test.manual!=ManualCut::none) {
      for(std::uint32_t record=0U;record<record_count;++record) {
        const auto address=tetra::gpu_hierarchy_address_from_lanes(snapshot.records[record].address);
        if(manually_selected(address))requested.push_back(address);
      }
    } else for(const auto record:oracle.selected_records)
      requested.push_back(tetra::gpu_hierarchy_address_from_lanes(snapshot.records[record].address));
    std::ranges::sort(requested);if(requested.empty())return false;
    tetra::WorldConformingClosureCache closure_oracle;
    const auto expected=tetra::close_world_conforming_cut(requested,&closure_oracle);
    if(expected.empty()||expected.size()!=closure_oracle.green_masks.size())return false;
    const auto owner_capacity=test.failure==FailureCase::overflow?0U:
        static_cast<std::uint32_t>(record_count);
    std::vector<std::uint32_t> selection_words(4U+record_count+mark_words,0U);
    if(test.failure==FailureCase::stale_selection)selection_words[3U]=1U;
    if(test.manual!=ManualCut::none)for(std::uint32_t record=0U;record<record_count;++record) {
      const auto address=tetra::gpu_hierarchy_address_from_lanes(snapshot.records[record].address);
      if(!manually_selected(address))continue;
      selection_words[4U+record_count+(record>>5U)]|=1U<<(record&31U);
    }
    std::vector<std::uint32_t> zeros(record_count,0U);
    std::vector<std::uint32_t> block_zeros(block_count,0U);
    std::vector<std::uint32_t> owner_words(
        std::max<std::size_t>(1U,static_cast<std::size_t>(owner_capacity)*12U),0U);
    constexpr std::uint32_t retained_owner_sentinel=0xa5c3f17eU;
    std::vector<std::uint32_t> retained_owner_words(owner_words.size(),retained_owner_sentinel);
    const std::array<std::uint32_t,5> status_zeros{};
    id<MTLBuffer> tuple_buffer=make(&tuple,sizeof(tuple));
    id<MTLBuffer> selection_buffer=make(selection_words.data(),
        selection_words.size()*sizeof(std::uint32_t));
    id<MTLBuffer> counts=make(zeros.data(),zeros.size()*sizeof(std::uint32_t));
    id<MTLBuffer> offsets=make(zeros.data(),zeros.size()*sizeof(std::uint32_t));
    id<MTLBuffer> added_offsets=make(zeros.data(),zeros.size()*sizeof(std::uint32_t));
    id<MTLBuffer> block_totals=make(block_zeros.data(),block_zeros.size()*sizeof(std::uint32_t));
    id<MTLBuffer> block_offsets=make(block_zeros.data(),block_zeros.size()*sizeof(std::uint32_t));
    id<MTLBuffer> scan_total=make(status_zeros.data(),sizeof(status_zeros));
    id<MTLBuffer> owner_buffer=make(owner_words.data(),owner_words.size()*sizeof(std::uint32_t));
    id<MTLBuffer> retained_owner_buffer=make(retained_owner_words.data(),
        retained_owner_words.size()*sizeof(std::uint32_t));
    id<MTLBuffer> status=make(status_zeros.data(),sizeof(status_zeros));
    std::vector<std::uint32_t> inactive_edges(
        std::max<std::size_t>(1U,snapshot.edge_ranges.size()),0U);
    id<MTLBuffer> edge_marks=make(inactive_edges.data(),
        inactive_edges.size()*sizeof(std::uint32_t));
    std::vector<std::uint32_t> no_red_promotions(
        std::max<std::size_t>(1U,static_cast<std::size_t>(mark_words)),0U);
    id<MTLBuffer> red_promotions=make(no_red_promotions.data(),
        no_red_promotions.size()*sizeof(std::uint32_t));
    id<MTLBuffer> closure_status=make(status_zeros.data(),sizeof(status_zeros));
    id<MTLBuffer> case_vertex_ranges=vertex_ranges;
    id<MTLBuffer> case_orientations=orientations;
    if(test.failure==FailureCase::malformed_topology) {
      auto malformed=snapshot.vertex_ranges;
      malformed.front().count=0U;
      case_vertex_ranges=make(malformed.data(),malformed.size()*sizeof(malformed.front()));
    }
    if(test.failure==FailureCase::malformed_orientation) {
      std::vector<std::uint32_t> malformed(snapshot.orientation_flags.size(),2U);
      case_orientations=make(malformed.data(),malformed.size()*sizeof(std::uint32_t));
    }
    if(tuple_buffer==nil||selection_buffer==nil||counts==nil||offsets==nil||
       added_offsets==nil||block_totals==nil||block_offsets==nil||scan_total==nil||
       owner_buffer==nil||status==nil||edge_marks==nil||red_promotions==nil||
       closure_status==nil||retained_owner_buffer==nil||case_vertex_ranges==nil||
       case_orientations==nil)return false;
    id<MTLCommandBuffer> command=[queue commandBuffer];
    const std::array<std::uint32_t,4> selection_parameters{
        record_count,record_count,mark_words,
        static_cast<std::uint32_t>(root_indices.size())};
    id<MTLComputeCommandEncoder> encoder=nil;
    if(test.manual==ManualCut::none) {
      encoder=[command computeCommandEncoder];
      [encoder setComputePipelineState:select];
      [encoder setBuffer:hierarchy offset:0U atIndex:0U];
      [encoder setBuffer:children offset:0U atIndex:1U];
      [encoder setBuffer:inputs offset:0U atIndex:2U];
      [encoder setBuffer:tuple_buffer offset:0U atIndex:3U];
      [encoder setBytes:selection_parameters.data() length:sizeof(selection_parameters) atIndex:4U];
      [encoder setBuffer:roots offset:0U atIndex:5U];
      [encoder setBuffer:selection_buffer offset:0U atIndex:6U];
      [encoder dispatchThreads:MTLSizeMake(root_indices.size(),1U,1U)
           threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)]; [encoder endEncoding];
    }
    const std::array<std::uint32_t,5> count_parameters{
        record_count,record_count,mark_words,owner_capacity,0U};
    // P7e3a is deliberately repeated after every device-side red repair:
    // `counts` and `offsets` are a view of the selected-bit tail, never an
    // authority that may survive a changed cut.
    const auto encode_frontier=[&](id<MTLCommandBuffer> frontier_command){
      id<MTLComputeCommandEncoder> frontier_encoder=[frontier_command computeCommandEncoder];
      [frontier_encoder setComputePipelineState:frontier];
      [frontier_encoder setBytes:count_parameters.data() length:sizeof(count_parameters) atIndex:0U];
      [frontier_encoder setBuffer:selection_buffer offset:0U atIndex:1U]; [frontier_encoder setBuffer:hierarchy offset:0U atIndex:2U];
      [frontier_encoder setBuffer:status offset:0U atIndex:3U]; [frontier_encoder setBuffer:offsets offset:0U atIndex:4U];
      [frontier_encoder setBuffer:counts offset:0U atIndex:5U]; [frontier_encoder setBuffer:canonical offset:0U atIndex:6U];
      [frontier_encoder setBuffer:owner_buffer offset:0U atIndex:7U];
      [frontier_encoder dispatchThreads:MTLSizeMake(block_count*256U,1U,1U)
           threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)]; [frontier_encoder endEncoding];
      const std::array<std::uint32_t,2> scan_parameters{record_count,0U};
      frontier_encoder=[frontier_command computeCommandEncoder]; [frontier_encoder setComputePipelineState:scan];
      [frontier_encoder setBytes:scan_parameters.data() length:sizeof(scan_parameters) atIndex:0U];
      [frontier_encoder setBuffer:offsets offset:0U atIndex:1U]; [frontier_encoder setBuffer:counts offset:0U atIndex:2U];
      [frontier_encoder setBuffer:block_totals offset:0U atIndex:3U];
      [frontier_encoder dispatchThreads:MTLSizeMake(block_count*256U,1U,1U)
           threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)]; [frontier_encoder endEncoding];
      const std::array<std::uint32_t,2> block_scan_parameters{block_count,0U};
      frontier_encoder=[frontier_command computeCommandEncoder]; [frontier_encoder setComputePipelineState:scan];
      [frontier_encoder setBytes:block_scan_parameters.data() length:sizeof(block_scan_parameters) atIndex:0U];
      [frontier_encoder setBuffer:block_offsets offset:0U atIndex:1U]; [frontier_encoder setBuffer:block_totals offset:0U atIndex:2U];
      [frontier_encoder setBuffer:scan_total offset:0U atIndex:3U];
      [frontier_encoder dispatchThreads:MTLSizeMake(256U,1U,1U)
           threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)]; [frontier_encoder endEncoding];
      const std::array<std::uint32_t,2> add_parameters{record_count,1U};
      frontier_encoder=[frontier_command computeCommandEncoder]; [frontier_encoder setComputePipelineState:scan];
      [frontier_encoder setBytes:add_parameters.data() length:sizeof(add_parameters) atIndex:0U];
      [frontier_encoder setBuffer:added_offsets offset:0U atIndex:1U]; [frontier_encoder setBuffer:offsets offset:0U atIndex:2U];
      [frontier_encoder setBuffer:block_offsets offset:0U atIndex:3U];
      [frontier_encoder dispatchThreads:MTLSizeMake(block_count*256U,1U,1U)
           threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)]; [frontier_encoder endEncoding];
      auto frontier_parameters=count_parameters;frontier_parameters[4]=1U;
      frontier_encoder=[frontier_command computeCommandEncoder]; [frontier_encoder setComputePipelineState:frontier];
      [frontier_encoder setBytes:frontier_parameters.data() length:sizeof(frontier_parameters) atIndex:0U];
      [frontier_encoder setBuffer:selection_buffer offset:0U atIndex:1U]; [frontier_encoder setBuffer:hierarchy offset:0U atIndex:2U];
      [frontier_encoder setBuffer:status offset:0U atIndex:3U]; [frontier_encoder setBuffer:added_offsets offset:0U atIndex:4U];
      [frontier_encoder setBuffer:counts offset:0U atIndex:5U]; [frontier_encoder setBuffer:canonical offset:0U atIndex:6U];
      [frontier_encoder setBuffer:owner_buffer offset:0U atIndex:7U];
      [frontier_encoder dispatchThreads:MTLSizeMake(1U,1U,1U) threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)]; [frontier_encoder endEncoding];
      frontier_parameters[4]=2U;
      frontier_encoder=[frontier_command computeCommandEncoder]; [frontier_encoder setComputePipelineState:frontier];
      [frontier_encoder setBytes:frontier_parameters.data() length:sizeof(frontier_parameters) atIndex:0U];
      [frontier_encoder setBuffer:selection_buffer offset:0U atIndex:1U]; [frontier_encoder setBuffer:hierarchy offset:0U atIndex:2U];
      [frontier_encoder setBuffer:status offset:0U atIndex:3U]; [frontier_encoder setBuffer:added_offsets offset:0U atIndex:4U];
      [frontier_encoder setBuffer:counts offset:0U atIndex:5U]; [frontier_encoder setBuffer:canonical offset:0U atIndex:6U];
      [frontier_encoder setBuffer:owner_buffer offset:0U atIndex:7U];
      [frontier_encoder dispatchThreads:MTLSizeMake(block_count*256U,1U,1U)
           threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)]; [frontier_encoder endEncoding];
    };
    // P7e3c1 is deliberately a separate device chain: P7e2 mark tail ->
    // P7e3a canonical offsets -> immutable topology -> owner/mask stream.
    // Readback below is fixture-only; no CPU closure result is supplied here.
    const std::array<std::uint32_t,12> closure_parameters{record_count,
        record_count,mark_words,owner_capacity,
        static_cast<std::uint32_t>(snapshot.edge_ranges.size()),
        static_cast<std::uint32_t>(snapshot.ancestor_edge_ranges.size()),0U,
        static_cast<std::uint32_t>(snapshot.child_indices.size()),
        static_cast<std::uint32_t>(snapshot.vertex_ranges.size()),
        static_cast<std::uint32_t>(snapshot.vertex_incidence.size()),0U,0U};
    const auto encode_closure=[&](id<MTLCommandBuffer> closure_command,
                                  std::uint32_t phase){
      auto parameters=closure_parameters;parameters[6]=phase;
      id<MTLComputeCommandEncoder> local=[closure_command computeCommandEncoder];
      [local setComputePipelineState:closure];
      [local setBytes:parameters.data() length:sizeof(parameters) atIndex:0U];
      [local setBuffer:selection_buffer offset:0U atIndex:1U];
      [local setBuffer:closure_status offset:0U atIndex:2U];
      [local setBuffer:hierarchy offset:0U atIndex:3U];
      [local setBuffer:faces offset:0U atIndex:4U];
      [local setBuffer:edge_topology offset:0U atIndex:5U];
      [local setBuffer:vertex_topology offset:0U atIndex:6U];
      [local setBuffer:case_vertex_ranges offset:0U atIndex:7U];
      [local setBuffer:vertex_incidence offset:0U atIndex:8U];
      [local setBuffer:edge_marks offset:0U atIndex:9U];
      [local setBuffer:canonical offset:0U atIndex:10U];
      [local setBuffer:red_promotions offset:0U atIndex:11U];
      [local setBuffer:counts offset:0U atIndex:12U];
      [local setBuffer:case_orientations offset:0U atIndex:13U];
      [local setBuffer:ancestors offset:0U atIndex:14U];
      [local setBuffer:children offset:0U atIndex:15U];
      [local setBuffer:added_offsets offset:0U atIndex:16U];
      [local setBuffer:owner_buffer offset:0U atIndex:17U];
      [local dispatchThreads:MTLSizeMake(block_count*256U,1U,1U)
           threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];
      [local endEncoding];
    };
    const auto retained_intact=[&]{
      const auto* retained=static_cast<const std::uint32_t*>(retained_owner_buffer.contents);
      return std::ranges::all_of(std::span(retained,retained_owner_words.size()),
          [](std::uint32_t word){return word==retained_owner_sentinel;});
    };
    bool closure_converged{};std::uint32_t closure_failure{};
    for(std::uint32_t repair_round=0U;repair_round<=tetra::maximum_world_red_depth;
        ++repair_round) {
      id<MTLBlitCommandEncoder> clear=[command blitCommandEncoder];
      [clear fillBuffer:closure_status range:NSMakeRange(0U,sizeof(status_zeros)) value:0U];
      [clear fillBuffer:edge_marks range:NSMakeRange(0U,edge_marks.length) value:0U];
      [clear fillBuffer:red_promotions range:NSMakeRange(0U,red_promotions.length) value:0U];
      [clear endEncoding];
      encode_frontier(command);
      encode_closure(command,0U);
      // A green mask can only gain one of six local edges in a round. Forty-
      // eight rounds exceeds the immutable hierarchy's maximum depth and the
      // six-edge local fixed point; a non-quiescent result fails below.
      for(std::uint32_t round=0U;round<48U;++round) {
        clear=[command blitCommandEncoder];
        [clear fillBuffer:closure_status range:NSMakeRange(sizeof(std::uint32_t),
            sizeof(std::uint32_t)) value:0U]; [clear endEncoding];
        encode_closure(command,1U);
        encode_closure(command,3U);
      }
      encode_closure(command,8U);
      encode_closure(command,2U);
      encode_closure(command,5U);
      [command commit]; [command waitUntilCompleted];
      if(command.status!=MTLCommandBufferStatusCompleted) {
        std::fprintf(stderr,"Metal hierarchy-frontier command failed: %ld\n",
            static_cast<long>(command.status));
        return false;
      }
      const auto* closure_round_state=static_cast<const std::uint32_t*>(closure_status.contents);
      if(closure_round_state[0U]!=0U) { closure_failure=closure_round_state[0U]; break; }
      if(closure_round_state[2U]==0U) { closure_converged=true; break; }
      command=[queue commandBuffer];
    }
    if(!closure_converged) {
      if(test.failure==FailureCase::none||closure_failure==0U||!retained_intact()) {
        std::fprintf(stderr,"Metal hierarchy-closure unexpected repair failure case %zu: %u\n",completed,closure_failure);
        return false;
      }
      ++completed;continue;
    }
    command=[queue commandBuffer];
    encode_closure(command,6U);
    [command commit]; [command waitUntilCompleted];
    if(command.status!=MTLCommandBufferStatusCompleted)return false;
    auto closure_state=static_cast<const std::uint32_t*>(closure_status.contents);
    if(closure_state[0U]!=0U) {
      if(test.failure==FailureCase::none||!retained_intact()) {
        std::fprintf(stderr,"Metal hierarchy-closure unexpected preflight failure case %zu: %u\n",completed,closure_state[0U]);
        return false;
      }
      ++completed;continue;
    }
    command=[queue commandBuffer];
    encode_closure(command,4U);
    [command commit]; [command waitUntilCompleted];
    if(command.status!=MTLCommandBufferStatusCompleted) {
      std::fprintf(stderr,"Metal hierarchy-frontier emission failed: %ld\n",
          static_cast<long>(command.status));
      return false;
    }
    closure_state=static_cast<const std::uint32_t*>(closure_status.contents);
    if(test.failure!=FailureCase::none) {
      if(closure_state[0U]==0U||!retained_intact()) {
        std::fprintf(stderr,"Metal hierarchy-closure expected rejection missing case %zu: %u\n",completed,closure_state[0U]);
        return false;
      }
      ++completed;continue;
    }
    const auto* state=static_cast<const std::uint32_t*>(status.contents);
    if(state[0u]!=0u||state[1u]!=expected.size()) {
      std::fprintf(stderr,"Metal hierarchy-frontier state %u/%u expected %zu\n",
          state[0u],state[1u],expected.size());
      return false;
    }
    if(closure_state[0U]!=0U||closure_state[1U]!=0U||closure_state[2U]!=0U||
       closure_state[3U]!=expected.size()||closure_state[4U]!=1U) {
      std::fprintf(stderr,"Metal hierarchy-closure state %u/%u/%u/%u/%u expected %zu\n",
          closure_state[0U],closure_state[1U],closure_state[2U],closure_state[3U],
          closure_state[4U],
          expected.size());
      return false;
    }
    const auto* device_words=static_cast<const std::uint32_t*>(owner_buffer.contents);
    for(std::size_t index=0U;index<expected.size();++index) {
      const std::array<std::uint32_t,4> lanes{{device_words[index*12U],
          device_words[index*12U+1U],device_words[index*12U+2U],device_words[index*12U+3U]}};
      if(tetra::gpu_hierarchy_address_from_lanes(lanes)!=expected[index]) {
        std::fprintf(stderr,"Metal hierarchy-frontier record %zu mismatched\n",index);
        return false;
      }
      const auto found=std::ranges::find_if(snapshot.records,[&](const auto& record){
        return tetra::gpu_hierarchy_address_from_lanes(record.address)==expected[index];
      });
      if(found==snapshot.records.end())return false;
      const auto record_index=static_cast<std::size_t>(found-snapshot.records.begin());
      for(std::size_t edge=0U;edge<6U;++edge)
        if(device_words[index*12U+4U+edge]!=snapshot.edge_topology[record_index].edge_ranges[edge])
          return false;
      if(device_words[index*12U+10U]!=closure_oracle.green_masks[index]||
         device_words[index*12U+11U]!=snapshot.orientation_flags[record_index])return false;
      saw_green_mask|=closure_oracle.green_masks[index]!=0U;
    }
    // c2 publication is a one-way device copy after the independent CPU
    // oracle has qualified the complete candidate. Rejected candidates above
    // never touch this retained owner front.
    command=[queue commandBuffer];
    id<MTLBlitCommandEncoder> publish=[command blitCommandEncoder];
    [publish copyFromBuffer:owner_buffer sourceOffset:0U toBuffer:retained_owner_buffer
              destinationOffset:0U size:owner_buffer.length];
    [publish endEncoding]; [command commit]; [command waitUntilCompleted];
    if(command.status!=MTLCommandBufferStatusCompleted)return false;
    if(std::memcmp(retained_owner_buffer.contents,owner_buffer.contents,
                   owner_buffer.length)!=0)return false;
    ++completed;
  }
  std::printf("{\"event\":\"metal_gpu_hierarchy_frontier\","
              "\"cases\":%zu,\"canonical\":true,\"closure\":true,\"green_mask\":%s,"
              "\"overflow\":true,\"passed\":true}\n",completed,
      saw_green_mask?"true":"false");
  return completed==cases.size();
}

// P7e4a1's compact green fixture intentionally stops before red repair and
// P8. It proves that the selected canonical list itself drives every green
// pass, without reviving P7e4a's record-count closure prototype.
bool run_metal_gpu_hierarchy_compact_green_closure_smoke_test(id<MTLDevice> device) {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  for(unsigned generation=0U;generation<3U;++generation)mesh.refine_all_binary();
  std::vector<tetra::WorldTetAddress> leaves;
  for(const auto owner:mesh.logical_red_owners())leaves.push_back(tetra::world_tet_address(owner));
  const tetra::WorldCutDirectory directory(tetra::make_sparse_world_cut_checkpoint(
      leaves,1U,71U,tetra::HierarchyResidencyTier::surface));
  const auto snapshot=tetra::make_gpu_hierarchy_snapshot(directory,73U);
  if(snapshot.records.empty()||snapshot.canonical_record_indices.size()!=snapshot.records.size()||
     snapshot.edge_topology.size()!=snapshot.records.size()) {
    std::fprintf(stderr,"Metal compact green fixture snapshot sidecars are incomplete\n");
    return false;
  }
  const auto shader_path=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR)/
      "gpu_hierarchy_compact_green_closure.comp.metal";
  id<MTLLibrary> library=make_file_shader_library(device,shader_path.string().c_str());
  NSError* error=nil;
  id<MTLFunction> function=library==nil?nil:[library newFunctionWithName:@"main0"];
  id<MTLComputePipelineState> pipeline=function==nil?nil:
      [device newComputePipelineStateWithFunction:function error:&error];
  const auto companion_pipeline=[&](const char* name)->id<MTLComputePipelineState>{
    const auto path=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR)/name;
    id<MTLLibrary> companion=make_file_shader_library(device,path.string().c_str());
    NSError* companion_error=nil;
    return companion==nil?nil:[device newComputePipelineStateWithFunction:
        [companion newFunctionWithName:@"main0"] error:&companion_error];
  };
  id<MTLComputePipelineState> red_pipeline=companion_pipeline("gpu_hierarchy_compact_red_repair.comp.metal");
  id<MTLComputePipelineState> red_scan_pipeline=companion_pipeline("gpu_hierarchy_compact_red_scan.comp.metal");
  id<MTLComputePipelineState> canonicalize_pipeline=companion_pipeline("gpu_hierarchy_canonicalize.comp.metal");
  id<MTLCommandQueue> queue=[device newCommandQueue];
  if(pipeline==nil||red_pipeline==nil||red_scan_pipeline==nil||canonicalize_pipeline==nil||queue==nil) {
    std::fprintf(stderr,"Metal compact green pipeline failed: %s\n",
        error==nil?"missing translated entry point":error.localizedDescription.UTF8String);
    return false;
  }
  const auto make=[&](const void* bytes,NSUInteger length){
    return [device newBufferWithBytes:bytes length:std::max<NSUInteger>(length,4U)
        options:MTLResourceStorageModeShared];
  };
  std::vector<std::uint32_t> ranks(snapshot.records.size());
  for(std::uint32_t rank=0U;rank<snapshot.canonical_record_indices.size();++rank)
    ranks[snapshot.canonical_record_indices[rank]]=rank;
  id<MTLBuffer> rank_buffer=make(ranks.data(),ranks.size()*sizeof(std::uint32_t));
  id<MTLBuffer> topology_buffer=make(snapshot.edge_topology.data(),
      snapshot.edge_topology.size()*sizeof(snapshot.edge_topology.front()));
  id<MTLBuffer> ancestor_buffer=make(snapshot.ancestor_edge_ranges.data(),
      snapshot.ancestor_edge_ranges.size()*sizeof(std::uint32_t));
  id<MTLBuffer> parent_buffer=make(snapshot.parent_records.data(),
      snapshot.parent_records.size()*sizeof(std::uint32_t));
  id<MTLBuffer> hierarchy_buffer=make(snapshot.records.data(),
      snapshot.records.size()*sizeof(snapshot.records.front()));
  id<MTLBuffer> child_buffer=make(snapshot.child_indices.data(),
      snapshot.child_indices.size()*sizeof(std::uint32_t));
  id<MTLBuffer> vertex_topology_buffer=make(snapshot.vertex_topology.data(),
      snapshot.vertex_topology.size()*sizeof(snapshot.vertex_topology.front()));
  id<MTLBuffer> vertex_ranges_buffer=make(snapshot.vertex_ranges.data(),
      snapshot.vertex_ranges.size()*sizeof(snapshot.vertex_ranges.front()));
  id<MTLBuffer> vertex_incidence_buffer=make(snapshot.vertex_incidence.data(),
      snapshot.vertex_incidence.size()*sizeof(snapshot.vertex_incidence.front()));
  if(rank_buffer==nil||topology_buffer==nil||ancestor_buffer==nil||parent_buffer==nil||
     hierarchy_buffer==nil||child_buffer==nil||vertex_topology_buffer==nil||
     vertex_ranges_buffer==nil||vertex_incidence_buffer==nil) {
    std::fprintf(stderr,"Metal compact green fixture buffer allocation failed\n");
    return false;
  }
  const auto record_count=static_cast<std::uint32_t>(snapshot.records.size());
  enum class Cut : std::uint8_t { fixed, root_seam, mixed, red_repaired };
  const std::array cuts{Cut::fixed,Cut::root_seam,Cut::mixed,Cut::red_repaired};
  std::size_t completed{}; bool saw_green{};
  for(const auto cut:cuts) {
    std::vector<std::uint32_t> selected;
    for(std::uint32_t record=0U;record<record_count;++record) {
      const auto address=tetra::gpu_hierarchy_address_from_lanes(snapshot.records[record].address);
      bool include=false;
      if(cut==Cut::fixed)include=address.red_depth()==0U;
      else if(cut==Cut::root_seam)
        include=address.root_id()==0U?address.red_depth()==1U:address.red_depth()==0U;
      else if(cut==Cut::mixed)
        include=(address.root_id()==0U||address.root_id()==1U)?
            address.red_depth()==1U:address.red_depth()==0U;
      else
        include=address.root_id()==0U?address.red_depth()==2U:
            (address.root_id()==1U?address.red_depth()==1U:address.red_depth()==0U);
      if(include)selected.push_back(record);
    }
    std::ranges::sort(selected,{},[&](std::uint32_t record){return ranks[record];});
    std::vector<tetra::WorldTetAddress> requested;
    for(const auto record:selected)
      requested.push_back(tetra::gpu_hierarchy_address_from_lanes(snapshot.records[record].address));
    // The CPU oracle defines closure order by WorldTetAddress. The compact
    // rank sidecar is required to represent that same canonical order below.
    std::ranges::sort(requested);
    tetra::WorldConformingClosureCache oracle_cache;
    const auto expected=tetra::close_world_conforming_cut(requested,&oracle_cache);
    // This is specifically a green-only slice. A cut needing red replacement
    // belongs to the next sparse-closure leaf, not a silently partial fixture.
    if(cut!=Cut::red_repaired&&
       (expected!=requested||expected.size()!=oracle_cache.green_masks.size())) {
      std::fprintf(stderr,"Compact green fixture cut %u unexpectedly needs red repair: requested=%zu closed=%zu\n",
          static_cast<unsigned>(cut),requested.size(),expected.size());
      return false;
    }
    if(cut==Cut::red_repaired&&expected.size()<=requested.size())return false;
    for(std::size_t index=0U;cut!=Cut::red_repaired&&index<selected.size();++index)
      if(tetra::gpu_hierarchy_address_from_lanes(snapshot.records[selected[index]].address)!=
         expected[index])return false;
    const auto active_capacity=cut==Cut::red_repaired?record_count:
        static_cast<std::uint32_t>(selected.size());
    std::vector<std::uint32_t> active_words(4U+active_capacity,0U);
    active_words[0U]=static_cast<std::uint32_t>(selected.size());
    active_words[1U]=active_capacity;
    std::copy(selected.begin(),selected.end(),active_words.begin()+4U);
    std::vector<std::uint32_t> edge_marks(std::max<std::size_t>(snapshot.edge_ranges.size(),1U),0U);
    std::array<std::uint32_t,5> control{};
    std::array<std::uint32_t,4> arguments{99U,99U,99U,0U};
    std::vector<std::uint32_t> mask_words(4U+active_capacity,0U);
    id<MTLBuffer> active_buffer=make(active_words.data(),active_words.size()*sizeof(std::uint32_t));
    id<MTLBuffer> edge_buffer=make(edge_marks.data(),edge_marks.size()*sizeof(std::uint32_t));
    id<MTLBuffer> control_buffer=make(control.data(),sizeof(control));
    id<MTLBuffer> arguments_buffer=make(arguments.data(),sizeof(arguments));
    id<MTLBuffer> masks_buffer=make(mask_words.data(),mask_words.size()*sizeof(std::uint32_t));
    if(active_buffer==nil||edge_buffer==nil||control_buffer==nil||arguments_buffer==nil||masks_buffer==nil) {
      std::fprintf(stderr,"Metal compact green candidate allocation failed\n");
      return false;
    }
    const auto encode=[&](id<MTLCommandBuffer> command,std::uint32_t phase,bool indirect,
                          std::uint32_t round_limit=8U){
      const std::array<std::uint32_t,5> parameters{record_count,
          static_cast<std::uint32_t>(snapshot.edge_ranges.size()),
          static_cast<std::uint32_t>(snapshot.ancestor_edge_ranges.size()),round_limit,phase};
      id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
      [encoder setComputePipelineState:pipeline];
      // Generated MSL ABI: queue, dispatch, ranks, active, parameters,
      // topology, masks, marks, ancestors.
      [encoder setBuffer:control_buffer offset:0U atIndex:0U];
      [encoder setBuffer:arguments_buffer offset:0U atIndex:1U];
      [encoder setBuffer:rank_buffer offset:0U atIndex:2U];
      [encoder setBuffer:active_buffer offset:0U atIndex:3U];
      [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:4U];
      [encoder setBuffer:topology_buffer offset:0U atIndex:5U];
      [encoder setBuffer:masks_buffer offset:0U atIndex:6U];
      [encoder setBuffer:edge_buffer offset:0U atIndex:7U];
      [encoder setBuffer:ancestor_buffer offset:0U atIndex:8U];
      if(indirect)[encoder dispatchThreadgroupsWithIndirectBuffer:arguments_buffer indirectBufferOffset:0U
          threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];
      else [encoder dispatchThreads:MTLSizeMake(1U,1U,1U)
          threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
      [encoder endEncoding];
    };
    id<MTLCommandBuffer> command=[queue commandBuffer];
    encode(command,0U,false); encode(command,1U,true); encode(command,2U,true);
    // Fixed command-buffer schedule: its only convergence observation is the
    // private queue latch, never a CPU readback between rounds.
    for(unsigned round=0U;round<8U;++round) {
      encode(command,3U,false); encode(command,4U,true); encode(command,5U,false);
    }
    encode(command,8U,false);encode(command,6U,true); [command commit]; [command waitUntilCompleted];
    const auto* state=static_cast<const std::uint32_t*>(control_buffer.contents);
    const auto* device_masks=static_cast<const std::uint32_t*>(masks_buffer.contents);
    const auto* device_args=static_cast<const std::uint32_t*>(arguments_buffer.contents);
    if(command.status!=MTLCommandBufferStatusCompleted||state==nullptr||device_masks==nullptr||
       device_args==nullptr||state[2U]!=0U||state[3U]!=1U||
       state[4U]==0U||device_args[0U]!=(selected.size()+255U)/256U||device_args[1U]!=1U||device_args[2U]!=1U||
       device_masks[0U]!=selected.size()||device_masks[2U]!=0U||device_masks[3U]!=1U) {
      std::fprintf(stderr,"Metal compact green state rejected: %u/%u/%u/%u\n",
          state==nullptr?0U:state[0U],state==nullptr?0U:state[1U],
          state==nullptr?0U:state[2U],state==nullptr?0U:state[3U]);
      return false;
    }
    for(std::size_t index=0U;cut!=Cut::red_repaired&&index<selected.size();++index) {
      if(device_masks[4U+index]!=oracle_cache.green_masks[index]) {
        std::fprintf(stderr,"Metal compact green mask mismatch at %zu: %u != %u\n",index,
            device_masks[4U+index],oracle_cache.green_masks[index]);
        return false;
      }
      saw_green|=device_masks[4U+index]!=0U;
    }
    if(cut==Cut::red_repaired) {
      const auto blocks=static_cast<std::uint32_t>((selected.size()+255U)/256U);
      std::vector<std::uint32_t> red_status(4U),expand_counts(record_count),expand_offsets(record_count);
      std::vector<std::uint32_t> block_totals(std::max<std::uint32_t>(blocks,1U)),block_offsets(block_totals.size()),scan_total(1U);
      std::vector<std::uint32_t> level_totals(std::max<std::uint32_t>((blocks+255U)/256U,1U)),level_offsets(level_totals.size());
      std::vector<std::uint32_t> red_words(4U+record_count),canonical_words(4U+record_count),
          final_active_words(4U+record_count),final_mask_words(4U+record_count);
      std::array<std::uint32_t,8> red_args{};
      id<MTLBuffer> red_status_buffer=make(red_status.data(),sizeof(red_status));
      id<MTLBuffer> count_buffer=make(expand_counts.data(),expand_counts.size()*sizeof(std::uint32_t));
      id<MTLBuffer> offset_buffer=make(expand_offsets.data(),expand_offsets.size()*sizeof(std::uint32_t));
      id<MTLBuffer> total_buffer=make(scan_total.data(),sizeof(std::uint32_t));
      id<MTLBuffer> block_total_buffer=make(block_totals.data(),block_totals.size()*sizeof(std::uint32_t));
      id<MTLBuffer> block_offset_buffer=make(block_offsets.data(),block_offsets.size()*sizeof(std::uint32_t));
      id<MTLBuffer> level_total_buffer=make(level_totals.data(),level_totals.size()*sizeof(std::uint32_t));
      id<MTLBuffer> level_offset_buffer=make(level_offsets.data(),level_offsets.size()*sizeof(std::uint32_t));
      id<MTLBuffer> red_output=make(red_words.data(),red_words.size()*sizeof(std::uint32_t));
      id<MTLBuffer> red_args_buffer=make(red_args.data(),sizeof(red_args));
      id<MTLBuffer> canonical_output=make(canonical_words.data(),canonical_words.size()*sizeof(std::uint32_t));
      id<MTLBuffer> final_active_buffer=make(final_active_words.data(),final_active_words.size()*sizeof(std::uint32_t));
      id<MTLBuffer> final_mask_buffer=make(final_mask_words.data(),final_mask_words.size()*sizeof(std::uint32_t));
      std::vector<std::uint32_t> histogram(std::max<std::uint32_t>(blocks*16U,1U)),histogram_offsets(histogram.size());
      std::array<std::uint32_t,16> bin_bases{};
      id<MTLBuffer> histogram_buffer=make(histogram.data(),histogram.size()*sizeof(std::uint32_t));
      id<MTLBuffer> histogram_offsets_buffer=make(histogram_offsets.data(),histogram_offsets.size()*sizeof(std::uint32_t));
      id<MTLBuffer> bin_bases_buffer=make(bin_bases.data(),sizeof(bin_bases));
      if(red_status_buffer==nil||count_buffer==nil||offset_buffer==nil||total_buffer==nil||
         block_total_buffer==nil||block_offset_buffer==nil||level_total_buffer==nil||level_offset_buffer==nil||red_output==nil||red_args_buffer==nil||
         canonical_output==nil||final_active_buffer==nil||final_mask_buffer==nil||
         histogram_buffer==nil||histogram_offsets_buffer==nil||bin_bases_buffer==nil)return false;
      const auto red_encode=[&](id<MTLCommandBuffer> target,std::uint32_t phase,bool indirect,
                                std::uint32_t repair_budget=8U){
        const std::array<std::uint32_t,6> p{record_count,static_cast<std::uint32_t>(snapshot.child_indices.size()),
          static_cast<std::uint32_t>(snapshot.vertex_ranges.size()),static_cast<std::uint32_t>(snapshot.vertex_incidence.size()),repair_budget,phase};
        id<MTLComputeCommandEncoder> e=[target computeCommandEncoder]; [e setComputePipelineState:red_pipeline];
        [e setBuffer:red_status_buffer offset:0 atIndex:0];[e setBuffer:red_args_buffer offset:0 atIndex:1];[e setBytes:p.data() length:sizeof(p) atIndex:2];
        [e setBuffer:rank_buffer offset:0 atIndex:3];[e setBuffer:active_buffer offset:0 atIndex:4];[e setBuffer:parent_buffer offset:0 atIndex:5];[e setBuffer:hierarchy_buffer offset:0 atIndex:6];[e setBuffer:vertex_topology_buffer offset:0 atIndex:7];[e setBuffer:vertex_ranges_buffer offset:0 atIndex:8];[e setBuffer:vertex_incidence_buffer offset:0 atIndex:9];[e setBuffer:red_output offset:0 atIndex:10];[e setBuffer:masks_buffer offset:0 atIndex:11];[e setBuffer:total_buffer offset:0 atIndex:12];[e setBuffer:final_active_buffer offset:0 atIndex:13];[e setBuffer:final_mask_buffer offset:0 atIndex:14];[e setBuffer:count_buffer offset:0 atIndex:15];[e setBuffer:offset_buffer offset:0 atIndex:16];[e setBuffer:child_buffer offset:0 atIndex:17];
        if(indirect)[e dispatchThreadgroupsWithIndirectBuffer:red_args_buffer indirectBufferOffset:(phase==3U||phase==7U)?4U*sizeof(std::uint32_t):0U threadsPerThreadgroup:MTLSizeMake(256,1,1)];else [e dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[e endEncoding];
      };
      const auto scan_encode=[&](id<MTLCommandBuffer> target,std::uint32_t phase,bool indirect){
        id<MTLComputeCommandEncoder> e=[target computeCommandEncoder];[e setComputePipelineState:red_scan_pipeline];
        [e setBuffer:active_buffer offset:0 atIndex:0];[e setBytes:&phase length:sizeof(phase) atIndex:1];[e setBuffer:count_buffer offset:0 atIndex:2];[e setBuffer:offset_buffer offset:0 atIndex:3];[e setBuffer:block_total_buffer offset:0 atIndex:4];[e setBuffer:block_offset_buffer offset:0 atIndex:5];[e setBuffer:level_total_buffer offset:0 atIndex:6];[e setBuffer:level_offset_buffer offset:0 atIndex:7];[e setBuffer:total_buffer offset:0 atIndex:8];
        if(indirect)[e dispatchThreadgroupsWithIndirectBuffer:red_args_buffer indirectBufferOffset:0 threadsPerThreadgroup:MTLSizeMake(256,1,1)];else [e dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[e endEncoding];
      };
      command=[queue commandBuffer];red_encode(command,0U,false);red_encode(command,1U,true);scan_encode(command,0U,true);scan_encode(command,1U,true);scan_encode(command,2U,false);scan_encode(command,3U,true);red_encode(command,2U,false);red_encode(command,3U,true);red_encode(command,4U,false);[command commit];[command waitUntilCompleted];
      const auto* red_state=static_cast<const std::uint32_t*>(red_status_buffer.contents);const auto* red_result=static_cast<const std::uint32_t*>(red_output.contents);
      if(command.status!=MTLCommandBufferStatusCompleted||red_state==nullptr||red_result==nullptr||red_state[0U]!=0U||red_state[1U]==0U||red_result[0U]!=expected.size())return false;
      id<MTLBuffer> canonical_input=red_output,canonical_destination=canonical_output;
      command=[queue commandBuffer];
      for(std::uint32_t shift=0U;shift<20U;shift+=4U)for(std::uint32_t phase=0U;phase<3U;++phase) {
        const std::array<std::uint32_t,4> p{record_count,phase,shift,0U};
        id<MTLComputeCommandEncoder> e=[command computeCommandEncoder];[e setComputePipelineState:canonicalize_pipeline];
        [e setBuffer:red_args_buffer offset:0 atIndex:0];[e setBuffer:canonical_input offset:0 atIndex:1];[e setBytes:p.data() length:sizeof(p) atIndex:2];[e setBuffer:canonical_destination offset:0 atIndex:3];[e setBuffer:rank_buffer offset:0 atIndex:4];[e setBuffer:histogram_buffer offset:0 atIndex:5];[e setBuffer:bin_bases_buffer offset:0 atIndex:6];[e setBuffer:histogram_offsets_buffer offset:0 atIndex:7];
        if(phase==1U)[e dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];else [e dispatchThreadgroupsWithIndirectBuffer:red_args_buffer indirectBufferOffset:4U*sizeof(std::uint32_t) threadsPerThreadgroup:MTLSizeMake(256,1,1)];[e endEncoding];
        if(phase==2U)std::swap(canonical_input,canonical_destination);
      }
      [command commit];[command waitUntilCompleted];
      const auto* canonical_result=static_cast<const std::uint32_t*>(canonical_input.contents);
      std::vector<std::uint32_t> expected_records;for(const auto address:expected){const auto found=std::ranges::find_if(snapshot.records,[&](const auto& r){return tetra::gpu_hierarchy_address_from_lanes(r.address)==address;});if(found==snapshot.records.end())return false;expected_records.push_back(static_cast<std::uint32_t>(found-snapshot.records.begin()));}std::ranges::sort(expected_records,{},[&](std::uint32_t r){return ranks[r];});
      if(command.status!=MTLCommandBufferStatusCompleted||canonical_result==nullptr||canonical_result[0U]!=expected_records.size()||canonical_result[2U]!=0U||!std::equal(expected_records.begin(),expected_records.end(),canonical_result+4U))return false;
      // The compact live encoder uses a fixed red repair schedule.  Verify
      // its terminal latch directly: a red predicate still present at the
      // exhausted budget must fail closed rather than leave a partial pong
      // list that a future P8 owner materializer could consume.
      command=[queue commandBuffer];
      id<MTLBlitCommandEncoder> reset_red=[command blitCommandEncoder];
      for(id<MTLBuffer> buffer:{red_status_buffer,red_args_buffer,red_output})
        [reset_red fillBuffer:buffer range:NSMakeRange(0U,buffer.length) value:0U];
      [reset_red endEncoding];
      red_encode(command,0U,false,0U);red_encode(command,1U,true,0U);
      red_encode(command,5U,false,0U);[command commit];[command waitUntilCompleted];
      const auto* exhausted_status=static_cast<const std::uint32_t*>(red_status_buffer.contents);
      if(command.status!=MTLCommandBufferStatusCompleted||exhausted_status==nullptr||
         (exhausted_status[0U]&32U)==0U)return false;
      // Red finalize must latch a capacity failure before scatter or any
      // publishable pong header is produced.
      std::vector<std::uint32_t> capacity_active{1U,1U,0U,0U,selected.front()};
      std::vector<std::uint32_t> capacity_masks{1U,1U,0U,1U,0U},capacity_total{2U};
      id<MTLBuffer> capacity_active_buffer=make(capacity_active.data(),capacity_active.size()*sizeof(std::uint32_t));
      id<MTLBuffer> capacity_masks_buffer=make(capacity_masks.data(),capacity_masks.size()*sizeof(std::uint32_t));
      id<MTLBuffer> capacity_total_buffer=make(capacity_total.data(),sizeof(std::uint32_t));
      if(capacity_active_buffer==nil||capacity_masks_buffer==nil||capacity_total_buffer==nil)return false;
      auto saved_red_active=active_buffer,saved_red_masks=masks_buffer,saved_red_total=total_buffer;
      active_buffer=capacity_active_buffer;masks_buffer=capacity_masks_buffer;total_buffer=capacity_total_buffer;
      command=[queue commandBuffer];
      reset_red=[command blitCommandEncoder];
      [reset_red fillBuffer:red_status_buffer range:NSMakeRange(0U,red_status_buffer.length) value:0U];
      [reset_red endEncoding]; [command commit]; [command waitUntilCompleted];
      if(command.status!=MTLCommandBufferStatusCompleted)return false;
      command=[queue commandBuffer];red_encode(command,0U,false);red_encode(command,2U,false);[command commit];[command waitUntilCompleted];
      const auto* capacity_status=static_cast<const std::uint32_t*>(red_status_buffer.contents);
      active_buffer=saved_red_active;masks_buffer=saved_red_masks;total_buffer=saved_red_total;
      if(command.status!=MTLCommandBufferStatusCompleted||capacity_status==nullptr||
         (capacity_status[0U]&8U)==0U)return false;
      std::array<std::uint32_t,4> post_control{},post_args{};
      std::vector<std::uint32_t> post_edges(edge_marks.size()),post_masks(4U+record_count);
      id<MTLBuffer> post_control_buffer=make(post_control.data(),sizeof(post_control));
      id<MTLBuffer> post_args_buffer=make(post_args.data(),sizeof(post_args));
      id<MTLBuffer> post_edge_buffer=make(post_edges.data(),post_edges.size()*sizeof(std::uint32_t));
      id<MTLBuffer> post_masks_buffer=make(post_masks.data(),post_masks.size()*sizeof(std::uint32_t));
      if(post_control_buffer==nil||post_args_buffer==nil||post_edge_buffer==nil||post_masks_buffer==nil)return false;
      auto saved_active=active_buffer,saved_control=control_buffer,saved_args=arguments_buffer,saved_edge=edge_buffer,saved_masks=masks_buffer;
      active_buffer=canonical_input;control_buffer=post_control_buffer;arguments_buffer=post_args_buffer;edge_buffer=post_edge_buffer;masks_buffer=post_masks_buffer;
      command=[queue commandBuffer];encode(command,0U,false);encode(command,1U,true);encode(command,2U,true);for(unsigned round=0U;round<8U;++round){encode(command,3U,false);encode(command,4U,true);encode(command,5U,false);}encode(command,8U,false);encode(command,6U,true);[command commit];[command waitUntilCompleted];
      const auto* post_state=static_cast<const std::uint32_t*>(post_control_buffer.contents);const auto* post_result=static_cast<const std::uint32_t*>(post_masks_buffer.contents);
      const bool masks_match=command.status==MTLCommandBufferStatusCompleted&&post_state!=nullptr&&post_result!=nullptr&&post_state[2U]==0U&&post_state[3U]==1U&&post_result[0U]==expected_records.size()&&std::equal(oracle_cache.green_masks.begin(),oracle_cache.green_masks.end(),post_result+4U);
      // One repair is sufficient for this known red cut.  Re-run only the
      // predicate/terminal pair on its post-repair canonical list to prove a
      // final allowed repair can converge rather than being rejected merely
      // because the pre-repair predicate was nonzero.
      command=[queue commandBuffer];reset_red=[command blitCommandEncoder];
      [reset_red fillBuffer:red_status_buffer range:NSMakeRange(0U,red_status_buffer.length) value:0U];
      [reset_red fillBuffer:red_args_buffer range:NSMakeRange(0U,red_args_buffer.length) value:0U];
      [reset_red endEncoding];red_encode(command,0U,false);red_encode(command,1U,true);
      red_encode(command,5U,false);red_encode(command,6U,false);red_encode(command,7U,true);
      [command commit];[command waitUntilCompleted];
      const auto* converged_status=static_cast<const std::uint32_t*>(red_status_buffer.contents);
      const auto* converged_args=static_cast<const std::uint32_t*>(red_args_buffer.contents);
      const auto* final_active=static_cast<const std::uint32_t*>(final_active_buffer.contents);
      const auto* final_masks=static_cast<const std::uint32_t*>(final_mask_buffer.contents);
      const bool final_repair_converged=command.status==MTLCommandBufferStatusCompleted&&
          converged_status!=nullptr&&converged_status[0U]==0U&&
          converged_status[1U]==0U&&converged_args!=nullptr&&
          converged_args[0U]==0U&&converged_args[3U]==1U&&
          final_active!=nullptr&&final_masks!=nullptr&&
          final_active[0U]==expected_records.size()&&final_active[2U]==0U&&
          final_active[3U]==1U&&final_masks[0U]==expected_records.size()&&
          final_masks[2U]==0U&&final_masks[3U]==1U&&
          std::equal(expected_records.begin(),expected_records.end(),final_active+4U)&&
          std::equal(oracle_cache.green_masks.begin(),oracle_cache.green_masks.end(),final_masks+4U);
      active_buffer=saved_active;control_buffer=saved_control;arguments_buffer=saved_args;edge_buffer=saved_edge;masks_buffer=saved_masks;
      if(!masks_match||!final_repair_converged)return false;
    }
    // Model publication with a distinct retained front. Invalid/budgeted
    // candidates below can write no retained word.
    std::vector<std::uint32_t> retained(mask_words.size(),0xa5c3f17eU);
    id<MTLBuffer> retained_buffer=make(retained.data(),retained.size()*sizeof(std::uint32_t));
    if(retained_buffer==nil) { std::fprintf(stderr,"Metal compact green retained allocation failed\n"); return false; }
    command=[queue commandBuffer]; id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
    [blit copyFromBuffer:masks_buffer sourceOffset:0U toBuffer:retained_buffer destinationOffset:0U
                    size:masks_buffer.length]; [blit endEncoding]; [command commit]; [command waitUntilCompleted];
    if(command.status!=MTLCommandBufferStatusCompleted||
       std::memcmp(retained_buffer.contents,masks_buffer.contents,masks_buffer.length)!=0)return false;
    const auto retained_before=std::vector<std::uint32_t>(
        static_cast<const std::uint32_t*>(retained_buffer.contents),
        static_cast<const std::uint32_t*>(retained_buffer.contents)+retained.size());
    const auto rejects_and_retains=[&](bool malformed,bool budget){
      auto bad_active=active_words;
      if(malformed)bad_active[4U]=record_count;
      id<MTLBuffer> bad_active_buffer=make(bad_active.data(),bad_active.size()*sizeof(std::uint32_t));
      std::array<std::uint32_t,5> bad_control{};std::array<std::uint32_t,4> bad_args{99U,99U,99U,0U};
      std::vector<std::uint32_t> bad_masks(4U+selected.size(),0U),bad_edges(edge_marks.size(),0U);
      id<MTLBuffer> bad_control_buffer=make(bad_control.data(),sizeof(bad_control));
      id<MTLBuffer> bad_args_buffer=make(bad_args.data(),sizeof(bad_args));
      id<MTLBuffer> bad_masks_buffer=make(bad_masks.data(),bad_masks.size()*sizeof(std::uint32_t));
      id<MTLBuffer> bad_edge_buffer=make(bad_edges.data(),bad_edges.size()*sizeof(std::uint32_t));
      if(bad_active_buffer==nil||bad_control_buffer==nil||bad_args_buffer==nil||bad_masks_buffer==nil||bad_edge_buffer==nil)return false;
      // Rebind the fixture's small private-style candidate set without ever
      // touching the retained front.
      auto saved_active=active_buffer,saved_control=control_buffer,saved_args=arguments_buffer,
          saved_masks=masks_buffer,saved_edge=edge_buffer;
      active_buffer=bad_active_buffer; control_buffer=bad_control_buffer; arguments_buffer=bad_args_buffer;
      masks_buffer=bad_masks_buffer; edge_buffer=bad_edge_buffer;
      id<MTLCommandBuffer> bad=[queue commandBuffer]; encode(bad,0U,false);
      if(budget) {
        // Device-side fault injection validates the budget latch and retained
        // front without assuming a particular legal cut takes N rounds.
        encode(bad,7U,false);
      } else {
        encode(bad,1U,true); encode(bad,2U,true);
        for(unsigned round=0U;round<8U;++round) {
          encode(bad,3U,false); encode(bad,4U,true); encode(bad,5U,false);
        }
      }
      encode(bad,8U,false);encode(bad,6U,true); [bad commit]; [bad waitUntilCompleted];
      const auto* bad_state=static_cast<const std::uint32_t*>(bad_control_buffer.contents);
      const bool rejected=bad.status==MTLCommandBufferStatusCompleted&&bad_state!=nullptr&&bad_state[2U]!=0U&&
          std::memcmp(retained_buffer.contents,retained_before.data(),retained_before.size()*sizeof(std::uint32_t))==0;
      if(!rejected)std::fprintf(stderr,"Metal compact green reject failed: malformed=%d budget=%d status=%ld state=%u/%u/%u/%u retained=%d\n",
          malformed,budget,static_cast<long>(bad.status),bad_state==nullptr?0U:bad_state[0U],
          bad_state==nullptr?0U:bad_state[1U],bad_state==nullptr?0U:bad_state[2U],
          bad_state==nullptr?0U:bad_state[3U],
          std::memcmp(retained_buffer.contents,retained_before.data(),retained_before.size()*sizeof(std::uint32_t))==0);
      active_buffer=saved_active; control_buffer=saved_control; arguments_buffer=saved_args;
      masks_buffer=saved_masks; edge_buffer=saved_edge;
      return rejected;
    };
    if(!rejects_and_retains(true,false)||
       (cut==Cut::red_repaired&&!rejects_and_retains(false,true)))return false;
    ++completed;
  }
  // Independent >1-block scan proof. These synthetic compact counts exercise
  // the same device headers and all scan levels without relying on a camera
  // cut being large enough to exceed one workgroup.
  constexpr std::uint32_t scan_count=257U;
  std::vector<std::uint32_t> scan_active(4U+scan_count),scan_counts(scan_count),scan_offsets(scan_count),scan_totals(2U),scan_block_offsets(2U),scan_level_totals(1U),scan_level_offsets(1U),scan_total(1U);
  scan_active[0U]=scan_count;scan_active[1U]=scan_count;std::uint32_t expected_total{};
  for(std::uint32_t i=0U;i<scan_count;++i){scan_counts[i]=(i%7U)+1U;expected_total+=scan_counts[i];}
  const auto scan_make=[&](const auto& values){return make(values.data(),values.size()*sizeof(values.front()));};
  id<MTLBuffer> sa=scan_make(scan_active),sc=scan_make(scan_counts),so=scan_make(scan_offsets),st=scan_make(scan_totals),sbo=scan_make(scan_block_offsets),slt=scan_make(scan_level_totals),slo=scan_make(scan_level_offsets),sum=scan_make(scan_total);
  if(sa==nil||sc==nil||so==nil||st==nil||sbo==nil||slt==nil||slo==nil||sum==nil)return false;
  std::array<std::uint32_t,4> scan_args{2U,1U,1U,0U};id<MTLBuffer> sargs=make(scan_args.data(),sizeof(scan_args));if(sargs==nil)return false;
  id<MTLCommandBuffer> scan_command=[queue commandBuffer];
  for(std::uint32_t phase=0U;phase<4U;++phase){id<MTLComputeCommandEncoder> e=[scan_command computeCommandEncoder];[e setComputePipelineState:red_scan_pipeline];[e setBuffer:sa offset:0 atIndex:0];[e setBytes:&phase length:sizeof(phase) atIndex:1];[e setBuffer:sc offset:0 atIndex:2];[e setBuffer:so offset:0 atIndex:3];[e setBuffer:st offset:0 atIndex:4];[e setBuffer:sbo offset:0 atIndex:5];[e setBuffer:slt offset:0 atIndex:6];[e setBuffer:slo offset:0 atIndex:7];[e setBuffer:sum offset:0 atIndex:8];if(phase==2U)[e dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];else [e dispatchThreadgroupsWithIndirectBuffer:sargs indirectBufferOffset:0 threadsPerThreadgroup:MTLSizeMake(256,1,1)];[e endEncoding];}
  [scan_command commit];[scan_command waitUntilCompleted];const auto* scan_result=static_cast<const std::uint32_t*>(so.contents);const auto* scan_sum=static_cast<const std::uint32_t*>(sum.contents);std::uint32_t prefix{};if(scan_command.status!=MTLCommandBufferStatusCompleted||scan_result==nullptr||scan_sum==nullptr||scan_sum[0U]!=expected_total)return false;for(std::uint32_t i=0U;i<scan_count;++i){if(scan_result[i]!=prefix)return false;prefix+=scan_counts[i];}
  std::printf("{\"event\":\"metal_gpu_compact_green_closure\",\"cases\":%zu,"
              "\"green\":%s,\"malformed_retained\":true,\"budget_retained\":true,"
              "\"red_terminal_latched\":true,\"device_quiesced\":true,\"passed\":true}\n",
      completed,saw_green?"true":"false");
  return completed==cuts.size()&&saw_green;
}

// P7a2's hardware gate uses no legacy terrain-cell payload and deliberately
// creates no drawable.  It verifies that the translated kernel reconstructs
// the compact P6 BCC owners and evaluates the P7a1 planetary field tuple.
bool run_metal_gpu_terrain_classify_smoke_test(id<MTLDevice> device) {
  const auto shader_path=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR)/
      "gpu_terrain_classify.comp.metal";
  id<MTLLibrary> library=make_file_shader_library(device,shader_path.string().c_str());
  id<MTLFunction> function=[library newFunctionWithName:@"main0"];
  NSError* error=nil;
  id<MTLComputePipelineState> pipeline=function==nil?nil:
      [device newComputePipelineStateWithFunction:function error:&error];
  if(pipeline==nil){
    std::fprintf(stderr,"Metal terrain-classification pipeline creation failed: %s\n",
        error==nil?"missing translated entry point":error.localizedDescription.UTF8String);
    return false;
  }
  std::vector<tetra::WorldTetAddress> candidates;
  for(std::uint8_t root=0U;root<tetra::bcc_root_tetrahedron_count;++root)
    candidates.push_back(tetra::WorldTetAddress::root(root));
  candidates.erase(std::ranges::find(candidates,tetra::WorldTetAddress::root(0U)));
  for(std::uint8_t child=0U;child<8U;++child)
    candidates.push_back(tetra::WorldTetAddress::root(0U).child(child));
  std::ranges::sort(candidates);
  const auto packet=tetra::make_gpu_green_mask_packet(candidates,101U);
  const auto templates=tetra::make_gpu_green_template_table();
  tetra::GpuTerrainFieldTupleParameters field_parameters;
  field_parameters.source_revision=101U;field_parameters.field_revision=19U;
  field_parameters.domain.world_extent=1.0;
  field_parameters.field.kind=tetra::ImplicitShapeKind::perlin_terrain;
  // Keep every classification sample away from an implicit zero crossing so
  // this parity gate tests the shared float grammar rather than tie-breaking
  // between CPU double and device float arithmetic.
  field_parameters.field.centre={.5,.52,.5};field_parameters.field.radius=.37;
  auto& terrain=field_parameters.field.terrain;
  terrain.planet_radius=.37;
  const auto tuple=tetra::make_gpu_terrain_field_tuple(field_parameters);
  const auto expected=tetra::gpu_terrain_classify_packet(packet,tuple,100000U);
  if(expected.overflow||expected.records.empty()){
    std::fprintf(stderr,"Metal terrain-classification host oracle is empty or overflowing\n");
    return false;
  }
  const auto make_buffer=[&](const void* bytes,NSUInteger length){
    return [device newBufferWithBytes:bytes length:length
        options:MTLResourceStorageModeShared];
  };
  id<MTLCommandQueue> queue=[device newCommandQueue];
  if(queue==nil){std::fprintf(stderr,"Metal terrain-classification queue creation failed\n");return false;}
  const auto dispatch=[&](const tetra::GpuTerrainFieldTuple& input_tuple,
                          std::vector<tetra::GpuGreenMaskOwnerRecord> owners,
                          std::uint32_t capacity,bool expect_failure){
    const auto expected_for_tuple=expect_failure?
        tetra::GpuTerrainClassification{}:tetra::gpu_terrain_classify_packet(
            packet,input_tuple,100000U);
    const auto expected_roots=expect_failure?std::vector<tetra::GpuTerrainRootRecord>{}:
        tetra::gpu_terrain_root_packet(packet,input_tuple,100000U);
    const std::size_t output_words=4U+
        std::max<std::size_t>(capacity,packet.owners.size()*24U)*24U;
    std::vector<std::uint32_t> zeroed(output_words);
    id<MTLBuffer> field_buffer=make_buffer(&input_tuple,sizeof(input_tuple));
    id<MTLBuffer> owner_buffer=make_buffer(owners.data(),
        owners.size()*sizeof(owners.front()));
    id<MTLBuffer> template_buffer=make_buffer(templates.data(),sizeof(templates));
    id<MTLBuffer> output=make_buffer(zeroed.data(),
        zeroed.size()*sizeof(zeroed.front()));
    const std::array<std::uint32_t,4> parameters{
        static_cast<std::uint32_t>(owners.size()),capacity,101U,0U};
    if(field_buffer==nil||owner_buffer==nil||template_buffer==nil||output==nil){
      std::fprintf(stderr,"Metal terrain-classification buffer allocation failed\n");return false;
    }
    id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:field_buffer offset:0U atIndex:0U];
    [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:1U];
    [encoder setBuffer:owner_buffer offset:0U atIndex:2U];
    [encoder setBuffer:output offset:0U atIndex:3U];
    [encoder setBuffer:template_buffer offset:0U atIndex:4U];
    [encoder dispatchThreads:MTLSizeMake(owners.size(),1U,1U)
         threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
    [encoder endEncoding];[command commit];[command waitUntilCompleted];
    if(command.status!=MTLCommandBufferStatusCompleted){
      std::fprintf(stderr,"Metal terrain-classification command failed\n");return false;
    }
    const auto* words=static_cast<const std::uint32_t*>(output.contents);
    if(expect_failure&&words[2]==0U){
      std::fprintf(stderr,"Metal terrain-classification failed to reject malformed input\n");return false;
    }
    if(expect_failure)return true;
    const bool header_matches=words[2]==0U&&words[0]==expected_for_tuple.attempted_records&&
        words[1]==expected_for_tuple.crossing_cells;
    std::uint32_t device_crossing_records{};
    for(std::size_t owner_index=0U;owner_index<owners.size();++owner_index)
      for(std::size_t cell=0U;cell<24U;++cell)
        if(words[4U+(owner_index*24U+cell)*24U+3U]>=3U)++device_crossing_records;
    for(std::size_t record_index=0U;record_index<expected_for_tuple.records.size();++record_index) {
      const auto& record=expected_for_tuple.records[record_index];
      const std::size_t output_index=static_cast<std::size_t>(record.owner_index)*24U+
          record.template_cell;
      if(output_index>=capacity){std::fprintf(stderr,"Metal terrain-classification output capacity mismatch\n");return false;}
      const auto offset=4U+output_index*24U;
      if(words[offset]!=record.owner_index||words[offset+1U]!=record.template_cell||
         words[offset+2U]!=record.corner_negative_mask||
         words[offset+3U]!=record.crossing_count){
        std::fprintf(stderr,"Metal terrain-classification record mismatch at %zu: %u/%u/%u/%u expected %u/%u/%u/%u\n",
            output_index,words[offset],words[offset+1U],words[offset+2U],words[offset+3U],
            record.owner_index,record.template_cell,record.corner_negative_mask,
            record.crossing_count);return false;
      }
      const auto& roots=expected_roots[record_index];
      if(words[offset+22U]!=roots.valid_edge_mask){
        std::fprintf(stderr,"Metal terrain root mask mismatch at %zu\n",output_index);return false;
      }
      for(std::size_t edge=0U;edge<6U;++edge)if((roots.valid_edge_mask&(1U<<edge))!=0U)
        for(std::size_t axis=0U;axis<3U;++axis) {
          const auto device_root=std::bit_cast<float>(words[offset+4U+edge*3U+axis]);
          const auto expected_root=axis==0U?roots.roots[edge].x:
              axis==1U?roots.roots[edge].y:roots.roots[edge].z;
          if(std::abs(static_cast<double>(device_root)-expected_root)>2.0e-5){
            std::fprintf(stderr,"Metal terrain root mismatch at %zu edge %zu: %.9g %.9g\n",output_index,edge,
                static_cast<double>(device_root),expected_root);return false;
          }
        }
    }
    if(!header_matches){
      std::fprintf(stderr,"Metal terrain-classification header mismatch %u %u %u expected %u %u records %u\n",
          words[0],words[1],words[2],expected.attempted_records,
          expected_for_tuple.crossing_cells,device_crossing_records);return false;
    }
    return true;
  };
  const auto capacity=static_cast<std::uint32_t>(packet.owners.size()*24U);
  if(!dispatch(tuple,packet.owners,capacity,false))return false;
  auto moved=tuple;moved.centre_radius[0]+=.03125F;
  moved.revision_lanes[2]+=1U;
  if(!dispatch(moved,packet.owners,capacity,false))return false;
  if(!dispatch(tuple,packet.owners,0U,true))return false;
  auto stale=tuple;stale.revision_lanes[0]^=1U;
  if(!dispatch(stale,packet.owners,capacity,true))return false;
  auto malformed=packet.owners;malformed.front().mask=64U;
  if(!dispatch(tuple,std::move(malformed),capacity,true))return false;
  std::printf("{\"event\":\"metal_gpu_terrain_classify\","
              "\"owners\":%zu,\"records\":%u,\"passed\":true}\n",
              packet.owners.size(),expected.attempted_records);
  return true;
}

// P7b2 chains P7b1's compact root buffer into a second, non-drawable kernel.
// The fixture deliberately reads the compact stream back only to compare it to
// the host oracle; no live renderer resource is created or promoted here.
bool run_metal_gpu_terrain_triangle_smoke_test(id<MTLDevice> device) {
  const auto shader_directory=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR);
  const auto make_pipeline=[&](const char* filename){
    id<MTLLibrary> library=make_file_shader_library(device,(shader_directory/filename).string().c_str());
    NSError* error=nil;id<MTLFunction> function=[library newFunctionWithName:@"main0"];
    id<MTLComputePipelineState> pipeline=function==nil?nil:
        [device newComputePipelineStateWithFunction:function error:&error];
    if(pipeline==nil)std::fprintf(stderr,"Metal terrain triangle pipeline creation failed: %s\n",
        error==nil?"missing translated entry point":error.localizedDescription.UTF8String);
    return pipeline;
  };
  id<MTLComputePipelineState> classify=make_pipeline("gpu_terrain_classify.comp.metal");
  id<MTLComputePipelineState> triangles=make_pipeline("gpu_terrain_triangles.comp.metal");
  if(classify==nil||triangles==nil)return false;
  std::vector<tetra::WorldTetAddress> candidates;
  for(std::uint8_t root=0U;root<tetra::bcc_root_tetrahedron_count;++root)
    candidates.push_back(tetra::WorldTetAddress::root(root));
  candidates.erase(std::ranges::find(candidates,tetra::WorldTetAddress::root(0U)));
  for(std::uint8_t child=0U;child<8U;++child)candidates.push_back(tetra::WorldTetAddress::root(0U).child(child));
  std::ranges::sort(candidates);
  const auto packet=tetra::make_gpu_green_mask_packet(candidates,101U);
  const auto templates=tetra::make_gpu_green_template_table();
  tetra::GpuTerrainFieldTupleParameters field_parameters;
  field_parameters.source_revision=101U;field_parameters.field_revision=19U;
  field_parameters.domain.world_extent=1.0;field_parameters.field.kind=tetra::ImplicitShapeKind::perlin_terrain;
  field_parameters.field.centre={.5,.52,.5};field_parameters.field.radius=.37;
  field_parameters.field.terrain.planet_radius=.37;
  const auto tuple=tetra::make_gpu_terrain_field_tuple(field_parameters);
  const auto make_buffer=[&](const void* bytes,NSUInteger length){return [device newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared];};
  id<MTLCommandQueue> queue=[device newCommandQueue];if(queue==nil)return false;
  const auto records=static_cast<std::uint32_t>(packet.owners.size()*24U);
  const auto dispatch=[&](const tetra::GpuTerrainFieldTuple& input_tuple,
                          std::vector<tetra::GpuGreenMaskOwnerRecord> owners,
                          std::uint32_t triangle_capacity,bool expect_failure,bool remove_root){
    const auto expected_roots=expect_failure?std::vector<tetra::GpuTerrainRootRecord>{}:
        tetra::gpu_terrain_root_packet(packet,input_tuple,100000U);
    const auto expected=expect_failure?std::vector<tetra::GpuTerrainBaseTriangleRecord>{}:
        tetra::gpu_terrain_base_triangles(expected_roots,100000U);
    std::vector<std::uint32_t> root_zeroes(4U+static_cast<std::size_t>(records)*24U);
    std::vector<std::uint32_t> triangle_zeroes(4U+std::max<std::size_t>(triangle_capacity,1U)*16U);
    id<MTLBuffer> field=make_buffer(&input_tuple,sizeof(input_tuple));
    id<MTLBuffer> owner=make_buffer(owners.data(),owners.size()*sizeof(owners.front()));
    id<MTLBuffer> stencil=make_buffer(templates.data(),sizeof(templates));
    id<MTLBuffer> root_output=make_buffer(root_zeroes.data(),root_zeroes.size()*sizeof(std::uint32_t));
    id<MTLBuffer> triangle_output=make_buffer(triangle_zeroes.data(),triangle_zeroes.size()*sizeof(std::uint32_t));
    if(field==nil||owner==nil||stencil==nil||root_output==nil||triangle_output==nil){std::fprintf(stderr,"Metal terrain triangle buffer allocation failed\n");return false;}
    const std::array<std::uint32_t,4> classify_parameters{static_cast<std::uint32_t>(owners.size()),records,101U,0U};
    id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:classify];[encoder setBuffer:field offset:0 atIndex:0];
    [encoder setBytes:classify_parameters.data() length:sizeof(classify_parameters) atIndex:1];
    [encoder setBuffer:owner offset:0 atIndex:2];[encoder setBuffer:root_output offset:0 atIndex:3];[encoder setBuffer:stencil offset:0 atIndex:4];
    [encoder dispatchThreads:MTLSizeMake(owners.size(),1U,1U) threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
    [encoder endEncoding];[command commit];[command waitUntilCompleted];
    if(command.status!=MTLCommandBufferStatusCompleted){std::fprintf(stderr,"Metal terrain triangle root command failed\n");return false;}
    auto* root_words=static_cast<std::uint32_t*>(root_output.contents);
    if(!expect_failure) {
      std::uint32_t rooted{};
      for(std::uint32_t index=0U;index<records;++index)
        rooted+=root_words[4U+index*24U+22U]!=0U?1U:0U;
      if(root_words[0U]!=expected_roots.size()||rooted==0U){
        std::fprintf(stderr,"Metal terrain triangle root stream mismatch %u %u expected %zu\n",root_words[0U],rooted,expected_roots.size());return false;
      }
    }
    if(remove_root&&root_words[2U]==0U) {
      bool removed=false;
      for(std::uint32_t index=0U;index<records&&!removed;++index) {
        auto& valid=root_words[4U+index*24U+22U];
        if(valid!=0U) { valid&=valid-1U;removed=true; }
      }
      if(!removed){std::fprintf(stderr,"Metal terrain triangle missing-root fixture had no roots\n");return false;}
    }
    command=[queue commandBuffer];encoder=[command computeCommandEncoder];
    const std::array<std::uint32_t,4> triangle_parameters{static_cast<std::uint32_t>(owners.size()),records,triangle_capacity,101U};
    [encoder setComputePipelineState:triangles];[encoder setBuffer:root_output offset:0 atIndex:0];
    [encoder setBytes:triangle_parameters.data() length:sizeof(triangle_parameters) atIndex:1];
    [encoder setBuffer:owner offset:0 atIndex:2];[encoder setBuffer:stencil offset:0 atIndex:3];
    [encoder setBuffer:triangle_output offset:0 atIndex:4];
    [encoder dispatchThreads:MTLSizeMake(1U,1U,1U) threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
    [encoder endEncoding];[command commit];[command waitUntilCompleted];
    if(command.status!=MTLCommandBufferStatusCompleted){std::fprintf(stderr,"Metal terrain triangle command failed\n");return false;}
    const auto* output=static_cast<const std::uint32_t*>(triangle_output.contents);
    if(expect_failure||remove_root){
      if(output[1U]==0U){std::fprintf(stderr,"Metal terrain triangles failed closed-output gate\n");return false;}
      return true;
    }
    if(output[1U]!=0U||output[0U]!=expected.size()){
      std::fprintf(stderr,"Metal terrain triangle header mismatch %u %u expected %zu\n",output[0U],output[1U],expected.size());return false;
    }
    for(std::size_t index=0U;index<expected.size();++index){
      const auto& want=expected[index];const auto offset=4U+index*16U;
      const auto packed=static_cast<std::uint32_t>(want.edges[0])|
          (static_cast<std::uint32_t>(want.edges[1])<<8U)|(static_cast<std::uint32_t>(want.edges[2])<<16U);
      if(output[offset]!=want.owner_index||output[offset+1U]!=want.template_cell||
         output[offset+2U]!=want.corner_negative_mask||output[offset+3U]!=packed){std::fprintf(stderr,"Metal terrain triangle record mismatch at %zu\n",index);return false;}
      for(std::size_t corner=0U;corner<3U;++corner)for(std::size_t axis=0U;axis<3U;++axis){
        const float actual=std::bit_cast<float>(output[offset+4U+corner*3U+axis]);
        const auto& point=want.roots[corner];const double target=axis==0U?point.x:axis==1U?point.y:point.z;
        if(std::abs(static_cast<double>(actual)-target)>2.0e-5){std::fprintf(stderr,"Metal terrain triangle root mismatch at %zu\n",index);return false;}
      }
    }
    return true;
  };
  const auto capacity=100000U;
  if(!dispatch(tuple,packet.owners,capacity,false,false))return false;
  auto moved=tuple;moved.centre_radius[0]+=.03125F;moved.revision_lanes[2]+=1U;
  if(!dispatch(moved,packet.owners,capacity,false,false))return false;
  if(!dispatch(tuple,packet.owners,0U,true,false))return false;
  if(!dispatch(tuple,packet.owners,capacity,false,true))return false;
  auto stale=tuple;stale.revision_lanes[0]^=1U;
  if(!dispatch(stale,packet.owners,capacity,true,false))return false;
  auto malformed=packet.owners;malformed.front().mask=64U;
  if(!dispatch(tuple,std::move(malformed),capacity,true,false))return false;
  // An empty P7b1 stream is a valid empty surface, distinct from failure.
  const std::array<std::uint32_t,4> empty_roots{};
  std::array<std::uint32_t,20> empty_triangles{};
  id<MTLBuffer> empty_root_buffer=make_buffer(empty_roots.data(),sizeof(empty_roots));
  id<MTLBuffer> empty_triangle_buffer=make_buffer(empty_triangles.data(),sizeof(empty_triangles));
  if(empty_root_buffer==nil||empty_triangle_buffer==nil)return false;
  id<MTLCommandBuffer> empty_command=[queue commandBuffer];
  id<MTLComputeCommandEncoder> empty_encoder=[empty_command computeCommandEncoder];
  const std::array<std::uint32_t,4> empty_parameters{0U,0U,1U,101U};
  [empty_encoder setComputePipelineState:triangles];
  [empty_encoder setBuffer:empty_root_buffer offset:0 atIndex:0];
  [empty_encoder setBytes:empty_parameters.data() length:sizeof(empty_parameters) atIndex:1];
  // No topology resource is touched for a zero-slot stream, so the empty
  // case intentionally needs only the root and output buffers.
  [empty_encoder setBuffer:empty_triangle_buffer offset:0 atIndex:4];
  [empty_encoder dispatchThreads:MTLSizeMake(1U,1U,1U) threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
  [empty_encoder endEncoding];[empty_command commit];[empty_command waitUntilCompleted];
  const auto* empty_words=static_cast<const std::uint32_t*>(empty_triangle_buffer.contents);
  if(empty_command.status!=MTLCommandBufferStatusCompleted||empty_words[0U]!=0U||empty_words[1U]!=0U)return false;
  std::printf("{\"event\":\"metal_gpu_terrain_triangles\",\"roots\":%u,\"passed\":true}\n",records);
  return true;
}

// P7c2b1b0 proves the replacement for P7b2's one-lane scan independently of
// the later live-slot chain.  The fixture deliberately retains shared output
// only for byte-for-byte comparison with the CPU oracle.
bool run_metal_gpu_terrain_parallel_triangle_smoke_test(id<MTLDevice> device) {
  const auto directory=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR);
  const auto pipeline=[&](const char* name)->id<MTLComputePipelineState>{
    id<MTLLibrary> library=make_file_shader_library(device,(directory/name).string().c_str());
    NSError* error=nil;id<MTLFunction> function=[library newFunctionWithName:@"main0"];
    id<MTLComputePipelineState> result=function==nil?nil:
        [device newComputePipelineStateWithFunction:function error:&error];
    if(result==nil)std::fprintf(stderr,"Metal parallel terrain pipeline failed: %s\n",
        error==nil?"missing entry point":error.localizedDescription.UTF8String);
    return result;
  };
  id<MTLComputePipelineState> classify=pipeline("gpu_terrain_classify.comp.metal");
  id<MTLComputePipelineState> count=pipeline("gpu_terrain_triangle_counts.comp.metal");
  id<MTLComputePipelineState> scan=pipeline("gpu_terrain_exclusive_scan.comp.metal");
  id<MTLComputePipelineState> finalize=pipeline("gpu_terrain_triangle_finalize.comp.metal");
  id<MTLComputePipelineState> scatter=pipeline("gpu_terrain_triangle_scatter.comp.metal");
  if(classify==nil||count==nil||scan==nil||finalize==nil||scatter==nil)return false;
  std::vector<tetra::WorldTetAddress> candidates;
  for(std::uint8_t root=0U;root<tetra::bcc_root_tetrahedron_count;++root)
    candidates.push_back(tetra::WorldTetAddress::root(root));
  candidates.erase(std::ranges::find(candidates,tetra::WorldTetAddress::root(0U)));
  for(std::uint8_t child=0U;child<8U;++child)candidates.push_back(
      tetra::WorldTetAddress::root(0U).child(child));
  std::ranges::sort(candidates);
  const auto packet=tetra::make_gpu_green_mask_packet(candidates,211U);
  const auto templates=tetra::make_gpu_green_template_table();
  tetra::GpuTerrainFieldTupleParameters parameters;
  parameters.source_revision=211U;parameters.field_revision=23U;
  parameters.domain.world_extent=1.0;parameters.field.kind=tetra::ImplicitShapeKind::perlin_terrain;
  parameters.field.centre={.5,.52,.5};parameters.field.radius=.37;
  parameters.field.terrain.planet_radius=.37;
  const auto tuple=tetra::make_gpu_terrain_field_tuple(parameters);
  const auto expected_roots=tetra::gpu_terrain_root_packet(packet,tuple,100000U);
  const auto expected=tetra::gpu_terrain_base_triangles(expected_roots,100000U);
  if(expected.empty())return false;
  const auto make=[&](const void* bytes,NSUInteger length){return [device newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared];};
  const auto slots=static_cast<std::uint32_t>(packet.owners.size()*24U);
  std::vector<std::uint32_t> root_zeroes(4U+static_cast<std::size_t>(slots)*24U);
  id<MTLBuffer> field=make(&tuple,sizeof(tuple));
  id<MTLBuffer> owners=make(packet.owners.data(),packet.owners.size()*sizeof(packet.owners.front()));
  id<MTLBuffer> stencil=make(templates.data(),sizeof(templates));
  id<MTLBuffer> roots=make(root_zeroes.data(),root_zeroes.size()*sizeof(std::uint32_t));
  id<MTLCommandQueue> queue=[device newCommandQueue];
  if(field==nil||owners==nil||stencil==nil||roots==nil||queue==nil)return false;
  id<MTLCommandBuffer> roots_command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[roots_command computeCommandEncoder];
  const std::array<std::uint32_t,4> classify_params{static_cast<std::uint32_t>(packet.owners.size()),slots,211U,0U};
  [encoder setComputePipelineState:classify];[encoder setBuffer:field offset:0 atIndex:0U];
  [encoder setBytes:classify_params.data() length:sizeof(classify_params) atIndex:1U];
  [encoder setBuffer:owners offset:0 atIndex:2U];[encoder setBuffer:roots offset:0 atIndex:3U];[encoder setBuffer:stencil offset:0 atIndex:4U];
  [encoder dispatchThreads:MTLSizeMake(packet.owners.size(),1U,1U) threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];[encoder endEncoding];
  [roots_command commit];[roots_command waitUntilCompleted];
  if(roots_command.status!=MTLCommandBufferStatusCompleted)return false;
  const auto* classified=static_cast<const std::uint32_t*>(roots.contents);
  if(classified[0U]!=expected_roots.size()||classified[2U]!=0U){
    std::fprintf(stderr,"parallel terrain root stream mismatch %u %u expected %zu\n",
        classified[0U],classified[2U],expected_roots.size());
    return false;
  }
  const auto run=[&](std::uint32_t root_count,std::uint32_t capacity,bool expect_failure)->bool{
    const auto* source=static_cast<const std::uint32_t*>(roots.contents);
    std::vector<std::uint32_t> expanded(4U+static_cast<std::size_t>(root_count)*24U);
    for(std::uint32_t index=0U;index<root_count;++index)
      std::memcpy(expanded.data()+4U+static_cast<std::size_t>(index)*24U,
          source+4U+static_cast<std::size_t>(index%slots)*24U,24U*sizeof(std::uint32_t));
    id<MTLBuffer> input=make(expanded.data(),expanded.size()*sizeof(std::uint32_t));
    std::vector<std::uint32_t> zeroes(root_count,0U),header(4U,0U),
        output_zeroes(4U+static_cast<std::size_t>(capacity)*16U,0U);
    id<MTLBuffer> counts=make(zeroes.data(),zeroes.size()*sizeof(std::uint32_t));
    id<MTLBuffer> status=make(header.data(),header.size()*sizeof(std::uint32_t));
    id<MTLBuffer> output=make(output_zeroes.data(),output_zeroes.size()*sizeof(std::uint32_t));
    if(input==nil||counts==nil||status==nil||output==nil)return false;
    std::vector<id<MTLBuffer>> offsets;
    std::vector<id<MTLBuffer>> totals;
    // Objective-C pointers in a C++ vector do not carry ARC ownership.  Keep
    // every scan level alive until the command buffer has completed.
    NSMutableArray<id<MTLBuffer>>* scan_keepalive=[NSMutableArray array];
    std::uint32_t level_count=root_count;id<MTLBuffer> level_input=counts;
    id<MTLCommandBuffer> command=[queue commandBuffer];
    encoder=[command computeCommandEncoder];const std::array<std::uint32_t,2> count_params{static_cast<std::uint32_t>(packet.owners.size()),root_count};
    [encoder setComputePipelineState:count];[encoder setBuffer:input offset:0 atIndex:0U];[encoder setBytes:count_params.data() length:sizeof(count_params) atIndex:1U];[encoder setBuffer:owners offset:0 atIndex:2U];[encoder setBuffer:stencil offset:0 atIndex:3U];[encoder setBuffer:status offset:0 atIndex:4U];[encoder setBuffer:counts offset:0 atIndex:5U];[encoder dispatchThreads:MTLSizeMake(root_count,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[encoder endEncoding];
    while(level_count>1U){const std::uint32_t blocks=(level_count+255U)/256U;std::vector<std::uint32_t> level_zeroes(level_count),block_zeroes(blocks);id<MTLBuffer> offset=make(level_zeroes.data(),level_zeroes.size()*sizeof(std::uint32_t));id<MTLBuffer> total=make(block_zeroes.data(),block_zeroes.size()*sizeof(std::uint32_t));if(offset==nil||total==nil)return false;[scan_keepalive addObject:offset];[scan_keepalive addObject:total];offsets.push_back(offset);totals.push_back(total);encoder=[command computeCommandEncoder];const std::array<std::uint32_t,2> scan_params{level_count,0U};[encoder setComputePipelineState:scan];[encoder setBytes:scan_params.data() length:sizeof(scan_params) atIndex:0U];[encoder setBuffer:offset offset:0 atIndex:1U];[encoder setBuffer:level_input offset:0 atIndex:2U];[encoder setBuffer:total offset:0 atIndex:3U];[encoder dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(blocks)*256U,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[encoder endEncoding];level_input=total;level_count=blocks;}
    for(std::size_t level=offsets.size();level-->1U;){const auto value_count=static_cast<std::uint32_t>([offsets[level-1U] length]/sizeof(std::uint32_t));std::vector<std::uint32_t> added_zeroes(value_count);id<MTLBuffer> added=make(added_zeroes.data(),added_zeroes.size()*sizeof(std::uint32_t));if(added==nil)return false;[scan_keepalive addObject:added];encoder=[command computeCommandEncoder];const std::array<std::uint32_t,2> scan_params{value_count,1U};[encoder setComputePipelineState:scan];[encoder setBytes:scan_params.data() length:sizeof(scan_params) atIndex:0U];[encoder setBuffer:added offset:0 atIndex:1U];[encoder setBuffer:offsets[level-1U] offset:0 atIndex:2U];[encoder setBuffer:offsets[level] offset:0 atIndex:3U];const auto blocks=(scan_params[0]+255U)/256U;[encoder dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(blocks)*256U,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[encoder endEncoding];offsets[level-1U]=added;}
    const std::array<std::uint32_t,2> final_params{root_count,capacity};encoder=[command computeCommandEncoder];[encoder setComputePipelineState:finalize];[encoder setBytes:final_params.data() length:sizeof(final_params) atIndex:0U];[encoder setBuffer:output offset:0 atIndex:1U];[encoder setBuffer:status offset:0 atIndex:2U];[encoder setBuffer:offsets.front() offset:0 atIndex:3U];[encoder setBuffer:counts offset:0 atIndex:4U];[encoder dispatchThreads:MTLSizeMake(1U,1U,1U) threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];[encoder endEncoding];encoder=[command computeCommandEncoder];[encoder setComputePipelineState:scatter];[encoder setBytes:final_params.data() length:sizeof(final_params) atIndex:0U];[encoder setBuffer:output offset:0 atIndex:1U];[encoder setBuffer:counts offset:0 atIndex:2U];[encoder setBuffer:offsets.front() offset:0 atIndex:3U];[encoder setBuffer:input offset:0 atIndex:4U];[encoder dispatchThreads:MTLSizeMake(root_count,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[encoder endEncoding];[command commit];[command waitUntilCompleted];if(command.status!=MTLCommandBufferStatusCompleted)return false;const auto* words=static_cast<const std::uint32_t*>(output.contents);if(expect_failure)return words[1U]!=0U&&words[0U]==0U;std::uint32_t expected_total=0U;for(std::uint32_t index=0U;index<root_count;++index){const auto signs=source[4U+static_cast<std::size_t>(index%slots)*24U+2U];std::uint32_t crossings=0U;constexpr std::array<std::array<std::uint32_t,2>,6> edges{{{{0U,1U}},{{0U,2U}},{{0U,3U}},{{1U,2U}},{{1U,3U}},{{2U,3U}}}};for(const auto& edge:edges)crossings+=((signs>>edge[0])&1U)!=((signs>>edge[1])&1U);expected_total+=crossings==3U?1U:crossings==4U?2U:0U;}if(words[1U]!=0U||words[0U]!=expected_total){const auto* count_words=static_cast<const std::uint32_t*>(counts.contents);const auto* offset_words=static_cast<const std::uint32_t*>(offsets.front().contents);std::uint32_t count_total=0U;for(std::uint32_t index=0U;index<root_count;++index)count_total+=count_words[index];std::fprintf(stderr,"parallel terrain header %u %u expected %u count %u last-offset %u last-count %u status %u\n",words[0U],words[1U],expected_total,count_total,offset_words[root_count-1U],count_words[root_count-1U],static_cast<const std::uint32_t*>(status.contents)[1U]);return false;}for(std::uint32_t index=0U;index<root_count;++index)if(static_cast<const std::uint32_t*>(counts.contents)[index]>0U&&static_cast<const std::uint32_t*>(offsets.front().contents)[index]>expected_total)return false;
    // The small fixture compares every compact record to the CPU oracle, not
    // merely its count: parallel scatter must retain P7b2's canonical order.
    if(root_count==slots)for(std::size_t index=0U;index<expected.size();++index){const auto& want=expected[index];const auto offset=4U+index*16U;const auto packed=static_cast<std::uint32_t>(want.edges[0])|(static_cast<std::uint32_t>(want.edges[1])<<8U)|(static_cast<std::uint32_t>(want.edges[2])<<16U);if(words[offset]!=want.owner_index||words[offset+1U]!=want.template_cell||words[offset+2U]!=want.corner_negative_mask||words[offset+3U]!=packed)return false;for(std::size_t corner=0U;corner<3U;++corner)for(std::size_t axis=0U;axis<3U;++axis){const float actual=std::bit_cast<float>(words[offset+4U+corner*3U+axis]);const auto& point=want.roots[corner];const double target=axis==0U?point.x:axis==1U?point.y:point.z;if(std::abs(static_cast<double>(actual)-target)>2.0e-5)return false;}}
    return true;
  };
  std::fprintf(stderr,"parallel terrain: mixed-depth scan\n");
  if(!run(slots,100000U,false))return false;
  // 131,072 slots force two scan levels; this is a bounded production-scale
  // P6-derived stream and remains entirely diagnostic.
  std::fprintf(stderr,"parallel terrain: production scan\n");
  if(!run(131072U,262144U,false))return false;
  std::fprintf(stderr,"parallel terrain: overflow gate\n");
  if(!run(slots,0U,true))return false;
  std::printf("{\"event\":\"metal_gpu_terrain_parallel_triangles\",\"roots\":%u,\"passed\":true}\n",slots);
  return true;
}

// P7c1b consumes the compact P7b2 stream but remains strictly diagnostic.
// This readback fixture is its hardware oracle: production draw, shadow,
// wireframe, and ray-tracing resources deliberately remain untouched.
// P10b executes the immutable closure journal through the same bounded
// count/scan/scatter shape used by the terrain compactor.  This is deliberately
// qualification-only: the readback is checked against P10a and no directory
// or published CPU volume is ever changed here.
// Retained as a transport regression fixture.  The P10b gate below exercises
// the actual fixed point before using this same bounded compaction shape.
[[maybe_unused]] bool run_metal_gpu_volume_split_closure_transport_test(id<MTLDevice> device) {
  const auto directory=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR);
  const auto pipeline=[&](const char* name)->id<MTLComputePipelineState>{
    id<MTLLibrary> library=make_file_shader_library(device,(directory/name).string().c_str());
    NSError* error=nil;id<MTLFunction> function=[library newFunctionWithName:@"main0"];
    id<MTLComputePipelineState> result=function==nil?nil:
        [device newComputePipelineStateWithFunction:function error:&error];
    if(result==nil)std::fprintf(stderr,"Metal GPU volume closure pipeline failed: %s\\n",
        error==nil?"missing entry point":error.localizedDescription.UTF8String);
    return result;
  };
  id<MTLComputePipelineState> closure_pipeline=pipeline("gpu_volume_split_closure.comp.metal");
  id<MTLComputePipelineState> scan_pipeline=pipeline("gpu_terrain_exclusive_scan.comp.metal");
  if(closure_pipeline==nil||scan_pipeline==nil)return false;
  std::vector<tetra::WorldTetAddress> roots;
  for(std::uint8_t root=0U;root<tetra::bcc_root_tetrahedron_count;++root)
    roots.push_back(tetra::WorldTetAddress::root(root));
  tetra::WorldCutDirectory source(tetra::make_complete_world_cut_checkpoint(
      roots,3U,801U,tetra::HierarchyResidencyTier::conforming_volume));
  const std::array requested{tetra::WorldTetAddress::root(0U)};
  const auto oracle=tetra::gpu_conforming_volume_split_proposal(
      source,requested,801U,802U,512U);
  std::vector<std::array<std::uint32_t,4>> closure_worklist=oracle.requested_splits;
  closure_worklist.insert(closure_worklist.end(),oracle.closure_splits.begin(),
      oracle.closure_splits.end());
  std::ranges::sort(closure_worklist);
  closure_worklist.erase(std::unique(closure_worklist.begin(),closure_worklist.end()),
      closure_worklist.end());
  if(oracle.header.status!=tetra::GpuConformingVolumeProposalStatus::ready||
     closure_worklist.empty()||closure_worklist.size()>256U) {
    std::fprintf(stderr,"GPU volume closure oracle unavailable status %u worklist %zu\n",
        static_cast<unsigned>(oracle.header.status),closure_worklist.size());
    return false;
  }
  const auto make=[&](const void* bytes,NSUInteger length) {
    return [device newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared];
  };
  const auto count=static_cast<std::uint32_t>(closure_worklist.size());
  std::vector<std::uint32_t> zeroes(count,0U),offset_zeroes(count,0U),
      total_zeroes(1U,0U),status_zeroes(1U,0U);
  std::vector<std::array<std::uint32_t,4>> output(count);
  id<MTLBuffer> input=make(closure_worklist.data(),
      closure_worklist.size()*sizeof(closure_worklist.front()));
  id<MTLBuffer> counts=make(zeroes.data(),zeroes.size()*sizeof(std::uint32_t));
  id<MTLBuffer> offsets=make(offset_zeroes.data(),offset_zeroes.size()*sizeof(std::uint32_t));
  id<MTLBuffer> totals=make(total_zeroes.data(),total_zeroes.size()*sizeof(std::uint32_t));
  id<MTLBuffer> result=make(output.data(),output.size()*sizeof(output.front()));
  id<MTLBuffer> status=make(status_zeroes.data(),status_zeroes.size()*sizeof(std::uint32_t));
  id<MTLCommandQueue> queue=[device newCommandQueue];
  if(input==nil||counts==nil||offsets==nil||totals==nil||result==nil||status==nil||queue==nil)return false;
  id<MTLCommandBuffer> command=[queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
  const std::array<std::uint32_t,3> count_parameters{count,count,0U};
  [encoder setComputePipelineState:closure_pipeline];[encoder setBytes:count_parameters.data() length:sizeof(count_parameters) atIndex:0U];
  [encoder setBuffer:input offset:0U atIndex:1U];[encoder setBuffer:counts offset:0U atIndex:2U];
  [encoder setBuffer:offsets offset:0U atIndex:3U];[encoder setBuffer:result offset:0U atIndex:4U];[encoder setBuffer:status offset:0U atIndex:5U];
  [encoder dispatchThreads:MTLSizeMake(count,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[encoder endEncoding];
  encoder=[command computeCommandEncoder];const std::array<std::uint32_t,2> scan_parameters{count,0U};
  [encoder setComputePipelineState:scan_pipeline];[encoder setBytes:scan_parameters.data() length:sizeof(scan_parameters) atIndex:0U];
  [encoder setBuffer:offsets offset:0U atIndex:1U];[encoder setBuffer:counts offset:0U atIndex:2U];[encoder setBuffer:totals offset:0U atIndex:3U];
  [encoder dispatchThreads:MTLSizeMake(256U,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[encoder endEncoding];
  encoder=[command computeCommandEncoder];const std::array<std::uint32_t,3> scatter_parameters{count,count,1U};
  [encoder setComputePipelineState:closure_pipeline];[encoder setBytes:scatter_parameters.data() length:sizeof(scatter_parameters) atIndex:0U];
  [encoder setBuffer:input offset:0U atIndex:1U];[encoder setBuffer:counts offset:0U atIndex:2U];[encoder setBuffer:offsets offset:0U atIndex:3U];[encoder setBuffer:result offset:0U atIndex:4U];[encoder setBuffer:status offset:0U atIndex:5U];
  [encoder dispatchThreads:MTLSizeMake(count,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[encoder endEncoding];
  [command commit];[command waitUntilCompleted];
  if(command.status!=MTLCommandBufferStatusCompleted||
     *static_cast<const std::uint32_t*>(status.contents)!=0U||
     *static_cast<const std::uint32_t*>(totals.contents)!=count||
     std::memcmp(result.contents,closure_worklist.data(),
         closure_worklist.size()*sizeof(closure_worklist.front()))!=0) {
    std::fprintf(stderr,"GPU volume closure mismatch status %lu journal %u total %u\n",
        static_cast<unsigned long>(command.status),
        *static_cast<const std::uint32_t*>(status.contents),
        *static_cast<const std::uint32_t*>(totals.contents));
    return false;
  }
  std::printf("{\"event\":\"metal_gpu_volume_split_closure\",\"closure\":%u,\"passed\":true}\n",count);
  return true;
}

bool run_metal_gpu_volume_split_closure_smoke_test(id<MTLDevice> device) {
  const auto directory=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR);
  const auto pipeline=[&](const char* name)->id<MTLComputePipelineState>{
    id<MTLLibrary> library=make_file_shader_library(device,(directory/name).string().c_str());
    NSError* error=nil; id<MTLFunction> function=[library newFunctionWithName:@"main0"];
    return function==nil?nil:[device newComputePipelineStateWithFunction:function error:&error];
  };
  id<MTLComputePipelineState> marks_pipeline=pipeline("gpu_volume_split_marks.comp.metal");
  id<MTLComputePipelineState> compact_pipeline=pipeline("gpu_volume_split_closure.comp.metal");
  id<MTLComputePipelineState> scan_pipeline=pipeline("gpu_terrain_exclusive_scan.comp.metal");
  if(marks_pipeline==nil||compact_pipeline==nil||scan_pipeline==nil)return false;
  // Root seams plus a mixed-depth front exercise both independent device
  // propagation mechanisms.  P10a remains the byte-for-byte oracle.
  std::vector<tetra::WorldTetAddress> roots;
  for(std::uint8_t root=0;root<tetra::bcc_root_tetrahedron_count;++root)
    roots.push_back(tetra::WorldTetAddress::root(root));
  tetra::WorldCutDirectory source(tetra::make_complete_world_cut_checkpoint(
      roots,3U,801U,tetra::HierarchyResidencyTier::conforming_volume));
  // Materialize a mixed-depth source before requesting adjacent children.
  // This retains a root seam while forcing the device face-balance path.
  const std::array first_split{tetra::WorldTopologyEdit{
      tetra::WorldTetAddress::root(0U),tetra::WorldTopologyOperation::split}};
  source.publish(source.stage_transaction(first_split,802U).manifest);
  const std::array requested{tetra::WorldTetAddress::root(0U).child(0U),
                             tetra::WorldTetAddress::root(0U).child(1U)};
  const auto oracle=tetra::gpu_conforming_volume_split_proposal(source,requested,802U,803U,512U);
  const auto packet=tetra::make_gpu_conforming_volume_source_packet(source,256U,4096U,4096U);
  if(oracle.header.status!=tetra::GpuConformingVolumeProposalStatus::ready||
     packet.owners.empty()||packet.owners.size()>256U)return false;
  const auto make=[&](const void* p,NSUInteger n){return [device newBufferWithBytes:p length:n options:MTLResourceStorageModeShared];};
  std::vector<std::uint32_t> zeros(packet.owners.size()), masks=packet.owner_masks, offsets(packet.owners.size());
  std::uint32_t changed=0U,status=0U,total=0U;
  std::vector<std::array<std::uint32_t,4>> output(packet.owners.size());
  id<MTLBuffer> owners=make(packet.owners.data(),packet.owners.size()*sizeof(packet.owners.front()));
  id<MTLBuffer> requests_buffer=make(requested.data(),requested.size()*sizeof(requested.front()));
  id<MTLBuffer> marks=make(zeros.data(),zeros.size()*sizeof(std::uint32_t));
  id<MTLBuffer> mask_buffer=make(masks.data(),masks.size()*sizeof(std::uint32_t));
  id<MTLBuffer> faces=make(packet.face_pairs.data(),packet.face_pairs.size()*sizeof(packet.face_pairs.front()));
  id<MTLBuffer> edges=make(packet.edge_pairs.data(),packet.edge_pairs.size()*sizeof(packet.edge_pairs.front()));
  id<MTLBuffer> changed_buffer=make(&changed,sizeof(changed)); id<MTLBuffer> counts=make(zeros.data(),zeros.size()*sizeof(std::uint32_t));
  id<MTLBuffer> offset_buffer=make(offsets.data(),offsets.size()*sizeof(std::uint32_t)); id<MTLBuffer> total_buffer=make(&total,sizeof(total));
  id<MTLBuffer> output_buffer=make(output.data(),output.size()*sizeof(output.front())); id<MTLBuffer> status_buffer=make(&status,sizeof(status));
  id<MTLCommandQueue> queue=[device newCommandQueue];
  if(owners==nil||requests_buffer==nil||marks==nil||mask_buffer==nil||faces==nil||edges==nil||changed_buffer==nil||counts==nil||offset_buffer==nil||total_buffer==nil||output_buffer==nil||status_buffer==nil||queue==nil)return false;
  const auto dispatch_marks=[&](std::uint32_t phase,std::uint32_t pairs){
    const std::array<std::uint32_t,4> params{static_cast<std::uint32_t>(packet.owners.size()),static_cast<std::uint32_t>(requested.size()),pairs,phase};
    id<MTLCommandBuffer> command=[queue commandBuffer]; id<MTLComputeCommandEncoder> e=[command computeCommandEncoder];
    [e setComputePipelineState:marks_pipeline]; [e setBuffer:changed_buffer offset:0 atIndex:0U];[e setBytes:params.data() length:sizeof(params) atIndex:1U];
    [e setBuffer:owners offset:0 atIndex:2U];[e setBuffer:requests_buffer offset:0 atIndex:3U];[e setBuffer:marks offset:0 atIndex:4U];[e setBuffer:mask_buffer offset:0 atIndex:5U];[e setBuffer:edges offset:0 atIndex:6U];[e setBuffer:faces offset:0 atIndex:7U];
    const auto work=phase==1U?pairs:phase==3U?pairs:static_cast<std::uint32_t>(packet.owners.size());
    [e dispatchThreads:MTLSizeMake(work,1,1) threadsPerThreadgroup:MTLSizeMake(256,1,1)];[e endEncoding];[command commit];[command waitUntilCompleted];return command.status==MTLCommandBufferStatusCompleted;
  };
  if(!dispatch_marks(0U,0U))return false;
  bool settled=false;
  for(std::uint32_t round=0;round<=packet.owners.size()+8U;++round) {
    *static_cast<std::uint32_t*>(changed_buffer.contents)=0U;
    if(!dispatch_marks(1U,packet.header.edge_pair_count)||!dispatch_marks(2U,0U)||!dispatch_marks(3U,packet.header.face_pair_count))return false;
    if(*static_cast<const std::uint32_t*>(changed_buffer.contents)==0U){settled=true;break;}
  }
  if(!settled)return false;
  const std::array<std::uint32_t,3> count_params{static_cast<std::uint32_t>(packet.owners.size()),static_cast<std::uint32_t>(output.size()),0U};
  id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> e=[command computeCommandEncoder];
  [e setComputePipelineState:compact_pipeline];[e setBytes:count_params.data() length:sizeof(count_params) atIndex:0U];[e setBuffer:status_buffer offset:0 atIndex:1U];[e setBuffer:counts offset:0 atIndex:2U];[e setBuffer:marks offset:0 atIndex:3U];[e setBuffer:offset_buffer offset:0 atIndex:4U];[e setBuffer:output_buffer offset:0 atIndex:5U];[e setBuffer:owners offset:0 atIndex:6U];[e dispatchThreads:MTLSizeMake(packet.owners.size(),1,1) threadsPerThreadgroup:MTLSizeMake(256,1,1)];[e endEncoding];
  e=[command computeCommandEncoder];const std::array<std::uint32_t,2> scan_params{static_cast<std::uint32_t>(packet.owners.size()),0U};[e setComputePipelineState:scan_pipeline];[e setBytes:scan_params.data() length:sizeof(scan_params) atIndex:0U];[e setBuffer:offset_buffer offset:0 atIndex:1U];[e setBuffer:counts offset:0 atIndex:2U];[e setBuffer:total_buffer offset:0 atIndex:3U];[e dispatchThreads:MTLSizeMake(256,1,1) threadsPerThreadgroup:MTLSizeMake(256,1,1)];[e endEncoding];
  const std::array<std::uint32_t,3> scatter_params{static_cast<std::uint32_t>(packet.owners.size()),static_cast<std::uint32_t>(output.size()),1U};e=[command computeCommandEncoder];[e setComputePipelineState:compact_pipeline];[e setBytes:scatter_params.data() length:sizeof(scatter_params) atIndex:0U];[e setBuffer:status_buffer offset:0 atIndex:1U];[e setBuffer:counts offset:0 atIndex:2U];[e setBuffer:marks offset:0 atIndex:3U];[e setBuffer:offset_buffer offset:0 atIndex:4U];[e setBuffer:output_buffer offset:0 atIndex:5U];[e setBuffer:owners offset:0 atIndex:6U];[e dispatchThreads:MTLSizeMake(packet.owners.size(),1,1) threadsPerThreadgroup:MTLSizeMake(256,1,1)];[e endEncoding];[command commit];[command waitUntilCompleted];
  if(command.status!=MTLCommandBufferStatusCompleted||*static_cast<const std::uint32_t*>(status_buffer.contents)!=0U)return false;
  std::vector<std::array<std::uint32_t,4>> expected=oracle.requested_splits;expected.insert(expected.end(),oracle.closure_splits.begin(),oracle.closure_splits.end());std::ranges::sort(expected);expected.erase(std::unique(expected.begin(),expected.end()),expected.end());
  const auto actual_count=*static_cast<const std::uint32_t*>(total_buffer.contents);
  if(actual_count!=expected.size()||actual_count>output.size()||
     std::memcmp(output_buffer.contents,expected.data(),actual_count*sizeof(expected.front()))!=0) {
    std::fprintf(stderr,"GPU volume device closure mismatch actual %u expected %zu\\n",actual_count,expected.size());
    for(std::uint32_t i=0;i<actual_count;++i) {
      const auto a=static_cast<const std::array<std::uint32_t,4>*>(output_buffer.contents)[i];
      std::fprintf(stderr," actual %u: %u %u %u %u\\n",i,a[0],a[1],a[2],a[3]);
    }
    for(std::size_t i=0;i<expected.size();++i)
      std::fprintf(stderr," expected %zu: %u %u %u %u\\n",i,expected[i][0],expected[i][1],expected[i][2],expected[i][3]);
    return false;
  }
  // P10c ingests the actual device-written closure journal into an inactive
  // complete volume slot.  The CPU oracle is a validator here, never the
  // journal producer; a failed ingestion leaves the previous slot untouched.
  std::vector<tetra::GpuConformingVolumeDeviceCommand> device_journal;
  device_journal.reserve(actual_count);
  const auto* device_output=static_cast<const std::array<std::uint32_t,4>*>(
      output_buffer.contents);
  for(std::uint32_t index=0U;index<actual_count;++index)
    device_journal.push_back({device_output[index],0U,{}});
  tetra::GpuConformingVolumeSlots volume_slots(source.checkpoint());
  const auto mutation=tetra::ingest_gpu_conforming_volume_journal(
      volume_slots.active(),device_journal,802U,803U,512U);
  if(mutation.header.status!=tetra::GpuConformingVolumeMutationStatus::ready||
     !volume_slots.commit(mutation,512U)||volume_slots.active().revision()!=803U||
     volume_slots.active().canonical_cut_hash()!=oracle.header.canonical_result_hash)
    return false;
  std::printf("{\"event\":\"metal_gpu_volume_split_closure\",\"closure\":%u,\"rounds_bounded\":true,\"passed\":true}\n",actual_count); return true;
}

bool run_metal_gpu_terrain_project_smoke_test(id<MTLDevice> device) {
  const auto shader_path=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR)/
      "gpu_terrain_project.comp.metal";
  id<MTLLibrary> library=make_file_shader_library(device,shader_path.string().c_str());
  NSError* error=nil;id<MTLFunction> function=[library newFunctionWithName:@"main0"];
  id<MTLComputePipelineState> pipeline=function==nil?nil:
      [device newComputePipelineStateWithFunction:function error:&error];
  if(pipeline==nil){std::fprintf(stderr,"Metal terrain project pipeline creation failed: %s\n",
      error==nil?"missing translated entry point":error.localizedDescription.UTF8String);return false;}
  tetra::GpuTerrainFieldTupleParameters field_parameters;
  field_parameters.source_revision=101U;field_parameters.field_revision=19U;
  field_parameters.domain.world_extent=1.0;field_parameters.field.kind=tetra::ImplicitShapeKind::perlin_terrain;
  field_parameters.field.centre={.5,.52,.5};field_parameters.field.radius=.37;
  field_parameters.field.sampling_footprint=.0625;field_parameters.field.terrain.planet_radius=.37;
  const auto tuple=tetra::make_gpu_terrain_field_tuple(field_parameters);
  std::vector<tetra::WorldTetAddress> candidates;
  for(std::uint8_t root=0U;root<tetra::bcc_root_tetrahedron_count;++root)
    candidates.push_back(tetra::WorldTetAddress::root(root));
  const auto packet=tetra::make_gpu_green_mask_packet(candidates,101U);
  const auto roots=tetra::gpu_terrain_root_packet(packet,tuple,100000U);
  const auto source_triangles=tetra::gpu_terrain_base_triangles(roots,100000U);
  if(source_triangles.empty()){std::fprintf(stderr,"Metal terrain project oracle is empty\n");return false;}
  struct alignas(16) ProjectParameters {
    std::uint32_t triangle_count{},capacity{},reserved0{},reserved1{};
    std::array<float,4> render_origin{};
    std::uint32_t source_revision_low{},source_revision_high{},padding0{},padding1{};
  };
  static_assert(sizeof(ProjectParameters)==48U);
  const auto make_buffer=[&](const void* bytes,NSUInteger length){return [device newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared];};
  id<MTLCommandQueue> queue=[device newCommandQueue];if(queue==nil)return false;
  const auto dispatch=[&](const tetra::GpuTerrainFieldTuple& input_tuple,
                          std::vector<tetra::GpuTerrainBaseTriangleRecord> sources,
                          tetra::Vec3 origin,std::uint32_t capacity,bool expect_failure){
    std::vector<std::uint32_t> input(4U+sources.size()*16U);input[0U]=static_cast<std::uint32_t>(sources.size());
    // The device receives P7b2's float roots, so quantize the oracle inputs
    // identically before asking the CPU to reproduce P7c1b's contract.
    for(std::size_t index=0U;index<sources.size();++index){
      auto& source=sources[index];const auto offset=4U+index*16U;
      input[offset]=source.owner_index;input[offset+1U]=source.template_cell;input[offset+2U]=source.corner_negative_mask;
      input[offset+3U]=static_cast<std::uint32_t>(source.edges[0])|(static_cast<std::uint32_t>(source.edges[1])<<8U)|(static_cast<std::uint32_t>(source.edges[2])<<16U);
      for(std::size_t corner=0U;corner<3U;++corner)for(std::size_t axis=0U;axis<3U;++axis){
        auto value=axis==0U?source.roots[corner].x:axis==1U?source.roots[corner].y:source.roots[corner].z;
        const float rounded=static_cast<float>(value);input[offset+4U+corner*3U+axis]=std::bit_cast<std::uint32_t>(rounded);
        if(axis==0U)source.roots[corner].x=rounded;else if(axis==1U)source.roots[corner].y=rounded;else source.roots[corner].z=rounded;
      }
    }
    std::vector<tetra::GpuTerrainProjectedTriangleRecord> expected;
    if(!expect_failure)expected=tetra::gpu_terrain_project_base_triangles(sources,
        tetra::gpu_terrain_field_tuple_sphere(input_tuple),origin,capacity);
    std::vector<std::uint32_t> output(4U+std::max<std::size_t>(capacity,1U)*36U);
    id<MTLBuffer> field=make_buffer(&input_tuple,sizeof(input_tuple));
    id<MTLBuffer> input_buffer=make_buffer(input.data(),input.size()*sizeof(input.front()));
    id<MTLBuffer> output_buffer=make_buffer(output.data(),output.size()*sizeof(output.front()));
    if(field==nil||input_buffer==nil||output_buffer==nil)return false;
    ProjectParameters parameters;parameters.triangle_count=static_cast<std::uint32_t>(sources.size());parameters.capacity=capacity;
    parameters.render_origin={static_cast<float>(origin.x),static_cast<float>(origin.y),static_cast<float>(origin.z),0.0F};
    parameters.source_revision_low=101U;parameters.source_revision_high=0U;
    id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];[encoder setBuffer:field offset:0U atIndex:0U];
    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:1U];[encoder setBuffer:output_buffer offset:0U atIndex:2U];
    [encoder setBuffer:input_buffer offset:0U atIndex:3U];[encoder dispatchThreads:MTLSizeMake(1U,1U,1U) threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
    [encoder endEncoding];[command commit];[command waitUntilCompleted];
    if(command.status!=MTLCommandBufferStatusCompleted)return false;
    const auto* words=static_cast<const std::uint32_t*>(output_buffer.contents);
    if(expect_failure)return words[0U]==0U&&words[1U]!=0U;
    if(words[0U]!=expected.size()||words[1U]!=0U){std::fprintf(stderr,"Metal terrain project header %u %u expected %zu\n",words[0U],words[1U],expected.size());return false;}
    for(std::size_t index=0U;index<expected.size();++index){
      const auto offset=4U+index*36U;const auto& want=expected[index];
      const auto packed_edges=static_cast<std::uint32_t>(want.source.edges[0])|
          (static_cast<std::uint32_t>(want.source.edges[1])<<8U)|
          (static_cast<std::uint32_t>(want.source.edges[2])<<16U);
      if(words[offset]!=want.source.owner_index||words[offset+1U]!=want.source.template_cell||words[offset+2U]!=want.source.corner_negative_mask||words[offset+3U]!=packed_edges){std::fprintf(stderr,"Metal terrain project identity mismatch %zu\n",index);return false;}
      for(std::size_t vertex=0U;vertex<6U;++vertex)for(std::size_t axis=0U;axis<3U;++axis){
        const float actual=std::bit_cast<float>(words[offset+4U+vertex*3U+axis]);const auto point=want.vertices[vertex];
        const double target=axis==0U?point.x:axis==1U?point.y:point.z;if(!std::isfinite(actual)||std::abs(static_cast<double>(actual)-target)>2.e-3){std::fprintf(stderr,"Metal terrain project vertex mismatch %zu/%zu/%zu %.9g %.9g\n",index,vertex,axis,static_cast<double>(actual),target);return false;}
      }
      for(std::size_t normal=0U;normal<4U;++normal)for(std::size_t axis=0U;axis<3U;++axis){
        const float actual=std::bit_cast<float>(words[offset+22U+normal*3U+axis]);const auto value=want.normals[normal];
        const double target=axis==0U?value.x:axis==1U?value.y:value.z;if(!std::isfinite(actual)||std::abs(static_cast<double>(actual)-target)>2.e-2){std::fprintf(stderr,"Metal terrain project normal mismatch %zu/%zu/%zu %.9g %.9g\n",index,normal,axis,static_cast<double>(actual),target);return false;}
      }
    }
    return true;
  };
  const auto capacity=static_cast<std::uint32_t>(source_triangles.size());
  if(!dispatch(tuple,source_triangles,{0.0,0.0,0.0},capacity,false)){std::fprintf(stderr,"Metal terrain project fixed parity failed\n");return false;}
  if(!dispatch(tuple,source_triangles,{.25,-.5,.75},capacity,false)){std::fprintf(stderr,"Metal terrain project moved-origin parity failed\n");return false;}
  auto moved=tuple;moved.centre_radius[0]+=.03125F;moved.revision_lanes[2]+=1U;
  const auto moved_roots=tetra::gpu_terrain_root_packet(packet,moved,100000U);
  if(!dispatch(moved,tetra::gpu_terrain_base_triangles(moved_roots,100000U),{},capacity,false)){std::fprintf(stderr,"Metal terrain project moved-field parity failed\n");return false;}
  // Classification receives normalized hierarchy points but roots are world
  // positions.  This translated-kernel case proves that projection consumes
  // the latter by using a non-default domain and a field centred in it.
  auto non_default_parameters=field_parameters;
  non_default_parameters.domain.world_origin={-1.0,-.25,-.5};
  non_default_parameters.domain.world_extent=2.0;
  non_default_parameters.field.centre={0.0,.75,.5};
  non_default_parameters.field.radius=.37;
  non_default_parameters.field.terrain.planet_radius=.37;
  const auto non_default=tetra::make_gpu_terrain_field_tuple(non_default_parameters);
  const auto non_default_roots=tetra::gpu_terrain_root_packet(packet,non_default,100000U);
  const auto non_default_triangles=tetra::gpu_terrain_base_triangles(non_default_roots,100000U);
  if(non_default_triangles.empty()||!dispatch(non_default,non_default_triangles,{0.0,.75,.5},
      static_cast<std::uint32_t>(non_default_triangles.size()),false)){
    std::fprintf(stderr,"Metal terrain project non-default-domain parity failed\n");return false;
  }
  if(!dispatch(tuple,source_triangles,{},0U,true)){std::fprintf(stderr,"Metal terrain project capacity rejection failed\n");return false;}
  auto stale=tuple;stale.revision_lanes[0]^=1U;if(!dispatch(stale,source_triangles,{},capacity,true)){std::fprintf(stderr,"Metal terrain project stale rejection failed\n");return false;}
  auto malformed=source_triangles;malformed.front().roots[0].x=std::numeric_limits<double>::quiet_NaN();
  if(!dispatch(tuple,std::move(malformed),{},capacity,true)){std::fprintf(stderr,"Metal terrain project malformed rejection failed\n");return false;}
  auto degenerate=source_triangles;degenerate.front().roots[1]=degenerate.front().roots[0];degenerate.front().roots[2]=degenerate.front().roots[0];
  if(!dispatch(tuple,std::move(degenerate),{},capacity,true)){std::fprintf(stderr,"Metal terrain project degeneracy rejection failed\n");return false;}
  if(!dispatch(tuple,{}, {},0U,false)){std::fprintf(stderr,"Metal terrain project empty-stream parity failed\n");return false;}
  std::printf("{\"event\":\"metal_gpu_terrain_project\",\"triangles\":%u,\"passed\":true}\n",capacity);
  return true;
}

// P7c2a turns the P7c1b diagnostic stream into the exact vertex ABI consumed
// by the renderer.  This remains an isolated readback gate: it intentionally
// creates neither a drawable nor a consumer binding.
bool run_metal_gpu_terrain_draw_smoke_test(id<MTLDevice> device) {
  const auto shader_path=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR)/
      "gpu_terrain_draw.comp.metal";
  id<MTLLibrary> library=make_file_shader_library(device,shader_path.string().c_str());
  NSError* error=nil;id<MTLFunction> function=[library newFunctionWithName:@"main0"];
  id<MTLComputePipelineState> pipeline=function==nil?nil:
      [device newComputePipelineStateWithFunction:function error:&error];
  if(pipeline==nil){std::fprintf(stderr,"Metal terrain draw pipeline creation failed: %s\n",
      error==nil?"missing translated entry point":error.localizedDescription.UTF8String);return false;}
  tetra::GpuTerrainFieldTupleParameters field_parameters;
  field_parameters.source_revision=101U;field_parameters.field_revision=19U;
  field_parameters.domain.world_extent=1.0;field_parameters.field.kind=tetra::ImplicitShapeKind::perlin_terrain;
  field_parameters.field.centre={.5,.52,.5};field_parameters.field.radius=.37;
  field_parameters.field.sampling_footprint=.0625;field_parameters.field.terrain.planet_radius=.37;
  const auto tuple=tetra::make_gpu_terrain_field_tuple(field_parameters);
  std::vector<tetra::WorldTetAddress> candidates;
  for(std::uint8_t root=0U;root<tetra::bcc_root_tetrahedron_count;++root)
    candidates.push_back(tetra::WorldTetAddress::root(root));
  const auto packet=tetra::make_gpu_green_mask_packet(candidates,101U);
  const auto roots=tetra::gpu_terrain_root_packet(packet,tuple,100000U);
  const auto base_triangles=tetra::gpu_terrain_base_triangles(roots,100000U);
  if(base_triangles.empty()){std::fprintf(stderr,"Metal terrain draw oracle is empty\n");return false;}
  struct alignas(16) DrawParameters {
    std::uint32_t triangle_count{},vertex_capacity{},reserved0{},reserved1{};
    std::array<float,4> render_origin{};
    std::uint32_t source_revision_low{},source_revision_high{},padding0{},padding1{};
  };
  static_assert(sizeof(DrawParameters)==48U);
  const auto make_buffer=[&](const void* bytes,NSUInteger length){return [device newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared];};
  id<MTLCommandQueue> queue=[device newCommandQueue];if(queue==nil)return false;
  const auto dispatch=[&](const tetra::GpuTerrainFieldTuple& input_tuple,
                          const std::vector<tetra::GpuTerrainProjectedTriangleRecord>& projected,
                          tetra::Vec3 origin,std::uint32_t capacity,bool expect_failure,
                          bool corrupt_header=false,bool corrupt_geometry=false){
    std::vector<std::uint32_t> input(4U+projected.size()*36U);
    input[0U]=static_cast<std::uint32_t>(projected.size());
    if(corrupt_header)input[0U]^=1U;
    for(std::size_t index=0U;index<projected.size();++index){
      const auto offset=4U+index*36U;const auto& record=projected[index];
      input[offset]=record.source.owner_index;input[offset+1U]=record.source.template_cell;
      input[offset+2U]=record.source.corner_negative_mask;
      input[offset+3U]=static_cast<std::uint32_t>(record.source.edges[0])|
          (static_cast<std::uint32_t>(record.source.edges[1])<<8U)|
          (static_cast<std::uint32_t>(record.source.edges[2])<<16U);
      for(std::size_t vertex=0U;vertex<6U;++vertex)for(std::size_t axis=0U;axis<3U;++axis){
        const auto point=record.vertices[vertex];const float value=static_cast<float>(axis==0U?point.x:axis==1U?point.y:point.z);
        input[offset+4U+vertex*3U+axis]=std::bit_cast<std::uint32_t>(value);
      }
      for(std::size_t normal=0U;normal<4U;++normal)for(std::size_t axis=0U;axis<3U;++axis){
        const auto value=record.normals[normal];const float component=static_cast<float>(axis==0U?value.x:axis==1U?value.y:value.z);
        input[offset+22U+normal*3U+axis]=std::bit_cast<std::uint32_t>(component);
      }
    }
    if(corrupt_geometry&&!projected.empty())input[8U]=std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
    const auto required=projected.size()*12U;
    std::vector<std::uint32_t> output(4U+std::max<std::size_t>(capacity,1U)*18U);
    id<MTLBuffer> field=make_buffer(&input_tuple,sizeof(input_tuple));
    id<MTLBuffer> input_buffer=make_buffer(input.data(),input.size()*sizeof(input.front()));
    id<MTLBuffer> output_buffer=make_buffer(output.data(),output.size()*sizeof(output.front()));
    if(field==nil||input_buffer==nil||output_buffer==nil)return false;
    DrawParameters parameters;parameters.triangle_count=static_cast<std::uint32_t>(projected.size());parameters.vertex_capacity=capacity;
    parameters.render_origin={static_cast<float>(origin.x),static_cast<float>(origin.y),static_cast<float>(origin.z),0.0F};
    parameters.source_revision_low=101U;parameters.source_revision_high=0U;
    id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];[encoder setBuffer:field offset:0U atIndex:0U];
    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:1U];[encoder setBuffer:output_buffer offset:0U atIndex:2U];
    [encoder setBuffer:input_buffer offset:0U atIndex:3U];[encoder dispatchThreads:MTLSizeMake(1U,1U,1U) threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
    [encoder endEncoding];[command commit];[command waitUntilCompleted];
    if(command.status!=MTLCommandBufferStatusCompleted)return false;
    const auto* words=static_cast<const std::uint32_t*>(output_buffer.contents);
    if(expect_failure)return words[0U]==0U&&words[1U]!=0U;
    if(words[0U]!=required||words[1U]!=0U||words[2U]!=projected.size()){std::fprintf(stderr,"Metal terrain draw header %u %u %u expected %zu\n",words[0U],words[1U],words[2U],required);return false;}
    const auto field_sphere=tetra::gpu_terrain_field_tuple_sphere(input_tuple);
    constexpr std::array<std::array<std::uint32_t,3>,4> faces{{{{0U,1U,2U}},{{1U,3U,4U}},{{2U,4U,5U}},{{1U,4U,2U}}}};
    for(std::size_t triangle=0U;triangle<projected.size();++triangle)for(std::size_t face=0U;face<faces.size();++face)for(std::size_t corner=0U;corner<3U;++corner){
      const auto vertex=triangle*12U+face*3U+corner;const auto offset=4U+vertex*18U;
      const auto& point=projected[triangle].vertices[faces[face][corner]];
      const auto& flat=projected[triangle].normals[face];
      const auto smooth=field_sphere.normal(point+origin);
      const std::array<double,3> expected_position{{point.x,point.y,point.z}};
      const std::array<double,3> expected_flat{{flat.x,flat.y,flat.z}};
      const std::array<double,3> expected_smooth{{smooth.x,smooth.y,smooth.z}};
      for(std::size_t axis=0U;axis<3U;++axis){
        const float position=std::bit_cast<float>(words[offset+axis]);
        const float colour=std::bit_cast<float>(words[offset+3U+axis]);
        const float normal=std::bit_cast<float>(words[offset+6U+axis]);
        const float barycentric=std::bit_cast<float>(words[offset+12U+axis]);
        const float smooth_normal=std::bit_cast<float>(words[offset+15U+axis]);
        const float wanted_colour=axis==0U?.43F:axis==1U?.45F:.47F;
        const float wanted_barycentric=axis==corner?1.0F:0.0F;
        if(!std::isfinite(position)||std::abs(static_cast<double>(position)-expected_position[axis])>2.e-5||
           std::abs(colour-wanted_colour)>1.e-6F||!std::isfinite(normal)||std::abs(static_cast<double>(normal)-expected_flat[axis])>2.e-3||
           std::abs(barycentric-wanted_barycentric)>1.e-6F||!std::isfinite(smooth_normal)||std::abs(static_cast<double>(smooth_normal)-expected_smooth[axis])>3.e-2){
          std::fprintf(stderr,"Metal terrain draw vertex mismatch %zu/%zu/%zu\n",triangle,face,corner);return false;
        }
      }
      if(std::abs(std::bit_cast<float>(words[offset+9U])+2.0F)>1.e-6F||words[offset+10U]!=0U||
         std::abs(std::bit_cast<float>(words[offset+11U])-7.0F)>1.e-6F){std::fprintf(stderr,"Metal terrain draw attributes mismatch\n");return false;}
    }
    return true;
  };
  const auto projected_for=[&](const tetra::GpuTerrainFieldTuple& field,tetra::Vec3 origin){
    return tetra::gpu_terrain_project_base_triangles(base_triangles,
        tetra::gpu_terrain_field_tuple_sphere(field),origin,100000U);
  };
  const auto fixed=projected_for(tuple,{});const auto capacity=static_cast<std::uint32_t>(fixed.size()*12U);
  if(!dispatch(tuple,fixed,{},capacity,false)){std::fprintf(stderr,"Metal terrain draw fixed parity failed\n");return false;}
  const tetra::Vec3 moved_origin{.25,-.5,.75};const auto rebased=projected_for(tuple,moved_origin);
  if(!dispatch(tuple,rebased,moved_origin,capacity,false)){std::fprintf(stderr,"Metal terrain draw moved-origin parity failed\n");return false;}
  auto moved=tuple;moved.centre_radius[0]+=.03125F;moved.revision_lanes[2]+=1U;
  const auto moved_roots=tetra::gpu_terrain_root_packet(packet,moved,100000U);
  const auto moved_base=tetra::gpu_terrain_base_triangles(moved_roots,100000U);
  const auto moved_projected=tetra::gpu_terrain_project_base_triangles(moved_base,tetra::gpu_terrain_field_tuple_sphere(moved),{},100000U);
  if(!dispatch(moved,moved_projected,{},static_cast<std::uint32_t>(moved_projected.size()*12U),false)){std::fprintf(stderr,"Metal terrain draw moved-field parity failed\n");return false;}
  if(!dispatch(tuple,fixed,{},capacity-1U,true)){std::fprintf(stderr,"Metal terrain draw capacity rejection failed\n");return false;}
  auto stale=tuple;stale.revision_lanes[0]^=1U;if(!dispatch(stale,fixed,{},capacity,true)){std::fprintf(stderr,"Metal terrain draw stale rejection failed\n");return false;}
  if(!dispatch(tuple,fixed,{},capacity,true,true)){std::fprintf(stderr,"Metal terrain draw header rejection failed\n");return false;}
  if(!dispatch(tuple,fixed,{},capacity,true,false,true)){std::fprintf(stderr,"Metal terrain draw geometry rejection failed\n");return false;}
  if(!dispatch(tuple,{}, {},0U,false)){std::fprintf(stderr,"Metal terrain draw empty-stream parity failed\n");return false;}
  std::printf("{\"event\":\"metal_gpu_terrain_draw\",\"vertices\":%u,\"passed\":true}\n",capacity);
  return true;
}

// P7c2b1a is deliberately a hardware-only chain.  The final buffer is shared
// solely for this bounded oracle; every intermediate is private and every
// downstream stage discovers its count from the preceding GPU header.
// Each invocation owns a distinct private candidate slot.  The live-slot
// smoke submits all three identities below; slot 1 changes the field and slot
// 2 changes the render origin, exercising completion identity rather than
// accidentally treating a previous candidate as current.
bool run_metal_gpu_terrain_native_chain_smoke_test(id<MTLDevice> device,
                                                   std::uint32_t slot=0U) {
  const auto directory=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR);
  const auto pipeline_for=[&](const char* filename)->id<MTLComputePipelineState>{
    id<MTLLibrary> library=make_file_shader_library(device,(directory/filename).string().c_str());
    NSError* error=nil;id<MTLFunction> function=library==nil?nil:[library newFunctionWithName:@"main0"];
    auto pipeline=function==nil?nil:[device newComputePipelineStateWithFunction:function error:&error];
    if(pipeline==nil)std::fprintf(stderr,"Metal native terrain chain pipeline %s failed: %s\n",filename,
        error==nil?"missing translated entry point":error.localizedDescription.UTF8String);
    return pipeline;
  };
  id<MTLComputePipelineState> classify=pipeline_for("gpu_terrain_classify.comp.metal");
  // This is the live-slot precursor, so it must use P7c2b1b0's ordered
  // parallel compaction rather than P7b2's one-lane diagnostic kernel.
  id<MTLComputePipelineState> count=pipeline_for("gpu_terrain_triangle_counts.comp.metal");
  id<MTLComputePipelineState> scan=pipeline_for("gpu_terrain_exclusive_scan.comp.metal");
  id<MTLComputePipelineState> finalize=pipeline_for("gpu_terrain_triangle_finalize.comp.metal");
  id<MTLComputePipelineState> scatter=pipeline_for("gpu_terrain_triangle_scatter.comp.metal");
  id<MTLComputePipelineState> project=pipeline_for("gpu_terrain_project.comp.metal");
  id<MTLComputePipelineState> draw=pipeline_for("gpu_terrain_draw.comp.metal");
  if(classify==nil||count==nil||scan==nil||finalize==nil||scatter==nil||project==nil||draw==nil)return false;
  tetra::GpuTerrainFieldTupleParameters field_parameters;
  const auto source_revision=101U+slot;
  field_parameters.source_revision=source_revision;field_parameters.field_revision=19U+slot;
  field_parameters.domain.world_extent=1.0;field_parameters.field.kind=tetra::ImplicitShapeKind::perlin_terrain;
  field_parameters.field.centre={.5+.015*static_cast<double>(slot),.52,.5};field_parameters.field.radius=.37;
  field_parameters.field.sampling_footprint=.0625;field_parameters.field.terrain.planet_radius=.37;
  const auto tuple=tetra::make_gpu_terrain_field_tuple(field_parameters);
  std::vector<tetra::WorldTetAddress> candidates;
  for(std::uint8_t root=0U;root<tetra::bcc_root_tetrahedron_count;++root)candidates.push_back(tetra::WorldTetAddress::root(root));
  const auto packet=tetra::make_gpu_green_mask_packet(candidates,source_revision);
  const auto templates=tetra::make_gpu_green_template_table();
  const auto expected_roots=tetra::gpu_terrain_root_packet(packet,tuple,100000U);
  auto expected_base=tetra::gpu_terrain_base_triangles(expected_roots,100000U);
  for(auto& triangle:expected_base)for(auto& root:triangle.roots){
    root.x=static_cast<float>(root.x);root.y=static_cast<float>(root.y);root.z=static_cast<float>(root.z);
  }
  const tetra::Vec3 render_origin=slot==2U?tetra::Vec3{.125,-.25,.0625}:tetra::Vec3{};
  const auto expected=tetra::gpu_terrain_project_base_triangles(expected_base,
      tetra::gpu_terrain_field_tuple_sphere(tuple),render_origin,100000U);
  if(expected.empty())return false;
  const auto make_shared=[&](const void* bytes,NSUInteger length){return [device newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared];};
  const auto make_private=[&](NSUInteger length){return [device newBufferWithLength:length options:MTLResourceStorageModePrivate];};
  const std::uint32_t owner_count=static_cast<std::uint32_t>(packet.owners.size());
  const std::uint32_t root_slots=owner_count*24U;
  const std::uint32_t compaction_blocks=(root_slots+255U)/256U;
  const std::uint32_t triangle_capacity=root_slots*2U;
  const std::uint32_t vertex_capacity=triangle_capacity*12U;
  id<MTLBuffer> field=make_shared(&tuple,sizeof(tuple));
  id<MTLBuffer> owners=make_shared(packet.owners.data(),packet.owners.size()*sizeof(packet.owners.front()));
  id<MTLBuffer> stencil=make_shared(templates.data(),sizeof(templates));
  id<MTLBuffer> roots=make_private((4U+static_cast<NSUInteger>(root_slots)*24U)*sizeof(std::uint32_t));
  id<MTLBuffer> compact=make_private((4U+static_cast<NSUInteger>(triangle_capacity)*16U)*sizeof(std::uint32_t));
  id<MTLBuffer> counts=make_private(static_cast<NSUInteger>(root_slots)*sizeof(std::uint32_t));
  id<MTLBuffer> offsets=make_private(static_cast<NSUInteger>(root_slots)*sizeof(std::uint32_t));
  id<MTLBuffer> added_offsets=make_private(static_cast<NSUInteger>(root_slots)*sizeof(std::uint32_t));
  id<MTLBuffer> block_totals=make_private(static_cast<NSUInteger>(compaction_blocks)*sizeof(std::uint32_t));
  id<MTLBuffer> block_offsets=make_private(static_cast<NSUInteger>(compaction_blocks)*sizeof(std::uint32_t));
  id<MTLBuffer> compaction_status=make_private(4U*sizeof(std::uint32_t));
  id<MTLBuffer> projected=make_private((4U+static_cast<NSUInteger>(triangle_capacity)*36U)*sizeof(std::uint32_t));
  id<MTLBuffer> output=[device newBufferWithLength:(4U+static_cast<NSUInteger>(vertex_capacity)*18U)*sizeof(std::uint32_t) options:MTLResourceStorageModeShared];
  id<MTLCommandQueue> queue=[device newCommandQueue];
  if(field==nil||owners==nil||stencil==nil||roots==nil||compact==nil||counts==nil||offsets==nil||added_offsets==nil||block_totals==nil||block_offsets==nil||compaction_status==nil||projected==nil||output==nil||queue==nil)return false;
  std::memset(output.contents,0,output.length);
  id<MTLCommandBuffer> command=[queue commandBuffer];
  id<MTLBlitCommandEncoder> clear=[command blitCommandEncoder];
  for(id<MTLBuffer> buffer: {roots,compact,counts,offsets,added_offsets,block_totals,block_offsets,compaction_status,projected})[clear fillBuffer:buffer range:NSMakeRange(0U,buffer.length) value:0U];
  [clear endEncoding];
  const std::array<std::uint32_t,4> classify_parameters{owner_count,root_slots,source_revision,0U};
  id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
  [encoder setComputePipelineState:classify];[encoder setBuffer:field offset:0U atIndex:0U];
  [encoder setBytes:classify_parameters.data() length:sizeof(classify_parameters) atIndex:1U];
  [encoder setBuffer:owners offset:0U atIndex:2U];[encoder setBuffer:roots offset:0U atIndex:3U];[encoder setBuffer:stencil offset:0U atIndex:4U];
  [encoder dispatchThreads:MTLSizeMake(owner_count,1U,1U) threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];[encoder endEncoding];
  const std::array<std::uint32_t,2> count_parameters{owner_count,root_slots};
  encoder=[command computeCommandEncoder];[encoder setComputePipelineState:count];[encoder setBuffer:roots offset:0U atIndex:0U];
  [encoder setBytes:count_parameters.data() length:sizeof(count_parameters) atIndex:1U];[encoder setBuffer:owners offset:0U atIndex:2U];
  [encoder setBuffer:stencil offset:0U atIndex:3U];[encoder setBuffer:compaction_status offset:0U atIndex:4U];[encoder setBuffer:counts offset:0U atIndex:5U];
  [encoder dispatchThreads:MTLSizeMake(root_slots,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[encoder endEncoding];
  const std::array<std::uint32_t,2> scan_parameters{root_slots,0U};
  encoder=[command computeCommandEncoder];[encoder setComputePipelineState:scan];[encoder setBytes:scan_parameters.data() length:sizeof(scan_parameters) atIndex:0U];
  [encoder setBuffer:offsets offset:0U atIndex:1U];[encoder setBuffer:counts offset:0U atIndex:2U];[encoder setBuffer:block_totals offset:0U atIndex:3U];
  [encoder dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(compaction_blocks)*256U,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[encoder endEncoding];
  const std::array<std::uint32_t,2> block_scan_parameters{compaction_blocks,0U};
  encoder=[command computeCommandEncoder];[encoder setComputePipelineState:scan];[encoder setBytes:block_scan_parameters.data() length:sizeof(block_scan_parameters) atIndex:0U];
  [encoder setBuffer:block_offsets offset:0U atIndex:1U];[encoder setBuffer:block_totals offset:0U atIndex:2U];[encoder setBuffer:compaction_status offset:0U atIndex:3U];
  [encoder dispatchThreads:MTLSizeMake(256U,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[encoder endEncoding];
  const std::array<std::uint32_t,2> add_parameters{root_slots,1U};
  encoder=[command computeCommandEncoder];[encoder setComputePipelineState:scan];[encoder setBytes:add_parameters.data() length:sizeof(add_parameters) atIndex:0U];
  [encoder setBuffer:added_offsets offset:0U atIndex:1U];[encoder setBuffer:offsets offset:0U atIndex:2U];[encoder setBuffer:block_offsets offset:0U atIndex:3U];
  [encoder dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(compaction_blocks)*256U,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[encoder endEncoding];
  const std::array<std::uint32_t,2> compact_parameters{root_slots,triangle_capacity};
  encoder=[command computeCommandEncoder];[encoder setComputePipelineState:finalize];[encoder setBytes:compact_parameters.data() length:sizeof(compact_parameters) atIndex:0U];
  [encoder setBuffer:compact offset:0U atIndex:1U];[encoder setBuffer:compaction_status offset:0U atIndex:2U];[encoder setBuffer:added_offsets offset:0U atIndex:3U];[encoder setBuffer:counts offset:0U atIndex:4U];
  [encoder dispatchThreads:MTLSizeMake(1U,1U,1U) threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];[encoder endEncoding];
  encoder=[command computeCommandEncoder];[encoder setComputePipelineState:scatter];[encoder setBytes:compact_parameters.data() length:sizeof(compact_parameters) atIndex:0U];
  [encoder setBuffer:compact offset:0U atIndex:1U];[encoder setBuffer:counts offset:0U atIndex:2U];[encoder setBuffer:added_offsets offset:0U atIndex:3U];[encoder setBuffer:roots offset:0U atIndex:4U];
  [encoder dispatchThreads:MTLSizeMake(root_slots,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[encoder endEncoding];
  struct alignas(16) GeometryParameters { std::uint32_t count{},capacity{},reserved0{},reserved1{};std::array<float,4> origin{};std::uint32_t source_low{},source_high{},padding0{},padding1{}; };
  static_assert(sizeof(GeometryParameters)==48U);
  GeometryParameters projection_parameters{std::numeric_limits<std::uint32_t>::max(),triangle_capacity,0U,0U,{static_cast<float>(render_origin.x),static_cast<float>(render_origin.y),static_cast<float>(render_origin.z),0},source_revision,0U,0U,0U};
  encoder=[command computeCommandEncoder];[encoder setComputePipelineState:project];[encoder setBuffer:field offset:0U atIndex:0U];
  [encoder setBytes:&projection_parameters length:sizeof(projection_parameters) atIndex:1U];[encoder setBuffer:projected offset:0U atIndex:2U];[encoder setBuffer:compact offset:0U atIndex:3U];
  [encoder dispatchThreads:MTLSizeMake(1U,1U,1U) threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];[encoder endEncoding];
  GeometryParameters draw_parameters{std::numeric_limits<std::uint32_t>::max(),vertex_capacity,0U,0U,{static_cast<float>(render_origin.x),static_cast<float>(render_origin.y),static_cast<float>(render_origin.z),0},source_revision,0U,0U,0U};
  encoder=[command computeCommandEncoder];[encoder setComputePipelineState:draw];[encoder setBuffer:field offset:0U atIndex:0U];
  [encoder setBytes:&draw_parameters length:sizeof(draw_parameters) atIndex:1U];[encoder setBuffer:output offset:0U atIndex:2U];[encoder setBuffer:projected offset:0U atIndex:3U];
  [encoder dispatchThreads:MTLSizeMake(1U,1U,1U) threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];[encoder endEncoding];
  [command commit];[command waitUntilCompleted];if(command.status!=MTLCommandBufferStatusCompleted)return false;
  const auto* words=static_cast<const std::uint32_t*>(output.contents);
  const auto expected_vertices=static_cast<std::uint32_t>(expected.size()*12U);
  if(words==nullptr||words[0U]!=expected_vertices||words[1U]!=0U||words[2U]!=expected.size()){
    std::fprintf(stderr,"Metal native terrain chain header mismatch %u %u %u expected %u\n",words==nullptr?0U:words[0U],words==nullptr?0U:words[1U],words==nullptr?0U:words[2U],expected_vertices);return false;
  }
  constexpr std::array<std::array<std::uint32_t,3>,4> faces{{{{0U,1U,2U}},{{1U,3U,4U}},{{2U,4U,5U}},{{1U,4U,2U}}}};
  for(std::size_t triangle=0U;triangle<expected.size();++triangle)for(std::size_t face=0U;face<4U;++face)for(std::size_t corner=0U;corner<3U;++corner){
    const auto offset=4U+(triangle*12U+face*3U+corner)*18U;const auto& point=expected[triangle].vertices[faces[face][corner]];
    for(std::size_t axis=0U;axis<3U;++axis){const float value=std::bit_cast<float>(words[offset+axis]);const double wanted=axis==0U?point.x:axis==1U?point.y:point.z;if(!std::isfinite(value)||std::abs(static_cast<double>(value)-wanted)>2.e-3){std::fprintf(stderr,"Metal native terrain chain position mismatch\n");return false;}}
  }
  std::printf("{\"event\":\"metal_gpu_terrain_native_chain\",\"slot\":%u,\"triangles\":%zu,\"vertices\":%u,\"passed\":true}\n",slot,expected.size(),expected_vertices);
  return true;
}

// P7c2b1b is still diagnostic-only: these three independent private resource
// sets model the rotating live slots, but this function never binds a render,
// shadow, or ray-tracing consumer.  Completion readback is the four-word
// candidate header plus the bounded fixture payload used for parity.
bool run_metal_gpu_terrain_live_slots_smoke_test(id<MTLDevice> device) {
  for(std::uint32_t slot=0U;slot<3U;++slot)
    if(!run_metal_gpu_terrain_native_chain_smoke_test(device,slot))return false;
  std::printf("{\"event\":\"metal_gpu_terrain_live_private_slots\",\"slots\":3,\"static\":true,\"moving\":true,\"rebase\":true,\"cpu_front_untouched\":true,\"passed\":true}\n");
  return true;
}

// P5b mirrors the Vulkan extraction qualification without connecting the
// result to Metal rendering.  The records are the legacy CPU-precomputed ABI;
// this fixture establishes that its translated kernel writes the same compact
// triangle-list payload and fails closed before P5c considers slot lifetime.
bool run_metal_gpu_terrain_extract_smoke_test(id<MTLDevice> device) {
  const auto shader_path=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR)/
      "gpu_terrain_extract.comp.metal";
  id<MTLLibrary> library=make_file_shader_library(device,shader_path.string().c_str());
  id<MTLFunction> function=[library newFunctionWithName:@"main0"];
  NSError* error=nil;
  id<MTLComputePipelineState> pipeline=function==nil?nil:
      [device newComputePipelineStateWithFunction:function error:&error];
  if(pipeline==nil){
    std::fprintf(stderr,"Metal terrain-extract pipeline creation failed: %s\n",
        error==nil?"missing translated entry point":error.localizedDescription.UTF8String);
    return false;
  }
  struct ExtractVertex { std::array<float,18> values{}; };
  static_assert(sizeof(ExtractVertex)==sizeof(float)*18U);
  const auto make_buffer=[&](const void* bytes,NSUInteger length){
    return [device newBufferWithBytes:bytes length:length
        options:MTLResourceStorageModeShared];
  };
  const auto point=[](float x,float y,float z){
    return std::array<float,4>{x,y,z,1.0F};
  };
  const auto normal=[](float x,float y,float z){
    return std::array<float,4>{x,y,z,1.0F};
  };
  tetra::GpuTerrainCellRecord triangle{};
  triangle.corners={{{0,0,0,-1},{1,0,0,1},{0,1,0,1},{0,0,1,1}}};
  triangle.edge_roots[0]=point(0,0,0);triangle.edge_roots[1]=point(1,0,0);
  triangle.edge_roots[2]=point(0,1,0);
  triangle.draw_roots[0]=point(0,0,0);triangle.draw_roots[1]=point(1,0,0);
  triangle.draw_roots[2]=point(0,1,0);triangle.draw_roots[3][3]=1.0F;
  triangle.subdivision_midpoints[0]=point(.5F,0,0);
  triangle.subdivision_midpoints[1]=point(.5F,.5F,0);
  triangle.subdivision_midpoints[2]=point(0,.5F,0);
  for(auto& value:triangle.subdivision_normals)value=normal(0,0,1);

  tetra::GpuTerrainCellRecord quad{};
  quad.corners={{{0,0,1,-1},{1,0,1,-1},{0,1,1,1},{1,1,1,1}}};
  quad.edge_roots[1]=point(0,0,1);quad.edge_roots[2]=point(1,0,1);
  quad.edge_roots[3]=point(0,1,1);quad.edge_roots[4]=point(1,1,1);
  quad.draw_roots[0]=point(0,0,1);quad.draw_roots[1]=point(1,0,1);
  quad.draw_roots[2]=point(0,1,1);quad.draw_roots[3]=point(1,1,1);
  quad.draw_roots[3][3]=3.0F;
  quad.subdivision_midpoints[0]=point(.5F,0,1);
  quad.subdivision_midpoints[1]=point(.5F,.5F,1);
  quad.subdivision_midpoints[2]=point(0,.5F,1);
  quad.subdivision_midpoints[3]=point(0,.5F,1);
  quad.subdivision_midpoints[4]=point(.5F,1,1);
  quad.subdivision_midpoints[5]=point(.5F,.5F,1);
  for(auto& value:quad.subdivision_normals)value=normal(0,0,1);
  const std::array records{triangle,quad};

  std::vector<ExtractVertex> expected;
  const auto append_vertex=[&](const std::array<float,4>& p,
                               const std::array<float,4>& n,
                               std::array<float,3> barycentric){
    ExtractVertex vertex{};
    vertex.values={p[0],p[1],p[2],.46F,.40F,.31F,n[0],n[1],n[2],0,0,1,
                   barycentric[0],barycentric[1],barycentric[2],n[0],n[1],n[2]};
    expected.push_back(vertex);
  };
  const auto append_triangle=[&](const std::array<float,4>& a,
                                 const std::array<float,4>& b,
                                 const std::array<float,4>& c,
                                 const std::array<float,4>& n){
    append_vertex(a,n,{1,0,0});append_vertex(b,n,{0,1,0});append_vertex(c,n,{0,0,1});
  };
  const auto append_subdivided=[&](const tetra::GpuTerrainCellRecord& record,
                                   const std::array<float,4>& a,
                                   const std::array<float,4>& b,
                                   const std::array<float,4>& c,
                                   std::size_t midpoint,std::size_t normals){
    const auto& ab=record.subdivision_midpoints[midpoint];
    const auto& bc=record.subdivision_midpoints[midpoint+1U];const auto& ca=record.subdivision_midpoints[midpoint+2U];
    append_triangle(a,ab,ca,record.subdivision_normals[normals]);
    append_triangle(ab,b,bc,record.subdivision_normals[normals+1U]);
    append_triangle(ca,bc,c,record.subdivision_normals[normals+2U]);
    append_triangle(ab,bc,ca,record.subdivision_normals[normals+3U]);
  };
  append_subdivided(triangle,triangle.draw_roots[0],triangle.draw_roots[1],
                    triangle.draw_roots[2],0U,0U);
  append_subdivided(quad,quad.draw_roots[0],quad.draw_roots[1],
                    quad.draw_roots[2],0U,0U);
  append_subdivided(quad,quad.draw_roots[0],quad.draw_roots[2],
                    quad.draw_roots[3],3U,4U);
  if(expected.size()!=36U)return false;

  id<MTLCommandQueue> queue=[device newCommandQueue];
  if(queue==nil)return false;
  const auto canonical=[](std::span<const ExtractVertex> vertices){
    std::vector<std::array<std::uint32_t,18>> result;
    result.reserve(vertices.size());
    for(const auto& vertex:vertices){std::array<std::uint32_t,18> bits{};
      for(std::size_t lane=0;lane<bits.size();++lane)
        bits[lane]=std::bit_cast<std::uint32_t>(vertex.values[lane]);
      result.push_back(bits);
    }
    std::ranges::sort(result);return result;
  };
  const auto dispatch=[&](std::span<const tetra::GpuTerrainCellRecord> input,
                          std::uint32_t vertex_capacity,bool expect_overflow,
                          bool expect_payload,std::uint32_t expected_linear){
    if(input.empty())return !expect_overflow&&!expect_payload&&expected_linear==0U;
    const auto index_capacity=vertex_capacity;
    std::array<std::uint32_t,8> header{};
    std::vector<ExtractVertex> vertices(std::max<std::uint32_t>(vertex_capacity,1U));
    std::vector<std::uint32_t> indices(std::max<std::uint32_t>(index_capacity,1U));
    std::vector<std::byte> output_bytes(sizeof(header)+
        vertices.size()*sizeof(vertices.front()));
    id<MTLBuffer> cells=make_buffer(input.data(),std::max<std::size_t>(
        input.size()*sizeof(input.front()),sizeof(tetra::GpuTerrainCellRecord)));
    id<MTLBuffer> output=make_buffer(output_bytes.data(),output_bytes.size());
    id<MTLBuffer> index=make_buffer(indices.data(),indices.size()*sizeof(indices.front()));
    const std::array<std::uint32_t,4> parameters{
        static_cast<std::uint32_t>(input.size()),vertex_capacity,index_capacity,1U};
    if(cells==nil||output==nil||index==nil)return false;
    id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:output offset:0U atIndex:0U];
    [encoder setBuffer:index offset:0U atIndex:1U];
    [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:2U];
    [encoder setBuffer:cells offset:0U atIndex:3U];
    [encoder dispatchThreads:MTLSizeMake(input.size(),1U,1U)
         threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
    [encoder endEncoding];[command commit];[command waitUntilCompleted];
    if(command.status!=MTLCommandBufferStatusCompleted){
      std::fprintf(stderr,"Metal terrain fixture command failed: %s\n",
          command.error.localizedDescription.UTF8String);return false;
    }
    const auto* result=static_cast<const std::uint32_t*>(output.contents);
    if((result[5]!=0U)!=expect_overflow||result[0]>vertex_capacity||
       result[0]>index_capacity){
      std::fprintf(stderr,"Metal terrain fixture header %u %u %u %u caps %u overflow %u\n",
          result[0],result[1],result[5],result[6],vertex_capacity,expect_overflow);return false;
    }
    if(!expect_payload)return result[0]==0U&&result[1]==0U&&result[6]==0U;
    if(expect_overflow)return result[6]==expected_linear;
    if(result[0]!=36U||result[1]!=1U||result[6]!=36U){
      std::fprintf(stderr,"Metal terrain fixture valid header %u %u %u\n",
          result[0],result[1],result[6]);return false;
    }
    const auto* output_vertices=reinterpret_cast<const ExtractVertex*>(result+8U);
    if(canonical(std::span{output_vertices,static_cast<std::size_t>(result[0])})!=
       canonical(expected)){std::fprintf(stderr,"Metal terrain fixture vertex mismatch\n");return false;}
    const auto* output_indices=static_cast<const std::uint32_t*>(index.contents);
    for(std::uint32_t value=0U;value<result[0];++value)
      if(output_indices[value]!=value){std::fprintf(stderr,"Metal terrain fixture index mismatch\n");return false;}
    return true;
  };
  tetra::GpuTerrainCellRecord malformed=triangle;
  malformed.edge_roots[0][3]=0.0F;
  if(!dispatch(records,36U,false,true,36U)){std::fprintf(stderr,"Metal terrain fixture valid case failed\n");return false;}
  if(!dispatch(records,35U,true,true,36U)){std::fprintf(stderr,"Metal terrain fixture capacity case failed\n");return false;}
  if(!dispatch(std::span{&malformed,1U},12U,true,true,0U)){std::fprintf(stderr,"Metal terrain fixture malformed case failed\n");return false;}
  if(!dispatch({},0U,false,false,0U)){std::fprintf(stderr,"Metal terrain fixture empty case failed\n");return false;}
  std::printf("{\"event\":\"metal_gpu_terrain_extract\","
              "\"cases\":4,\"vertices\":36,\"passed\":true}\n");
  return true;
}

// P5c1 consumes the same asynchronous CPU publication used by the application
// but remains a headless readback qualification.  In particular, it does not
// create a GLFW window, alter MetalTerrainDisplayFront, or make GPU output
// eligible for a draw.
bool run_metal_gpu_terrain_runtime_smoke_test(id<MTLDevice> device) {
  auto runtime=tetra_viewer::make_production_terrain_runtime();
  runtime->set_gpu_terrain_extraction_diagnostic(true);
  tetra::Camera camera;
  camera.position={0.5,0.72,0.68};camera.forward={0.0,-0.2,-1.0};
  runtime->set_camera(camera,false);
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(120);
  while(std::chrono::steady_clock::now()<deadline){
    static_cast<void>(runtime->update());
    const auto diagnostics=runtime->diagnostics();
    if(diagnostics.converged&&!diagnostics.busy&&
       !runtime->world_surface_gpu_cells().empty()&&
       !runtime->scene().triangle_vertices.empty())break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto diagnostics=runtime->diagnostics();
  const auto cells=runtime->world_surface_gpu_cells();
  const auto& scene=runtime->scene();
  const auto* directory=runtime->world_cut_directory();
  const auto origin=runtime->render_origin();
  // Every identity is captured after the same completed publication.  This is
  // deliberately checked before allocating or encoding GPU work so a stale or
  // origin-shifted packet has no path to become a future drawable result.
  if(!diagnostics.converged||diagnostics.busy||cells.empty()||
     scene.triangle_vertices.empty()||directory==nullptr||
     diagnostics.scene_generation==0U||origin.x!=scene.render_origin.x||
     origin.y!=scene.render_origin.y||origin.z!=scene.render_origin.z){
    std::fprintf(stderr,"Metal runtime terrain fixture unavailable: converged=%u busy=%u cells=%zu vertices=%zu directory=%p revision=%llu origin=%g,%g,%g scene=%g,%g,%g\n",
        diagnostics.converged,diagnostics.busy,cells.size(),scene.triangle_vertices.size(),
        static_cast<const void*>(directory),static_cast<unsigned long long>(diagnostics.scene_generation),
        origin.x,origin.y,origin.z,scene.render_origin.x,scene.render_origin.y,scene.render_origin.z);
    return false;
  }
  const std::uint64_t source_revision=diagnostics.scene_generation;
  if(source_revision==0U){std::fprintf(stderr,"Metal runtime terrain fixture has zero source revision\n");return false;}

  const auto shader_path=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR)/
      "gpu_terrain_extract.comp.metal";
  id<MTLLibrary> library=make_file_shader_library(device,shader_path.string().c_str());
  id<MTLFunction> function=[library newFunctionWithName:@"main0"];
  NSError* error=nil;
  id<MTLComputePipelineState> pipeline=function==nil?nil:
      [device newComputePipelineStateWithFunction:function error:&error];
  if(pipeline==nil){std::fprintf(stderr,"Metal runtime terrain fixture pipeline unavailable\n");return false;}
  static_assert(sizeof(tetra_viewer::SceneVertex)==sizeof(float)*18U);
  const std::size_t vertex_count=scene.triangle_vertices.size();
  if(vertex_count>std::numeric_limits<std::uint32_t>::max()||
     cells.size()>std::numeric_limits<std::uint32_t>::max()){std::fprintf(stderr,"Metal runtime terrain fixture input too large\n");return false;}
  const auto make_buffer=[&](const void* bytes,NSUInteger length){
    return [device newBufferWithBytes:bytes length:length
        options:MTLResourceStorageModeShared];
  };
  std::vector<std::byte> output_bytes(sizeof(std::uint32_t)*8U+
      vertex_count*sizeof(tetra_viewer::SceneVertex));
  std::vector<std::uint32_t> index_bytes(vertex_count);
  id<MTLBuffer> cell_buffer=make_buffer(cells.data(),
      cells.size()*sizeof(tetra::GpuTerrainCellRecord));
  id<MTLBuffer> output_buffer=make_buffer(output_bytes.data(),output_bytes.size());
  id<MTLBuffer> index_buffer=make_buffer(index_bytes.data(),
      index_bytes.size()*sizeof(index_bytes.front()));
  id<MTLCommandQueue> queue=[device newCommandQueue];
  const std::array<std::uint32_t,4> parameters{
      static_cast<std::uint32_t>(cells.size()),static_cast<std::uint32_t>(vertex_count),
      static_cast<std::uint32_t>(vertex_count),1U};
  if(cell_buffer==nil||output_buffer==nil||index_buffer==nil||queue==nil){std::fprintf(stderr,"Metal runtime terrain fixture buffers unavailable\n");return false;}
  id<MTLCommandBuffer> command=[queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
  [encoder setComputePipelineState:pipeline];
  [encoder setBuffer:output_buffer offset:0U atIndex:0U];
  [encoder setBuffer:index_buffer offset:0U atIndex:1U];
  [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:2U];
  [encoder setBuffer:cell_buffer offset:0U atIndex:3U];
  [encoder dispatchThreads:MTLSizeMake(cells.size(),1U,1U)
       threadsPerThreadgroup:MTLSizeMake(64U,1U,1U)];
  [encoder endEncoding];[command commit];[command waitUntilCompleted];
  if(command.status!=MTLCommandBufferStatusCompleted){std::fprintf(stderr,"Metal runtime terrain fixture command failed\n");return false;}
  const auto* header=static_cast<const std::uint32_t*>(output_buffer.contents);
  if(header==nullptr||header[0]!=vertex_count||header[1]!=1U||header[5]!=0U||
     header[6]!=vertex_count){std::fprintf(stderr,"Metal runtime terrain fixture header %u %u %u %u expected %zu\n",header==nullptr?0U:header[0],header==nullptr?0U:header[1],header==nullptr?0U:header[5],header==nullptr?0U:header[6],vertex_count);return false;}
  const auto* output=reinterpret_cast<const tetra_viewer::SceneVertex*>(header+8U);
  const auto canonical_position_normals=[](
      std::span<const tetra_viewer::SceneVertex> vertices){
    std::vector<std::array<std::uint32_t,6>> result;result.reserve(vertices.size());
    for(const auto& vertex:vertices){
      result.push_back({std::bit_cast<std::uint32_t>(vertex.position[0]),
          std::bit_cast<std::uint32_t>(vertex.position[1]),
          std::bit_cast<std::uint32_t>(vertex.position[2]),
          std::bit_cast<std::uint32_t>(vertex.normal[0]),
          std::bit_cast<std::uint32_t>(vertex.normal[1]),
          std::bit_cast<std::uint32_t>(vertex.normal[2])});
    }
    std::ranges::sort(result);return result;
  };
  const auto canonical_triangles=[](std::span<const tetra_viewer::SceneVertex> vertices){
    using Position=std::array<std::uint32_t,3>;
    std::vector<std::array<Position,3>> result;result.reserve(vertices.size()/3U);
    for(std::size_t first=0U;first<vertices.size();first+=3U){
      std::array<Position,3> triangle{};
      for(std::size_t corner=0U;corner<3U;++corner)triangle[corner]={
          std::bit_cast<std::uint32_t>(vertices[first+corner].position[0]),
          std::bit_cast<std::uint32_t>(vertices[first+corner].position[1]),
          std::bit_cast<std::uint32_t>(vertices[first+corner].position[2])};
      std::ranges::sort(triangle);result.push_back(triangle);
    }
    std::ranges::sort(result);return result;
  };
  if(canonical_position_normals(std::span{output,vertex_count})!=
     canonical_position_normals(scene.triangle_vertices)||
     canonical_triangles(std::span{output,vertex_count})!=
     canonical_triangles(scene.triangle_vertices)){
    std::fprintf(stderr,"Metal runtime terrain fixture geometry mismatch\n");return false;}
  const auto* indices=static_cast<const std::uint32_t*>(index_buffer.contents);
  if(indices==nullptr){std::fprintf(stderr,"Metal runtime terrain fixture index map unavailable\n");return false;}
  for(std::uint32_t index=0U;index<vertex_count;++index)
    if(indices[index]!=index){std::fprintf(stderr,"Metal runtime terrain fixture index mismatch\n");return false;}
  std::printf("{\"event\":\"metal_gpu_terrain_runtime\","
              "\"source_revision\":%llu,\"cells\":%zu,"
              "\"vertices\":%zu,\"passed\":true}\n",
              static_cast<unsigned long long>(source_revision),cells.size(),vertex_count);
  return true;
}

// P7d is the publication gate for P8.  It deliberately composes the
// independent hardware oracles rather than replacing them with a synthetic
// image comparison: the compact stream oracle proves BCC incidence and
// winding, projection proves the camera-relative surface, draw proves the
// SceneVertex normal/colour contract, and the runtime capture proves the
// retained CPU front's complete raster payload.  The native route remains a
// diagnostic readback route here; this test must not make it readback-free or
// relax the CPU fallback policy.
bool run_metal_gpu_terrain_surface_parity_smoke_test(id<MTLDevice> device) {
  // The mixed-depth compact stream covers terrain detail boundaries and the
  // parallel overflow gate.  The three live identities cover a static near
  // surface, a changed implicit field (silhouette/back-lit relief), and a
  // rebased view (horizon/limb camera motion).
  if(!run_metal_gpu_terrain_parallel_triangle_smoke_test(device) ||
     !run_metal_gpu_terrain_live_slots_smoke_test(device) ||
     !run_metal_gpu_terrain_project_smoke_test(device) ||
     !run_metal_gpu_terrain_draw_smoke_test(device) ||
     !run_metal_gpu_terrain_runtime_smoke_test(device)) {
    std::fprintf(stderr,"Metal GPU terrain surface parity qualification failed\n");
    return false;
  }
  std::printf("{\"event\":\"metal_gpu_terrain_surface_parity\","
              "\"cases\":[\"near\",\"horizon_limb\",\"silhouette\","
              "\"back_lit\",\"edits\",\"cutaway\",\"implicit\"],"
              "\"stream\":true,\"incidence_winding\":true,"
              "\"normals\":true,\"depth\":true,\"colour\":true,"
              "\"stale_partial_nonfinite_degenerate_overflow\":true,"
              "\"cpu_fallback_retained\":true,\"passed\":true}\n");
  return true;
}

enum class AtmosphereTextureRole {
  radiance,
  transmittance,
  screen,
  screen_transmittance
};
bool atmosphere_half_radiance_experiment{};
bool atmosphere_private_radiance_experiment{};
bool atmosphere_legacy_planet_umbra_work{};
// P4d-b2: the qualified production format for reconstructed coloured
// transmittance.  Setting the environment switch to 0 retains the float32
// control for paired native qualification.
bool atmosphere_half_screen_transmittance_experiment{true};

id<MTLTexture> make_atmosphere_texture(id<MTLDevice> device,NSUInteger width,
                                       NSUInteger height,NSUInteger depth=1U,
                                       AtmosphereTextureRole role=
                                           AtmosphereTextureRole::radiance) {
  MTLTextureDescriptor* descriptor=[MTLTextureDescriptor new];
  // Roles are intentionally explicit even while all default to float32. A
  // future format experiment must opt in per physical meaning rather than
  // accidentally changing endpoint/history precision with a LUT trial.
  switch(role){
    case AtmosphereTextureRole::radiance:
      descriptor.pixelFormat=atmosphere_half_radiance_experiment?
          MTLPixelFormatRGBA16Float:MTLPixelFormatRGBA32Float;
      break;
    case AtmosphereTextureRole::transmittance:
    case AtmosphereTextureRole::screen:
      descriptor.pixelFormat=MTLPixelFormatRGBA32Float;
      break;
    case AtmosphereTextureRole::screen_transmittance:
      descriptor.pixelFormat=atmosphere_half_screen_transmittance_experiment?
          MTLPixelFormatRGBA16Float:MTLPixelFormatRGBA32Float;
      break;
  }
  descriptor.width=width;
  descriptor.height=height;
  descriptor.depth=depth;
  descriptor.textureType=depth>1U?MTLTextureType3D:MTLTextureType2D;
  descriptor.mipmapLevelCount=1U;
  descriptor.storageMode=role==AtmosphereTextureRole::radiance&&
      atmosphere_private_radiance_experiment?
          MTLStorageModePrivate:MTLStorageModeShared;
  descriptor.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
  return [device newTextureWithDescriptor:descriptor];
}

std::array<float,96> make_atmosphere_smoke_uniform() {
  const auto parameters=tetra_viewer::atmosphere_preset(
      tetra_viewer::default_world_atmosphere_preset);
  std::array<float,96> uniform{};
  const auto spectrum=[&](std::size_t offset,
                          const tetra_viewer::AtmosphereSpectrum& value,
                          float fourth){
    uniform[offset]=static_cast<float>(value[0]);
    uniform[offset+1U]=static_cast<float>(value[1]);
    uniform[offset+2U]=static_cast<float>(value[2]);
    uniform[offset+3U]=fourth;
  };
  spectrum(0U,parameters.rayleigh_scattering_per_metre,
           static_cast<float>(parameters.ground_radius_metres));
  spectrum(4U,parameters.mie_scattering_per_metre,static_cast<float>(
      parameters.ground_radius_metres+parameters.atmosphere_height_metres));
  spectrum(8U,parameters.mie_absorption_per_metre,
           static_cast<float>(parameters.rayleigh_scale_height_metres));
  spectrum(12U,parameters.absorption_per_metre,
           static_cast<float>(parameters.mie_scale_height_metres));
  spectrum(16U,parameters.ground_albedo,
           static_cast<float>(parameters.mie_anisotropy));
  spectrum(20U,parameters.solar_irradiance,
           static_cast<float>(parameters.absorption_peak_altitude_metres));
  uniform[24]=static_cast<float>(parameters.absorption_half_width_metres);
  uniform[25]=static_cast<float>(parameters.metres_per_world_unit);
  uniform[26]=static_cast<float>(parameters.solar_angular_radius_radians);
  uniform[27]=1.0F;
  uniform[29]=static_cast<float>(parameters.ground_radius_metres+1'000.0);
  uniform[31]=0.01F;
  uniform[32]=-1.0F;
  uniform[35]=16.0F/9.0F;
  uniform[37]=-1.0F;
  uniform[39]=1.0F;
  uniform[42]=-1.0F;
  uniform[43]=static_cast<float>(
      tetra_viewer::default_world_aerial_distance_metres);
  uniform[44]=0.35F;
  uniform[45]=0.82F;
  uniform[46]=0.45F;
  uniform[47]=0.65F;
  uniform[53]=1.0F+static_cast<float>(
      tetra_viewer::default_atmosphere_rendering_method);
  uniform[54]=2.0F;
  uniform[55]=0.0F;
  uniform[57]=1.0F;
  uniform[59]=1'000.0F;
  uniform[60]=0.6139601F;
  uniform[62]=0.7893370F;
  uniform[63]=static_cast<float>(std::sqrt(
      parameters.atmosphere_height_metres*
      (2.0*parameters.ground_radius_metres+
       parameters.atmosphere_height_metres)));
  uniform[84]=384.0F;
  uniform[85]=216.0F;
  uniform[86]=384.0F;
  uniform[87]=216.0F;
  return uniform;
}

bool finite_nonzero_texture(id<MTLTexture> texture,const char* label) {
  const NSUInteger row_floats=texture.width*4U;
  const NSUInteger image_floats=row_floats*texture.height;
  std::vector<float> values(image_floats*texture.depth);
  [texture getBytes:values.data()
          bytesPerRow:row_floats*sizeof(float)
        bytesPerImage:image_floats*sizeof(float)
           fromRegion:MTLRegionMake3D(0U,0U,0U,texture.width,texture.height,
                                      texture.depth)
          mipmapLevel:0U
                slice:0U];
  std::size_t finite_values{};
  double energy{};
  for(float value:values){
    if(std::isfinite(value)){
      ++finite_values;
      energy+=std::abs(static_cast<double>(value));
    }
  }
  std::printf("{\"event\":\"metal_atmosphere_lut\",\"name\":\"%s\","
              "\"finite\":%zu,\"values\":%zu,\"energy\":%.9g}\n",
              label,finite_values,values.size(),energy);
  return finite_values==values.size()&&energy>1.0e-8;
}

int run_atmosphere_lut_smoke_test(id<MTLDevice> device) {
  constexpr std::size_t lookup_mode_count=5U;
  std::array<id<MTLComputePipelineState>,lookup_mode_count> pipelines{};
  for(std::size_t mode=0;mode<lookup_mode_count;++mode){
    const auto path=std::filesystem::path(
        TETRA_METAL_ATMOSPHERE_SHADER_DIR)/
        ("atmosphere_mode_"+std::to_string(mode)+".metal");
    id<MTLLibrary> atmosphere_library=make_file_shader_library(
        device,path.string().c_str());
    id<MTLFunction> function=[atmosphere_library newFunctionWithName:@"main0"];
    NSError* error=nil;
    pipelines[mode]=function==nil?nil:
        [device newComputePipelineStateWithFunction:function error:&error];
    if(pipelines[mode]==nil){
      std::fprintf(stderr,"Unable to create atmosphere mode %zu: %s\n",mode,
                   error.localizedDescription.UTF8String);
      return 1;
    }
  }
  const auto quality=tetra_viewer::atmosphere_quality_settings(
      tetra_viewer::AtmosphereQuality::standard);
  id<MTLTexture> transmittance=make_atmosphere_texture(
      device,quality.transmittance_width,quality.transmittance_height);
  id<MTLTexture> multiple=make_atmosphere_texture(
      device,quality.multiple_scattering_size,
      quality.multiple_scattering_size);
  id<MTLTexture> sky=make_atmosphere_texture(
      device,quality.sky_width,quality.sky_height);
  id<MTLTexture> aerial_scattering=make_atmosphere_texture(
      device,quality.aerial_width,quality.aerial_height,quality.aerial_depth);
  id<MTLTexture> aerial_transmittance=make_atmosphere_texture(
      device,quality.aerial_width,quality.aerial_height,quality.aerial_depth);
  id<MTLTexture> irradiance=make_atmosphere_texture(
      device,quality.irradiance_width,quality.irradiance_height);
  if(transmittance==nil||multiple==nil||sky==nil||
     aerial_scattering==nil||aerial_transmittance==nil||irradiance==nil)
    return 1;

  MTLTextureDescriptor* shadow_descriptor=[MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                  width:1U height:1U mipmapped:NO];
  shadow_descriptor.textureType=MTLTextureType2DArray;
  shadow_descriptor.arrayLength=5U;
  shadow_descriptor.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
  id<MTLTexture> shadow=[device newTextureWithDescriptor:shadow_descriptor];
  const auto uniform=make_atmosphere_smoke_uniform();
  id<MTLBuffer> uniform_buffer=[device newBufferWithBytes:uniform.data()
      length:uniform.size()*sizeof(float) options:MTLResourceStorageModeShared];
  std::array<float,92> shadow_cascades{};
  for(std::size_t matrix=0;matrix<5U;++matrix)
    for(std::size_t diagonal=0;diagonal<4U;++diagonal)
      shadow_cascades[matrix*16U+diagonal*5U]=1.0F;
  id<MTLBuffer> shadow_buffer=[device newBufferWithBytes:shadow_cascades.data()
      length:shadow_cascades.size()*sizeof(float)
      options:MTLResourceStorageModeShared];
  const std::array<std::uint32_t,4> empty_minmax{};
  id<MTLBuffer> minmax_buffer=[device newBufferWithBytes:empty_minmax.data()
      length:sizeof(empty_minmax) options:MTLResourceStorageModeShared];
  MTLSamplerDescriptor* sampler_descriptor=[MTLSamplerDescriptor new];
  sampler_descriptor.minFilter=MTLSamplerMinMagFilterLinear;
  sampler_descriptor.magFilter=MTLSamplerMinMagFilterLinear;
  sampler_descriptor.sAddressMode=MTLSamplerAddressModeClampToEdge;
  sampler_descriptor.tAddressMode=MTLSamplerAddressModeClampToEdge;
  id<MTLSamplerState> shadow_sampler=
      [device newSamplerStateWithDescriptor:sampler_descriptor];
  id<MTLCommandQueue> queue=[device newCommandQueue];
  id<MTLCommandBuffer> command=[queue commandBuffer];
  for(NSUInteger slice=0;slice<5U;++slice){
    MTLRenderPassDescriptor* clear=[MTLRenderPassDescriptor renderPassDescriptor];
    clear.depthAttachment.texture=shadow;
    clear.depthAttachment.slice=slice;
    clear.depthAttachment.loadAction=MTLLoadActionClear;
    clear.depthAttachment.storeAction=MTLStoreActionStore;
    clear.depthAttachment.clearDepth=1.0;
    id<MTLRenderCommandEncoder> encoder=
        [command renderCommandEncoderWithDescriptor:clear];
    [encoder endEncoding];
  }
  const auto dispatch=[&](std::size_t mode,NSUInteger width,NSUInteger height,
                          NSUInteger depth){
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:pipelines[mode]];
    [encoder setBuffer:uniform_buffer offset:0 atIndex:0];
    if(mode==0U)[encoder setTexture:transmittance atIndex:0];
    if(mode==1U){
      [encoder setTexture:transmittance atIndex:0];
      [encoder setTexture:multiple atIndex:1];
    }
    if(mode==2U){
      [encoder setBuffer:shadow_buffer offset:0 atIndex:1];
      [encoder setBuffer:minmax_buffer offset:0 atIndex:2];
      [encoder setTexture:transmittance atIndex:0];
      [encoder setTexture:multiple atIndex:1];
      [encoder setTexture:shadow atIndex:2];
      [encoder setTexture:sky atIndex:3];
      [encoder setSamplerState:shadow_sampler atIndex:0];
    }
    if(mode==3U){
      [encoder setTexture:transmittance atIndex:0];
      [encoder setTexture:multiple atIndex:1];
      [encoder setTexture:aerial_scattering atIndex:2];
      [encoder setTexture:aerial_transmittance atIndex:3];
    }
    if(mode==4U){
      [encoder setTexture:sky atIndex:0];
      [encoder setTexture:irradiance atIndex:1];
    }
    [encoder dispatchThreads:MTLSizeMake(width,height,depth)
        threadsPerThreadgroup:MTLSizeMake(8U,8U,1U)];
    [encoder endEncoding];
  };
  dispatch(0U,quality.transmittance_width,quality.transmittance_height,1U);
  dispatch(1U,quality.multiple_scattering_size,
           quality.multiple_scattering_size,1U);
  dispatch(2U,quality.sky_width,quality.sky_height,1U);
  dispatch(4U,quality.irradiance_width,quality.irradiance_height,1U);
  dispatch(3U,quality.aerial_width,quality.aerial_height,quality.aerial_depth);
  [command commit];
  [command waitUntilCompleted];
  if(command.status!=MTLCommandBufferStatusCompleted){
    std::fprintf(stderr,"Atmosphere LUT command failed: %s\n",
                 command.error.localizedDescription.UTF8String);
    return 1;
  }
  const bool valid=finite_nonzero_texture(transmittance,"transmittance")&&
      finite_nonzero_texture(multiple,"multiple_scattering")&&
      finite_nonzero_texture(sky,"sky_view")&&
      finite_nonzero_texture(irradiance,"sky_irradiance")&&
      finite_nonzero_texture(aerial_scattering,"aerial_scattering")&&
      finite_nonzero_texture(aerial_transmittance,"aerial_transmittance");
  std::printf("{\"event\":\"metal_atmosphere_lut_smoke\",\"passed\":%s}\n",
              valid?"true":"false");
  return valid?0:1;
}

id<MTLComputePipelineState> make_atmosphere_compute_pipeline(
    id<MTLDevice> device,std::size_t mode) {
  const auto path=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR)/
      ("atmosphere_mode_"+std::to_string(mode)+".metal");
  id<MTLLibrary> library=make_file_shader_library(device,path.string().c_str());
  id<MTLFunction> function=[library newFunctionWithName:@"main0"];
  NSError* error=nil;
  id<MTLComputePipelineState> pipeline=function==nil?nil:
      [device newComputePipelineStateWithFunction:function error:&error];
  if(pipeline==nil)
    std::fprintf(stderr,"Atmosphere pipeline %zu failed: %s\n",mode,
                 error.localizedDescription.UTF8String);
  return pipeline;
}

id<MTLRenderPipelineState> make_translated_composite_pipeline(
    id<MTLDevice> device,MTLPixelFormat colour_format,bool faithful=false) {
  const auto directory=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR);
  id<MTLLibrary> vertex_library=make_file_shader_library(
      device,(directory/"fullscreen.vert.metal").string().c_str());
  id<MTLLibrary> fragment_library=make_file_shader_library(
      device,(directory/(faithful?"tone_map_faithful.frag.metal":
                                  "tone_map.frag.metal")).string().c_str());
  MTLRenderPipelineDescriptor* descriptor=[MTLRenderPipelineDescriptor new];
  descriptor.label=@"TetWorld physical atmosphere composite";
  descriptor.vertexFunction=[vertex_library newFunctionWithName:@"main0"];
  descriptor.fragmentFunction=[fragment_library newFunctionWithName:@"main0"];
  descriptor.colorAttachments[0].pixelFormat=colour_format;
  NSError* error=nil;
  id<MTLRenderPipelineState> pipeline=
      [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  if(pipeline==nil)
    std::fprintf(stderr,"Atmosphere composite pipeline failed: %s\n",
                 error.localizedDescription.UTF8String);
  return pipeline;
}

MTLVertexDescriptor* make_production_scene_vertex_descriptor() {
  MTLVertexDescriptor* vertices=[MTLVertexDescriptor vertexDescriptor];
  const auto attribute=[&](NSUInteger index,MTLVertexFormat format,
                           NSUInteger offset){
    vertices.attributes[index].format=format;
    vertices.attributes[index].offset=offset;
    vertices.attributes[index].bufferIndex=1U;
  };
  attribute(0U,MTLVertexFormatFloat3,
            offsetof(tetra_viewer::SceneVertex,position));
  attribute(1U,MTLVertexFormatFloat3,
            offsetof(tetra_viewer::SceneVertex,colour));
  attribute(2U,MTLVertexFormatFloat3,
            offsetof(tetra_viewer::SceneVertex,normal));
  attribute(3U,MTLVertexFormatFloat2,
            offsetof(tetra_viewer::SceneVertex,diagnostics));
  attribute(4U,MTLVertexFormatFloat3,
            offsetof(tetra_viewer::SceneVertex,barycentric));
  attribute(5U,MTLVertexFormatFloat,
            offsetof(tetra_viewer::SceneVertex,edge_flags));
  attribute(6U,MTLVertexFormatFloat3,
            offsetof(tetra_viewer::SceneVertex,smooth_normal));
  vertices.layouts[1].stride=sizeof(tetra_viewer::SceneVertex);
  vertices.layouts[1].stepFunction=MTLVertexStepFunctionPerVertex;
  return vertices;
}

id<MTLRenderPipelineState> make_translated_scene_pipeline(
    id<MTLDevice> device,MTLPixelFormat colour_format,
    MTLPixelFormat depth_format,NSUInteger samples) {
  const auto directory=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR);
  id<MTLLibrary> vertex_library=make_file_shader_library(
      device,(directory/"scene.vert.metal").string().c_str());
  id<MTLLibrary> fragment_library=make_file_shader_library(
      device,(directory/"scene.frag.metal").string().c_str());
  MTLRenderPipelineDescriptor* descriptor=[MTLRenderPipelineDescriptor new];
  descriptor.label=@"TetWorld production terrain";
  descriptor.vertexFunction=[vertex_library newFunctionWithName:@"main0"];
  descriptor.fragmentFunction=[fragment_library newFunctionWithName:@"main0"];
  descriptor.vertexDescriptor=make_production_scene_vertex_descriptor();
  descriptor.colorAttachments[0].pixelFormat=colour_format;
  descriptor.depthAttachmentPixelFormat=depth_format;
  descriptor.rasterSampleCount=samples;
  NSError* error=nil;
  id<MTLRenderPipelineState> pipeline=
      [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  if(pipeline==nil)
    std::fprintf(stderr,"Production terrain pipeline failed: %s\n",
                 error.localizedDescription.UTF8String);
  return pipeline;
}

id<MTLRenderPipelineState> make_translated_wire_pipeline(
    id<MTLDevice> device,MTLPixelFormat colour_format,
    MTLPixelFormat depth_format,NSUInteger samples) {
  const auto directory=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR);
  id<MTLLibrary> vertex_library=make_file_shader_library(
      device,(directory/"scene.vert.metal").string().c_str());
  id<MTLLibrary> fragment_library=make_file_shader_library(
      device,(directory/"wire.frag.metal").string().c_str());
  MTLRenderPipelineDescriptor* descriptor=[MTLRenderPipelineDescriptor new];
  descriptor.label=@"TetWorld production terrain wireframe";
  descriptor.vertexFunction=[vertex_library newFunctionWithName:@"main0"];
  descriptor.fragmentFunction=[fragment_library newFunctionWithName:@"main0"];
  descriptor.vertexDescriptor=make_production_scene_vertex_descriptor();
  descriptor.colorAttachments[0].pixelFormat=colour_format;
  descriptor.depthAttachmentPixelFormat=depth_format;
  descriptor.rasterSampleCount=samples;
  NSError* error=nil;
  id<MTLRenderPipelineState> pipeline=
      [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  if(pipeline==nil)
    std::fprintf(stderr,"Production wireframe pipeline failed: %s\n",
                 error.localizedDescription.UTF8String);
  return pipeline;
}

struct MetalAtmosphereResources {
  std::array<id<MTLComputePipelineState>,17> pipelines{};
  id<MTLComputePipelineState> reference_pipeline=nil;
  id<MTLComputePipelineState> ray_visibility_pipeline=nil;
  id<MTLRenderPipelineState> composite_pipeline=nil;
  id<MTLRenderPipelineState> faithful_composite_pipeline=nil;
  id<MTLTexture> transmittance=nil;
  id<MTLTexture> multiple_scattering=nil;
  id<MTLTexture> sky_view=nil;
  id<MTLTexture> sky_irradiance=nil;
  id<MTLTexture> long_shadow=nil;
  id<MTLTexture> dummy_long_shadow=nil;
  id<MTLTexture> aerial_scattering=nil;
  id<MTLTexture> aerial_transmittance=nil;
  id<MTLTexture> dummy_aerial_scattering=nil;
  id<MTLTexture> dummy_aerial_transmittance=nil;
  id<MTLTexture> froxel_scattering=nil;
  id<MTLTexture> froxel_transmittance=nil;
  id<MTLTexture> dummy_froxel_scattering=nil;
  id<MTLTexture> dummy_froxel_transmittance=nil;
  id<MTLTexture> dummy_screen=nil;
  id<MTLTexture> screen_endpoint=nil;
  id<MTLTexture> screen_scattering=nil;
  id<MTLTexture> screen_transmittance=nil;
  id<MTLTexture> terrain_ray_visibility=nil;
  std::array<id<MTLTexture>,2> history_visibility{};
  std::array<id<MTLTexture>,2> history_scattering{};
  std::array<id<MTLTexture>,2> history_transmittance{};
  std::array<id<MTLTexture>,2> history_endpoint{};
  NSUInteger screen_width{};
  NSUInteger screen_height{};
  std::uint32_t screen_divisor{2U};
  std::uint32_t history_write_index{};
  std::uint32_t history_sequence{};
  std::uint32_t history_sample_count{};
  std::uint64_t ray_visibility_scene_generation{};
  std::array<tetra_viewer::AtmosphereScreenHistoryIdentity,2>
      history_identities{};
  std::uint32_t history_present_index{};
  bool history_present_valid{};
  bool last_visibility_backend_ray_traced{};
  bool history_valid{};
  std::array<float,16> last_temporal_camera{};
  id<MTLTexture> dummy_shadow=nil;
  id<MTLBuffer> shadow_uniform=nil;
  id<MTLBuffer> minmax=nil;
  id<MTLBuffer> dummy_minmax=nil;
  id<MTLSamplerState> sampler=nil;
  bool optical_ready{};
  bool view_ready{};
  bool reference_lookup_ready{};
  bool long_shadow_ready{};
  bool dummy_shadow_cleared{};
  std::array<float,64> last_view_uniform{};
  std::array<float,96> last_reference_lookup_uniform{};
  ProductionShadowUniforms last_reference_lookup_shadows{};
  std::uint64_t last_reference_lookup_generation{};
  std::array<float,64> last_long_uniform{};
  ProductionShadowUniforms last_long_shadows{};
  std::uint64_t last_long_scene_generation{};
  std::uint64_t minmax_scene_generation{};
  int minmax_kind{};
  std::uint32_t atmosphere_shadow_resolution{};
  std::uint32_t long_shadow_width{};
  std::uint32_t long_shadow_height{};
  std::uint32_t aerial_width{};
  std::uint32_t aerial_height{};
  std::uint32_t aerial_depth{};
  std::size_t minmax_element_count{};
  std::uint64_t ray_visibility_dispatches{};
  std::uint32_t last_ray_visibility_query_count{};
  std::uint64_t temporal_history_attempts{};
  std::uint64_t temporal_history_compatible{};
  std::uint64_t temporal_history_invalidations{};
  std::uint64_t temporal_camera_refreshes{};
  std::array<std::uint64_t,9> temporal_invalidation_reasons{};
  std::uint64_t reference_lookup_attempts{};
  std::uint64_t reference_lookup_skips{};
  std::array<std::uint64_t,17> dispatch_counts{};
};

void hash_metal_history_scalar(std::uint64_t& hash,double value) {
  constexpr std::uint64_t prime=1099511628211ULL;
  const auto bits=std::bit_cast<std::uint64_t>(value);
  for(unsigned byte=0;byte<8U;++byte){
    hash^=(bits>>(byte*8U))&0xffU;
    hash*=prime;
  }
}

std::uint64_t hash_metal_history_values(
    std::initializer_list<double> values) {
  std::uint64_t hash=1469598103934665603ULL;
  for(const double value:values)hash_metal_history_scalar(hash,value);
  return hash;
}

tetra_viewer::AtmosphereScreenHistoryIdentity
make_metal_atmosphere_history_identity(
    const std::array<float,96>& stable_uniform,
    const tetra_viewer::AtmosphereParameters& parameters,
    std::uint64_t terrain_generation,const tetra::Vec3& render_origin,
    std::uint32_t width,std::uint32_t height,std::uint32_t divisor,
    int transport,int rendering_method,bool valid) {
  const tetra::Vec3 camera_from_centre{
      stable_uniform[28],stable_uniform[29],stable_uniform[30]};
  const auto vector_hash=[](std::initializer_list<tetra::Vec3> vectors,
                            std::initializer_list<double> scalars={}){
    std::uint64_t hash=1469598103934665603ULL;
    for(const auto& vector:vectors){
      hash_metal_history_scalar(hash,vector.x);
      hash_metal_history_scalar(hash,vector.y);
      hash_metal_history_scalar(hash,vector.z);
    }
    for(const double scalar:scalars)hash_metal_history_scalar(hash,scalar);
    return hash;
  };
  const tetra::Vec3 right{stable_uniform[32],stable_uniform[33],
                          stable_uniform[34]};
  const tetra::Vec3 down{stable_uniform[36],stable_uniform[37],
                         stable_uniform[38]};
  const tetra::Vec3 forward{stable_uniform[40],stable_uniform[41],
                            stable_uniform[42]};
  const tetra::Vec3 sun{stable_uniform[44],stable_uniform[45],
                        stable_uniform[46]};
  const auto optical=tetra_viewer::atmosphere_optical_hash(parameters);
  const auto scattering=tetra_viewer::atmosphere_scattering_hash(parameters);
  const std::uint64_t result_generation=vector_hash(
      {camera_from_centre,right,down,forward,sun,render_origin},
      {stable_uniform[35],stable_uniform[39],
       static_cast<double>(terrain_generation),
       static_cast<double>(optical),static_cast<double>(scattering)});
  return {
      .revisions={
          .optical={optical},
          .scattering={scattering},
          .sun={vector_hash({sun})},
          .camera_position={vector_hash({camera_from_centre})},
          .sky_position=tetra_viewer::atmosphere_sky_position_revision(
              camera_from_centre,parameters),
          .camera_orientation={vector_hash(
              {right,down,forward},{stable_uniform[35],stable_uniform[39]})},
          .shadow_integrator={static_cast<std::uint64_t>(
              std::max(0,static_cast<int>(stable_uniform[55])))+1U},
          .shadow={hash_metal_history_values(
              {static_cast<double>(terrain_generation),
               static_cast<double>(stable_uniform[54])})},
          .render_origin={vector_hash({render_origin})}},
      .terrain_generation=terrain_generation,
      .result_generation=result_generation,
      .width=width,.height=height,
      .linear_resolution_divisor=divisor,
      .sample_count=rendering_method==3?2U:32U,
      .transport=static_cast<tetra_viewer::AtmosphereTransport>(transport),
      .rendering_method=
          static_cast<tetra_viewer::AtmosphereRenderingMethod>(rendering_method),
      .valid=valid};
}

MetalAtmosphereResources make_live_atmosphere_resources(
    id<MTLDevice> device,MTLPixelFormat display_format,
    tetra_viewer::AtmosphereQuality quality) {
  MetalAtmosphereResources resources;
  for(std::size_t mode=0;mode<resources.pipelines.size();++mode)
    resources.pipelines[mode]=make_atmosphere_compute_pipeline(device,mode);
  {
    const auto path=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR)/
        "atmosphere_reference_hillaire.metal";
    id<MTLLibrary> library=make_file_shader_library(
        device,path.string().c_str());
    id<MTLFunction> function=[library newFunctionWithName:@"main0"];
    NSError* error=nil;
  resources.reference_pipeline=function==nil?nil:
        [device newComputePipelineStateWithFunction:function error:&error];
    if(resources.reference_pipeline==nil)
      std::fprintf(stderr,"Reference atmosphere pipeline failed: %s\n",
                   error.localizedDescription.UTF8String);
  }
  resources.ray_visibility_pipeline=make_atmosphere_ray_visibility_pipeline(device);
  resources.composite_pipeline=make_translated_composite_pipeline(
      device,display_format);
  resources.faithful_composite_pipeline=make_translated_composite_pipeline(
      device,display_format,true);
  const auto settings=tetra_viewer::atmosphere_quality_settings(quality);
  resources.atmosphere_shadow_resolution=
      settings.atmosphere_shadow_resolution;
  resources.transmittance=make_atmosphere_texture(
      device,settings.transmittance_width,settings.transmittance_height,1U,
      AtmosphereTextureRole::transmittance);
  resources.multiple_scattering=make_atmosphere_texture(
      device,settings.multiple_scattering_size,settings.multiple_scattering_size);
  resources.sky_view=make_atmosphere_texture(
      device,settings.sky_width,settings.sky_height);
  resources.sky_irradiance=make_atmosphere_texture(
      device,settings.irradiance_width,settings.irradiance_height);
  resources.long_shadow_width=settings.long_shadow_width;
  resources.long_shadow_height=settings.long_shadow_height;
  resources.aerial_width=settings.aerial_width;
  resources.aerial_height=settings.aerial_height;
  resources.aerial_depth=settings.aerial_depth;
  resources.dummy_aerial_scattering=make_atmosphere_texture(device,1U,1U,1U);
  resources.dummy_aerial_transmittance=make_atmosphere_texture(device,1U,1U,1U);
  resources.aerial_scattering=resources.dummy_aerial_scattering;
  resources.aerial_transmittance=resources.dummy_aerial_transmittance;
  // Most routes bind but never sample froxel volumes. Keep a type-correct
  // writable fallback for those translated entry points and materialize the
  // full 32^3 pair only for renderer 4.
  resources.dummy_froxel_scattering=make_atmosphere_texture(device,1U,1U,1U);
  resources.dummy_froxel_transmittance=make_atmosphere_texture(device,1U,1U,1U);
  resources.froxel_scattering=resources.dummy_froxel_scattering;
  resources.froxel_transmittance=resources.dummy_froxel_transmittance;
  resources.dummy_screen=make_atmosphere_texture(device,1U,1U);
  resources.dummy_long_shadow=make_atmosphere_texture(device,1U,1U);
  resources.long_shadow=resources.dummy_long_shadow;
  MTLTextureDescriptor* shadow=[MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                  width:1U height:1U mipmapped:NO];
  shadow.textureType=MTLTextureType2DArray;
  shadow.arrayLength=5U;
  shadow.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
  resources.dummy_shadow=[device newTextureWithDescriptor:shadow];
  std::array<float,92> shadow_data{};
  for(std::size_t matrix=0;matrix<5U;++matrix)
    for(std::size_t diagonal=0;diagonal<4U;++diagonal)
      shadow_data[matrix*16U+diagonal*5U]=1.0F;
  resources.shadow_uniform=[device newBufferWithBytes:shadow_data.data()
      length:shadow_data.size()*sizeof(float)
      options:MTLResourceStorageModeShared];
  std::size_t square_elements{};
  for(std::uint32_t size=settings.atmosphere_shadow_resolution;;
      size=(size+1U)/2U){
    square_elements+=static_cast<std::size_t>(size)*size;
    if(size==1U)break;
  }
  const auto epipolar=tetra_viewer::atmosphere_epipolar_layout(
      settings.atmosphere_shadow_resolution);
  resources.minmax_element_count=std::max(square_elements,
                                           epipolar.element_count)+2U;
  resources.dummy_minmax=[device newBufferWithLength:sizeof(std::uint32_t)*2U
      options:MTLResourceStorageModeShared];
  resources.minmax=resources.dummy_minmax;
  MTLSamplerDescriptor* sampler=[MTLSamplerDescriptor new];
  sampler.minFilter=MTLSamplerMinMagFilterLinear;
  sampler.magFilter=MTLSamplerMinMagFilterLinear;
  sampler.mipFilter=MTLSamplerMipFilterNotMipmapped;
  sampler.sAddressMode=MTLSamplerAddressModeClampToEdge;
  sampler.tAddressMode=MTLSamplerAddressModeClampToEdge;
  sampler.rAddressMode=MTLSamplerAddressModeClampToEdge;
  resources.sampler=[device newSamplerStateWithDescriptor:sampler];
  return resources;
}

bool live_atmosphere_resources_valid(const MetalAtmosphereResources& resources) {
  return std::ranges::all_of(resources.pipelines,
      [](id<MTLComputePipelineState> pipeline){return pipeline!=nil;})&&
      resources.reference_pipeline!=nil&&
      resources.ray_visibility_pipeline!=nil&&
      resources.composite_pipeline!=nil&&resources.transmittance!=nil&&
      resources.faithful_composite_pipeline!=nil&&
      resources.multiple_scattering!=nil&&resources.sky_view!=nil&&
      resources.sky_irradiance!=nil&&resources.long_shadow!=nil&&
      resources.dummy_long_shadow!=nil&&
      resources.aerial_scattering!=nil&&
      resources.aerial_transmittance!=nil&&resources.froxel_scattering!=nil&&
      resources.dummy_aerial_scattering!=nil&&
      resources.dummy_aerial_transmittance!=nil&&
      resources.froxel_transmittance!=nil&&resources.dummy_froxel_scattering!=nil&&
      resources.dummy_froxel_transmittance!=nil&&resources.dummy_screen!=nil&&
      resources.dummy_shadow!=nil&&resources.shadow_uniform!=nil&&
      resources.minmax!=nil&&resources.dummy_minmax!=nil&&resources.sampler!=nil;
}

std::size_t atmosphere_texture_bytes(id<MTLTexture> texture) {
  if(texture==nil)return 0U;
  std::size_t bytes_per_pixel{};
  switch(texture.pixelFormat){
    case MTLPixelFormatRGBA32Float:bytes_per_pixel=16U;break;
    case MTLPixelFormatRGBA16Float:bytes_per_pixel=8U;break;
    case MTLPixelFormatRG32Uint:bytes_per_pixel=8U;break;
    case MTLPixelFormatR8Uint:bytes_per_pixel=1U;break;
    case MTLPixelFormatDepth32Float:bytes_per_pixel=4U;break;
    default:return 0U;
  }
  return static_cast<std::size_t>(texture.width)*texture.height*texture.depth*
      texture.arrayLength*bytes_per_pixel*texture.sampleCount;
}

std::size_t live_atmosphere_allocation_bytes(
    const MetalAtmosphereResources& resources) {
  std::size_t total=resources.shadow_uniform==nil?0U:resources.shadow_uniform.length;
  total+=resources.minmax==nil?0U:resources.minmax.length;
  for(id<MTLTexture> texture:std::array{
          resources.transmittance,resources.multiple_scattering,
          resources.sky_view,resources.sky_irradiance,resources.long_shadow,
          resources.aerial_scattering,resources.aerial_transmittance,
          resources.froxel_scattering,resources.froxel_transmittance,
          resources.dummy_screen,resources.screen_endpoint,
          resources.screen_scattering,resources.screen_transmittance,
          resources.terrain_ray_visibility,
          resources.dummy_shadow})
    total+=atmosphere_texture_bytes(texture);
  for(const auto& textures:std::array{
          resources.history_visibility,resources.history_scattering,
          resources.history_transmittance,resources.history_endpoint})
    for(id<MTLTexture> texture:textures)
      total+=atmosphere_texture_bytes(texture);
  return total;
}

bool ensure_shadowed_froxel_resources(id<MTLDevice> device,
                                      MetalAtmosphereResources& resources) {
  if(resources.froxel_scattering!=resources.dummy_froxel_scattering&&
     resources.froxel_transmittance!=resources.dummy_froxel_transmittance)
    return true;
  resources.froxel_scattering=make_atmosphere_texture(device,32U,32U,32U);
  resources.froxel_transmittance=make_atmosphere_texture(device,32U,32U,32U);
  return resources.froxel_scattering!=nil&&resources.froxel_transmittance!=nil;
}

bool ensure_aerial_resources(id<MTLDevice> device,
                             MetalAtmosphereResources& resources) {
  if(resources.aerial_scattering!=resources.dummy_aerial_scattering&&
     resources.aerial_transmittance!=resources.dummy_aerial_transmittance)
    return true;
  resources.aerial_scattering=make_atmosphere_texture(
      device,resources.aerial_width,resources.aerial_height,
      resources.aerial_depth);
  resources.aerial_transmittance=make_atmosphere_texture(
      device,resources.aerial_width,resources.aerial_height,
      resources.aerial_depth);
  return resources.aerial_scattering!=nil&&resources.aerial_transmittance!=nil;
}

bool ensure_long_shadow_resources(id<MTLDevice> device,
                                  MetalAtmosphereResources& resources) {
  if(resources.long_shadow!=resources.dummy_long_shadow)return true;
  resources.long_shadow=make_atmosphere_texture(
      device,resources.long_shadow_width,resources.long_shadow_height);
  return resources.long_shadow!=nil;
}

bool ensure_shadow_minmax_resources(id<MTLDevice> device,
                                    MetalAtmosphereResources& resources) {
  if(resources.minmax!=resources.dummy_minmax)return true;
  resources.minmax=[device newBufferWithLength:
      resources.minmax_element_count*sizeof(std::uint32_t)*2U
      options:MTLResourceStorageModeShared];
  return resources.minmax!=nil;
}

struct MetalTimingIdentity {
  std::uint64_t terrain_generation{};
  std::uint32_t output_width{};
  std::uint32_t output_height{};
  std::uint32_t render_width{};
  std::uint32_t render_height{};
  std::uint32_t atmosphere_divisor{};
  std::uint32_t samples{};
  std::int32_t transport{};
  std::int32_t renderer{};
  bool metalfx{};
};

struct MetalGpuStageTimings {
  std::atomic<double> shadows_milliseconds{};
  std::atomic<double> atmosphere_milliseconds{};
  std::atomic<double> terrain_milliseconds{};
  std::atomic<double> terrain_generation_milliseconds{};
  std::atomic<double> composite_milliseconds{};
  std::atomic<double> depth_reduction_milliseconds{};
  std::atomic<double> screen_integration_milliseconds{};
  std::atomic<double> temporal_reconstruction_milliseconds{};
  std::atomic<double> metalfx_milliseconds{};
  std::atomic<double> optical_lookup_milliseconds{};
  std::atomic<double> sky_view_lookup_milliseconds{};
  std::atomic<double> irradiance_lookup_milliseconds{};
  std::atomic<double> aerial_lookup_milliseconds{};
  std::atomic<bool> valid{};
  std::atomic<bool> screen_stages_valid{};
  std::atomic<bool> metalfx_valid{};
  std::atomic<std::uint64_t> frame_sequence{};
  std::atomic<std::uint64_t> terrain_generation{};
  std::atomic<std::uint32_t> output_width{};
  std::atomic<std::uint32_t> output_height{};
  std::atomic<std::uint32_t> render_width{};
  std::atomic<std::uint32_t> render_height{};
  std::atomic<std::uint32_t> atmosphere_divisor{};
  std::atomic<std::uint32_t> samples{};
  std::atomic<std::int32_t> transport{};
  std::atomic<std::int32_t> renderer{};
  std::atomic<bool> metalfx{};
};

// A profile sample is deliberately recorded by the command-buffer completion
// handler, rather than by the producer loop.  This makes its GPU interval the
// same completed interval used by the adaptive-resolution controller.
struct MetalTimingProfileSamples {
  mutable std::mutex mutex;
  std::vector<double> gpu_milliseconds;
  std::vector<double> optical_lookup_milliseconds;
  std::vector<double> sky_view_lookup_milliseconds;
  std::vector<double> irradiance_lookup_milliseconds;
  std::vector<double> aerial_lookup_milliseconds;
  std::vector<double> screen_integration_milliseconds;
  std::vector<double> terrain_generation_milliseconds;

  void add(double milliseconds) {
    std::lock_guard lock(mutex);
    // The producer can submit a few frames ahead of the completion handler.
    // Cap at the declared sample count so a profile has a fixed, comparable
    // population even while the main loop observes the terminal condition.
    if(gpu_milliseconds.size()<300U)gpu_milliseconds.push_back(milliseconds);
  }

  [[nodiscard]] std::size_t size() const {
    std::lock_guard lock(mutex);
    return gpu_milliseconds.size();
  }

  [[nodiscard]] std::vector<double> ordered() const {
    std::lock_guard lock(mutex);
    auto result=gpu_milliseconds;
    std::ranges::sort(result);
    return result;
  }

  void add_optical_lookup(double optical) {
    std::lock_guard lock(mutex);
    if(optical_lookup_milliseconds.size()<300U)
      optical_lookup_milliseconds.push_back(optical);
  }

  void add_sky_view_lookup(double sky_view) {
    std::lock_guard lock(mutex);
    if(sky_view_lookup_milliseconds.size()<300U)
      sky_view_lookup_milliseconds.push_back(sky_view);
  }

  void add_irradiance_lookup(double irradiance) {
    std::lock_guard lock(mutex);
    if(irradiance_lookup_milliseconds.size()<300U)
      irradiance_lookup_milliseconds.push_back(irradiance);
  }

  void add_aerial_lookup(double aerial) {
    std::lock_guard lock(mutex);
    if(aerial_lookup_milliseconds.size()<300U)
      aerial_lookup_milliseconds.push_back(aerial);
  }

  void add_screen_integration(double integration) {
    std::lock_guard lock(mutex);
    if(screen_integration_milliseconds.size()<300U)
      screen_integration_milliseconds.push_back(integration);
  }

  // Unlike frame samples, this is recorded only for a command buffer that
  // actually encoded the owner-direct terrain generation route.
  void add_terrain_generation(double generation) {
    std::lock_guard lock(mutex);
    if(terrain_generation_milliseconds.size()<30U)
      terrain_generation_milliseconds.push_back(generation);
  }

  [[nodiscard]] std::vector<double> ordered_terrain_generation() const {
    std::lock_guard lock(mutex);
    auto result=terrain_generation_milliseconds;
    std::ranges::sort(result);
    return result;
  }

  [[nodiscard]] std::array<std::vector<double>,5U>
  ordered_lookups() const {
    std::lock_guard lock(mutex);
    auto optical=optical_lookup_milliseconds;
    auto sky=sky_view_lookup_milliseconds;
    auto irradiance=irradiance_lookup_milliseconds;
    auto aerial=aerial_lookup_milliseconds;
    auto integration=screen_integration_milliseconds;
    std::ranges::sort(optical);
    std::ranges::sort(sky);
    std::ranges::sort(irradiance);
    std::ranges::sort(aerial);
    std::ranges::sort(integration);
    return {std::move(optical),std::move(sky),std::move(irradiance),
            std::move(aerial),std::move(integration)};
  }
};

double timing_percentile(const std::vector<double>& ordered,double fraction) {
  if(ordered.empty())return 0.0;
  const auto index=static_cast<std::size_t>(std::ceil(
      fraction*static_cast<double>(ordered.size()-1U)));
  return ordered[std::min(index,ordered.size()-1U)];
}

constexpr NSUInteger gpu_base_timestamp_count=7U;
// 0..24 are the established frame/stage schema.  The final pair brackets
// only owner-direct terrain generation through private publication.
constexpr NSUInteger gpu_timestamp_count=27U;
constexpr std::size_t gpu_timestamp_flight_count=3U;

struct MetalTimestampFlight {
  id<MTLCounterSampleBuffer> samples=nil;
  id<MTLBuffer> results=nil;
  id<MTLBuffer> scratch=nil;
  std::shared_ptr<std::atomic<bool>> in_use=
      std::make_shared<std::atomic<bool>>(false);
};

// MTLCounterResultTimestamp values are expressed in nanoseconds.  They use a
// different clock representation from MTLDevice sampleTimestamps, so they
// must not be calibrated through the latter's GPU tick frequency.
constexpr double counter_timestamp_milliseconds=1.0e-6;

id<MTLCounterSet> timestamp_counter_set(id<MTLDevice> device) {
  if(![device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
    return nil;
  for(id<MTLCounterSet> counter_set in device.counterSets)
    if([counter_set.name isEqualToString:MTLCommonCounterSetTimestamp])
      return counter_set;
  return nil;
}

id<MTLCounterSampleBuffer> make_timestamp_sample_buffer(
    id<MTLDevice> device,id<MTLCounterSet> counter_set,NSUInteger count) {
  if(counter_set==nil)return nil;
  MTLCounterSampleBufferDescriptor* descriptor=
      [MTLCounterSampleBufferDescriptor new];
  descriptor.label=@"TetWorldMetal stage timestamps";
  descriptor.counterSet=counter_set;
  descriptor.storageMode=MTLStorageModeShared;
  descriptor.sampleCount=count;
  NSError* error=nil;
  return [device newCounterSampleBufferWithDescriptor:descriptor error:&error];
}

MetalTimestampFlight make_timestamp_flight(
    id<MTLDevice> device,id<MTLCounterSet> counter_set) {
  MetalTimestampFlight flight;
  flight.samples=make_timestamp_sample_buffer(
      device,counter_set,gpu_timestamp_count);
  if(flight.samples!=nil){
    flight.results=[device newBufferWithLength:
        gpu_timestamp_count*sizeof(MTLCounterResultTimestamp)
                                  options:MTLResourceStorageModeShared];
    flight.scratch=[device newBufferWithLength:4U
                                       options:MTLResourceStorageModeShared];
  }
  if(flight.results==nil||flight.scratch==nil){
    flight.samples=nil;
    flight.results=nil;
    flight.scratch=nil;
  }
  return flight;
}

void encode_timestamp_marker(id<MTLCommandBuffer> command,
                             id<MTLCounterSampleBuffer> samples,
                             id<MTLBuffer> scratch,NSUInteger index) {
  if(samples==nil)return;
  MTLBlitPassDescriptor* pass=[MTLBlitPassDescriptor blitPassDescriptor];
  auto* attachment=pass.sampleBufferAttachments[0];
  attachment.sampleBuffer=samples;
  attachment.startOfEncoderSampleIndex=index;
  attachment.endOfEncoderSampleIndex=MTLCounterDontSample;
  id<MTLBlitCommandEncoder> marker=
      [command blitCommandEncoderWithDescriptor:pass];
  [marker fillBuffer:scratch range:NSMakeRange(0U,4U)
                value:static_cast<std::uint8_t>(index)];
  [marker endEncoding];
}

id<MTLComputeCommandEncoder> timestamped_compute_encoder(
    id<MTLCommandBuffer> command,id<MTLCounterSampleBuffer> samples,
    NSUInteger start_index,NSUInteger end_index) {
  if(samples==nil)return [command computeCommandEncoder];
  MTLComputePassDescriptor* pass=
      [MTLComputePassDescriptor computePassDescriptor];
  auto* attachment=pass.sampleBufferAttachments[0];
  attachment.sampleBuffer=samples;
  attachment.startOfEncoderSampleIndex=start_index;
  attachment.endOfEncoderSampleIndex=end_index;
  return [command computeCommandEncoderWithDescriptor:pass];
}

ProductionShadowUniforms make_production_shadow_uniforms(
    const ShadowUniforms& local,
    const std::optional<tetra_viewer::AtmosphereShadowMapFit>& fitted,
    bool fitted_initialized,double fitted_receiver_distance,
    const tetra_viewer::AtmosphereQualitySettings& quality,
    std::size_t minmax_element_count,NSUInteger shadow_resolution) {
  ProductionShadowUniforms result{};
  for(std::size_t index=0;index<tetra_viewer::shadow_cascade_count;++index)
    result.matrices[index]=local.matrices[index];
  if(fitted)result.matrices[4]=fitted->matrix;
  else for(std::size_t diagonal=0;diagonal<4U;++diagonal)
    result.matrices[4][diagonal*5U]=1.0F;
  result.splits=local.splits;
  result.local_depth_spans=local.depth_spans;
  if(fitted&&fitted_initialized){
    result.atmosphere_metadata[0]=1.0F;
    result.atmosphere_metadata[1]=static_cast<float>(fitted_receiver_distance);
    result.atmosphere_metadata[2]=static_cast<float>(
        tetra_viewer::atmosphere_fitted_shadow_depth_bias(
            fitted->depth_world_span,fitted->texel_world_size_x,
            fitted->texel_world_size_y));
    result.atmosphere_metadata[3]=
        static_cast<float>(quality.atmosphere_shadow_resolution)/
        static_cast<float>(shadow_resolution);
  }
  const auto epipolar=tetra_viewer::atmosphere_epipolar_layout(
      quality.atmosphere_shadow_resolution);
  result.epipolar_metadata[0]=static_cast<float>(epipolar.radial_resolution);
  result.epipolar_metadata[1]=static_cast<float>(epipolar.angular_rows);
  result.epipolar_metadata[2]=0.0036F;
  result.epipolar_metadata[3]=static_cast<float>(minmax_element_count);
  return result;
}

id<MTLTexture> make_uint_atmosphere_texture(id<MTLDevice> device,
                                            NSUInteger width,
                                            NSUInteger height) {
  MTLTextureDescriptor* descriptor=[MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRG32Uint
                                  width:width height:height mipmapped:NO];
  descriptor.storageMode=MTLStorageModeShared;
  descriptor.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
  return [device newTextureWithDescriptor:descriptor];
}

id<MTLTexture> make_ray_visibility_texture(id<MTLDevice> device,
                                            NSUInteger width,
                                            NSUInteger height) {
  MTLTextureDescriptor* descriptor=[MTLTextureDescriptor new];
  descriptor.textureType=MTLTextureType3D;
  descriptor.pixelFormat=MTLPixelFormatR8Uint;
  descriptor.width=width;
  descriptor.height=height;
  descriptor.depth=32U;
  descriptor.mipmapLevelCount=1U;
  descriptor.storageMode=MTLStorageModePrivate;
  descriptor.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
  return [device newTextureWithDescriptor:descriptor];
}

bool ensure_screen_atmosphere_resources(id<MTLDevice> device,
                                        MetalAtmosphereResources& resources,
                                        NSUInteger render_width,
                                        NSUInteger render_height,
                                        std::uint32_t divisor=2U,
                                        bool needs_visibility_history=false) {
  divisor=std::clamp(divisor,1U,4U);
  const NSUInteger width=(render_width+divisor-1U)/divisor;
  const NSUInteger height=(render_height+divisor-1U)/divisor;
  if(resources.screen_width==width&&resources.screen_height==height&&
     resources.screen_divisor==divisor&&resources.screen_endpoint!=nil&&
     (!needs_visibility_history||
      (resources.terrain_ray_visibility!=nil&&
       resources.history_visibility[0]!=nil&&
       resources.history_visibility[1]!=nil)))
    return true;
  resources.screen_endpoint=make_atmosphere_texture(
      device,width,height,1U,AtmosphereTextureRole::screen);
  resources.screen_scattering=make_atmosphere_texture(
      device,width,height,1U,AtmosphereTextureRole::screen);
  resources.screen_transmittance=make_atmosphere_texture(
      device,width,height,1U,AtmosphereTextureRole::screen_transmittance);
  // The qualified reference route evaluates terrain visibility in its own
  // screen integration and never binds this 3D query volume or its packed
  // binary histories.  Allocate the family only once a non-reference route
  // can consume it; a mode switch calls this routine before encoding.
  if(needs_visibility_history){
    resources.terrain_ray_visibility=make_ray_visibility_texture(
        device,width,height);
    resources.history_visibility[0]=make_uint_atmosphere_texture(
        device,width,height);
    resources.history_visibility[1]=make_uint_atmosphere_texture(
        device,width,height);
  }else{
    resources.terrain_ray_visibility=nil;
    resources.history_visibility={};
  }
  for(std::size_t index=0;index<2U;++index){
    resources.history_scattering[index]=make_atmosphere_texture(
        device,width,height,1U,AtmosphereTextureRole::screen);
    resources.history_transmittance[index]=make_atmosphere_texture(
        device,width,height,1U,
        AtmosphereTextureRole::screen_transmittance);
    resources.history_endpoint[index]=make_atmosphere_texture(
        device,width,height,1U,AtmosphereTextureRole::screen);
  }
  resources.screen_width=width;
  resources.screen_height=height;
  resources.screen_divisor=divisor;
  resources.history_valid=false;
  resources.history_write_index=0U;
  resources.history_sequence=0U;
  resources.history_sample_count=0U;
  resources.history_identities={};
  return resources.screen_endpoint!=nil&&resources.screen_scattering!=nil&&
      resources.screen_transmittance!=nil&&
      (!needs_visibility_history||
       (resources.terrain_ray_visibility!=nil&&
        resources.history_visibility[0]!=nil&&
        resources.history_visibility[1]!=nil))&&
      std::ranges::all_of(resources.history_scattering,
          [](id<MTLTexture> texture){return texture!=nil;})&&
      std::ranges::all_of(resources.history_transmittance,
          [](id<MTLTexture> texture){return texture!=nil;})&&
      std::ranges::all_of(resources.history_endpoint,
          [](id<MTLTexture> texture){return texture!=nil;});
}

std::array<float,96> make_live_atmosphere_uniform(
    const tetra_viewer::AtmosphereParameters& parameters,
    tetra::Vec3 camera_relative,tetra::Vec3 planet_centre_relative,
    tetra::Vec3 camera_right,tetra::Vec3 camera_down,tetra::Vec3 camera_forward,
    tetra::Vec3 sun,double vertical_tangent,double aspect_ratio,
    double maximum_aerial_distance,float exposure,int debug_view,
    int transport,int rendering_method,int shadow_filter,
    int shadow_integrator,std::uint32_t screen_divisor,bool enabled,
    int width,int height) {
  auto uniform=make_atmosphere_smoke_uniform();
  const auto spectrum=[&](std::size_t offset,
                          const tetra_viewer::AtmosphereSpectrum& value,
                          float fourth){
    uniform[offset]=static_cast<float>(value[0]);
    uniform[offset+1U]=static_cast<float>(value[1]);
    uniform[offset+2U]=static_cast<float>(value[2]);
    uniform[offset+3U]=fourth;
  };
  spectrum(0U,parameters.rayleigh_scattering_per_metre,
           static_cast<float>(parameters.ground_radius_metres));
  spectrum(4U,parameters.mie_scattering_per_metre,static_cast<float>(
      parameters.ground_radius_metres+parameters.atmosphere_height_metres));
  spectrum(8U,parameters.mie_absorption_per_metre,
           static_cast<float>(parameters.rayleigh_scale_height_metres));
  spectrum(12U,parameters.absorption_per_metre,
           static_cast<float>(parameters.mie_scale_height_metres));
  spectrum(16U,parameters.ground_albedo,
           static_cast<float>(parameters.mie_anisotropy));
  spectrum(20U,parameters.solar_irradiance,
           static_cast<float>(parameters.absorption_peak_altitude_metres));
  uniform[24]=static_cast<float>(parameters.absorption_half_width_metres);
  uniform[25]=static_cast<float>(parameters.metres_per_world_unit);
  uniform[26]=static_cast<float>(parameters.solar_angular_radius_radians);
  uniform[27]=enabled?1.0F:0.0F;
  const auto physical=(camera_relative-planet_centre_relative)*
      parameters.metres_per_world_unit;
  const auto camera_from_centre=tetra_viewer::clamp_atmosphere_camera_to_medium(
      physical,parameters);
  const double camera_radius=std::sqrt(camera_from_centre.x*camera_from_centre.x+
      camera_from_centre.y*camera_from_centre.y+
      camera_from_centre.z*camera_from_centre.z);
  const double local_distance=tetra_viewer::atmosphere_local_aerial_distance(
      parameters,camera_radius-parameters.ground_radius_metres,
      maximum_aerial_distance);
  uniform[28]=static_cast<float>(camera_from_centre.x);
  uniform[29]=static_cast<float>(camera_from_centre.y);
  uniform[30]=static_cast<float>(camera_from_centre.z);
  uniform[31]=static_cast<float>(tetra_viewer::default_camera_near_plane*
                                 parameters.metres_per_world_unit);
  uniform[32]=static_cast<float>(camera_right.x);
  uniform[33]=static_cast<float>(camera_right.y);
  uniform[34]=static_cast<float>(camera_right.z);
  uniform[35]=static_cast<float>(vertical_tangent*aspect_ratio);
  uniform[36]=static_cast<float>(camera_down.x);
  uniform[37]=static_cast<float>(camera_down.y);
  uniform[38]=static_cast<float>(camera_down.z);
  uniform[39]=static_cast<float>(vertical_tangent);
  uniform[40]=static_cast<float>(camera_forward.x);
  uniform[41]=static_cast<float>(camera_forward.y);
  uniform[42]=static_cast<float>(camera_forward.z);
  uniform[43]=static_cast<float>(local_distance);
  uniform[44]=static_cast<float>(sun.x);
  uniform[45]=static_cast<float>(sun.y);
  uniform[46]=static_cast<float>(sun.z);
  uniform[47]=exposure;
  uniform[48]=static_cast<float>(camera_relative.x);
  uniform[49]=static_cast<float>(camera_relative.y);
  uniform[50]=static_cast<float>(camera_relative.z);
  uniform[51]=5'000.0F;
  uniform[52]=static_cast<float>(debug_view);
  uniform[53]=transport==0?0.0F:
      (transport==2?10.0F:0.0F)+1.0F+static_cast<float>(rendering_method);
  uniform[54]=static_cast<float>(shadow_filter);
  uniform[55]=static_cast<float>(shadow_integrator);
  const double inverse_radius=1.0/std::max(camera_radius,1.0e-12);
  const tetra::Vec3 local_up=camera_from_centre*inverse_radius;
  const double projection=sun.x*local_up.x+sun.y*local_up.y+sun.z*local_up.z;
  auto tangent=sun-local_up*projection;
  double tangent_length=std::sqrt(tangent.x*tangent.x+tangent.y*tangent.y+
                                  tangent.z*tangent.z);
  if(tangent_length<1.0e-5){
    const tetra::Vec3 reference=std::abs(local_up.z)<0.9?
        tetra::Vec3{0.0,0.0,1.0}:tetra::Vec3{1.0,0.0,0.0};
    tangent=reference-local_up*(reference.x*local_up.x+
        reference.y*local_up.y+reference.z*local_up.z);
    tangent_length=std::sqrt(tangent.x*tangent.x+tangent.y*tangent.y+
                             tangent.z*tangent.z);
  }
  tangent=tangent/std::max(tangent_length,1.0e-12);
  uniform[56]=static_cast<float>(local_up.x);
  uniform[57]=static_cast<float>(local_up.y);
  uniform[58]=static_cast<float>(local_up.z);
  uniform[59]=static_cast<float>(camera_radius-parameters.ground_radius_metres);
  uniform[60]=static_cast<float>(tangent.x);
  uniform[61]=static_cast<float>(tangent.y);
  uniform[62]=static_cast<float>(tangent.z);
  uniform[63]=static_cast<float>(std::sqrt(
      parameters.atmosphere_height_metres*
      (2.0*parameters.ground_radius_metres+parameters.atmosphere_height_metres)));
  uniform[84]=static_cast<float>(width);
  uniform[85]=static_cast<float>(height);
  const bool half_resolution=rendering_method==2||rendering_method==3;
  uniform[86]=static_cast<float>(half_resolution?
      (width+static_cast<int>(screen_divisor)-1)/
          static_cast<int>(screen_divisor):width);
  uniform[87]=static_cast<float>(half_resolution?
      (height+static_cast<int>(screen_divisor)-1)/
          static_cast<int>(screen_divisor):height);
  return uniform;
}

void encode_deterministic_screen_atmosphere(
    id<MTLCommandBuffer> command,MetalAtmosphereResources& resources,
    std::array<float,96> uniform,
    tetra_viewer::AtmosphereScreenHistoryIdentity current_identity,
    const ProductionShadowUniforms& shadows,id<MTLTexture> scene_depth,
    id<MTLTexture> sun_shadows,id<MTLAccelerationStructure> terrain,
    std::uint64_t terrain_generation,float terrain_ray_maximum_distance,
    bool ray_traced_visibility,
    std::uint32_t ray_query_count,bool temporal,
    id<MTLCounterSampleBuffer> timestamp_samples=nil,
    bool legacy_native_depth_scan=false,
    bool elide_reference_sky_transport=false) {
  resources.history_present_valid=false;
  const bool reference=uniform[53]>=9.5F;
  if(resources.last_visibility_backend_ray_traced!=ray_traced_visibility)
    current_identity.valid=false;
  resources.last_visibility_backend_ray_traced=ray_traced_visibility;
  if(ray_traced_visibility&&terrain_generation!=0U&&
     resources.ray_visibility_scene_generation!=terrain_generation){
    current_identity.valid=false;
    resources.ray_visibility_scene_generation=terrain_generation;
  }
  const std::uint32_t output_index=resources.history_write_index;
  const std::uint32_t previous_index=output_index^1U;
  const auto compatibility=tetra_viewer::atmosphere_screen_history_compatibility(
      resources.history_identities[previous_index],current_identity);
  if(temporal){
    ++resources.temporal_history_attempts;
    if(compatibility.compatible())++resources.temporal_history_compatible;
    else{
      ++resources.temporal_history_invalidations;
      for(std::size_t reason=0;
          reason<resources.temporal_invalidation_reasons.size();++reason)
        if((compatibility.invalidation_mask&(1U<<reason))!=0U)
          ++resources.temporal_invalidation_reasons[reason];
    }
    if(compatibility.camera_changed||compatibility.render_origin_changed)
      ++resources.temporal_camera_refreshes;
  }
  std::array<float,16> current_camera{};
  std::copy_n(uniform.begin()+28U,current_camera.size(),current_camera.begin());
  const bool history_compatible=temporal&&compatibility.compatible();
  const bool camera_changed=history_compatible&&
      (compatibility.camera_changed||compatibility.render_origin_changed);
  if(temporal){
    if(history_compatible)
      std::copy(resources.last_temporal_camera.begin(),
                resources.last_temporal_camera.end(),uniform.begin()+64U);
    else std::copy_n(uniform.begin()+28U,16U,uniform.begin()+64U);
    const std::uint32_t sample_count=history_compatible&&!camera_changed?
        std::min(resources.history_sample_count+1U,8U):1U;
    uniform[80]=history_compatible?1.0F:0.0F;
    uniform[81]=std::bit_cast<float>(static_cast<std::uint32_t>(
        resources.history_identities[previous_index].result_generation));
    uniform[82]=static_cast<float>(sample_count-1U)/sample_count;
  }
  const std::array<std::uint32_t,4> endpoint_control{
      12U,static_cast<std::uint32_t>(current_identity.result_generation),
      resources.screen_divisor,0U};
  id<MTLComputeCommandEncoder> endpoint=timestamped_compute_encoder(
      command,timestamp_samples,7U,8U);
  [endpoint setComputePipelineState:reference?resources.reference_pipeline:
                                             resources.pipelines[12]];
  [endpoint setBytes:uniform.data() length:uniform.size()*sizeof(float)
                atIndex:0];
  if(reference){
    [endpoint setBytes:&shadows length:sizeof(shadows) atIndex:1];
    [endpoint setBytes:endpoint_control.data() length:sizeof(endpoint_control)
                  atIndex:2];
    [endpoint setTexture:resources.transmittance atIndex:0];
    [endpoint setTexture:resources.multiple_scattering atIndex:1];
    [endpoint setTexture:sun_shadows atIndex:2];
    [endpoint setTexture:resources.sky_view atIndex:3];
    [endpoint setTexture:scene_depth atIndex:4];
    [endpoint setTexture:resources.screen_endpoint atIndex:5];
    [endpoint setTexture:resources.screen_scattering atIndex:6];
    [endpoint setTexture:resources.screen_transmittance atIndex:7];
    [endpoint setTexture:resources.froxel_scattering atIndex:8];
    [endpoint setTexture:resources.froxel_transmittance atIndex:9];
    [endpoint setSamplerState:resources.sampler atIndex:0];
    [endpoint setSamplerState:resources.sampler atIndex:1];
  }else{
    [endpoint setBytes:endpoint_control.data() length:sizeof(endpoint_control)
                  atIndex:1];
    [endpoint setTexture:scene_depth atIndex:0];
    [endpoint setTexture:resources.screen_endpoint atIndex:1];
    [endpoint setSamplerState:resources.sampler atIndex:0];
  }
  [endpoint dispatchThreads:MTLSizeMake(resources.screen_width,
                                        resources.screen_height,1U)
      threadsPerThreadgroup:MTLSizeMake(8U,8U,1U)];
  [endpoint endEncoding];
  ++resources.dispatch_counts[12];

  const bool use_ray_traced_visibility=!reference&&ray_traced_visibility&&
      terrain!=nil&&resources.terrain_ray_visibility!=nil;
  ray_query_count=std::clamp(ray_query_count,1U,32U);
  // Never initialise or invalidate the temporal field with fabricated
  // sunlight. A full physical refresh is required once; steady frames then
  // follow the two-/one-ray desktop/iOS rotating schedule.
  if(use_ray_traced_visibility&&
     (!temporal||!history_compatible||camera_changed))
    ray_query_count=32U;
  const std::uint32_t visibility_phase=ray_traced_visibility?
      ((resources.history_sequence+1U)&(32U/ray_query_count-1U)):
      (((resources.history_sequence/2U)+1U)&15U);
  const std::uint32_t visibility_control=temporal?
      visibility_phase|(previous_index<<5U)|
      (output_index<<6U)|(history_compatible?128U:0U)|256U|
      (camera_changed?512U:0U):0U;
  const std::array<std::uint32_t,4> integration_control{
      13U,resources.screen_divisor,
      use_ray_traced_visibility?ray_query_count:
      tetra_viewer::atmosphere_visibility_refresh_intervals(
          temporal,compatibility),
      visibility_control|(legacy_native_depth_scan?1024U:0U)|
      (elide_reference_sky_transport?2048U:0U)|
      (atmosphere_legacy_planet_umbra_work?4096U:0U)};
  // Endpoint reconstruction establishes exactly the same camera ray and
  // maximum distance that the integration pass will use.  Query each of its
  // 32 radial sample positions directly against terrain before consuming it.
  uniform[83]=use_ray_traced_visibility?1.0F:0.0F;
  if(use_ray_traced_visibility){
    const std::array<float,28> ray_uniform{
        uniform[48],uniform[49],uniform[50],uniform[25],
        uniform[28],uniform[29],uniform[30],uniform[3],
        uniform[32],uniform[33],uniform[34],uniform[35],
        uniform[36],uniform[37],uniform[38],uniform[39],
        uniform[40],uniform[41],uniform[42],uniform[7],
        uniform[44],uniform[45],uniform[46],terrain_ray_maximum_distance,
        uniform[15],static_cast<float>(ray_query_count),
        static_cast<float>(visibility_phase),uniform[26]};
    id<MTLComputeCommandEncoder> rays=[command computeCommandEncoder];
    [rays setComputePipelineState:resources.ray_visibility_pipeline];
    [rays setBytes:ray_uniform.data() length:ray_uniform.size()*sizeof(float)
           atIndex:0];
    [rays setTexture:resources.screen_endpoint atIndex:0];
    [rays setTexture:resources.terrain_ray_visibility atIndex:1];
    [rays setAccelerationStructure:terrain atBufferIndex:1];
    [rays dispatchThreads:MTLSizeMake(resources.screen_width,
                                      resources.screen_height,ray_query_count)
        threadsPerThreadgroup:MTLSizeMake(4U,4U,4U)];
    [rays endEncoding];
    ++resources.ray_visibility_dispatches;
    resources.last_ray_visibility_query_count=ray_query_count;
  }
  id<MTLComputeCommandEncoder> integration=timestamped_compute_encoder(
      command,timestamp_samples,9U,10U);
  [integration setComputePipelineState:reference?resources.reference_pipeline:
                                                resources.pipelines[13]];
  [integration setBytes:uniform.data() length:uniform.size()*sizeof(float)
                   atIndex:0];
  [integration setBytes:&shadows length:sizeof(shadows) atIndex:1];
  [integration setBytes:integration_control.data()
                   length:sizeof(integration_control) atIndex:2];
  [integration setTexture:resources.transmittance atIndex:0];
  [integration setTexture:resources.multiple_scattering atIndex:1];
  [integration setTexture:sun_shadows atIndex:2];
  if(reference){
    [integration setTexture:resources.sky_view atIndex:3];
    [integration setTexture:scene_depth atIndex:4];
    [integration setTexture:resources.screen_endpoint atIndex:5];
    [integration setTexture:resources.screen_scattering atIndex:6];
    [integration setTexture:resources.screen_transmittance atIndex:7];
    [integration setTexture:resources.froxel_scattering atIndex:8];
    [integration setTexture:resources.froxel_transmittance atIndex:9];
  }else{
    [integration setTexture:resources.history_visibility[0] atIndex:3];
    [integration setTexture:resources.history_visibility[1] atIndex:4];
    [integration setTexture:resources.terrain_ray_visibility atIndex:5];
    [integration setTexture:resources.screen_endpoint atIndex:6];
    [integration setTexture:scene_depth atIndex:7];
    [integration setTexture:resources.screen_scattering atIndex:8];
    [integration setTexture:resources.screen_transmittance atIndex:9];
  }
  [integration setSamplerState:resources.sampler atIndex:0];
  [integration setSamplerState:resources.sampler atIndex:1];
  [integration dispatchThreads:MTLSizeMake(resources.screen_width,
                                           resources.screen_height,1U)
      threadsPerThreadgroup:MTLSizeMake(8U,8U,1U)];
  [integration endEncoding];
  ++resources.dispatch_counts[13];
  if(!temporal)return;

  const std::array<std::uint32_t,4> temporal_control{
      14U,previous_index,output_index,
      elide_reference_sky_transport?2048U:0U};
  id<MTLComputeCommandEncoder> accumulate=timestamped_compute_encoder(
      command,timestamp_samples,11U,12U);
  [accumulate setComputePipelineState:resources.pipelines[14]];
  [accumulate setBytes:uniform.data() length:uniform.size()*sizeof(float)
                  atIndex:0];
  [accumulate setBytes:temporal_control.data() length:sizeof(temporal_control)
                  atIndex:1];
  [accumulate setTexture:resources.history_scattering[0] atIndex:0];
  [accumulate setTexture:resources.history_scattering[1] atIndex:1];
  [accumulate setTexture:resources.history_transmittance[0] atIndex:2];
  [accumulate setTexture:resources.history_transmittance[1] atIndex:3];
  [accumulate setTexture:resources.history_endpoint[0] atIndex:4];
  [accumulate setTexture:resources.history_endpoint[1] atIndex:5];
  [accumulate setTexture:resources.screen_endpoint atIndex:6];
  [accumulate setTexture:resources.screen_scattering atIndex:7];
  [accumulate setTexture:resources.screen_transmittance atIndex:8];
  [accumulate dispatchThreads:MTLSizeMake(resources.screen_width,
                                          resources.screen_height,1U)
      threadsPerThreadgroup:MTLSizeMake(8U,8U,1U)];
  [accumulate endEncoding];
  ++resources.dispatch_counts[14];

  // The accumulation output is already the generation the composite needs.
  // Bind it directly instead of copying two full screen textures solely for
  // presentation.
  resources.history_present_index=output_index;
  resources.history_present_valid=true;
  resources.last_temporal_camera=current_camera;
  resources.history_sample_count=history_compatible&&!camera_changed?
      std::min(resources.history_sample_count+1U,8U):1U;
  current_identity.valid=true;
  resources.history_identities[output_index]=current_identity;
  resources.history_valid=true;
  ++resources.history_sequence;
  resources.history_write_index=previous_index;
}

void encode_shadowed_froxel_atmosphere(
    id<MTLDevice> device,id<MTLCommandBuffer> command,
    MetalAtmosphereResources& resources,
    const std::array<float,96>& uniform,
    const ProductionShadowUniforms& shadows,id<MTLTexture> sun_shadows,
    id<MTLTexture> scene_depth) {
  if(!ensure_shadowed_froxel_resources(device,resources))return;
  const bool reference=uniform[53]>=9.5F;
  id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
  [encoder setComputePipelineState:reference?resources.reference_pipeline:
                                            resources.pipelines[16]];
  [encoder setBytes:uniform.data() length:uniform.size()*sizeof(float)
               atIndex:0];
  [encoder setBytes:&shadows length:sizeof(shadows) atIndex:1];
  const std::array<std::uint32_t,4> control{16U,0U,0U,0U};
  if(reference)[encoder setBytes:control.data() length:sizeof(control) atIndex:2];
  [encoder setTexture:resources.transmittance atIndex:0];
  [encoder setTexture:resources.multiple_scattering atIndex:1];
  [encoder setTexture:sun_shadows atIndex:2];
  if(reference){
    [encoder setTexture:resources.sky_view atIndex:3];
    [encoder setTexture:scene_depth atIndex:4];
    [encoder setTexture:resources.screen_endpoint atIndex:5];
    [encoder setTexture:resources.screen_scattering atIndex:6];
    [encoder setTexture:resources.screen_transmittance atIndex:7];
    [encoder setTexture:resources.froxel_scattering atIndex:8];
    [encoder setTexture:resources.froxel_transmittance atIndex:9];
    [encoder setSamplerState:resources.sampler atIndex:1];
  }else{
    [encoder setTexture:resources.history_visibility[0] atIndex:3];
    [encoder setTexture:resources.history_visibility[1] atIndex:4];
    [encoder setTexture:resources.froxel_scattering atIndex:5];
    [encoder setTexture:resources.froxel_transmittance atIndex:6];
  }
  [encoder setSamplerState:resources.sampler atIndex:0];
  [encoder dispatchThreads:MTLSizeMake(32U,32U,32U)
      threadsPerThreadgroup:MTLSizeMake(8U,8U,1U)];
  [encoder endEncoding];
  ++resources.dispatch_counts[16];
}

void encode_long_shadow_atmosphere(
    id<MTLDevice> device,id<MTLCommandBuffer> command,
    MetalAtmosphereResources& resources,
    const std::array<float,96>& uniform,
    const ProductionShadowUniforms& shadows,id<MTLTexture> sun_shadows,
    std::uint64_t scene_generation) {
  if(!ensure_long_shadow_resources(device,resources)||
     !ensure_shadow_minmax_resources(device,resources))return;
  const bool unchanged=resources.long_shadow_ready&&
      resources.last_long_scene_generation==scene_generation&&
      std::equal(resources.last_long_uniform.begin(),
                 resources.last_long_uniform.end(),uniform.begin())&&
      resources.last_long_shadows.matrices==shadows.matrices&&
      resources.last_long_shadows.splits==shadows.splits&&
      resources.last_long_shadows.atmosphere_metadata==
          shadows.atmosphere_metadata;
  if(unchanged)return;
  const std::array<std::uint32_t,4> control{6U,0xffffffffU,0U,0U};
  id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
  [encoder setComputePipelineState:resources.pipelines[6]];
  [encoder setBytes:uniform.data() length:uniform.size()*sizeof(float)
               atIndex:0];
  [encoder setBytes:&shadows length:sizeof(shadows) atIndex:1];
  [encoder setBuffer:resources.minmax offset:0 atIndex:2];
  [encoder setBytes:control.data() length:sizeof(control) atIndex:3];
  [encoder setTexture:resources.transmittance atIndex:0];
  [encoder setTexture:sun_shadows atIndex:1];
  [encoder setTexture:resources.long_shadow atIndex:2];
  [encoder setSamplerState:resources.sampler atIndex:0];
  [encoder dispatchThreads:MTLSizeMake(resources.long_shadow.width,
                                        resources.long_shadow.height,1U)
      threadsPerThreadgroup:MTLSizeMake(8U,8U,1U)];
  [encoder endEncoding];
  ++resources.dispatch_counts[6];
  std::copy_n(uniform.begin(),resources.last_long_uniform.size(),
              resources.last_long_uniform.begin());
  resources.last_long_shadows=shadows;
  resources.last_long_scene_generation=scene_generation;
  resources.long_shadow_ready=true;
}

void encode_shadow_minmax_hierarchy(
    id<MTLDevice> device,id<MTLCommandBuffer> command,
    MetalAtmosphereResources& resources,
    id<MTLTexture> sun_shadows,std::uint64_t scene_generation) {
  if(!ensure_shadow_minmax_resources(device,resources))return;
  if(resources.minmax_scene_generation==scene_generation&&
     resources.minmax_kind==1)return;
  std::uint32_t level_size=resources.atmosphere_shadow_resolution;
  std::uint32_t level{};
  while(true){
    const std::array<std::uint32_t,4> control{
        8U,level,resources.atmosphere_shadow_resolution,4U};
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:resources.pipelines[8]];
    [encoder setBuffer:resources.minmax offset:0 atIndex:0];
    [encoder setBytes:control.data() length:sizeof(control) atIndex:1];
    [encoder setTexture:sun_shadows atIndex:0];
    [encoder setSamplerState:resources.sampler atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(level_size,level_size,1U)
        threadsPerThreadgroup:MTLSizeMake(8U,8U,1U)];
    [encoder endEncoding];
    ++resources.dispatch_counts[8];
    if(level_size==1U)break;
    level_size=(level_size+1U)/2U;
    ++level;
  }
  resources.minmax_scene_generation=scene_generation;
  resources.minmax_kind=1;
  resources.long_shadow_ready=false;
}

void encode_shadow_epipolar_hierarchy(
    id<MTLDevice> device,id<MTLCommandBuffer> command,
    MetalAtmosphereResources& resources,
    const std::array<float,96>& uniform,
    const ProductionShadowUniforms& shadows,id<MTLTexture> sun_shadows,
    std::uint64_t scene_generation) {
  if(!ensure_shadow_minmax_resources(device,resources))return;
  if(resources.minmax_scene_generation==scene_generation&&
     resources.minmax_kind==2)return;
  const auto layout=tetra_viewer::atmosphere_epipolar_layout(
      resources.atmosphere_shadow_resolution);
  const std::array<std::uint32_t,4> base_control{
      9U,resources.atmosphere_shadow_resolution,
      static_cast<std::uint32_t>(layout.radial_resolution),
      static_cast<std::uint32_t>(layout.angular_rows)};
  id<MTLComputeCommandEncoder> base=[command computeCommandEncoder];
  [base setComputePipelineState:resources.pipelines[9]];
  [base setBytes:uniform.data() length:uniform.size()*sizeof(float) atIndex:0];
  [base setBytes:&shadows length:sizeof(shadows) atIndex:1];
  [base setBytes:base_control.data() length:sizeof(base_control) atIndex:2];
  [base setBuffer:resources.minmax offset:0 atIndex:3];
  [base setTexture:sun_shadows atIndex:0];
  [base setSamplerState:resources.sampler atIndex:0];
  [base dispatchThreads:MTLSizeMake(layout.radial_resolution,
                                    layout.angular_rows,1U)
      threadsPerThreadgroup:MTLSizeMake(8U,8U,1U)];
  [base endEncoding];
  ++resources.dispatch_counts[9];
  std::uint32_t level=1U;
  std::uint32_t width=static_cast<std::uint32_t>(
      (layout.radial_resolution+1U)/2U);
  while(true){
    const std::array<std::uint32_t,4> control{
        10U,level,static_cast<std::uint32_t>(layout.radial_resolution),
        static_cast<std::uint32_t>(layout.angular_rows)};
    id<MTLComputeCommandEncoder> mip=[command computeCommandEncoder];
    [mip setComputePipelineState:resources.pipelines[10]];
    [mip setBuffer:resources.minmax offset:0 atIndex:0];
    [mip setBytes:control.data() length:sizeof(control) atIndex:1];
    [mip dispatchThreads:MTLSizeMake(width,layout.angular_rows,1U)
        threadsPerThreadgroup:MTLSizeMake(8U,8U,1U)];
    [mip endEncoding];
    ++resources.dispatch_counts[10];
    if(width==1U)break;
    width=(width+1U)/2U;
    ++level;
  }
  id<MTLComputeCommandEncoder> reset=[command computeCommandEncoder];
  [reset setComputePipelineState:resources.pipelines[11]];
  [reset setBytes:&shadows length:sizeof(shadows) atIndex:0];
  [reset setBuffer:resources.minmax offset:0 atIndex:1];
  [reset dispatchThreads:MTLSizeMake(1U,1U,1U)
       threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
  [reset endEncoding];
  ++resources.dispatch_counts[11];
  resources.minmax_scene_generation=scene_generation;
  resources.minmax_kind=2;
  resources.long_shadow_ready=false;
}

bool encode_live_atmosphere_lookups(
    id<MTLDevice> device,id<MTLCommandBuffer> command,
    MetalAtmosphereResources& resources,
    const std::array<float,96>& uniform,bool rebuild_optical,
    bool aerial_consumed=true,id<MTLCounterSampleBuffer> timestamp_samples=nil) {
  const bool reference=uniform[53]>=9.5F;
  if(!resources.dummy_shadow_cleared){
    for(NSUInteger slice=0;slice<5U;++slice){
      MTLRenderPassDescriptor* clear=[MTLRenderPassDescriptor renderPassDescriptor];
      clear.depthAttachment.texture=resources.dummy_shadow;
      clear.depthAttachment.slice=slice;
      clear.depthAttachment.loadAction=MTLLoadActionClear;
      clear.depthAttachment.storeAction=MTLStoreActionStore;
      clear.depthAttachment.clearDepth=1.0;
      id<MTLRenderCommandEncoder> encoder=
          [command renderCommandEncoderWithDescriptor:clear];
      [encoder endEncoding];
    }
    resources.dummy_shadow_cleared=true;
  }
  const auto dispatch=[&](std::size_t mode,id<MTLTexture> output,
                          NSUInteger start=MTLCounterDontSample,
                          NSUInteger end=MTLCounterDontSample){
    id<MTLComputeCommandEncoder> encoder=timestamped_compute_encoder(
        command,timestamp_samples,start,end);
    [encoder setComputePipelineState:reference&&mode==2U?
        resources.reference_pipeline:resources.pipelines[mode]];
    [encoder setBytes:uniform.data() length:uniform.size()*sizeof(float)
                  atIndex:0];
    if(mode==0U)[encoder setTexture:resources.transmittance atIndex:0];
    if(mode==1U){
      [encoder setTexture:resources.transmittance atIndex:0];
      [encoder setTexture:resources.multiple_scattering atIndex:1];
    }
    if(mode==2U){
      [encoder setBuffer:resources.shadow_uniform offset:0 atIndex:1];
      if(reference){
        const std::array<std::uint32_t,4> control{2U,0U,0U,0U};
        [encoder setBytes:control.data() length:sizeof(control) atIndex:2];
        [encoder setTexture:resources.transmittance atIndex:0];
        [encoder setTexture:resources.multiple_scattering atIndex:1];
        [encoder setTexture:resources.dummy_shadow atIndex:2];
        [encoder setTexture:resources.sky_view atIndex:3];
        for(NSUInteger index=4U;index<=7U;++index)
          [encoder setTexture:resources.dummy_screen atIndex:index];
        [encoder setTexture:resources.aerial_scattering atIndex:8];
        [encoder setTexture:resources.aerial_transmittance atIndex:9];
        [encoder setSamplerState:resources.sampler atIndex:0];
        [encoder setSamplerState:resources.sampler atIndex:1];
      }else{
        [encoder setBuffer:resources.minmax offset:0 atIndex:2];
        [encoder setTexture:resources.transmittance atIndex:0];
        [encoder setTexture:resources.multiple_scattering atIndex:1];
        [encoder setTexture:resources.dummy_shadow atIndex:2];
        [encoder setTexture:resources.sky_view atIndex:3];
        [encoder setSamplerState:resources.sampler atIndex:0];
      }
    }
    if(mode==3U){
      [encoder setTexture:resources.transmittance atIndex:0];
      [encoder setTexture:resources.multiple_scattering atIndex:1];
      [encoder setTexture:resources.aerial_scattering atIndex:2];
      [encoder setTexture:resources.aerial_transmittance atIndex:3];
    }
    if(mode==4U){
      [encoder setTexture:resources.sky_view atIndex:0];
      [encoder setTexture:resources.sky_irradiance atIndex:1];
    }
    [encoder dispatchThreads:MTLSizeMake(output.width,output.height,output.depth)
        threadsPerThreadgroup:MTLSizeMake(8U,8U,1U)];
    [encoder endEncoding];
    ++resources.dispatch_counts[mode];
  };
  if(rebuild_optical||!resources.optical_ready){
    // The two optical LUTs are one dependency-ordered lookup family. Sample
    // their complete rebuild interval without perturbing their scheduling.
    dispatch(0U,resources.transmittance,21U,MTLCounterDontSample);
    dispatch(1U,resources.multiple_scattering,MTLCounterDontSample,22U);
    resources.optical_ready=true;
    resources.history_valid=false;
    // The timestamp consumer must distinguish this frame's new counter values
    // from retained results in a reused sample buffer.
    const bool optical_rebuilt=true;
    const bool view_changed=rebuild_optical||!resources.view_ready||
        !std::equal(resources.last_view_uniform.begin(),
                    resources.last_view_uniform.end(),uniform.begin());
    if(view_changed){
      // The reference transport needs the fitted terrain shadow, which is not
      // available until later in the frame. Its sky and irradiance passes are
      // encoded by encode_reference_sky_lookup with the real shadow state.
      if(!reference){
        dispatch(2U,resources.sky_view);
        dispatch(4U,resources.sky_irradiance);
      }
      if(aerial_consumed&&ensure_aerial_resources(device,resources))
        dispatch(3U,resources.aerial_scattering,23U,24U);
      std::copy_n(uniform.begin(),resources.last_view_uniform.size(),
                  resources.last_view_uniform.begin());
      resources.view_ready=true;
    }
    return optical_rebuilt;
  }
  const bool view_changed=rebuild_optical||!resources.view_ready||
      !std::equal(resources.last_view_uniform.begin(),
                  resources.last_view_uniform.end(),uniform.begin());
  if(view_changed){
    // The reference transport needs the fitted terrain shadow, which is not
    // available until later in the frame. Its sky and irradiance passes are
    // encoded by encode_reference_sky_lookup with the real shadow state.
    if(!reference){
      dispatch(2U,resources.sky_view);
      dispatch(4U,resources.sky_irradiance);
    }
    if(aerial_consumed&&ensure_aerial_resources(device,resources))
      dispatch(3U,resources.aerial_scattering,23U,24U);
    std::copy_n(uniform.begin(),resources.last_view_uniform.size(),
                resources.last_view_uniform.begin());
    resources.view_ready=true;
  }
  return false;
}

bool encode_reference_sky_lookup(
    id<MTLCommandBuffer> command,MetalAtmosphereResources& resources,
    const std::array<float,96>& uniform,
    const ProductionShadowUniforms& shadows,id<MTLTexture> sun_shadows,
    std::uint64_t terrain_generation,
    id<MTLCounterSampleBuffer> timestamp_samples) {
  ++resources.reference_lookup_attempts;
  const bool unchanged=resources.reference_lookup_ready&&
      resources.last_reference_lookup_uniform==uniform&&
      std::memcmp(&resources.last_reference_lookup_shadows,&shadows,
                  sizeof(shadows))==0&&
      resources.last_reference_lookup_generation==terrain_generation;
  if(unchanged){
    ++resources.reference_lookup_skips;
    return false;
  }
  const std::array<std::uint32_t,4> control{2U,0U,0U,0U};
  id<MTLComputeCommandEncoder> sky=timestamped_compute_encoder(
      command,timestamp_samples,17U,18U);
  [sky setComputePipelineState:resources.reference_pipeline];
  [sky setBytes:uniform.data() length:uniform.size()*sizeof(float) atIndex:0];
  [sky setBytes:&shadows length:sizeof(shadows) atIndex:1];
  [sky setBytes:control.data() length:sizeof(control) atIndex:2];
  [sky setTexture:resources.transmittance atIndex:0];
  [sky setTexture:resources.multiple_scattering atIndex:1];
  [sky setTexture:sun_shadows atIndex:2];
  [sky setTexture:resources.sky_view atIndex:3];
  [sky setTexture:resources.dummy_screen atIndex:4];
  [sky setTexture:resources.dummy_screen atIndex:5];
  [sky setTexture:resources.dummy_screen atIndex:6];
  [sky setTexture:resources.dummy_screen atIndex:7];
  [sky setTexture:resources.aerial_scattering atIndex:8];
  [sky setTexture:resources.aerial_transmittance atIndex:9];
  [sky setSamplerState:resources.sampler atIndex:0];
  [sky setSamplerState:resources.sampler atIndex:1];
  [sky dispatchThreads:MTLSizeMake(resources.sky_view.width,
                                   resources.sky_view.height,1U)
      threadsPerThreadgroup:MTLSizeMake(8U,8U,1U)];
  [sky endEncoding];
  ++resources.dispatch_counts[2];
  id<MTLComputeCommandEncoder> irradiance=timestamped_compute_encoder(
      command,timestamp_samples,19U,20U);
  [irradiance setComputePipelineState:resources.pipelines[4]];
  [irradiance setBytes:uniform.data() length:uniform.size()*sizeof(float)
                 atIndex:0];
  [irradiance setTexture:resources.sky_view atIndex:0];
  [irradiance setTexture:resources.sky_irradiance atIndex:1];
  [irradiance dispatchThreads:MTLSizeMake(resources.sky_irradiance.width,
                                          resources.sky_irradiance.height,1U)
      threadsPerThreadgroup:MTLSizeMake(8U,8U,1U)];
  [irradiance endEncoding];
  ++resources.dispatch_counts[4];
  resources.last_reference_lookup_uniform=uniform;
  resources.last_reference_lookup_shadows=shadows;
  resources.last_reference_lookup_generation=terrain_generation;
  resources.reference_lookup_ready=true;
  return true;
}

MTLVertexDescriptor* make_scene_vertex_descriptor() {
  MTLVertexDescriptor* vertices=[MTLVertexDescriptor vertexDescriptor];
  vertices.attributes[0].format=MTLVertexFormatFloat3;
  vertices.attributes[0].offset=offsetof(tetra_viewer::SceneVertex,position);
  vertices.attributes[0].bufferIndex=0;
  vertices.attributes[1].format=MTLVertexFormatFloat3;
  vertices.attributes[1].offset=offsetof(tetra_viewer::SceneVertex,colour);
  vertices.attributes[1].bufferIndex=0;
  vertices.attributes[2].format=MTLVertexFormatFloat3;
  vertices.attributes[2].offset=offsetof(tetra_viewer::SceneVertex,normal);
  vertices.attributes[2].bufferIndex=0;
  vertices.attributes[3].format=MTLVertexFormatFloat3;
  vertices.attributes[3].offset=offsetof(tetra_viewer::SceneVertex,smooth_normal);
  vertices.attributes[3].bufferIndex=0;
  vertices.attributes[4].format=MTLVertexFormatFloat3;
  vertices.attributes[4].offset=offsetof(tetra_viewer::SceneVertex,barycentric);
  vertices.attributes[4].bufferIndex=0;
  vertices.attributes[5].format=MTLVertexFormatFloat;
  vertices.attributes[5].offset=offsetof(tetra_viewer::SceneVertex,edge_flags);
  vertices.attributes[5].bufferIndex=0;
  vertices.layouts[0].stride=sizeof(tetra_viewer::SceneVertex);
  vertices.layouts[0].stepFunction=MTLVertexStepFunctionPerVertex;
  return vertices;
}

id<MTLRenderPipelineState> make_scene_pipeline(
    id<MTLDevice> device,id<MTLLibrary> library,MTLPixelFormat colour_format,
    MTLPixelFormat depth_format,NSUInteger sample_count) {
  MTLRenderPipelineDescriptor* descriptor=[MTLRenderPipelineDescriptor new];
  descriptor.label=@"TetWorld terrain";
  descriptor.vertexFunction=[library newFunctionWithName:@"scene_vertex"];
  descriptor.fragmentFunction=[library newFunctionWithName:@"scene_fragment"];
  descriptor.vertexDescriptor=make_scene_vertex_descriptor();
  descriptor.colorAttachments[0].pixelFormat=colour_format;
  descriptor.depthAttachmentPixelFormat=depth_format;
  descriptor.rasterSampleCount=sample_count;
  NSError* error=nil;
  id<MTLRenderPipelineState> pipeline=
      [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  if(pipeline==nil)
    std::fprintf(stderr,"Metal pipeline creation failed: %s\n",
                 error.localizedDescription.UTF8String);
  return pipeline;
}

id<MTLRenderPipelineState> make_overlay_pipeline(
    id<MTLDevice> device,id<MTLLibrary> library,MTLPixelFormat colour_format,
    MTLPixelFormat depth_format,NSUInteger sample_count) {
  MTLRenderPipelineDescriptor* descriptor=[MTLRenderPipelineDescriptor new];
  descriptor.label=@"TetWorld overlays";
  descriptor.vertexFunction=[library newFunctionWithName:@"scene_vertex"];
  descriptor.fragmentFunction=[library newFunctionWithName:@"overlay_fragment"];
  descriptor.vertexDescriptor=make_scene_vertex_descriptor();
  descriptor.colorAttachments[0].pixelFormat=colour_format;
  descriptor.depthAttachmentPixelFormat=depth_format;
  descriptor.rasterSampleCount=sample_count;
  NSError* error=nil;
  id<MTLRenderPipelineState> pipeline=
      [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  if(pipeline==nil)
    std::fprintf(stderr,"Metal overlay pipeline creation failed: %s\n",
                 error.localizedDescription.UTF8String);
  return pipeline;
}

id<MTLRenderPipelineState> make_shadow_pipeline(
    id<MTLDevice> device,id<MTLLibrary> library,MTLPixelFormat depth_format) {
  MTLRenderPipelineDescriptor* descriptor=[MTLRenderPipelineDescriptor new];
  descriptor.label=@"TetWorld shadow depth";
  descriptor.vertexFunction=[library newFunctionWithName:@"shadow_vertex"];
  descriptor.vertexDescriptor=make_scene_vertex_descriptor();
  descriptor.depthAttachmentPixelFormat=depth_format;
  NSError* error=nil;
  id<MTLRenderPipelineState> pipeline=
      [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  if(pipeline==nil)
    std::fprintf(stderr,"Metal shadow pipeline creation failed: %s\n",
                 error.localizedDescription.UTF8String);
  return pipeline;
}

id<MTLRenderPipelineState> make_composite_pipeline(
    id<MTLDevice> device,id<MTLLibrary> library,MTLPixelFormat colour_format) {
  MTLRenderPipelineDescriptor* descriptor=[MTLRenderPipelineDescriptor new];
  descriptor.label=@"TetWorld composite";
  descriptor.vertexFunction=[library newFunctionWithName:@"composite_vertex"];
  descriptor.fragmentFunction=[library newFunctionWithName:@"composite_fragment"];
  descriptor.colorAttachments[0].pixelFormat=colour_format;
  NSError* error=nil;
  id<MTLRenderPipelineState> pipeline=
      [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  if(pipeline==nil)
    std::fprintf(stderr,"Metal composite pipeline creation failed: %s\n",
                 error.localizedDescription.UTF8String);
  return pipeline;
}

id<MTLTexture> make_render_texture(id<MTLDevice> device,int width,int height,
                                   MTLPixelFormat format,NSUInteger samples,
                                   MTLTextureUsage usage);

id<MTLRenderPipelineState> make_temporal_motion_pipeline(
    id<MTLDevice> device,id<MTLLibrary> library) {
  MTLRenderPipelineDescriptor* descriptor=[MTLRenderPipelineDescriptor new];
  descriptor.label=@"TetWorld MetalFX motion and reactive mask";
  descriptor.vertexFunction=[library newFunctionWithName:@"temporal_vertex"];
  descriptor.fragmentFunction=
      [library newFunctionWithName:@"temporal_motion_fragment"];
  descriptor.colorAttachments[0].pixelFormat=MTLPixelFormatRG16Float;
  descriptor.colorAttachments[1].pixelFormat=MTLPixelFormatR8Unorm;
  NSError* error=nil;
  id<MTLRenderPipelineState> pipeline=
      [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  if(pipeline==nil)
    std::fprintf(stderr,"MetalFX motion pipeline creation failed: %s\n",
                 error.localizedDescription.UTF8String);
  return pipeline;
}

id<MTLRenderPipelineState> make_temporal_present_pipeline(
    id<MTLDevice> device,id<MTLLibrary> library,MTLPixelFormat format) {
  MTLRenderPipelineDescriptor* descriptor=[MTLRenderPipelineDescriptor new];
  descriptor.label=@"TetWorld MetalFX presentation";
  descriptor.vertexFunction=[library newFunctionWithName:@"temporal_vertex"];
  descriptor.fragmentFunction=
      [library newFunctionWithName:@"temporal_present_fragment"];
  descriptor.colorAttachments[0].pixelFormat=format;
  NSError* error=nil;
  id<MTLRenderPipelineState> pipeline=
      [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  if(pipeline==nil)
    std::fprintf(stderr,"MetalFX present pipeline creation failed: %s\n",
                 error.localizedDescription.UTF8String);
  return pipeline;
}

bool ensure_metal_fx_temporal_resources(
    id<MTLDevice> device,MetalFxTemporalResources& resources,
    int input_width,int input_height,int output_width,int output_height,
    bool direct_output) {
  if(resources.scaler!=nil&&resources.input_width==input_width&&
     resources.input_height==input_height&&
     resources.output_width==output_width&&
     resources.output_height==output_height&&
     resources.direct_output==direct_output)
    return true;
  resources=MetalFxTemporalResources{};
  if(input_width<=0||input_height<=0||output_width<=0||output_height<=0||
     input_width>=output_width||input_height>=output_height){
    resources.failure="MetalFX requires a smaller input than output";
    return false;
  }
  if(![MTLFXTemporalScalerDescriptor supportsDevice:device]){
    resources.failure="MetalFX temporal scaling is unsupported by this GPU";
    return false;
  }
  MTLFXTemporalScalerDescriptor* descriptor=
      [MTLFXTemporalScalerDescriptor new];
  descriptor.colorTextureFormat=MTLPixelFormatBGRA8Unorm;
  descriptor.depthTextureFormat=MTLPixelFormatDepth32Float;
  descriptor.motionTextureFormat=MTLPixelFormatRG16Float;
  descriptor.outputTextureFormat=MTLPixelFormatBGRA8Unorm;
  descriptor.inputWidth=static_cast<NSUInteger>(input_width);
  descriptor.inputHeight=static_cast<NSUInteger>(input_height);
  descriptor.outputWidth=static_cast<NSUInteger>(output_width);
  descriptor.outputHeight=static_cast<NSUInteger>(output_height);
  descriptor.autoExposureEnabled=NO;
  descriptor.requiresSynchronousInitialization=NO;
  if(@available(macOS 14.4,*)){
    descriptor.reactiveMaskTextureEnabled=YES;
    descriptor.reactiveMaskTextureFormat=MTLPixelFormatR8Unorm;
  }
  resources.scaler=[descriptor newTemporalScalerWithDevice:device];
  if(resources.scaler==nil){
    resources.failure="MetalFX rejected the temporal scaler configuration";
    return false;
  }
  const auto texture=[&](MTLPixelFormat format,int width,int height,
                         MTLTextureUsage usage){
    return make_render_texture(device,width,height,format,1U,usage);
  };
  resources.input_colour=texture(MTLPixelFormatBGRA8Unorm,input_width,
      input_height,MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|
          resources.scaler.colorTextureUsage);
  resources.motion=texture(MTLPixelFormatRG16Float,input_width,input_height,
      MTLTextureUsageRenderTarget|resources.scaler.motionTextureUsage);
  resources.reactive=texture(MTLPixelFormatR8Unorm,input_width,input_height,
      MTLTextureUsageRenderTarget|resources.scaler.reactiveTextureUsage);
  if(!direct_output)
    resources.output_colour=texture(MTLPixelFormatBGRA8Unorm,output_width,
        output_height,MTLTextureUsageShaderRead|
            resources.scaler.outputTextureUsage);
  MTLTextureDescriptor* exposure_descriptor=[MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Float
                                  width:1U height:1U mipmapped:NO];
  exposure_descriptor.storageMode=MTLStorageModeShared;
  exposure_descriptor.usage=MTLTextureUsageShaderRead;
  resources.exposure=[device newTextureWithDescriptor:exposure_descriptor];
  const std::uint16_t half_one=0x3c00U;
  [resources.exposure replaceRegion:MTLRegionMake2D(0U,0U,1U,1U)
                        mipmapLevel:0U withBytes:&half_one bytesPerRow:2U];
  if(resources.input_colour==nil||resources.motion==nil||
     resources.reactive==nil||(!direct_output&&resources.output_colour==nil)||
     resources.exposure==nil){
    resources.failure="MetalFX temporal texture allocation failed";
    resources.scaler=nil;
    return false;
  }
  resources.input_width=input_width;
  resources.input_height=input_height;
  resources.output_width=output_width;
  resources.output_height=output_height;
  resources.direct_output=direct_output;
  resources.history_valid=false;
  return true;
}

float halton(std::uint64_t index,std::uint32_t base) {
  float result=0.0F;
  float fraction=1.0F/static_cast<float>(base);
  while(index!=0U){
    result+=fraction*static_cast<float>(index%base);
    index/=base;
    fraction/=static_cast<float>(base);
  }
  return result;
}

float half_to_float(std::uint16_t value) {
  const std::uint32_t sign=static_cast<std::uint32_t>(value&0x8000U)<<16U;
  int exponent=static_cast<int>((value>>10U)&0x1fU);
  std::uint32_t mantissa=value&0x03ffU;
  std::uint32_t bits{};
  if(exponent==0U){
    if(mantissa==0U)bits=sign;
    else{
      exponent=1U;
      while((mantissa&0x0400U)==0U){mantissa<<=1U;--exponent;}
      mantissa&=0x03ffU;
      bits=sign|(static_cast<std::uint32_t>(exponent+112)<<23U)|
          (mantissa<<13U);
    }
  }else if(exponent==31U)
    bits=sign|0x7f800000U|(mantissa<<13U);
  else bits=sign|(static_cast<std::uint32_t>(exponent+112)<<23U)|
      (mantissa<<13U);
  return std::bit_cast<float>(bits);
}

id<MTLDepthStencilState> make_depth_state(id<MTLDevice> device) {
  MTLDepthStencilDescriptor* descriptor=[MTLDepthStencilDescriptor new];
  descriptor.label=@"TetWorld reversed depth";
  descriptor.depthCompareFunction=MTLCompareFunctionGreater;
  descriptor.depthWriteEnabled=YES;
  return [device newDepthStencilStateWithDescriptor:descriptor];
}

id<MTLDepthStencilState> make_overlay_depth_state(id<MTLDevice> device) {
  MTLDepthStencilDescriptor* descriptor=[MTLDepthStencilDescriptor new];
  descriptor.label=@"TetWorld overlay depth";
  descriptor.depthCompareFunction=MTLCompareFunctionGreaterEqual;
  descriptor.depthWriteEnabled=NO;
  return [device newDepthStencilStateWithDescriptor:descriptor];
}

id<MTLDepthStencilState> make_shadow_depth_state(id<MTLDevice> device) {
  MTLDepthStencilDescriptor* descriptor=[MTLDepthStencilDescriptor new];
  descriptor.label=@"TetWorld shadow depth test";
  descriptor.depthCompareFunction=MTLCompareFunctionLess;
  descriptor.depthWriteEnabled=YES;
  return [device newDepthStencilStateWithDescriptor:descriptor];
}

id<MTLTexture> make_shadow_texture(id<MTLDevice> device,
                                   MTLPixelFormat format,
                                   NSUInteger resolution=
                                       tetra_viewer::shadow_map_resolution) {
  MTLTextureDescriptor* descriptor=[MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:format
                                  width:resolution
                                 height:resolution
                              mipmapped:NO];
  descriptor.textureType=MTLTextureType2DArray;
  descriptor.arrayLength=tetra_viewer::shadow_cascade_count+1U;
  descriptor.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
  descriptor.storageMode=MTLStorageModePrivate;
  return [device newTextureWithDescriptor:descriptor];
}

id<MTLTexture> make_render_texture(id<MTLDevice> device,int width,int height,
                                   MTLPixelFormat format,NSUInteger samples,
                                   MTLTextureUsage usage) {
  if(width<=0||height<=0)return nil;
  MTLTextureDescriptor* descriptor=[MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:format
                                  width:static_cast<NSUInteger>(width)
                                 height:static_cast<NSUInteger>(height)
                              mipmapped:NO];
  descriptor.usage=usage;
  descriptor.storageMode=MTLStorageModePrivate;
  if(samples>1U){
    descriptor.textureType=MTLTextureType2DMultisample;
    descriptor.sampleCount=samples;
  }
  return [device newTextureWithDescriptor:descriptor];
}

bool key_down(GLFWwindow* window,int key) {
  return glfwGetKey(window,key)==GLFW_PRESS;
}

bool checkbox_with_hotkey(const char* label,const char* hotkey,ImGuiKey key,
                          bool* value) {
  const std::string visible_label=std::string(label)+" ("+hotkey+")";
  bool changed=ImGui::Checkbox(visible_label.c_str(),value);
  const auto& input=ImGui::GetIO();
  if(!input.WantTextInput&&!ImGui::IsAnyItemActive()&&
     ImGui::IsKeyPressed(key,false)){
    *value=!*value;
    changed=true;
  }
  return changed;
}

}  // namespace

int main(int argc,char** argv) {
  // Keep the graphics-free world automation surface identical to tetra_world.
  // These paths intentionally return before Metal or GLFW initialization.
  if(argc>=2&&std::strcmp(argv[1],"--script-help")==0){
    if(argc!=2){
      std::fprintf(stderr,"--script-help does not accept arguments\n");
      return 2;
    }
    tetra_viewer::print_world_script_help(std::cout);
    return 0;
  }
  if(argc>=2&&std::strcmp(argv[1],"--script")==0){
    if(argc!=3){
      std::fprintf(stderr,
          "usage: tetra_world_metal --script \"command[,command...]\"\n");
      return 2;
    }
    return tetra_viewer::run_world_script(argv[2],std::cout,std::cerr);
  }
  if(argc>=2&&std::strcmp(argv[1],"--runtime-benchmark")==0){
    if(argc!=2){
      std::fprintf(stderr,"--runtime-benchmark does not accept arguments\n");
      return 2;
    }
    return tetra_viewer::run_world_runtime_benchmark(std::cout,std::cerr);
  }
  const bool device_check=argc==2&&std::strcmp(argv[1],"--metal-device-check")==0;
  const bool ray_visibility_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-ray-visibility-smoke-test")==0;
  const bool terrain_ray_oracle_test=argc==2&&
      std::strcmp(argv[1],"--metal-terrain-ray-oracle-smoke-test")==0;
  const bool atmosphere_compiler_check=argc==2&&
      std::strcmp(argv[1],"--metal-atmosphere-compiler-check")==0;
  const bool gpu_lod_selector_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-lod-selector-smoke-test")==0;
  const bool gpu_live_selection_state_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-live-selection-state-smoke-test")==0;
  const bool gpu_hierarchy_frontier_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-hierarchy-frontier-smoke-test")==0;
  const bool gpu_compact_green_closure_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-compact-green-closure-smoke-test")==0;
  const bool gpu_compact_red_scan_large_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-compact-red-scan-large-smoke-test")==0;
  const bool gpu_compact_live_closure_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-compact-live-closure-smoke-test")==0;
  const bool gpu_compact_live_parity_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-compact-live-parity-smoke-test")==0;
  const bool gpu_production_front_parity_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-production-front-parity-smoke-test")==0;
  const bool gpu_compact_live_performance_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-compact-live-performance-smoke-test")==0;
  const bool gpu_compact_hybrid_parity_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-compact-hybrid-parity-smoke-test")==0;
  const bool gpu_compact_owner_materialize_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-compact-owner-materialize-smoke-test")==0;
  const bool gpu_compact_owner_p8_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-compact-owner-p8-smoke-test")==0;
  const bool gpu_terrain_extract_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-terrain-extract-smoke-test")==0;
  const bool gpu_terrain_classify_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-terrain-classify-smoke-test")==0;
  const bool gpu_terrain_triangle_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-terrain-triangle-smoke-test")==0;
  const bool gpu_terrain_parallel_triangle_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-terrain-parallel-triangle-smoke-test")==0;
  const bool gpu_terrain_project_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-terrain-project-smoke-test")==0;
  const bool gpu_terrain_draw_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-terrain-draw-smoke-test")==0;
  const bool gpu_terrain_native_chain_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-terrain-native-chain-smoke-test")==0;
  const bool gpu_terrain_live_slots_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-terrain-live-slots-smoke-test")==0;
  const bool gpu_terrain_runtime_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-terrain-runtime-smoke-test")==0;
  const bool gpu_terrain_surface_parity_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-terrain-surface-parity-smoke-test")==0;
  const bool gpu_terrain_performance_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-terrain-performance-smoke-test")==0;
  const bool gpu_volume_split_closure_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-volume-split-closure-smoke-test")==0;
  const bool atmosphere_lut_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-atmosphere-lut-smoke-test")==0;
  const bool atmosphere_capture=argc==3&&
      std::strcmp(argv[1],"--metal-atmosphere-capture")==0;
  const bool atmosphere_mountain_capture=atmosphere_capture&&
      std::getenv("TETWORLD_METAL_REPORTED_MOUNTAIN")!=nullptr;
  const bool atmosphere_visible_sun_capture=atmosphere_capture&&
      std::getenv("TETWORLD_METAL_VISIBLE_SUN")!=nullptr;
  // Named deterministic poses exercise the real reference-temporal Metal
  // route at the altitude regimes that a sky-view LUT must survive.  They are
  // deliberately available only to the existing atmosphere capture command:
  // this is a qualification fixture, not a second camera/compositing path.
  const char* atmosphere_capture_pose=atmosphere_capture?
      std::getenv("TETWORLD_METAL_ATMOSPHERE_CAPTURE_POSE"):nullptr;
  if(atmosphere_capture_pose!=nullptr&&atmosphere_capture_pose[0]=='\0')
    atmosphere_capture_pose=nullptr;
  if(atmosphere_capture_pose!=nullptr&&
     (atmosphere_mountain_capture||atmosphere_visible_sun_capture)){
    std::fprintf(stderr,"TETWORLD_METAL_ATMOSPHERE_CAPTURE_POSE cannot be "
        "combined with TETWORLD_METAL_REPORTED_MOUNTAIN or "
        "TETWORLD_METAL_VISIBLE_SUN\\n");
    return 2;
  }
  if(atmosphere_capture_pose!=nullptr&&
     std::strcmp(atmosphere_capture_pose,"flight")!=0&&
     std::strcmp(atmosphere_capture_pose,"atmosphere-top")!=0&&
     std::strcmp(atmosphere_capture_pose,"orbit")!=0&&
     std::strcmp(atmosphere_capture_pose,"orbit-motion-a")!=0&&
     std::strcmp(atmosphere_capture_pose,"orbit-motion-b")!=0){
    std::fprintf(stderr,"TETWORLD_METAL_ATMOSPHERE_CAPTURE_POSE must be "
        "flight, atmosphere-top, orbit, orbit-motion-a, or orbit-motion-b\\n");
    return 2;
  }
  const bool atmosphere_froxel_test=argc==2&&
      std::strcmp(argv[1],"--metal-atmosphere-froxel-smoke-test")==0;
  const bool atmosphere_minmax_test=argc==2&&
      std::strcmp(argv[1],"--metal-atmosphere-minmax-smoke-test")==0;
  const bool atmosphere_epipolar_test=argc==2&&
      std::strcmp(argv[1],"--metal-atmosphere-epipolar-smoke-test")==0;
  const bool atmosphere_reference_test=argc==2&&
      std::strcmp(argv[1],"--metal-atmosphere-reference-smoke-test")==0;
  const bool atmosphere_lookup_invalidation_test=argc==2&&
      std::strcmp(argv[1],"--metal-atmosphere-lookup-invalidation-smoke-test")==0;
  const bool atmosphere_quarter_test=argc==2&&
      std::strcmp(argv[1],"--metal-atmosphere-quarter-smoke-test")==0;
  const bool atmosphere_fallback_test=argc==2&&
      std::strcmp(argv[1],"--metal-atmosphere-fallback-smoke-test")==0;
  const bool atmosphere_60_degree_test=argc==2&&
      std::strcmp(argv[1],"--metal-atmosphere-60deg-smoke-test")==0;
  const bool atmosphere_quality_test=argc==2&&
      std::strcmp(argv[1],"--metal-atmosphere-quality-smoke-test")==0;
  const bool atmosphere_frame_test=(argc==2&&
      std::strcmp(argv[1],"--metal-atmosphere-frame-smoke-test")==0)||
      atmosphere_capture||atmosphere_froxel_test||atmosphere_minmax_test||
      atmosphere_epipolar_test||atmosphere_reference_test||
      atmosphere_lookup_invalidation_test||
      atmosphere_fallback_test||atmosphere_60_degree_test;
  const bool any_atmosphere_frame_test=atmosphere_frame_test||
      atmosphere_quarter_test;
  const bool smoke_test=argc==2&&std::strcmp(argv[1],"--metal-smoke-test")==0;
  // Automation needs a bounded counterpart for the normal interactive
  // device-front selection. The production launch itself selects this route
  // without an argument.
  const bool device_front_default_test=argc==2&&
      std::strcmp(argv[1],"--metal-device-front-default-smoke-test")==0;
  const bool motion_test=argc==2&&
      (std::strcmp(argv[1],"--metal-motion-smoke-test")==0||
       device_front_default_test);
  const bool render_test=argc==2&&
      std::strcmp(argv[1],"--metal-render-smoke-test")==0;
  const bool metalfx_test=argc==2&&
      std::strcmp(argv[1],"--metal-metalfx-smoke-test")==0;
  const bool auto_resolution_test=argc==2&&
      std::strcmp(argv[1],"--metal-auto-resolution-smoke-test")==0;
  const bool soak_test=argc==2&&
      std::strcmp(argv[1],"--metal-soak-smoke-test")==0;
  const bool timing_profile_test=argc==2&&
      std::strcmp(argv[1],"--metal-timing-profile-smoke-test")==0;
  enum class TimingProfileClass { stable, moving, terminator, lookup_refresh,
                                  optical_refresh, aerial_refresh, preview,
                                  exact_handoff, ray_tracing, shadow_lookup };
  TimingProfileClass timing_profile_class=TimingProfileClass::stable;
  const char* timing_profile_class_name="stable";
  if(timing_profile_test){
    const char* requested=std::getenv("TETWORLD_METAL_TIMING_PROFILE");
    if(requested!=nullptr&&requested[0]!='\0'){
      if(std::strcmp(requested,"stable")==0){}
      else if(std::strcmp(requested,"moving")==0)
        timing_profile_class=TimingProfileClass::moving;
      else if(std::strcmp(requested,"terminator")==0)
        timing_profile_class=TimingProfileClass::terminator;
      else if(std::strcmp(requested,"lookup-refresh")==0)
        timing_profile_class=TimingProfileClass::lookup_refresh;
      else if(std::strcmp(requested,"optical-refresh")==0)
        timing_profile_class=TimingProfileClass::optical_refresh;
      else if(std::strcmp(requested,"aerial-refresh")==0)
        timing_profile_class=TimingProfileClass::aerial_refresh;
      else if(std::strcmp(requested,"preview")==0)
        timing_profile_class=TimingProfileClass::preview;
      else if(std::strcmp(requested,"exact-handoff")==0)
        timing_profile_class=TimingProfileClass::exact_handoff;
      else if(std::strcmp(requested,"ray-tracing")==0)
        timing_profile_class=TimingProfileClass::ray_tracing;
      else if(std::strcmp(requested,"shadow-lookup")==0)
        timing_profile_class=TimingProfileClass::shadow_lookup;
      else {
        std::fprintf(stderr,"TETWORLD_METAL_TIMING_PROFILE must be stable, "
        "moving, terminator, lookup-refresh, optical-refresh, aerial-refresh, preview, exact-handoff, "
            "ray-tracing, or shadow-lookup\\n");
        return 2;
      }
      timing_profile_class_name=requested;
    }
  }
  const bool overlay_test=argc==2&&
      std::strcmp(argv[1],"--metal-overlay-smoke-test")==0;
  const bool shadow_test=argc==2&&
      std::strcmp(argv[1],"--metal-shadow-smoke-test")==0;
  std::optional<std::array<double,6>> capture_view_coordinates;
  if(argc>=2&&std::strcmp(argv[1],"--capture-view")==0){
    if(argc!=9){
      std::fprintf(stderr,"usage: tetra_world_metal --capture-view <path.ppm> "
          "<camera-x> <camera-y> <camera-z> "
          "<target-x> <target-y> <target-z>\n");
      return 2;
    }
    std::array<double,6> values{};
    for(std::size_t index=0;index<values.size();++index){
      char* end=nullptr;
      values[index]=std::strtod(argv[index+3U],&end);
      if(end==argv[index+3U]||*end!='\0'||!std::isfinite(values[index])){
        std::fprintf(stderr,
                     "capture view coordinates must be finite numbers\n");
        return 2;
      }
    }
    capture_view_coordinates=values;
  }
  const bool capture_view=capture_view_coordinates.has_value();
  const bool write_capture=argc==3&&
      (std::strcmp(argv[1],"--metal-capture")==0||
       std::strcmp(argv[1],"--capture")==0)||capture_view;
  const bool validation_test=argc==3&&
      std::strcmp(argv[1],"--metal-validate-geometry")==0;
  const bool capture_test=write_capture||validation_test;
  const bool automated_test=smoke_test||motion_test||render_test||metalfx_test||soak_test||
      gpu_terrain_performance_smoke_test||
      auto_resolution_test||timing_profile_test||overlay_test||shadow_test||capture_test||
      any_atmosphere_frame_test||atmosphere_quality_test||terrain_ray_oracle_test;
  // P6b uses the same profile knobs as the timing matrix for native captures
  // and temporal smokes.  Keep them opt-in and automation-only: interactive
  // resolution/MSAA policy must remain independent until a row passes every
  // physical and motion gate.
  const bool raster_profile_qualification=
      std::getenv("TETWORLD_METAL_RASTER_PROFILE_QUALIFICATION")!=nullptr;
  if(raster_profile_qualification&&
     !(atmosphere_capture||motion_test||metalfx_test)){
    std::fprintf(stderr,"TETWORLD_METAL_RASTER_PROFILE_QUALIFICATION requires "
                        "a Metal atmosphere capture, motion, or MetalFX smoke\\n");
    return 2;
  }
  const bool profile_interactive_rendering=
      (auto_resolution_test||soak_test||render_test||atmosphere_capture||timing_profile_test||
       (raster_profile_qualification&&(motion_test||metalfx_test)))&&
      (std::getenv("TETWORLD_METAL_PROFILE_INTERACTIVE")!=nullptr||
       timing_profile_test||raster_profile_qualification);
  // The finite fast front remains opt-in until its coverage can include every
  // visible terrain pixel.  Shipping it as the default would expose its outer
  // rectangle as a missing-terrain patch. Exact terrain remains the complete
  // production display front in the meantime.
  bool preview_enabled=false;
  if(const char* value=std::getenv("TETWORLD_METAL_PREVIEW");value!=nullptr){
    if(std::strcmp(value,"0")==0)preview_enabled=false;
    else if(std::strcmp(value,"1")==0)preview_enabled=true;
    else {
      std::fprintf(stderr,"TETWORLD_METAL_PREVIEW must be 0 or 1\n");
      return 2;
    }
  }
  if(timing_profile_test&&
     (timing_profile_class==TimingProfileClass::preview||
      timing_profile_class==TimingProfileClass::exact_handoff))
    preview_enabled=true;
  const bool require_exact_handoff_capture=
      std::getenv("TETWORLD_METAL_CAPTURE_EXACT_HANDOFF")!=nullptr||
      (timing_profile_test&&
       timing_profile_class==TimingProfileClass::exact_handoff);
  const bool visible_test_window=automated_test&&
      std::getenv("TETWORLD_METAL_VISIBLE_TEST_WINDOW")!=nullptr;
  const bool background_requested=
      std::getenv("TETWORLD_METAL_BACKGROUND")!=nullptr;
  // P5c2 is opt-in until a completed Metal slot has passed the moving-camera
  // qualification.  Requesting the packet changes only CPU publication work;
  // the renderer continues to draw its authoritative CPU display front.
  const bool metal_gpu_terrain_diagnostic=
      std::getenv("TETWORLD_METAL_GPU_TERRAIN_DIAGNOSTIC")!=nullptr;
  // P7c2b1b's opt-in live qualification is intentionally distinct from the
  // legacy extraction diagnostic.  It starts from a completed P6 directory
  // packet and never consults `world_surface_gpu_cells()`.
  const bool metal_gpu_terrain_native_diagnostic=
      std::getenv("TETWORLD_METAL_GPU_TERRAIN_NATIVE_DIAGNOSTIC")!=nullptr;
  // Readback is a qualification-only escape hatch.  The normal GPU route
  // deliberately observes completion, never candidate buffer contents.
  const bool metal_gpu_terrain_qualification=
      metal_gpu_terrain_native_diagnostic||
      std::getenv("TETWORLD_METAL_GPU_TERRAIN_QUALIFICATION")!=nullptr;
  // The legacy P6 mesh-emission route is an opt-in comparison route.  It is
  // CPU-fed and intentionally distinct from the experimental P7e4
  // GPU-resident terrain route below.
  bool gpu_terrain_renderer_selected=gpu_terrain_performance_smoke_test;
  if(const char* value=std::getenv("TETWORLD_METAL_GPU_TERRAIN_RENDERER");
     value!=nullptr){
    if(std::strcmp(value,"0")==0)gpu_terrain_renderer_selected=false;
    else if(std::strcmp(value,"1")==0)gpu_terrain_renderer_selected=true;
    else { std::fprintf(stderr,"TETWORLD_METAL_GPU_TERRAIN_RENDERER must be 0 or 1\\n");return 2; }
  }
  if(gpu_terrain_performance_smoke_test)gpu_terrain_renderer_selected=true;
  // The compact device front is the ordinary interactive route. Tests and
  // legacy P6 diagnostics retain their explicit CPU choices; the bounded
  // ordinary-launch fixture below exercises the same route under automation.
  // Parse an override as a value rather than an env-presence switch.
  bool metal_gpu_terrain_device_front=!automated_test||device_front_default_test;
  bool metal_gpu_terrain_device_front_explicit=false;
  if(const char* value=std::getenv("TETWORLD_METAL_GPU_TERRAIN_DEVICE_FRONT");
     value!=nullptr){
    metal_gpu_terrain_device_front_explicit=true;
    if(std::strcmp(value,"0")==0)metal_gpu_terrain_device_front=false;
    else if(std::strcmp(value,"1")==0)metal_gpu_terrain_device_front=true;
    else {
      std::fprintf(stderr,
          "TETWORLD_METAL_GPU_TERRAIN_DEVICE_FRONT must be 0 or 1\\n");
      return 2;
    }
  }
  const bool metal_gpu_terrain_live_selection_requested=
      std::getenv("TETWORLD_METAL_GPU_TERRAIN_LIVE_SELECTION")!=nullptr;
  // These established qualification routes intentionally exercise their
  // CPU-fed/CPU-front contracts. Do not make a default interactive choice
  // silently alter their provenance; an explicit conflicting device request
  // still reaches the existing incompatibility error below.
  if(!metal_gpu_terrain_device_front_explicit&&
     (metal_gpu_terrain_live_selection_requested||gpu_terrain_renderer_selected||
      metal_gpu_terrain_native_diagnostic||metal_gpu_terrain_diagnostic))
    metal_gpu_terrain_device_front=false;
  // These test-only fault switches exercise P7e4a's private rejection path.
  // They do not make the route selectable or change its normal output.
  const bool metal_gpu_terrain_device_front_inject_failure=
      metal_gpu_terrain_device_front&&
      std::getenv("TETWORLD_METAL_GPU_TERRAIN_DEVICE_FRONT_INJECT_FAILURE")!=nullptr;
  const bool metal_gpu_terrain_device_front_inject_capacity_failure=
      metal_gpu_terrain_device_front&&
      std::getenv("TETWORLD_METAL_GPU_TERRAIN_DEVICE_FRONT_INJECT_CAPACITY_FAILURE")!=nullptr;
  const bool metal_gpu_terrain_device_front_inject_green_budget_failure=
      metal_gpu_terrain_device_front&&
      std::getenv("TETWORLD_METAL_GPU_TERRAIN_DEVICE_FRONT_INJECT_GREEN_BUDGET_FAILURE")!=nullptr;
  if(static_cast<unsigned>(metal_gpu_terrain_device_front_inject_failure)+
     static_cast<unsigned>(metal_gpu_terrain_device_front_inject_capacity_failure)+
     static_cast<unsigned>(metal_gpu_terrain_device_front_inject_green_budget_failure)>1U){
    std::fprintf(stderr,"only one device-front failure injection may be active\\n");
    return 2;
  }
  // A device-front moving smoke is deliberately bounded independently of the
  // larger image suites.  Its terminal diagnostic names the last completed
  // device phase, so a launch/runtime delay is never reported as P8 failure.
  int device_front_smoke_timeout_seconds=60;
  if(const char* value=std::getenv("TETWORLD_METAL_DEVICE_FRONT_SMOKE_TIMEOUT_SECONDS");
     value!=nullptr){
    char* end=nullptr;
    const long seconds=std::strtol(value,&end,10);
    if(!metal_gpu_terrain_device_front||!motion_test||end==value||*end!='\0'||
       seconds<10L||seconds>120L){
      std::fprintf(stderr,"TETWORLD_METAL_DEVICE_FRONT_SMOKE_TIMEOUT_SECONDS requires "
                          "a device-front motion smoke and must be 10..120\\n");
      return 2;
    }
    device_front_smoke_timeout_seconds=static_cast<int>(seconds);
  }
  bool metal_gpu_terrain_live_selection=metal_gpu_terrain_device_front||
      metal_gpu_terrain_live_selection_requested;
  if(metal_gpu_terrain_live_selection&&
     (gpu_terrain_renderer_selected||metal_gpu_terrain_native_diagnostic||
      metal_gpu_terrain_diagnostic)){
    std::fprintf(stderr,"TETWORLD_METAL_GPU_TERRAIN_LIVE_SELECTION cannot be "
                        "combined with CPU-source GPU mesh emission diagnostics\n");
    return 2;
  }
  // P8c: MetalFX writes the final result directly to a non-framebuffer-only
  // drawable, avoiding the persistent output texture and presentation draw.
  // Keep the former path as an explicit paired qualification control.
  bool metalfx_direct_drawable=true;
  if(const char* value=std::getenv("TETWORLD_METAL_DIRECT_DRAWABLE");
     value!=nullptr){
    if(std::strcmp(value,"0")==0)metalfx_direct_drawable=false;
    else if(std::strcmp(value,"1")!=0){
      std::fprintf(stderr,"TETWORLD_METAL_DIRECT_DRAWABLE must be 0 or 1\n");
      return 2;
    }
  }
  // P8b exercises the selected private display front in the real moving
  // render loop.  It permits no candidate payload readback and is
  // automation-only, leaving normal GPU selection a quiet presentation
  // choice. P8c owns promotion of the root-expanded native generator.
  const bool metal_gpu_terrain_private_front_qualification=
      gpu_terrain_performance_smoke_test||
      std::getenv("TETWORLD_METAL_GPU_TERRAIN_PRIVATE_FRONT_QUALIFICATION")!=nullptr;
  if(metal_gpu_terrain_private_front_qualification&&
     !gpu_terrain_renderer_selected){
    std::fprintf(stderr,"TETWORLD_METAL_GPU_TERRAIN_PRIVATE_FRONT_QUALIFICATION "
                        "requires TETWORLD_METAL_GPU_TERRAIN_RENDERER=1\\n");
    return 2;
  }
  // 200x100 is Hillaire's reference sky-view resolution and is now the
  // qualified production default.  Keep 0 as an explicit 384x216 control for
  // the paired native qualification harness.
  bool sky_view_reference_profile=true;
  if(const char* value=std::getenv("TETWORLD_METAL_SKY_VIEW_REFERENCE");
     value!=nullptr){
    if(std::strcmp(value,"0")==0)sky_view_reference_profile=false;
    else if(std::strcmp(value,"1")==0)sky_view_reference_profile=true;
    else {
      std::fprintf(stderr,
          "TETWORLD_METAL_SKY_VIEW_REFERENCE must be 0 or 1\n");
      return 2;
    }
  }
  if(const char* value=std::getenv("TETWORLD_METAL_HALF_RADIANCE");
     value!=nullptr){
    if(std::strcmp(value,"0")==0)atmosphere_half_radiance_experiment=false;
    else if(std::strcmp(value,"1")==0)atmosphere_half_radiance_experiment=true;
    else {
      std::fprintf(stderr,"TETWORLD_METAL_HALF_RADIANCE must be 0 or 1\n");
      return 2;
    }
  }
  if(const char* value=std::getenv("TETWORLD_METAL_PRIVATE_RADIANCE");
     value!=nullptr){
    if(std::strcmp(value,"0")==0)atmosphere_private_radiance_experiment=false;
    else if(std::strcmp(value,"1")==0)atmosphere_private_radiance_experiment=true;
    else {
      std::fprintf(stderr,"TETWORLD_METAL_PRIVATE_RADIANCE must be 0 or 1\n");
      return 2;
    }
  }
  if(const char* value=std::getenv("TETWORLD_METAL_HALF_SCREEN_TRANSMITTANCE");
     value!=nullptr){
    if(std::strcmp(value,"0")==0)
      atmosphere_half_screen_transmittance_experiment=false;
    else if(std::strcmp(value,"1")==0)
      atmosphere_half_screen_transmittance_experiment=true;
    else {
      std::fprintf(stderr,
          "TETWORLD_METAL_HALF_SCREEN_TRANSMITTANCE must be 0 or 1\n");
      return 2;
    }
  }
  const bool legacy_native_depth_scan=
      std::getenv("TETWORLD_METAL_LEGACY_NATIVE_DEPTH_SCAN")!=nullptr;
  // P5b qualification control: retain direct-light work that has already
  // been proven radiometrically zero by the solid-planet umbra.
  atmosphere_legacy_planet_umbra_work=
      std::getenv("TETWORLD_METAL_LEGACY_PLANET_UMBRA_WORK")!=nullptr;
  // P4c's production route elides only reference-temporal true-sky transport;
  // diagnostics and non-reference renderers retain their full screen work.
  // Keep the old complete integration as an explicit paired-test control.
  const bool legacy_reference_sky_transport=
      std::getenv("TETWORLD_METAL_LEGACY_REFERENCE_SKY_TRANSPORT")!=nullptr;
  const bool elide_reference_sky_transport=
      !legacy_reference_sky_transport;
  // Automated runs must not steal focus or briefly flash a window. Keep an
  // explicit visible mode for debugging, and retain the old hidden variable
  // as a backwards-compatible no-op in the already-hidden default case.
  const bool hidden_window=background_requested||
      (automated_test&&!visible_test_window)||
      std::getenv("TETWORLD_METAL_HIDDEN_WINDOW")!=nullptr;
  const bool interactive_capture_resolution=atmosphere_capture&&
      std::getenv("TETWORLD_METAL_CAPTURE_INTERACTIVE_RESOLUTION")!=nullptr;
  if(argc>1&&!device_check&&!ray_visibility_smoke_test&&!terrain_ray_oracle_test&&!atmosphere_compiler_check&&!gpu_lod_selector_smoke_test&&!gpu_live_selection_state_smoke_test&&!gpu_hierarchy_frontier_smoke_test&&!gpu_compact_green_closure_smoke_test&&!gpu_compact_red_scan_large_smoke_test&&!gpu_compact_live_closure_smoke_test&&!gpu_compact_live_parity_smoke_test&&!gpu_production_front_parity_smoke_test&&!gpu_compact_live_performance_smoke_test&&!gpu_compact_hybrid_parity_smoke_test&&!gpu_compact_owner_materialize_smoke_test&&!gpu_compact_owner_p8_smoke_test&&!gpu_terrain_extract_smoke_test&&!gpu_terrain_classify_smoke_test&&!gpu_terrain_triangle_smoke_test&&!gpu_terrain_parallel_triangle_smoke_test&&!gpu_terrain_project_smoke_test&&!gpu_terrain_draw_smoke_test&&!gpu_terrain_native_chain_smoke_test&&!gpu_terrain_live_slots_smoke_test&&!gpu_terrain_runtime_smoke_test&&!gpu_terrain_surface_parity_smoke_test&&!gpu_terrain_performance_smoke_test&&!gpu_volume_split_closure_smoke_test&&
     !atmosphere_lut_smoke_test&&!smoke_test&&
     !any_atmosphere_frame_test&&
     !atmosphere_quality_test&&
     !motion_test&&!render_test&&!metalfx_test&&!auto_resolution_test&&!soak_test&&
     !timing_profile_test&&
     !overlay_test&&!shadow_test&&!capture_test){
    std::fprintf(stderr,"usage: %s [--metal-device-check|"
                        "--metal-ray-visibility-smoke-test|"
                        "--metal-terrain-ray-oracle-smoke-test|--metal-smoke-test|"
                        "--metal-atmosphere-compiler-check|"
                        "--metal-gpu-lod-selector-smoke-test|"
                        "--metal-gpu-live-selection-state-smoke-test|"
                        "--metal-gpu-hierarchy-frontier-smoke-test|"
                        "--metal-gpu-compact-green-closure-smoke-test|"
                        "--metal-gpu-compact-red-scan-large-smoke-test|"
                        "--metal-gpu-compact-live-closure-smoke-test|"
                        "--metal-gpu-compact-live-parity-smoke-test|"
                        "--metal-gpu-production-front-parity-smoke-test|"
                        "--metal-gpu-compact-live-performance-smoke-test|"
                        "--metal-gpu-compact-owner-materialize-smoke-test|"
                        "--metal-gpu-compact-owner-p8-smoke-test|"
                        "--metal-gpu-terrain-extract-smoke-test|"
                        "--metal-gpu-terrain-classify-smoke-test|"
                        "--metal-gpu-terrain-triangle-smoke-test|"
                        "--metal-gpu-terrain-parallel-triangle-smoke-test|"
                        "--metal-gpu-terrain-project-smoke-test|"
                        "--metal-gpu-terrain-draw-smoke-test|"
                        "--metal-gpu-terrain-native-chain-smoke-test|"
                        "--metal-gpu-terrain-live-slots-smoke-test|"
                        "--metal-gpu-terrain-runtime-smoke-test|"
                        "--metal-gpu-terrain-surface-parity-smoke-test|"
                        "--metal-gpu-terrain-performance-smoke-test|"
                        "--metal-atmosphere-lut-smoke-test|"
                        "--metal-atmosphere-frame-smoke-test|"
                        "--metal-atmosphere-capture <path.ppm>|"
                        "--metal-atmosphere-froxel-smoke-test|"
                        "--metal-atmosphere-minmax-smoke-test|"
                        "--metal-atmosphere-epipolar-smoke-test|"
                        "--metal-atmosphere-reference-smoke-test|"
                        "--metal-atmosphere-lookup-invalidation-smoke-test|"
                        "--metal-atmosphere-quarter-smoke-test|"
                        "--metal-atmosphere-fallback-smoke-test|"
                        "--metal-atmosphere-60deg-smoke-test|"
                        "--metal-atmosphere-quality-smoke-test|"
                        "--metal-motion-smoke-test|"
                        "--metal-render-smoke-test|"
                        "--metal-metalfx-smoke-test|"
                        "--metal-auto-resolution-smoke-test|"
                        "--metal-timing-profile-smoke-test|"
                        "--metal-overlay-smoke-test|"
                        "--metal-shadow-smoke-test|"
                        "--script-help|--script <commands>|"
                        "--runtime-benchmark|"
                        "--metal-capture <path.ppm>|"
                        "--capture <path.ppm>|"
                        "--capture-view <path.ppm> <camera-x> <camera-y> "
                        "<camera-z> <target-x> <target-y> <target-z>|"
                        "--metal-validate-geometry <vulkan-mask.pgm>]\n",
                 argv[0]);
    return 2;
  }
  int result=0;
  @autoreleasepool {
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();
    if(device==nil){
      std::fprintf(stderr,"No Metal device is available.\n");
      return 1;
    }
    id<MTLLibrary> library=make_shader_library(device);
    if(library==nil)return 1;
    id<MTLComputePipelineState> gpu_terrain_extract_pipeline=nil;
    id<MTLComputePipelineState> gpu_terrain_classify_pipeline=nil;
    id<MTLComputePipelineState> gpu_terrain_count_pipeline=nil;
    id<MTLComputePipelineState> gpu_terrain_scan_pipeline=nil;
    id<MTLComputePipelineState> gpu_terrain_finalize_pipeline=nil;
    id<MTLComputePipelineState> gpu_terrain_scatter_pipeline=nil;
    id<MTLComputePipelineState> gpu_terrain_owner_count_pipeline=nil;
    id<MTLComputePipelineState> gpu_terrain_owner_finalize_pipeline=nil;
    id<MTLComputePipelineState> gpu_terrain_owner_emit_pipeline=nil;
    id<MTLComputePipelineState> gpu_terrain_project_pipeline=nil;
    id<MTLComputePipelineState> gpu_terrain_draw_pipeline=nil;
    id<MTLComputePipelineState> gpu_terrain_commit_validate_pipeline=nil;
    id<MTLComputePipelineState> gpu_terrain_commit_copy_pipeline=nil;
    id<MTLComputePipelineState> gpu_terrain_commit_publish_pipeline=nil;
    id<MTLComputePipelineState> gpu_lod_live_selection_pipeline=nil;
    id<MTLComputePipelineState> gpu_hierarchy_compact_worklist_pipeline=nil;
    id<MTLComputePipelineState> gpu_hierarchy_canonicalize_pipeline=nil;
    id<MTLComputePipelineState> gpu_hierarchy_compact_green_pipeline=nil;
    id<MTLComputePipelineState> gpu_hierarchy_compact_red_pipeline=nil;
    id<MTLComputePipelineState> gpu_hierarchy_compact_red_scan_pipeline=nil;
    id<MTLComputePipelineState> gpu_hierarchy_compact_owner_materialize_pipeline=nil;
    MetalCompactOwnerP8Pipelines gpu_hierarchy_compact_owner_p8_pipelines;
    if(metal_gpu_terrain_diagnostic){
      const auto path=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR)/
          "gpu_terrain_extract.comp.metal";
      id<MTLLibrary> extract_library=make_file_shader_library(device,path.string().c_str());
      NSError* extract_error=nil;
      gpu_terrain_extract_pipeline=extract_library==nil?nil:
          [device newComputePipelineStateWithFunction:[extract_library newFunctionWithName:@"main0"] error:&extract_error];
      if(gpu_terrain_extract_pipeline==nil)return 1;
    }
    if(metal_gpu_terrain_native_diagnostic||gpu_terrain_renderer_selected||
       metal_gpu_terrain_live_selection){
      const auto make_pipeline=[&](const char* name)->id<MTLComputePipelineState>{
        const auto path=std::filesystem::path(TETRA_METAL_ATMOSPHERE_SHADER_DIR)/name;
        id<MTLLibrary> shader_library=make_file_shader_library(device,path.string().c_str());
        NSError* error=nil;
        return shader_library==nil?nil:[device newComputePipelineStateWithFunction:
            [shader_library newFunctionWithName:@"main0"] error:&error];
      };
      gpu_terrain_classify_pipeline=make_pipeline("gpu_terrain_classify.comp.metal");
      gpu_terrain_count_pipeline=make_pipeline("gpu_terrain_triangle_counts.comp.metal");
      gpu_terrain_scan_pipeline=make_pipeline("gpu_terrain_exclusive_scan.comp.metal");
      gpu_terrain_finalize_pipeline=make_pipeline("gpu_terrain_triangle_finalize.comp.metal");
      gpu_terrain_scatter_pipeline=make_pipeline("gpu_terrain_triangle_scatter.comp.metal");
      gpu_terrain_owner_count_pipeline=make_pipeline("gpu_terrain_owner_counts.comp.metal");
      gpu_terrain_owner_finalize_pipeline=make_pipeline("gpu_terrain_owner_finalize.comp.metal");
      gpu_terrain_owner_emit_pipeline=make_pipeline("gpu_terrain_owner_emit.comp.metal");
      gpu_terrain_project_pipeline=make_pipeline("gpu_terrain_project.comp.metal");
      gpu_terrain_draw_pipeline=make_pipeline("gpu_terrain_draw.comp.metal");
      gpu_terrain_commit_validate_pipeline=make_pipeline("gpu_terrain_commit_validate.comp.metal");
      gpu_terrain_commit_copy_pipeline=make_pipeline("gpu_terrain_commit_copy.comp.metal");
      gpu_terrain_commit_publish_pipeline=make_pipeline("gpu_terrain_commit_publish.comp.metal");
      if(metal_gpu_terrain_live_selection)
        gpu_lod_live_selection_pipeline=make_pipeline("gpu_lod.comp.metal");
      if(metal_gpu_terrain_device_front){
        gpu_hierarchy_compact_worklist_pipeline=
            make_pipeline("gpu_hierarchy_compact_worklist.comp.metal");
        gpu_hierarchy_canonicalize_pipeline=
            make_pipeline("gpu_hierarchy_canonicalize.comp.metal");
        gpu_hierarchy_compact_green_pipeline=
            make_pipeline("gpu_hierarchy_compact_green_closure.comp.metal");
        gpu_hierarchy_compact_red_pipeline=
            make_pipeline("gpu_hierarchy_compact_red_repair.comp.metal");
        gpu_hierarchy_compact_red_scan_pipeline=
            make_pipeline("gpu_hierarchy_compact_red_scan.comp.metal");
        gpu_hierarchy_compact_owner_materialize_pipeline=
            make_pipeline("gpu_hierarchy_compact_owner_materialize.comp.metal");
        gpu_hierarchy_compact_owner_p8_pipelines={
            make_pipeline("gpu_terrain_compact_owner_control.comp.metal"),
            make_pipeline("gpu_terrain_compact_owner_count.comp.metal"),
            make_pipeline("gpu_terrain_compact_owner_scan.comp.metal"),
            make_pipeline("gpu_terrain_compact_owner_emit.comp.metal"),
            make_pipeline("gpu_terrain_compact_owner_triangle_emit.comp.metal"),
            make_pipeline("gpu_terrain_compact_owner_validate.comp.metal"),
            make_pipeline("gpu_terrain_compact_owner_copy.comp.metal"),
            make_pipeline("gpu_terrain_compact_owner_publish.comp.metal"),
            make_pipeline("gpu_terrain_compact_owner_microbatch.comp.metal"),
            make_pipeline("gpu_terrain_compact_owner_microbatch_validate.comp.metal"),
            make_pipeline("gpu_terrain_compact_owner_hybrid_scan.comp.metal"),
            make_pipeline("gpu_terrain_compact_owner_hybrid_finalize.comp.metal")};
      }
      if(gpu_terrain_classify_pipeline==nil||gpu_terrain_count_pipeline==nil||
         gpu_terrain_scan_pipeline==nil||gpu_terrain_finalize_pipeline==nil||
         gpu_terrain_scatter_pipeline==nil||
         gpu_terrain_owner_count_pipeline==nil||
         gpu_terrain_owner_finalize_pipeline==nil||
         gpu_terrain_owner_emit_pipeline==nil||
         gpu_terrain_project_pipeline==nil||gpu_terrain_draw_pipeline==nil||
         gpu_terrain_commit_validate_pipeline==nil||
         gpu_terrain_commit_copy_pipeline==nil||
         gpu_terrain_commit_publish_pipeline==nil||
         (metal_gpu_terrain_live_selection&&gpu_lod_live_selection_pipeline==nil)||
         (metal_gpu_terrain_device_front&&
          (gpu_hierarchy_compact_worklist_pipeline==nil||
           gpu_hierarchy_canonicalize_pipeline==nil||
           gpu_hierarchy_compact_green_pipeline==nil||
           gpu_hierarchy_compact_red_pipeline==nil||
           gpu_hierarchy_compact_red_scan_pipeline==nil||
           gpu_hierarchy_compact_owner_materialize_pipeline==nil||
           gpu_hierarchy_compact_owner_p8_pipelines.control==nil||
           gpu_hierarchy_compact_owner_p8_pipelines.count==nil||
           gpu_hierarchy_compact_owner_p8_pipelines.scan==nil||
           gpu_hierarchy_compact_owner_p8_pipelines.emit==nil||
           gpu_hierarchy_compact_owner_p8_pipelines.triangle_emit==nil||
           gpu_hierarchy_compact_owner_p8_pipelines.validate==nil||
           gpu_hierarchy_compact_owner_p8_pipelines.copy==nil||
           gpu_hierarchy_compact_owner_p8_pipelines.publish==nil||
           gpu_hierarchy_compact_owner_p8_pipelines.hybrid_scan==nil||
           gpu_hierarchy_compact_owner_p8_pipelines.hybrid_finalize==nil)))return 1;
    }
    const bool metal_ray_tracing_supported=[](id<MTLDevice> candidate){
      if(@available(macOS 11.0,*))return candidate.supportsRaytracing;
      return false;
    }(device);
    if(device_check){
      std::printf("{\"event\":\"metal_device\",\"name\":\"%s\","
                  "\"ray_tracing_supported\":%s}\n",
                  device.name.UTF8String,
                  metal_ray_tracing_supported?"true":"false");
      return 0;
    }
    if(ray_visibility_smoke_test)return run_ray_visibility_smoke_test(device);
    if(atmosphere_lut_smoke_test)return run_atmosphere_lut_smoke_test(device);
    if(gpu_lod_selector_smoke_test)
      return run_metal_gpu_lod_selector_smoke_test(device)?0:1;
    if(gpu_live_selection_state_smoke_test)
      return run_metal_gpu_live_selection_state_smoke_test(device)?0:1;
    if(gpu_hierarchy_frontier_smoke_test)
      return run_metal_gpu_hierarchy_frontier_smoke_test(device)?0:1;
    if(gpu_compact_green_closure_smoke_test)
      return run_metal_gpu_hierarchy_compact_green_closure_smoke_test(device)?0:1;
    if(gpu_compact_red_scan_large_smoke_test)
      return run_metal_gpu_hierarchy_compact_red_scan_large_smoke_test(device)?0:1;
    if(gpu_compact_live_closure_smoke_test)
      return run_metal_gpu_compact_live_closure_smoke_test(device)?0:1;
    if(gpu_compact_live_parity_smoke_test)
      return run_metal_gpu_compact_live_parity_smoke_test(device)?0:1;
    if(gpu_production_front_parity_smoke_test)
      return run_metal_gpu_production_front_parity_smoke_test(device)?0:1;
    if(gpu_compact_live_performance_smoke_test)
      return run_metal_gpu_compact_live_parity_smoke_test(device,true,true)?0:1;
    if(gpu_compact_hybrid_parity_smoke_test)
      return run_metal_gpu_compact_live_parity_smoke_test(device,false,true)?0:1;
    if(gpu_compact_owner_materialize_smoke_test)
      return run_metal_gpu_compact_owner_materialize_smoke_test(device)?0:1;
    if(gpu_compact_owner_p8_smoke_test)
      return run_metal_gpu_compact_owner_p8_smoke_test(device)?0:1;
    if(gpu_terrain_extract_smoke_test)
      return run_metal_gpu_terrain_extract_smoke_test(device)?0:1;
    if(gpu_terrain_classify_smoke_test)
      return run_metal_gpu_terrain_classify_smoke_test(device)?0:1;
    if(gpu_terrain_triangle_smoke_test)
      return run_metal_gpu_terrain_triangle_smoke_test(device)?0:1;
    if(gpu_terrain_parallel_triangle_smoke_test)
      return run_metal_gpu_terrain_parallel_triangle_smoke_test(device)?0:1;
    if(gpu_terrain_project_smoke_test)
      return run_metal_gpu_terrain_project_smoke_test(device)?0:1;
    if(gpu_terrain_draw_smoke_test)
      return run_metal_gpu_terrain_draw_smoke_test(device)?0:1;
    if(gpu_terrain_native_chain_smoke_test)
      return run_metal_gpu_terrain_native_chain_smoke_test(device)?0:1;
    if(gpu_terrain_live_slots_smoke_test)
      return run_metal_gpu_terrain_live_slots_smoke_test(device)?0:1;
    if(gpu_terrain_runtime_smoke_test)
      return run_metal_gpu_terrain_runtime_smoke_test(device)?0:1;
    if(gpu_terrain_surface_parity_smoke_test)
      return run_metal_gpu_terrain_surface_parity_smoke_test(device)?0:1;
    if(gpu_volume_split_closure_smoke_test)
      return run_metal_gpu_volume_split_closure_smoke_test(device)?0:1;
    if(atmosphere_compiler_check){
      for(std::size_t mode=0;mode<=16U;++mode){
        const auto path=std::filesystem::path(
            TETRA_METAL_ATMOSPHERE_SHADER_DIR)/
            ("atmosphere_mode_"+std::to_string(mode)+".metal");
        id<MTLLibrary> atmosphere_library=make_file_shader_library(
            device,path.string().c_str());
        id<MTLFunction> atmosphere_function=
            [atmosphere_library newFunctionWithName:@"main0"];
        if(atmosphere_library==nil||atmosphere_function==nil){
          if(atmosphere_library!=nil)
            std::fprintf(stderr,
                "Generated atmosphere kernel main0 is missing for mode %zu.\n",
                mode);
          return 1;
        }
      }
      {
        const auto path=std::filesystem::path(
            TETRA_METAL_ATMOSPHERE_SHADER_DIR)/
            "atmosphere_reference_hillaire.metal";
        id<MTLLibrary> reference_library=make_file_shader_library(
            device,path.string().c_str());
        if(reference_library==nil||
           [reference_library newFunctionWithName:@"main0"]==nil)return 1;
      }
      {
        const auto path=std::filesystem::path(
            TETRA_METAL_ATMOSPHERE_SHADER_DIR)/"gpu_lod.comp.metal";
        id<MTLLibrary> selector_library=make_file_shader_library(
            device,path.string().c_str());
        if(selector_library==nil||
           [selector_library newFunctionWithName:@"main0"]==nil)return 1;
      }
      {
        const auto path=std::filesystem::path(
            TETRA_METAL_ATMOSPHERE_SHADER_DIR)/"gpu_terrain_extract.comp.metal";
        id<MTLLibrary> extractor_library=make_file_shader_library(
            device,path.string().c_str());
        if(extractor_library==nil||
           [extractor_library newFunctionWithName:@"main0"]==nil)return 1;
      }
      {
        const auto path=std::filesystem::path(
            TETRA_METAL_ATMOSPHERE_SHADER_DIR)/"gpu_terrain_classify.comp.metal";
        id<MTLLibrary> classifier_library=make_file_shader_library(
            device,path.string().c_str());
        if(classifier_library==nil||
           [classifier_library newFunctionWithName:@"main0"]==nil)return 1;
      }
      constexpr std::array<const char*,11> graphics_shaders{
          "scene.vert","scene.frag","wire.frag","edge.vert","edge.frag",
          "shadow.vert","sky.vert","sky.frag","fullscreen.vert",
          "tone_map.frag","tone_map_faithful.frag"};
      for(const auto* name:graphics_shaders){
        const auto path=std::filesystem::path(
            TETRA_METAL_ATMOSPHERE_SHADER_DIR)/(std::string(name)+".metal");
        id<MTLLibrary> graphics_library=make_file_shader_library(
            device,path.string().c_str());
        id<MTLFunction> graphics_function=
            [graphics_library newFunctionWithName:@"main0"];
        if(graphics_library==nil||graphics_function==nil){
          if(graphics_library!=nil)
            std::fprintf(stderr,"Generated graphics entry point is missing: %s\n",
                         name);
          return 1;
        }
      }
      std::printf("{\"event\":\"metal_atmosphere_compiler\","
                  "\"source_directory\":\"%s\",\"compute_kernels\":19,"
                  "\"reference_kernels\":1,"
                  "\"graphics_stages\":11,"
                  "\"passed\":true}\n",TETRA_METAL_ATMOSPHERE_SHADER_DIR);
      return 0;
    }

    if(hidden_window){
      [NSApplication sharedApplication];
      [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    }
    glfwSetErrorCallback(glfw_error_callback);
    if(!glfwInit())return 1;
    glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE,hidden_window?GLFW_FALSE:GLFW_TRUE);
    GLFWwindow* window=glfwCreateWindow(capture_test?768:
                                        ((smoke_test||motion_test||gpu_terrain_performance_smoke_test||render_test||metalfx_test||
                                          overlay_test||shadow_test||
                                          any_atmosphere_frame_test||soak_test)?
                                             (interactive_capture_resolution?1440:960):1440),
                                        capture_test?480:
                                        ((smoke_test||motion_test||gpu_terrain_performance_smoke_test||render_test||metalfx_test||
                                          overlay_test||shadow_test||
                                          any_atmosphere_frame_test||soak_test)?
                                             (interactive_capture_resolution?900:600):900),
                                        "TetWorldMetal",nullptr,nullptr);
    if(window==nullptr){glfwTerminate();return 1;}

    NSWindow* native_window=glfwGetCocoaWindow(window);
    if(hidden_window){
      [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
      [native_window orderOut:nil];
    }
    CAMetalLayer* layer=[CAMetalLayer layer];
    layer.device=device;
    layer.pixelFormat=MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly=(capture_test||any_atmosphere_frame_test||
                           metalfx_test||metalfx_direct_drawable)?NO:YES;
    layer.maximumDrawableCount=3;
    layer.displaySyncEnabled=YES;
    native_window.contentView.layer=layer;
    native_window.contentView.wantsLayer=YES;

    constexpr MTLPixelFormat depth_format=MTLPixelFormatDepth32Float;
    constexpr MTLPixelFormat scene_colour_format=MTLPixelFormatRGBA16Float;
    constexpr MTLPixelFormat temporal_colour_format=MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> scene_pipeline_1=make_translated_scene_pipeline(
        device,scene_colour_format,depth_format,1U);
    id<MTLRenderPipelineState> scene_pipeline_2=
        [device supportsTextureSampleCount:2U]?
          make_translated_scene_pipeline(device,scene_colour_format,
                                         depth_format,2U):nil;
    id<MTLRenderPipelineState> scene_pipeline_4=
        [device supportsTextureSampleCount:4U]?
          make_translated_scene_pipeline(device,scene_colour_format,
                                         depth_format,4U):nil;
    id<MTLRenderPipelineState> wire_pipeline_1=make_translated_wire_pipeline(
        device,scene_colour_format,depth_format,1U);
    id<MTLRenderPipelineState> wire_pipeline_2=
        [device supportsTextureSampleCount:2U]?
          make_translated_wire_pipeline(device,scene_colour_format,
                                        depth_format,2U):nil;
    id<MTLRenderPipelineState> wire_pipeline_4=
        [device supportsTextureSampleCount:4U]?
          make_translated_wire_pipeline(device,scene_colour_format,
                                        depth_format,4U):nil;
    id<MTLRenderPipelineState> overlay_pipeline_1=make_overlay_pipeline(
        device,library,scene_colour_format,depth_format,1U);
    id<MTLRenderPipelineState> overlay_pipeline_2=
        [device supportsTextureSampleCount:2U]?
          make_overlay_pipeline(device,library,scene_colour_format,depth_format,2U):nil;
    id<MTLRenderPipelineState> overlay_pipeline_4=
        [device supportsTextureSampleCount:4U]?
          make_overlay_pipeline(device,library,scene_colour_format,depth_format,4U):nil;
    id<MTLRenderPipelineState> composite_pipeline=make_composite_pipeline(
        device,library,layer.pixelFormat);
    id<MTLRenderPipelineState> temporal_motion_pipeline=
        make_temporal_motion_pipeline(device,library);
    id<MTLRenderPipelineState> temporal_present_pipeline=
        make_temporal_present_pipeline(device,library,layer.pixelFormat);
    id<MTLRenderPipelineState> temporal_composite_pipeline=
        make_translated_composite_pipeline(
            device,temporal_colour_format,false);
    id<MTLRenderPipelineState> temporal_faithful_composite_pipeline=
        make_translated_composite_pipeline(
            device,temporal_colour_format,true);
    id<MTLRenderPipelineState> shadow_pipeline=make_shadow_pipeline(
        device,library,depth_format);
    id<MTLDepthStencilState> depth_state=make_depth_state(device);
    id<MTLDepthStencilState> overlay_depth_state=make_overlay_depth_state(device);
    id<MTLDepthStencilState> shadow_depth_state=make_shadow_depth_state(device);
    id<MTLTexture> shadow_texture=make_shadow_texture(device,depth_format);
    NSUInteger shadow_texture_resolution=tetra_viewer::shadow_map_resolution;
    id<MTLCommandQueue> command_queue=[device newCommandQueue];
    id<MTLCounterSet> gpu_timestamp_counter_set=timestamp_counter_set(device);
    const bool gpu_stage_timestamps_enabled=[&] {
      const char* value=std::getenv("TETWORLD_METAL_STAGE_TIMESTAMPS");
      return gpu_terrain_performance_smoke_test||
          (value!=nullptr&&std::strcmp(value,"0")!=0);
    }();
    // The owner-direct performance qualification needs thirty actual GPU
    // timestamp pairs, rather than whichever of its infrequent owner jobs
    // happen to acquire one of the three asynchronous flights.  Serialize
    // command completion for that test only, so each completed flight is
    // retired before the next frame chooses a flight.  This does not change
    // normal renderer scheduling or the measured device interval.  Other
    // stage studies remain explicitly opt-in.
    const bool serial_timestamp_collection=gpu_terrain_performance_smoke_test||
        (timing_profile_test&&
         std::getenv("TETWORLD_METAL_SERIAL_STAGE_TIMESTAMPS")!=nullptr);
    std::array<MetalTimestampFlight,gpu_timestamp_flight_count>
        gpu_timestamp_flights{};
    if(gpu_stage_timestamps_enabled)
      for(auto& flight:gpu_timestamp_flights)
        flight=make_timestamp_flight(device,gpu_timestamp_counter_set);
    auto atmosphere_resources=make_live_atmosphere_resources(
        device,layer.pixelFormat,tetra_viewer::AtmosphereQuality::standard);
    // Hillaire's published 200x100 sky-view LUT passed low-sun, ascent,
    // orbital, numeric, and moving-camera qualification. The 384x216 table
    // remains available through the explicit P3 control override above.
    if(sky_view_reference_profile)
      atmosphere_resources.sky_view=make_atmosphere_texture(device,200U,100U);
    const bool metalfx_temporal_supported=
        [MTLFXTemporalScalerDescriptor supportsDevice:device]&&
        temporal_motion_pipeline!=nil&&temporal_present_pipeline!=nil&&
        temporal_composite_pipeline!=nil&&
        temporal_faithful_composite_pipeline!=nil;
    MetalFxTemporalResources metalfx_resources;
    if(scene_pipeline_1==nil||wire_pipeline_1==nil||overlay_pipeline_1==nil||
       composite_pipeline==nil||depth_state==nil||overlay_depth_state==nil||
       shadow_pipeline==nil||shadow_depth_state==nil||shadow_texture==nil||
       command_queue==nil||!live_atmosphere_resources_valid(atmosphere_resources)){
      glfwDestroyWindow(window);glfwTerminate();return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io=ImGui::GetIO();
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window,true);
    ImGui_ImplMetal_Init(device);

    tetra_viewer::FirstPersonController controller;
    controller.state().feet=tetra_viewer::default_world_camera_feet;
    controller.state().yaw=tetra_viewer::default_world_camera_yaw_radians;
    controller.state().pitch=tetra_viewer::default_world_camera_pitch_radians;
    if(capture_view){
      const auto& values=*capture_view_coordinates;
      const tetra::Vec3 camera_position{values[0],values[1],values[2]};
      const tetra::Vec3 target{values[3],values[4],values[5]};
      const auto direction=target-camera_position;
      const double length=std::sqrt(direction.x*direction.x+
          direction.y*direction.y+direction.z*direction.z);
      if(length<=1.0e-12){
        std::fprintf(stderr,"capture view camera and target must differ\n");
        glfwDestroyWindow(window);glfwTerminate();return 2;
      }
      const tetra_viewer::FirstPersonConfiguration configuration;
      controller.state().feet=camera_position-
          tetra::Vec3{0.0,configuration.eye_height,0.0};
      controller.state().yaw=std::atan2(direction.x,direction.z);
      // Match the interactive controller's pitch limit.  A precisely vertical
      // capture direction is parallel to the fixed world-up vector and cannot
      // form the camera basis required by terrain LOD projection.
      controller.state().pitch=std::clamp(std::asin(std::clamp(
          direction.y/length,-1.0,1.0)),-1.52,1.52);
    }
    int width=1,height=1;
    glfwGetFramebufferSize(window,&width,&height);
    auto camera=controller.camera(
        static_cast<double>(std::max(height,1)),
        static_cast<double>(std::max(width,1))/std::max(height,1));
    const auto world_profile=tetra_viewer::production_world_profile();
    // Seed the streaming runtime from its small certified bootstrap front and
    // submit the real camera through the normal update path below.  Building
    // a cold front directly at a high-altitude camera can legitimately exceed
    // the hierarchy budget before the runtime has an older coherent front to
    // retain, which made both interactive startup and visual captures abort.
    auto runtime_startup=tetra_viewer::make_production_terrain_runtime_async(
        world_profile);
    std::unique_ptr<tetra_viewer::TerrainRuntime> runtime;
    tetra_viewer::TerrainRuntimeDiagnostics diagnostics;
    tetra_viewer::TerrainFrontCoordinator terrain_front_coordinator;
    tetra_viewer::PreviewSurfaceWorker preview_surface_worker;
    const tetra_viewer::PreviewSurfaceConfiguration preview_configuration{
        // Six welded rings extend beyond the ground-view horizon while the
        // 48-cell outer rings retain enough shape to represent mountains,
        // rather than flattening the horizon into a coarse silhouette.
        .level_count=6U,.cells_per_side=48U,.finest_spacing=0.125};
    tetra_viewer::TerrainDisplayPublicationPlanner terrain_display_planner;
    MetalTerrainDisplayFront terrain_display_front;
    // Preserve the complete CPU bootstrap publication, not only its buffer
    // identities.  The interactive device-front toggle restores this atomically
    // before letting the CPU runtime resume publication.
    MetalTerrainDisplayFront device_front_bootstrap_display;
    MetalGpuTerrainActiveFront gpu_terrain_active_front;
    // Keep the one permitted CPU bootstrap draw handles solely as a fallback
    // identity witness.  Device-front P8 rejection must continue drawing
    // these handles; no failed private front may become visible.
    id<MTLBuffer> device_front_bootstrap_vertices=nil;
    id<MTLBuffer> device_front_bootstrap_indirect_arguments=nil;
    std::array<MetalGpuTerrainDiagnosticSlot,3> gpu_terrain_slots;
    std::uint64_t gpu_terrain_slot_cursor{};
    std::array<MetalGpuTerrainNativeDiagnosticSlot,3>
        gpu_terrain_native_slots;
    MetalGpuTerrainPacketUpload gpu_terrain_packet_upload;
    std::uint64_t gpu_terrain_native_slot_cursor{};
    bool gpu_terrain_renderer_available{};
    MetalGpuHierarchyLiveSelection gpu_hierarchy_live_selection;
    auto gpu_terrain_counters=
        std::make_shared<MetalGpuTerrainDiagnosticCounters>();
    std::size_t peak_terrain_display_transition_bytes{};
    std::uint64_t next_terrain_render_generation{1U};
    id<MTLBuffer> scene_vertices=nil;
    id<MTLBuffer> player_overlay_vertices=nil;
    std::size_t player_overlay_vertex_count{};
    id<MTLBuffer> lod_overlay_vertices=nil;
    std::size_t lod_overlay_vertex_count{};
    std::uint64_t lod_overlay_epoch=std::numeric_limits<std::uint64_t>::max();
    std::uint64_t lod_overlay_generation{};
    std::size_t scene_vertex_count{};
    std::uint64_t uploaded_generation{};
    MetalTerrainAccelerationStructure terrain_acceleration_structure;
    id<MTLBuffer> terrain_ray_oracle_inputs=nil;
    id<MTLBuffer> terrain_ray_oracle_outputs=nil;
    std::vector<std::uint32_t> terrain_ray_oracle_expected;
    std::size_t terrain_ray_oracle_triangles{};
    bool terrain_ray_oracle_encoded=false;
    id<MTLTexture> scene_colour_texture=nil;
    id<MTLTexture> multisample_colour_texture=nil;
    id<MTLTexture> depth_texture=nil;
    id<MTLTexture> multisample_depth_texture=nil;
    int render_width{},render_height{};
    NSUInteger allocated_samples{};
    std::array<std::array<float,16>,tetra_viewer::shadow_cascade_count>
        cached_shadow_matrices{};
    std::array<std::uint64_t,tetra_viewer::shadow_cascade_count>
        cached_shadow_generations{};
    std::array<bool,tetra_viewer::shadow_cascade_count> shadow_initialized{};
    std::uint64_t shadow_cascade_refreshes{};
    std::array<std::size_t,tetra_viewer::shadow_cascade_count>
        shadow_cpu_candidates{};
    std::array<float,16> cached_fitted_shadow_matrix{};
    std::uint64_t cached_fitted_shadow_generation{};
    bool fitted_shadow_initialized=false;
    std::uint64_t fitted_shadow_refreshes{};
    bool pointer_captured=!automated_test;
    bool free_fly=true;
    bool show_surface_edges=overlay_test;
    bool smooth_normals=true;
    bool paused=false;
    bool single_step=false;
    bool lock_lod_camera=false;
    bool show_capsule=overlay_test;
    bool show_contact_normal=overlay_test;
    bool show_lod_zones=overlay_test;
    bool vsync=true;
    bool animate_sun=false;
    float sun_azimuth=tetra_viewer::default_world_sun_azimuth_radians;
    float sun_elevation=atmosphere_60_degree_test?
        60.0F*std::numbers::pi_v<float>/180.0F:
        tetra_viewer::default_world_sun_elevation_radians;
    if(timing_profile_test&&
       timing_profile_class==TimingProfileClass::terminator){
      // Deterministic below-horizon fixture for P5b. It preserves the normal
      // screen marcher, but gives the solid-planet umbra enough coverage to
      // measure direct-light work that is mathematically zero.
      sun_elevation=-8.0F*std::numbers::pi_v<float>/180.0F;
    }
    if(atmosphere_mountain_capture){
      // The originally reported back-lit mountain pose.  Capture diagnostics
      // use this fixed camera/sun pair, while ordinary interactive startup is
      // left entirely unchanged.
      controller.state().feet={117.761,16.141,-134.089};
      controller.state().yaw=135.7*std::numbers::pi/180.0;
      controller.state().pitch=0.1*std::numbers::pi/180.0;
      sun_azimuth=-53.0*std::numbers::pi/180.0;
      sun_elevation=tetra_viewer::default_world_sun_elevation_radians;
      const auto override_position=[&](const char* name,double& value){
        const char* text=std::getenv(name);
        if(text==nullptr||text[0]=='\0')return true;
        char* end=nullptr;
        const double parsed=std::strtod(text,&end);
        if(end==text||*end!='\0'||!std::isfinite(parsed)||
           std::abs(parsed)>65'536.0){
          std::fprintf(stderr,
              "%s must be finite within +/-65536 world units\n",name);
          return false;
        }
        value=parsed;
        return true;
      };
      if(!override_position("TETWORLD_METAL_MOUNTAIN_FEET_X",
                            controller.state().feet.x)||
         !override_position("TETWORLD_METAL_MOUNTAIN_FEET_Y",
                            controller.state().feet.y)||
         !override_position("TETWORLD_METAL_MOUNTAIN_FEET_Z",
                            controller.state().feet.z)){
        glfwDestroyWindow(window);glfwTerminate();return 2;
      }
      if(const char* offset_text=std::getenv(
             "TETWORLD_METAL_MOUNTAIN_YAW_OFFSET_DEGREES");
         offset_text!=nullptr&&offset_text[0]!='\0'){
        char* end=nullptr;
        const double offset=std::strtod(offset_text,&end);
        if(end==offset_text||*end!='\0'||!std::isfinite(offset)||
           std::abs(offset)>15.0){
          std::fprintf(stderr,
              "TETWORLD_METAL_MOUNTAIN_YAW_OFFSET_DEGREES must be finite within +/-15\n");
          glfwDestroyWindow(window);glfwTerminate();return 2;
        }
        controller.state().yaw+=offset*std::numbers::pi/180.0;
      }
      const auto override_degrees=[&](const char* name,double minimum,
                                      double maximum,float& radians){
        const char* text=std::getenv(name);
        if(text==nullptr||text[0]=='\0')return true;
        char* end=nullptr;
        const double degrees=std::strtod(text,&end);
        if(end==text||*end!='\0'||!std::isfinite(degrees)||
           degrees<minimum||degrees>maximum){
          std::fprintf(stderr,"%s must be finite within [%.0f, %.0f]\n",
                       name,minimum,maximum);
          return false;
        }
        radians=static_cast<float>(degrees*std::numbers::pi/180.0);
        return true;
      };
      float pitch_override=static_cast<float>(controller.state().pitch);
      if(!override_degrees("TETWORLD_METAL_MOUNTAIN_PITCH_DEGREES",
                           -89.0,89.0,pitch_override)||
         !override_degrees("TETWORLD_METAL_MOUNTAIN_SUN_AZIMUTH_DEGREES",
                           -180.0,180.0,sun_azimuth)||
         !override_degrees("TETWORLD_METAL_MOUNTAIN_SUN_ELEVATION_DEGREES",
                           -90.0,90.0,sun_elevation)){
        glfwDestroyWindow(window);glfwTerminate();return 2;
      }
      controller.state().pitch=pitch_override;
    }
    if(atmosphere_visible_sun_capture){
      // Deterministic clear-sky solar-disc qualification. Aim directly at a
      // raised sun from the reported terrain location so the same executable
      // can prove both clear visibility and mountain occlusion headlessly.
      controller.state().feet={117.761,16.148,-134.429};
      sun_azimuth=-51.5*std::numbers::pi/180.0;
      sun_elevation=20.0*std::numbers::pi/180.0;
      controller.state().yaw=std::numbers::pi/2.0-sun_azimuth;
      controller.state().pitch=sun_elevation;
    }
    if(atmosphere_capture_pose!=nullptr){
      // World coordinates are ten metres per unit.  The gameplay planet's
      // north-pole surface is y=0.5, its atmosphere ends at y=2000.5, and the
      // explicit orbital poses sit well outside it.  These views mirror the
      // documented native Metal qualification matrix rather than borrowing
      // the older Vulkan capture utility.
      if(std::strcmp(atmosphere_capture_pose,"flight")==0){
        controller.state().feet={0.5,100.5,0.5};
        controller.state().yaw=std::numbers::pi;
        controller.state().pitch=-5.0*std::numbers::pi/180.0;
        sun_azimuth=-103.1324F*std::numbers::pi_v<float>/180.0F;
        sun_elevation=25.0F*std::numbers::pi_v<float>/180.0F;
      }else if(std::strcmp(atmosphere_capture_pose,"atmosphere-top")==0){
        controller.state().feet={0.5,2000.5,0.5};
        controller.state().yaw=std::numbers::pi;
        controller.state().pitch=-38.5*std::numbers::pi/180.0;
        sun_azimuth=-103.1324F*std::numbers::pi_v<float>/180.0F;
        sun_elevation=10.0F*std::numbers::pi_v<float>/180.0F;
      }else {
        controller.state().feet={0.5,25000.5,0.5};
        controller.state().yaw=std::numbers::pi;
        controller.state().pitch=(std::strcmp(atmosphere_capture_pose,
            "orbit-motion-a")==0?-88.00:-87.96)*std::numbers::pi/180.0;
        sun_azimuth=-103.1324F*std::numbers::pi_v<float>/180.0F;
        sun_elevation=5.0F*std::numbers::pi_v<float>/180.0F;
      }
    }
    float sun_orbit_azimuth=sun_azimuth;
    double sun_orbit_phase=sun_elevation;
    double sun_cycle_seconds=tetra_viewer::default_world_sun_cycle_seconds;
    double exposure_ev=-0.62;
    bool atmosphere_enabled=!automated_test||any_atmosphere_frame_test||
                            metalfx_test||profile_interactive_rendering||
                            atmosphere_quality_test;
    // The independent Hillaire 2020 screen marcher is the qualified
    // production transport. It evaluates terrain visibility four times in
    // every retained direct-scattering interval and avoids the coherent
    // silhouette bands of the legacy midpoint marcher.
    int atmosphere_transport=2;
    if(const char* requested_transport=std::getenv(
           "TETWORLD_METAL_ATMOSPHERE_TRANSPORT");
       requested_transport!=nullptr&&requested_transport[0]!='\0'){
      char* end=nullptr;
      const long value=std::strtol(requested_transport,&end,10);
      if(end==requested_transport||*end!='\0'||value<0L||value>2L){
        std::fprintf(stderr,
            "TETWORLD_METAL_ATMOSPHERE_TRANSPORT must be an integer 0..2\n");
        glfwDestroyWindow(window);glfwTerminate();return 2;
      }
      atmosphere_transport=static_cast<int>(value);
    }
    if(timing_profile_test&&
       timing_profile_class==TimingProfileClass::ray_tracing)
      atmosphere_transport=1;
    // Metal defaults to the qualified temporal screen marcher. The legacy
    // current-qualified fragment path remains selectable for comparison, but
    // its coherent low-sun visibility bands are not suitable as production
    // output.
    int atmosphere_renderer=atmosphere_froxel_test?4:3;
    if(const char* requested_renderer=std::getenv(
           "TETWORLD_METAL_ATMOSPHERE_RENDERER");
       requested_renderer!=nullptr&&requested_renderer[0]!='\0'){
      char* end=nullptr;
      const long value=std::strtol(requested_renderer,&end,10);
      if(end==requested_renderer||*end!='\0'||value<0L||value>4L){
        std::fprintf(stderr,
            "TETWORLD_METAL_ATMOSPHERE_RENDERER must be an integer 0..4\n");
        glfwDestroyWindow(window);glfwTerminate();return 2;
      }
      atmosphere_renderer=static_cast<int>(value);
    }
    int atmosphere_debug_view=0;
    // Headless captures can select an existing physical transport diagnostic
    // without inventing a second compositing path.  This keeps direct and
    // multiple-scattering evidence reproducible from the normal executable.
    if(const char* requested_debug=std::getenv(
           "TETWORLD_METAL_ATMOSPHERE_DEBUG_VIEW");
       requested_debug!=nullptr&&requested_debug[0]!='\0'){
      char* end=nullptr;
      const long value=std::strtol(requested_debug,&end,10);
      if(end==requested_debug||*end!='\0'||value<0L||value>=31L){
        std::fprintf(stderr,
            "TETWORLD_METAL_ATMOSPHERE_DEBUG_VIEW must be an integer 0..30\n");
        glfwDestroyWindow(window);glfwTerminate();return 2;
      }
      atmosphere_debug_view=static_cast<int>(value);
    }
    // The aerial volume has no normal reference-temporal consumer. This
    // profile deliberately selects the existing aerial diagnostic so it times
    // allocation and refresh of a real sampled resource without making that
    // work part of ordinary reference frames.
    if(timing_profile_test&&
       timing_profile_class==TimingProfileClass::aerial_refresh)
      atmosphere_debug_view=4;
    int shadow_integration=atmosphere_epipolar_test?5:
        (atmosphere_minmax_test?2:0);
    int shadow_bias=1;
    int shadow_filter=2;
    int atmosphere_preset_index=0;
    int atmosphere_quality_index=1;
    int atmosphere_screen_divisor=atmosphere_quarter_test?4:2;
    int atmosphere_visibility_backend=atmosphere_fallback_test?2:0;
    bool ios_performance_mode=atmosphere_quarter_test;
    double atmosphere_aerial_range=
        tetra_viewer::default_world_aerial_distance_metres;
    auto atmosphere_parameters=tetra_viewer::atmosphere_preset(
        tetra_viewer::default_world_atmosphere_preset);
    atmosphere_parameters.metres_per_world_unit=10.0;
    tetra::Sphere terrain_field;
    terrain_field.kind=world_profile.shape;
    terrain_field.terrain=world_profile.terrain;
    terrain_field.secondary=world_profile.octave_detail_amplitude;
    terrain_field.frequency=world_profile.octave_detail_frequency;
    const double maximum_terrain_relief_metres=
        tetra::terrain_height_magnitude_bound(terrain_field)*
        atmosphere_parameters.metres_per_world_unit;
    atmosphere_parameters=tetra_viewer::adapt_compact_atmosphere_to_relief(
        atmosphere_parameters,maximum_terrain_relief_metres);
    bool atmosphere_optical_dirty=true;
    std::array<float,96> atmosphere_uniform{};
    std::array<float,96> stable_atmosphere_lookup_uniform{};
    bool force_runtime_camera=false;
    bool runtime_camera_interactive=false;
    // 0 native, 1 fixed, 2 automatic. Automated captures stay native and
    // single-sampled so their depth oracle remains pixel-aligned with Vulkan,
    // except the explicit P6b final-drawable raster qualification.
    // P2 profiles use a fixed 0.70 scale.  Allowing the controller to react
    // to a deliberately expensive refresh/motion class changes the pixel
    // population under measurement and makes its percentile incomparable to
    // the steady class.
    int render_resolution_mode=auto_resolution_test?2:
        ((timing_profile_test||raster_profile_qualification)?1:
         profile_interactive_rendering?2:
        (metalfx_test?1:(automated_test?0:1)));
    // P6b selected this fixed profile after native image, temporal, orbital,
    // and repeated timing qualification against the former 0.70/2x control.
    float fixed_render_scale=(timing_profile_test||raster_profile_qualification)?
        0.70F:0.50F;
    if(timing_profile_test||raster_profile_qualification){
      if(const char* value=std::getenv("TETWORLD_METAL_TIMING_PROFILE_SCALE");
         value!=nullptr&&value[0]!='\0'){
        char* end=nullptr;
        const float parsed=std::strtof(value,&end);
        if(end==value||*end!='\0'||!std::isfinite(parsed)||
           parsed<1.0F/3.0F||parsed>1.0F){
          std::fprintf(stderr,
              "TETWORLD_METAL_TIMING_PROFILE_SCALE must be 0.333..1\n");
          glfwDestroyWindow(window);glfwTerminate();return 2;
        }
        fixed_render_scale=parsed;
      }
    }
    // P9 consumes only the two P6-qualified MetalFX/2x profiles.  It never
    // changes atmosphere sampling, shadow coverage, or MSAA dynamically.
    const std::vector<tetra_viewer::MetalRasterQualityProfile>
        automatic_quality_profiles{{0.5F,2U},{0.7F,2U}};
    float automatic_render_scale=0.5F;
    int display_refresh_hz=std::max(1,static_cast<int>(
        native_window.screen.maximumFramesPerSecond));
    bool automatic_target_display=true;
    int automatic_target_fps=display_refresh_hz;
    tetra_viewer::MetalQualityController automatic_quality_controller(
        automatic_quality_profiles,0U,
        1000.0/static_cast<double>(automatic_target_fps));
    float upscale_sharpening=0.2F;
    bool metalfx_temporal_enabled=
        (!automated_test||metalfx_test||profile_interactive_rendering)&&
                                  metalfx_temporal_supported;
    std::optional<tetra_viewer::CameraProjection> previous_temporal_projection;
    std::optional<std::uint64_t> previous_temporal_visual_signature;
    tetra::Vec3 previous_temporal_render_origin{};
    std::uint64_t previous_temporal_scene_generation{};
    std::uint64_t metalfx_frame_index{};
    bool terrain_msaa=(!automated_test||profile_interactive_rendering)&&
                      scene_pipeline_4!=nil;
    int terrain_sample_count=scene_pipeline_2!=nil?2:
                             (scene_pipeline_4!=nil?4:1);
    if(timing_profile_test||raster_profile_qualification){
      if(const char* value=std::getenv("TETWORLD_METAL_TIMING_PROFILE_MSAA");
         value!=nullptr&&value[0]!='\0'){
        char* end=nullptr;
        const long samples=std::strtol(value,&end,10);
        if(end==value||*end!='\0'||(samples!=1L&&samples!=2L&&samples!=4L)||
           (samples==2L&&scene_pipeline_2==nil)||
           (samples==4L&&scene_pipeline_4==nil)){
          std::fprintf(stderr,
              "TETWORLD_METAL_TIMING_PROFILE_MSAA must be supported 1, 2, or 4\n");
          glfwDestroyWindow(window);glfwTerminate();return 2;
        }
        terrain_msaa=samples>1L;
        terrain_sample_count=static_cast<int>(samples);
      }
    }
    std::size_t automatic_stable_frames{};
    std::uint64_t automatic_quality_changes{};
    tetra_viewer::MetalQualityChange automatic_last_change=
        tetra_viewer::MetalQualityChange::none;
    double automatic_gpu_median_milliseconds{};
    double automatic_gpu_percentile_95_milliseconds{};
    auto gpu_frame_milliseconds=
        std::make_shared<std::atomic<double>>(0.0);
    auto gpu_frame_sequence=std::make_shared<std::atomic<std::uint64_t>>(0U);
    auto gpu_frame_maintenance=std::make_shared<std::atomic<bool>>(false);
    auto gpu_frame_moving=std::make_shared<std::atomic<bool>>(false);
    auto gpu_stage_timings=std::make_shared<MetalGpuStageTimings>();
    auto cpu_submission_milliseconds=
        std::make_shared<std::atomic<double>>(0.0);
    auto timing_profile_samples=std::make_shared<MetalTimingProfileSamples>();
    std::uint64_t consumed_gpu_frame_sequence{};
    glfwSetInputMode(window,GLFW_CURSOR,pointer_captured?
                     GLFW_CURSOR_DISABLED:GLFW_CURSOR_NORMAL);
    double previous_cursor_x{},previous_cursor_y{};
    glfwGetCursorPos(window,&previous_cursor_x,&previous_cursor_y);
    auto previous_time=std::chrono::steady_clock::now();
    // Production terrain publication is intentionally background work.  The
    // debug configuration can take longer than the old 60 s automation cap
    // on a busy desktop, while normal interactive startup must remain
    // non-blocking.  Give qualification runs a bounded but realistic window.
#ifdef NDEBUG
    constexpr int basic_automation_timeout_seconds=60;
#else
    constexpr int basic_automation_timeout_seconds=180;
#endif
    // P8b is specifically a no-stall live-motion gate.  A qualified private
    // front must make observable progress quickly; retain the longer timeout
    // for the unrelated image and timing automation suites.
    const int smoke_timeout_seconds=
        (metal_gpu_terrain_device_front&&motion_test)?
        device_front_smoke_timeout_seconds:
        (metal_gpu_terrain_private_front_qualification||
         gpu_terrain_performance_smoke_test)?120:
        (any_atmosphere_frame_test||metalfx_test||timing_profile_test||motion_test||soak_test?300:
         basic_automation_timeout_seconds);
    const auto smoke_deadline=previous_time+
        std::chrono::seconds(smoke_timeout_seconds);
    const auto motion_start=controller.state().feet;
    std::size_t motion_rendered_frames{};
    std::size_t render_test_frames{};
    std::size_t metalfx_test_frames{};
    std::size_t soak_rendered_frames{};
    // Three deterministic route samples per simulated second leave enough
    // wall-clock budget for exact-front handoffs on the full altitude sweep.
    constexpr std::size_t soak_simulated_frames=900U;
    std::size_t metalfx_generation_changes{};
    id<MTLBuffer> metalfx_motion_probe_buffer=nil;
    id<MTLBuffer> metalfx_reactive_probe_buffer=nil;
    NSUInteger metalfx_motion_probe_row_bytes{};
    NSUInteger metalfx_reactive_probe_row_bytes{};
    std::size_t auto_resolution_test_frames{};
    const std::size_t auto_resolution_required_frames=
        std::getenv("TETWORLD_METAL_AUTO_LONG_SESSION")!=nullptr?1200U:
        (profile_interactive_rendering?300U:240U);
    std::size_t overlay_test_frames{};
    std::size_t shadow_test_frames{};
    std::uint64_t wireframe_draws{};
    std::size_t atmosphere_test_frames{};
    bool lookup_invalidation_sun_changed{};
    std::size_t atmosphere_quality_test_frames{};
    bool atmosphere_quality_switches_ok=true;
    std::size_t low_atmosphere_allocation{};
    std::size_t default_atmosphere_allocation{};
    std::size_t high_atmosphere_allocation{};
    // Exact handoff is an event, not a durable coordinator state: normal
    // interactive use is allowed to request the next preview immediately.
    // A timing profile instead needs a stable post-handoff population.
    bool timing_profile_exact_handoff_observed{};

    const auto apply_atmosphere_quality=[&](int quality_index){
      auto replacement=make_live_atmosphere_resources(
          device,layer.pixelFormat,
          static_cast<tetra_viewer::AtmosphereQuality>(quality_index));
      if(!live_atmosphere_resources_valid(replacement))return false;
      atmosphere_resources=std::move(replacement);
      atmosphere_quality_index=quality_index;
      const auto quality=tetra_viewer::atmosphere_quality_settings(
          static_cast<tetra_viewer::AtmosphereQuality>(quality_index));
      shadow_texture_resolution=quality.shadow_resolution;
      shadow_texture=make_shadow_texture(
          device,depth_format,shadow_texture_resolution);
      shadow_initialized.fill(false);
      fitted_shadow_initialized=false;
      if(render_width>0&&render_height>0&&
         !ensure_screen_atmosphere_resources(
             device,atmosphere_resources,render_width,render_height,
             atmosphere_screen_divisor,atmosphere_transport!=2))
        return false;
      atmosphere_optical_dirty=true;
      return shadow_texture!=nil;
    };
    const auto apply_visibility_settings=[&](){
      const auto plan=tetra_viewer::resolve_atmosphere_visibility_plan(
          {.requested=static_cast<tetra_viewer::AtmosphereVisibilityBackend>(
               atmosphere_visibility_backend),
           .ios_performance_mode=ios_performance_mode},
          metal_ray_tracing_supported&&
              terrain_acceleration_structure.active!=nil&&
              terrain_acceleration_structure.active_generation==
                  terrain_display_front.render_generation&&
              atmosphere_resources.ray_visibility_pipeline!=nil);
      atmosphere_screen_divisor=plan.screen_divisor;
      return ensure_screen_atmosphere_resources(
          device,atmosphere_resources,render_width,render_height,
          atmosphere_screen_divisor,atmosphere_transport!=2);
    };

    const auto publish_terrain_display=[&](
        std::shared_ptr<const tetra_viewer::PreviewSurfaceFront> preview,
        std::optional<tetra_viewer::PreviewRequestIdentity> preview_identity){
      if(!runtime)return false;
      // The device-front route permits exactly one already-complete CPU
      // display publication as its bootstrap.  Any later call would construct
      // a CPU display/surface candidate and is a provenance violation.
      if(metal_gpu_terrain_device_front&&terrain_display_front.ready())
        gpu_terrain_counters->cpu_surface_build_requests.fetch_add(
            1U,std::memory_order_relaxed);
      const auto exact_generation=runtime->diagnostics().scene_generation;
      const auto exact_view=runtime->published_view_identity();
      const auto& exact_scene=runtime->scene();
      if(exact_generation==0U||!exact_view.valid()||
         exact_scene.triangle_vertices.empty())return false;
      if(static_cast<bool>(preview)!=preview_identity.has_value())
        throw std::logic_error("preview display identity is incomplete");
      if(preview&&
         (preview->field_revision()!=exact_view.field_revision||
          preview->field_signature()!=exact_view.field_signature))return false;

      const tetra_viewer::TerrainDisplayIdentity identity{
          exact_generation,exact_view,preview_identity,exact_scene.render_origin};
      std::optional<tetra_viewer::TerrainDisplayComposition> composition;
      std::size_t upload_bytes{};
      double composition_milliseconds{};
      try {
        if(preview){
          const auto composition_started=std::chrono::steady_clock::now();
          composition.emplace(tetra_viewer::compose_terrain_display(
              {},exact_scene.render_origin,
              runtime->field(),*preview));
          composition_milliseconds=
              std::chrono::duration<double,std::milli>(
                  std::chrono::steady_clock::now()-composition_started).count();
          upload_bytes=composition->metrics.upload_bytes;
        }
      }catch(const std::exception& error){
        std::fprintf(stderr,"Preview display composition failed: %s\n",
                     error.what());
        return false;
      }
      constexpr std::size_t maximum_preview_upload_bytes=16U*1024U*1024U;
      if(!terrain_display_planner.prepare(
             identity,upload_bytes,maximum_preview_upload_bytes))return false;

      MetalTerrainDisplayFront candidate;
      candidate.identity=identity;
      candidate.preview_cpu=std::move(preview);
      candidate.upload_bytes=upload_bytes;
      if(!composition){
        candidate.exact_vertex_count=exact_scene.triangle_vertices.size();
        const auto bytes=exact_scene.triangle_vertices.size()*
            sizeof(tetra_viewer::SceneVertex);
        candidate.exact_vertices=[device
            newBufferWithBytes:exact_scene.triangle_vertices.data()
                        length:bytes options:MTLResourceStorageModeShared];
      }
      if(composition){
        candidate.indexed_exact_selection=true;
        const auto make_buffer=[&](const auto& values)->id<MTLBuffer>{
          if(values.empty())return nil;
          return [device newBufferWithBytes:values.data()
                                    length:values.size()*sizeof(values.front())
                                   options:MTLResourceStorageModeShared];
        };
        // The preview is a self-contained, welded display front.  Do not
        // retain an exact draw at its boundary: partial exact ownership caused
        // both overlapping coarse sheets and non-watertight skirts.
        candidate.exact_index_count=0U;
        candidate.preview_vertices=make_buffer(composition->preview_vertices);
        candidate.preview_vertex_count=composition->preview_vertices.size();
        candidate.preview_indices=make_buffer(composition->preview_indices);
        candidate.preview_index_count=composition->preview_indices.size();
      }
      const bool upload_succeeded=composition?
          candidate.preview_vertices!=nil&&candidate.preview_indices!=nil:
          candidate.exact_vertices!=nil;
      const std::size_t transition_owned_bytes=
          terrain_display_front.upload_bytes+candidate.upload_bytes;
      peak_terrain_display_transition_bytes=std::max(
          peak_terrain_display_transition_bytes,transition_owned_bytes);
      if(automated_test&&composition)std::printf(
          "{\"event\":\"metal_preview_candidate\","
          "\"exact_input_triangles\":%zu,"
          "\"exact_selected_triangles\":%zu,"
          "\"exact_suppressed_triangles\":%zu,"
          "\"preview_triangles\":%zu,\"upload_bytes\":%zu,"
          "\"prior_upload_bytes\":%zu,"
          "\"transition_owned_bytes\":%zu,"
          "\"peak_transition_owned_bytes\":%zu,"
          "\"upload_succeeded\":%s,"
          "\"build_ms\":%.6f,\"composition_ms\":%.6f,"
          "\"minimum_y\":%.6f,\"maximum_y\":%.6f,"
          "\"exact_minimum_y\":%.6f,\"exact_maximum_y\":%.6f}\n",
          composition->metrics.exact_input_triangles,
          composition->metrics.exact_selected_triangles,
          composition->metrics.exact_suppressed_triangles,
          composition->metrics.preview_triangles,upload_bytes,
          terrain_display_front.upload_bytes,transition_owned_bytes,
          peak_terrain_display_transition_bytes,
          upload_succeeded?"true":"false",
          candidate.preview_cpu->diagnostics().build_milliseconds,
          composition_milliseconds,
          candidate.preview_cpu->covered_world_bounds().minimum.y,
          candidate.preview_cpu->covered_world_bounds().maximum.y,
          composition->metrics.suppressed_minimum_world_y,
          composition->metrics.suppressed_maximum_world_y);
      if(!terrain_display_planner.complete(
             identity,upload_succeeded,identity))return false;
      candidate.render_generation=next_terrain_render_generation++;
      if(next_terrain_render_generation==0U)next_terrain_render_generation=1U;
      terrain_display_front=std::move(candidate);
      if(metal_gpu_terrain_device_front&&device_front_bootstrap_vertices==nil&&
         device_front_bootstrap_indirect_arguments==nil&&!terrain_display_front.preview_cpu){
        device_front_bootstrap_vertices=terrain_display_front.exact_vertices;
        device_front_bootstrap_indirect_arguments=
            terrain_display_front.exact_indirect_arguments;
        device_front_bootstrap_display=terrain_display_front;
      }
      // Seed a fresh private active front from the complete CPU publication.
      // This is upload, not readback: subsequent GPU candidates replace it
      // only through P8's checked private-to-private commit passes.
      gpu_terrain_renderer_available=false;
      if(!composition&&gpu_terrain_draw_pipeline!=nil&&
         exact_scene.triangle_vertices.size()<=
             std::numeric_limits<std::uint32_t>::max()){
        const auto active_bytes=exact_scene.triangle_vertices.size()*
            sizeof(tetra_viewer::SceneVertex);
        const std::array<std::uint32_t,4> initial_arguments{
            static_cast<std::uint32_t>(exact_scene.triangle_vertices.size()),
            1U,0U,0U};
        gpu_terrain_active_front.vertices=[device newBufferWithLength:active_bytes
            options:MTLResourceStorageModePrivate];
        gpu_terrain_active_front.indirect_arguments=[device newBufferWithLength:
            sizeof(initial_arguments) options:MTLResourceStorageModePrivate];
        gpu_terrain_active_front.seed_vertices=[device newBufferWithBytes:
            exact_scene.triangle_vertices.data() length:active_bytes
            options:MTLResourceStorageModeShared];
        gpu_terrain_active_front.seed_arguments=[device newBufferWithBytes:
            initial_arguments.data() length:sizeof(initial_arguments)
            options:MTLResourceStorageModeShared];
        if(gpu_terrain_active_front.vertices!=nil&&
           gpu_terrain_active_front.indirect_arguments!=nil&&
           gpu_terrain_active_front.seed_vertices!=nil&&
           gpu_terrain_active_front.seed_arguments!=nil){
          gpu_terrain_active_front.identity=identity;
          gpu_terrain_active_front.vertex_capacity=initial_arguments[0U];
          gpu_terrain_active_front.seed_pending=true;
          gpu_terrain_active_front.promoted=false;
          gpu_terrain_active_front.completed->store(false,
              std::memory_order_release);
        }else gpu_terrain_active_front={};
      }
      scene_vertices=terrain_display_front.preview_cpu?
          terrain_display_front.preview_vertices:terrain_display_front.exact_vertices;
      scene_vertex_count=terrain_display_front.triangle_count()*3U;
      uploaded_generation=exact_generation;
      terrain_acceleration_structure.maximum_vertex_radius_world=0.0F;
      const auto include_radius=[&](
          std::span<const tetra_viewer::SceneVertex> vertices){
        for(const auto& vertex:vertices)
          terrain_acceleration_structure.maximum_vertex_radius_world=std::max(
              terrain_acceleration_structure.maximum_vertex_radius_world,
              std::sqrt(vertex.position[0]*vertex.position[0]+
                        vertex.position[1]*vertex.position[1]+
                        vertex.position[2]*vertex.position[2]));
      };
      if(composition)include_radius(composition->preview_vertices);
      else include_radius(exact_scene.triangle_vertices);
      return true;
    };

    while(!glfwWindowShouldClose(window)){
      @autoreleasepool {
        glfwPollEvents();
        const auto now=std::chrono::steady_clock::now();
        const double elapsed=std::min(0.1,
            std::chrono::duration<double>(now-previous_time).count());
        previous_time=now;
        if(animate_sun){
          sun_orbit_phase=tetra_viewer::advance_world_sun_orbit_phase(
              sun_orbit_phase,elapsed,sun_cycle_seconds);
          const auto angles=tetra_viewer::world_sun_orbit_angles(
              sun_orbit_azimuth,sun_orbit_phase);
          sun_azimuth=static_cast<float>(angles.azimuth_radians);
          sun_elevation=static_cast<float>(angles.elevation_radians);
        }

        if(key_down(window,GLFW_KEY_ESCAPE)&&pointer_captured){
          pointer_captured=false;
          glfwSetInputMode(window,GLFW_CURSOR,GLFW_CURSOR_NORMAL);
        }
        double cursor_x{},cursor_y{};
        glfwGetCursorPos(window,&cursor_x,&cursor_y);
        bool camera_changed=false;
        if(pointer_captured){
          const double dx=cursor_x-previous_cursor_x;
          const double dy=cursor_y-previous_cursor_y;
          if(dx!=0.0||dy!=0.0){controller.look(dx,dy);camera_changed=true;}
        }
        previous_cursor_x=cursor_x;previous_cursor_y=cursor_y;

        tetra_viewer::FirstPersonInput movement;
        if(pointer_captured||!io.WantCaptureKeyboard){
          movement.forward=(key_down(window,GLFW_KEY_W)?1.0:0.0)-
                           (key_down(window,GLFW_KEY_S)?1.0:0.0);
          movement.right=(key_down(window,GLFW_KEY_D)?1.0:0.0)-
                         (key_down(window,GLFW_KEY_A)?1.0:0.0);
          movement.sprint=key_down(window,GLFW_KEY_LEFT_SHIFT)||
                          key_down(window,GLFW_KEY_RIGHT_SHIFT);
          movement.super_speed=key_down(window,GLFW_KEY_LEFT_CONTROL)||
                               key_down(window,GLFW_KEY_RIGHT_CONTROL);
        }
        // Exercise both halves of the application camera protocol: move long
        // enough to publish interactive work, then release input and wait for
        // the exact settled pose before completing the smoke test.
        if((((motion_test||gpu_terrain_performance_smoke_test)&&
             motion_rendered_frames<30U)||
            (timing_profile_test&&
             timing_profile_class==TimingProfileClass::moving&&
             timing_profile_samples->size()<300U))&&
           scene_vertex_count!=0U)
          // This smoke exercises interactive-to-settled publication, not a
          // terrain-detail stress path. Keep its deterministic displacement
          // above the success threshold while remaining inside the published
          // production resource envelope; large travel belongs to the
          // dedicated camera-path benchmark.
          movement.forward=(motion_test||gpu_terrain_performance_smoke_test)?0.05:1.0;
        if(metalfx_test&&scene_vertex_count!=0U&&metalfx_test_frames<20U){
          movement.forward=1.0;
          movement.right=0.35;
          movement.super_speed=true;
          controller.look(0.35,-0.08);
          camera_changed=true;
        }
        if(metalfx_test&&scene_vertex_count!=0U&&metalfx_test_frames==25U)
          sun_elevation=60.0*std::numbers::pi/180.0;
        if(metalfx_test&&scene_vertex_count!=0U&&metalfx_test_frames==35U){
          controller.state().feet={117.761,16.148,-134.429};
          controller.state().yaw=144.4*std::numbers::pi/180.0;
          controller.state().pitch=1.1*std::numbers::pi/180.0;
          sun_azimuth=-51.5*std::numbers::pi/180.0;
          sun_elevation=tetra_viewer::default_world_sun_elevation_radians;
          atmosphere_debug_view=0;
          camera_changed=true;
        }
        if(soak_test&&scene_vertex_count!=0U&&
           soak_rendered_frames<soak_simulated_frames){
          // Five simulated minutes at six representative samples per second:
          // ground, flight, atmosphere top, orbit, then the same route home.
          const double t=static_cast<double>(soak_rendered_frames)/
              static_cast<double>(soak_simulated_frames-1U);
          const std::array<tetra::Vec3,5> route{{
              {117.761,16.148,-134.429},{0.5,100.5,0.5},
              {0.5,2000.5,0.5},{0.5,25000.5,0.5},
              {117.761,16.148,-134.429}}};
          const double segment=std::min(t*4.0,3.999999);
          const auto index=static_cast<std::size_t>(segment);
          const double fraction=segment-static_cast<double>(index);
          controller.state().feet=route[index]*(1.0-fraction)+route[index+1U]*fraction;
          controller.state().yaw=std::numbers::pi;
          controller.state().pitch=index==3U?-0.15:-0.05;
          sun_azimuth=-103.1324F*std::numbers::pi_v<float>/180.0F;
          sun_elevation=(index==0U||index==3U?5.0F:25.0F)*
              std::numbers::pi_v<float>/180.0F;
          camera_changed=true;
        }
        movement.jump=key_down(window,GLFW_KEY_SPACE);
        const bool advance_simulation=!paused||single_step;
        if(!advance_simulation){
          movement={};
        }else if(free_fly){
          const double vertical=(key_down(window,GLFW_KEY_SPACE)?1.0:0.0)-
                                (key_down(window,GLFW_KEY_C)?1.0:0.0);
          auto direction=controller.forward()*movement.forward+
              controller.right()*movement.right+tetra::Vec3{0.0,vertical,0.0};
          const double direction_length=std::sqrt(direction.x*direction.x+
              direction.y*direction.y+direction.z*direction.z);
          if(direction_length>1.0)direction=direction/direction_length;
          const tetra_viewer::FirstPersonConfiguration movement_configuration;
          const double speed=movement_configuration.walk_speed*
              tetra_viewer::movement_speed_multiplier(movement,
                                                       movement_configuration);
          if(direction_length>0.0){
            controller.state().feet=controller.state().feet+
                direction*speed*elapsed;
            camera_changed=true;
          }
          controller.state().velocity={};
          controller.state().grounded=false;
        }else if(runtime&&!((motion_test||gpu_terrain_performance_smoke_test)&&scene_vertex_count!=0U&&
                             motion_rendered_frames>=30U)){
          const auto previous_feet=controller.state().feet;
          const auto volume_authority=runtime->world_volume_authority_token();
          const auto* collision_field=volume_authority?
              runtime->field(*volume_authority):nullptr;
          // First-person collision is analytic SDF collision, but it is only
          // sampled once the field and complete conforming volume front share
          // one authority token. A stale front retains the prior pose.
          if(collision_field!=nullptr)
            controller.advance(elapsed,movement,*collision_field);
          camera_changed=camera_changed||
              controller.state().feet.x!=previous_feet.x||
              controller.state().feet.y!=previous_feet.y||
              controller.state().feet.z!=previous_feet.z;
        }

        glfwGetFramebufferSize(window,&width,&height);
        width=std::max(width,1);height=std::max(height,1);
        camera=controller.camera(static_cast<double>(height),
                                 static_cast<double>(width)/height);
        bool runtime_started_this_frame=false;
        if(!runtime&&runtime_startup.valid()&&
           runtime_startup.wait_for(std::chrono::seconds(0))==
               std::future_status::ready){
          runtime=runtime_startup.get();
          runtime_started_this_frame=true;
        }
        std::optional<tetra::GpuHierarchySelectionTuple>
            gpu_hierarchy_live_selection_tuple;
        if(runtime){
          if(metal_gpu_terrain_live_selection){
            // This is the P7e2 cutover boundary. The runtime's completed
            // bootstrap publication supplies immutable hierarchy storage once,
            // but camera motion below may not call set_camera(), update(),
            // CPU surface construction, or P6 packet creation.
            const auto published_view=runtime->published_view_identity();
            if(!terrain_front_coordinator.state().current_view.valid()&&
               published_view.valid())
              terrain_front_coordinator=
                  tetra_viewer::TerrainFrontCoordinator(published_view);
            const auto* directory=runtime->world_cut_directory();
            const auto field_revision=published_view.valid()?
                published_view.field_revision:0U;
            if(directory!=nullptr&&field_revision!=0U){
              try {
                const auto source_revision=directory->revision();
                // `make_gpu_hierarchy_snapshot` copies immutable CPU world
                // metadata.  Do that only when the immutable source identity
                // changes, never as a per-camera-frame precondition.
                if(!gpu_hierarchy_live_selection.ready()||
                   gpu_hierarchy_live_selection.source_revision!=source_revision||
                   gpu_hierarchy_live_selection.field_revision!=field_revision){
                  gpu_terrain_counters->immutable_snapshot_builds.fetch_add(
                      1U,std::memory_order_relaxed);
                  static_cast<void>(configure_metal_gpu_hierarchy_live_selection(
                      device,gpu_hierarchy_live_selection,
                      tetra::make_gpu_hierarchy_snapshot(*directory,field_revision,
                          metal_gpu_terrain_device_front),
                      field_revision,runtime->diagnostics().scene_generation));
                }
                // Unlike the diagnostic LOD selector, the drawable route
                // cannot omit an off-frustum branch or substitute a coarse
                // ancestor: the CPU front it replaces contains the complete
                // published cut.
                gpu_hierarchy_live_selection.require_complete_front=
                    metal_gpu_terrain_device_front;
                if(gpu_hierarchy_live_selection.ready()){
                  const auto& profile=runtime->profile();
                  const auto& field=runtime->field();
                  if(metal_gpu_terrain_device_front&&
                     gpu_hierarchy_live_selection.compact_p8_field==nil) {
                    tetra::GpuTerrainFieldTupleParameters terrain_parameters;
                    // The CPU display front extracts its surface with a
                    // camera-quantized footprint. Reusing the unfiltered
                    // world field here changes the implicit surface itself,
                    // so the device path cannot have matching coverage.
                    auto surface_field=field;
                    surface_field.sampling_footprint=
                        tetra_viewer::planetary_surface_sampling_footprint(
                            field,camera,profile.pixel_threshold);
                    terrain_parameters.field=surface_field;
                    terrain_parameters.domain=profile.domain;
                    terrain_parameters.source_revision=source_revision;
                    terrain_parameters.field_revision=field_revision;
                    const auto terrain_tuple=
                        tetra::make_gpu_terrain_field_tuple(terrain_parameters);
                    const auto templates=tetra::make_gpu_green_template_table();
                    gpu_hierarchy_live_selection.compact_p8_field=
                        [device newBufferWithBytes:&terrain_tuple length:sizeof(terrain_tuple)
                          options:MTLResourceStorageModeShared];
                    gpu_hierarchy_live_selection.compact_p8_templates=
                        [device newBufferWithBytes:templates.data() length:sizeof(templates)
                          options:MTLResourceStorageModeShared];
                  }
                  auto selector_camera=camera;
                  selector_camera.position=profile.domain.to_root(camera.position);
                  const auto selector_field_centre=
                      profile.domain.to_root(field.centre);
                  gpu_hierarchy_live_selection_tuple=
                      tetra::make_gpu_hierarchy_selection_tuple({
                        .camera=selector_camera,.render_origin={},
                        .field_centre=selector_field_centre,
                        .planet_radius=field.terrain.planet_radius/
                            profile.domain.world_extent,
                        .terrain_height_bound=tetra::terrain_height_magnitude_bound(field)/
                            profile.domain.world_extent,
                        .field_lipschitz=tetra::implicit_field_lipschitz_bound(field)*
                            profile.domain.world_extent,
                        .edge_threshold=profile.pixel_threshold,
                        .field_threshold=profile.field_error_pixel_threshold,
                        .limb_threshold=profile.limb_error_pixel_threshold,
                        .merge_ratio=profile.lod_merge_threshold_ratio,
                        .source_revision=directory->revision(),
                        .field_revision=field_revision});
                }
              }catch(const std::exception& error){
                std::fprintf(stderr,"GPU live hierarchy selection disabled: %s\n",
                             error.what());
              }
            }
            runtime_camera_interactive=false;
            force_runtime_camera=false;
            diagnostics=runtime->diagnostics();
            if(gpu_hierarchy_live_selection.ready()&&
               diagnostics.scene_generation!=
                   gpu_hierarchy_live_selection.bootstrap_scene_generation)
              ++gpu_hierarchy_live_selection.cpu_generation_violations;
            // The bootstrap CPU front is deliberately rendered unchanged
            // until P7e3 can turn these marks into a conforming owner stream.
            // Publishing this one front is permitted bootstrap work; later
            // camera frames above do not ask the runtime for another scene.
            if(diagnostics.scene_generation!=0U&&
               (diagnostics.scene_generation!=uploaded_generation||
                !terrain_display_front.ready()))
              static_cast<void>(publish_terrain_display({},std::nullopt));
            // P8 completions are meaningful on the device-front route even
            // though P7e4a intentionally leaves the CPU display front drawn.
            // Retire them here; the legacy branch below is not entered.
            for(auto& slot:gpu_terrain_native_slots)if(slot.pending&&
                slot.completed->load(std::memory_order_acquire)){
              slot.pending=false;
              const auto* current_directory=runtime->world_cut_directory();
              const auto current_source=current_directory==nullptr?0U:
                  current_directory->revision();
              const auto current_field=runtime->published_view_identity().field_revision;
              if(!slot.matches(diagnostics.scene_generation,current_source,
                               current_field,runtime->render_origin())){
                slot.succeeded->store(false,std::memory_order_release);
                gpu_terrain_counters->stale_rejected.fetch_add(
                    1U,std::memory_order_relaxed);
              }else if(slot.succeeded->load(std::memory_order_acquire)){
                gpu_terrain_counters->accepted.fetch_add(
                    1U,std::memory_order_relaxed);
                gpu_terrain_renderer_available=true;
              }
            }
          }else{
          runtime->set_gpu_terrain_extraction_diagnostic(
              metal_gpu_terrain_diagnostic||metal_gpu_terrain_native_diagnostic||
              gpu_terrain_renderer_selected);
          const auto published_view=runtime->published_view_identity();
          if(!terrain_front_coordinator.state().current_view.valid()&&
             published_view.valid())
            terrain_front_coordinator=
                tetra_viewer::TerrainFrontCoordinator(published_view);
          const std::uint64_t field_revision=published_view.valid()?
              published_view.field_revision:1U;
          const auto field_signature=
              tetra_viewer::preview_surface_field_signature(runtime->field());
          const auto view=terrain_front_coordinator.observe_view(
              camera,field_revision,field_signature);
          if(preview_enabled){
            const auto support=tetra_viewer::plan_preview_surface(
                view,camera,runtime->field(),preview_configuration);
            terrain_front_coordinator.apply_preview_support(support);
          }else {
            terrain_front_coordinator.apply_preview_support(
                {tetra_viewer::PreviewSupportReason::unsupported_field,
                 std::nullopt});
          }
          runtime->set_view_identity(view);
          if(preview_enabled)
            if(const auto request=terrain_front_coordinator.request_preview()){
              tetra_viewer::PreviewSurfaceRequest work{
                  request->requested_view,request->spatial_key,camera};
              static_cast<void>(preview_surface_worker.submit(
                  std::move(work),runtime->field(),preview_configuration));
            }
          const bool request_interactive_camera=
              (force_runtime_camera||camera_changed)&&
              !(free_fly&&lock_lod_camera);
          if(runtime_started_this_frame){
            runtime->set_camera(camera,false);
            runtime_camera_interactive=false;
          }else if((motion_test||gpu_terrain_performance_smoke_test)&&scene_vertex_count!=0U&&
                   motion_rendered_frames>=30U){
            // A hidden test window can run vastly faster than wall-clock
            // physics.  Hold the scripted final pose and state its settled
            // intent explicitly, rather than treating numerical contact
            // updates as continuing user input forever.
            runtime->set_camera(camera,false);
            runtime_camera_interactive=false;
            force_runtime_camera=false;
          }else if(request_interactive_camera){
            runtime->set_camera(camera,true);
            runtime_camera_interactive=true;
            force_runtime_camera=false;
          }else if(runtime_camera_interactive){
            // Match Vulkan's interactive-to-settled transition. The final
            // sample can be below the ordinary spatial request threshold;
            // submitting it explicitly lets the runtime retire a coalesced
            // movement front at the exact pose where input stopped.
            runtime->set_camera(camera,false);
            runtime_camera_interactive=false;
          }
          static_cast<void>(runtime->update());
          diagnostics=runtime->diagnostics();
          // Retire completed candidates before selecting a consumer front.
          // A candidate is current only when every identity component still
          // matches the CPU-published cut; an older or partial slot can never
          // reach any draw, shadow, or ray-tracing consumer.
          for(auto& slot:gpu_terrain_slots)if(slot.pending&&
              slot.completed->load(std::memory_order_acquire)){
            slot.pending=false;
            if(!slot.matches(diagnostics.scene_generation,
                             runtime->scene().render_origin)){
              slot.succeeded->store(false,std::memory_order_release);
              gpu_terrain_counters->stale_rejected.fetch_add(
                  1U,std::memory_order_relaxed);
            }else if(slot.succeeded->load(std::memory_order_acquire)){
              gpu_terrain_counters->accepted.fetch_add(
                  1U,std::memory_order_relaxed);
            }
          }
          // The seed completion is the first readback-free publication.  Its
          // contents are a complete CPU front copied into private memory, so
          // the GPU selector can never expose uninitialised candidate memory.
          if(gpu_terrain_active_front.vertices!=nil&&
             gpu_terrain_active_front.indirect_arguments!=nil&&
             gpu_terrain_active_front.completed->load(std::memory_order_acquire)&&
             !terrain_display_front.preview_cpu&&
             gpu_terrain_active_front.identity==terrain_display_front.identity){
            gpu_terrain_renderer_available=true;
            if(gpu_terrain_renderer_selected&&
               !gpu_terrain_active_front.promoted){
              terrain_display_front.exact_vertices=gpu_terrain_active_front.vertices;
              terrain_display_front.exact_indirect_arguments=
                  gpu_terrain_active_front.indirect_arguments;
              terrain_display_front.indexed_exact_selection=false;
              terrain_display_front.render_generation=next_terrain_render_generation++;
              if(next_terrain_render_generation==0U)
                next_terrain_render_generation=1U;
              scene_vertices=terrain_display_front.exact_vertices;
              gpu_terrain_active_front.promoted=true;
            }
          }
          for(auto& slot:gpu_terrain_native_slots)if(slot.pending&&
              slot.completed->load(std::memory_order_acquire)){
            slot.pending=false;
            const auto* current_directory=runtime->world_cut_directory();
            const auto current_source=current_directory==nullptr?0U:
                current_directory->revision();
            const auto current_field=runtime->published_view_identity().field_revision;
            if(!slot.matches(diagnostics.scene_generation,current_source,
                             current_field,runtime->render_origin())){
              slot.succeeded->store(false,std::memory_order_release);
              gpu_terrain_counters->stale_rejected.fetch_add(
                  1U,std::memory_order_relaxed);
            }else if(slot.succeeded->load(std::memory_order_acquire)){
              gpu_terrain_counters->accepted.fetch_add(
                  1U,std::memory_order_relaxed);
              gpu_terrain_renderer_available=true;
            }
          }
          const auto exact_now=runtime->published_view_identity();
          if(terrain_display_front.preview_cpu&&exact_now.valid())
            terrain_front_coordinator.publish_exact(
                exact_now,terrain_display_front.preview_cpu->coverage());
          const bool coordinator_has_visible_preview=
              terrain_front_coordinator.state().preview_visible.has_value();
          const bool display_has_visible_preview=
              static_cast<bool>(terrain_display_front.preview_cpu);
          if(diagnostics.scene_generation!=0U&&
             (diagnostics.scene_generation!=uploaded_generation||
              !terrain_display_front.ready()||
              coordinator_has_visible_preview!=display_has_visible_preview)){
            std::shared_ptr<const tetra_viewer::PreviewSurfaceFront> retained;
            std::optional<tetra_viewer::PreviewRequestIdentity> retained_identity;
            const auto& coordinator_state=terrain_front_coordinator.state();
            if(coordinator_state.preview_visible&&
               terrain_display_front.preview_cpu&&
               terrain_display_front.preview_cpu->spatial_key()==
                   coordinator_state.preview_visible->request.spatial_key){
              retained=terrain_display_front.preview_cpu;
              retained_identity=coordinator_state.preview_visible->request;
            }
            static_cast<void>(publish_terrain_display(
                std::move(retained),retained_identity));
          }

          if(auto completion=preview_surface_worker.take_completed()){
            const auto request=tetra_viewer::PreviewRequestIdentity{
                completion->request().requested_view,
                completion->request().spatial_key};
            if(completion->has_contract_error()){
              static_cast<void>(terrain_front_coordinator.complete_preview(
                  request,tetra_viewer::PreviewFrontOutcome::failed));
            }else {
              const auto& result=completion->result();
              if(result.ready()){
                const auto front=result.front();
                if(exact_now.valid())
                  terrain_front_coordinator.publish_exact(
                      exact_now,front->coverage());
                if(terrain_front_coordinator.complete_preview(
                       request,result.outcome(),front->coverage())){
                  const bool uploaded=publish_terrain_display(front,request);
                  static_cast<void>(
                      terrain_front_coordinator.complete_preview_upload(
                          request,uploaded));
                }
              }else static_cast<void>(
                  terrain_front_coordinator.complete_preview(
                      request,result.outcome()));
            }
          }
          if(timing_profile_test&&
             timing_profile_class==TimingProfileClass::exact_handoff&&
             terrain_display_front.preview_cpu==nullptr&&
             terrain_front_coordinator.state().preview_retirement_reason==
                 tetra_viewer::PreviewRetirementReason::exact_handoff){
            timing_profile_exact_handoff_observed=true;
            // Preserve the exact front after the event. This is test-only;
            // normal interactive preview reacquisition remains unchanged.
            preview_enabled=false;
          }
          }
        }
        single_step=false;

        if(runtime){
          const auto add_line=[&](std::vector<tetra_viewer::SceneVertex>& lines,
                                  tetra::Vec3 first,tetra::Vec3 second,
                                  std::array<float,3> colour){
            const auto vertex=[&](tetra::Vec3 point){
              tetra_viewer::SceneVertex output{};
              point=point-(terrain_display_front.ready()?
                  terrain_display_front.identity.render_origin:
                  runtime->render_origin());
              output.position[0]=static_cast<float>(point.x);
              output.position[1]=static_cast<float>(point.y);
              output.position[2]=static_cast<float>(point.z);
              std::ranges::copy(colour,output.colour);
              return output;
            };
            lines.push_back(vertex(first));lines.push_back(vertex(second));
          };
          std::vector<tetra_viewer::SceneVertex> player_lines;
          const auto& player=controller.state();
          if(show_capsule){
            constexpr std::size_t segments=24U;
            constexpr double radius=0.025;
            constexpr double capsule_height=0.16;
            for(double y:std::array{radius,capsule_height-radius})
              for(std::size_t index=0;index<segments;++index){
                const double first=2.0*std::numbers::pi*index/segments;
                const double second=2.0*std::numbers::pi*(index+1U)/segments;
                add_line(player_lines,
                         player.feet+tetra::Vec3{radius*std::cos(first),y,
                                                  radius*std::sin(first)},
                         player.feet+tetra::Vec3{radius*std::cos(second),y,
                                                  radius*std::sin(second)},
                         {0.95F,0.78F,0.18F});
              }
            for(double angle:std::array{0.0,std::numbers::pi*0.5,
                                        std::numbers::pi,std::numbers::pi*1.5})
              add_line(player_lines,
                       player.feet+tetra::Vec3{radius*std::cos(angle),radius,
                                                radius*std::sin(angle)},
                       player.feet+tetra::Vec3{radius*std::cos(angle),
                                                capsule_height-radius,
                                                radius*std::sin(angle)},
                       {0.95F,0.78F,0.18F});
          }
          if(show_contact_normal)
            add_line(player_lines,
                     player.feet+tetra::Vec3{0.0,0.003,0.0},
                     player.feet+player.contact_normal*0.12,
                     {0.18F,0.86F,0.96F});
          const auto player_bytes=
              player_lines.size()*sizeof(tetra_viewer::SceneVertex);
          player_overlay_vertices=player_bytes==0U?nil:
              [device newBufferWithBytes:player_lines.data()
                                  length:player_bytes
                                 options:MTLResourceStorageModeShared];
          player_overlay_vertex_count=player_lines.size();

          if(show_lod_zones&&
             (lod_overlay_epoch!=diagnostics.hierarchy_demand_epoch||
              lod_overlay_generation!=uploaded_generation)){
            std::vector<tetra_viewer::SceneVertex> lod_lines;
            for(const auto& line:runtime->lod_zone_lines())
              add_line(lod_lines,line.first,line.second,line.colour);
            const auto lod_bytes=
                lod_lines.size()*sizeof(tetra_viewer::SceneVertex);
            lod_overlay_vertices=lod_bytes==0U?nil:
                [device newBufferWithBytes:lod_lines.data() length:lod_bytes
                                   options:MTLResourceStorageModeShared];
            lod_overlay_vertex_count=lod_lines.size();
            lod_overlay_epoch=diagnostics.hierarchy_demand_epoch;
            lod_overlay_generation=uploaded_generation;
          }else if(!show_lod_zones){
            lod_overlay_vertices=nil;lod_overlay_vertex_count=0U;
            lod_overlay_epoch=std::numeric_limits<std::uint64_t>::max();
            lod_overlay_generation=0U;
          }
        }else{
          player_overlay_vertices=nil;player_overlay_vertex_count=0U;
          lod_overlay_vertices=nil;lod_overlay_vertex_count=0U;
        }

        if(render_test&&scene_vertex_count!=0U){
          if(render_test_frames<10U){
            render_resolution_mode=0;terrain_msaa=false;
          }else if(render_test_frames<20U){
            render_resolution_mode=1;fixed_render_scale=0.5F;
            terrain_msaa=false;
          }else if(render_test_frames<30U){
            render_resolution_mode=1;fixed_render_scale=2.0F/3.0F;
            terrain_msaa=scene_pipeline_2!=nil;terrain_sample_count=2;
          }else{
            render_resolution_mode=1;fixed_render_scale=0.75F;
            terrain_msaa=scene_pipeline_4!=nil;
            terrain_sample_count=scene_pipeline_4!=nil?4:2;
          }
        }

        layer.drawableSize=CGSizeMake(width,height);
        const int detected_refresh=std::max(1,static_cast<int>(
            native_window.screen.maximumFramesPerSecond));
        if(automatic_target_display&&detected_refresh!=automatic_target_fps){
          automatic_target_fps=detected_refresh;
          automatic_quality_controller.set_target_milliseconds(
              1000.0/static_cast<double>(automatic_target_fps));
        }
        display_refresh_hz=detected_refresh;
        const double gpu_milliseconds=gpu_frame_milliseconds->load(
            std::memory_order_relaxed);
        const auto completed_gpu_frame=gpu_frame_sequence->load(
            std::memory_order_relaxed);
        if(render_resolution_mode==2&&gpu_milliseconds>0.0&&
           completed_gpu_frame!=consumed_gpu_frame_sequence&&
           automatic_stable_frames>=30U){
          consumed_gpu_frame_sequence=completed_gpu_frame;
          const auto decision=automatic_quality_controller.observe(
              gpu_milliseconds,gpu_frame_moving->load(std::memory_order_relaxed)?
                  tetra_viewer::MetalQualityFrameClass::moving:
                  tetra_viewer::MetalQualityFrameClass::steady,
              gpu_frame_maintenance->load(std::memory_order_relaxed));
          if(decision.percentile_95_milliseconds>0.0){
            automatic_gpu_percentile_95_milliseconds=
                decision.percentile_95_milliseconds;
            // The controller currently uses one p95 window; retain the
            // established diagnostic field rather than presenting an invented
            // mean as a second statistic.
            automatic_gpu_median_milliseconds=gpu_milliseconds;
          }
          if(decision.change!=tetra_viewer::MetalQualityChange::none){
            ++automatic_quality_changes;
            automatic_last_change=decision.change;
          }
          const float next_scale=automatic_quality_controller.profile().render_scale;
          if(next_scale!=automatic_render_scale)automatic_stable_frames=0U;
          automatic_render_scale=next_scale;
          terrain_msaa=automatic_quality_controller.profile().terrain_samples>1U;
          terrain_sample_count=static_cast<int>(
              automatic_quality_controller.profile().terrain_samples);
        }
        const float active_render_scale=render_resolution_mode==0?1.0F:
            (render_resolution_mode==1?fixed_render_scale:
                                       automatic_render_scale);
        const int desired_render_width=std::max(1,static_cast<int>(
            std::lround(static_cast<double>(width)*active_render_scale)));
        const int desired_render_height=std::max(1,static_cast<int>(
            std::lround(static_cast<double>(height)*active_render_scale)));
        const bool metalfx_requested=runtime!=nullptr&&metalfx_temporal_enabled&&
            metalfx_temporal_supported&&active_render_scale<0.999F&&
            desired_render_width<width&&desired_render_height<height;
        const bool metalfx_temporal_active=metalfx_requested&&
            ensure_metal_fx_temporal_resources(
                device,metalfx_resources,desired_render_width,
                desired_render_height,width,height,
                metalfx_direct_drawable);
        if(render_width==desired_render_width&&
           render_height==desired_render_height)
          ++automatic_stable_frames;
        else automatic_stable_frames=0U;
        const NSUInteger active_samples=terrain_msaa?
            static_cast<NSUInteger>(terrain_sample_count):1U;
        if(scene_colour_texture==nil||render_width!=desired_render_width||
           render_height!=desired_render_height||
           allocated_samples!=active_samples){
          render_width=desired_render_width;
          render_height=desired_render_height;
          allocated_samples=active_samples;
          scene_colour_texture=make_render_texture(device,render_width,
              render_height,scene_colour_format,1U,
              MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead);
          const MTLTextureUsage temporal_depth_usage=
              metalfx_temporal_active?
                  metalfx_resources.scaler.depthTextureUsage:
                  MTLTextureUsageUnknown;
          depth_texture=make_render_texture(device,render_width,render_height,
              depth_format,1U,
              MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|
                  temporal_depth_usage);
          multisample_colour_texture=active_samples>1U?
              make_render_texture(device,render_width,render_height,
                  scene_colour_format,active_samples,MTLTextureUsageRenderTarget):nil;
          multisample_depth_texture=active_samples>1U?
              make_render_texture(device,render_width,render_height,
                  depth_format,active_samples,MTLTextureUsageRenderTarget):nil;
          if(!ensure_screen_atmosphere_resources(
                 device,atmosphere_resources,render_width,render_height,
                 atmosphere_screen_divisor,atmosphere_transport!=2)){
            std::fprintf(stderr,"Unable to allocate screen atmosphere targets.\n");
            result=1;
            glfwSetWindowShouldClose(window,GLFW_TRUE);
          }
        }
        if(atmosphere_quality_test&&scene_vertex_count!=0U){
          if(atmosphere_quality_test_frames==5U){
            atmosphere_quality_switches_ok&=apply_atmosphere_quality(0);
            low_atmosphere_allocation=
                live_atmosphere_allocation_bytes(atmosphere_resources);
          }else if(atmosphere_quality_test_frames==25U){
            atmosphere_quality_switches_ok&=apply_atmosphere_quality(2);
            high_atmosphere_allocation=
                live_atmosphere_allocation_bytes(atmosphere_resources);
          }else if(atmosphere_quality_test_frames==45U){
            atmosphere_quality_switches_ok&=apply_atmosphere_quality(1);
            default_atmosphere_allocation=
                live_atmosphere_allocation_bytes(atmosphere_resources);
          }
        }
        // Change a true physical lookup dependency midway through an otherwise
        // static reference run.  The unjittered cache identity must rebuild
        // sky view and irradiance exactly once more; MetalFX-style jitter
        // alone must not cause that work.
        if(atmosphere_lookup_invalidation_test&&
           atmosphere_test_frames==5U&&!lookup_invalidation_sun_changed){
          sun_azimuth+=0.125F;
          lookup_invalidation_sun_changed=true;
        }
        // The refresh class intentionally changes a physical lookup key every
        // sampled frame.  It measures rebuild work, rather than accidentally
        // reporting a mostly-cache-hit average with one startup refresh.
        if(timing_profile_test&&
           timing_profile_class==TimingProfileClass::lookup_refresh&&
           scene_vertex_count!=0U&&timing_profile_samples->size()<300U)
          sun_azimuth+=0.003F;
        // Keep the physical state fixed: this measures only the ordered
        // transmittance/multiple-scattering rebuild, not sky-view refresh.
        if(timing_profile_test&&
           timing_profile_class==TimingProfileClass::optical_refresh&&
           scene_vertex_count!=0U&&timing_profile_samples->size()<300U)
          atmosphere_optical_dirty=true;
        // Aerial lookup identity includes the physical view/sun uniform. Move
        // that real input only in the diagnostic profile so the active aerial
        // view refreshes once per retained sample; optical tables remain
        // cached and their cost cannot be attributed to the aerial interval.
        if(timing_profile_test&&
           timing_profile_class==TimingProfileClass::aerial_refresh&&
           scene_vertex_count!=0U&&timing_profile_samples->size()<300U)
          sun_azimuth+=0.003F;
        id<CAMetalDrawable> drawable=[layer nextDrawable];
        if(drawable==nil)continue;

        MTLRenderPassDescriptor* scene_pass=
            [MTLRenderPassDescriptor renderPassDescriptor];
        scene_pass.colorAttachments[0].texture=active_samples>1U?
            multisample_colour_texture:scene_colour_texture;
        scene_pass.colorAttachments[0].resolveTexture=active_samples>1U?
            scene_colour_texture:nil;
        scene_pass.colorAttachments[0].loadAction=MTLLoadActionClear;
        scene_pass.colorAttachments[0].storeAction=active_samples>1U?
            MTLStoreActionMultisampleResolve:MTLStoreActionStore;
        // The atmosphere composite uses resolved alpha as the fractional
        // terrain coverage at MSAA silhouettes.  Preserve that contract by
        // clearing uncovered samples to transparent black, matching Vulkan;
        // an opaque coloured clear turns every resolved skyline pixel into a
        // full terrain sample and creates a bright one-pixel mountain rim.
        const bool resolve_atmosphere_coverage=
            active_samples>1U&&atmosphere_enabled;
        scene_pass.colorAttachments[0].clearColor=
            resolve_atmosphere_coverage?MTLClearColorMake(0.0,0.0,0.0,0.0):
                MTLClearColorMake(0.035,0.055,0.085,1.0);
        scene_pass.depthAttachment.texture=active_samples>1U?
            multisample_depth_texture:depth_texture;
        scene_pass.depthAttachment.resolveTexture=active_samples>1U?
            depth_texture:nil;
        scene_pass.depthAttachment.loadAction=MTLLoadActionClear;
        scene_pass.depthAttachment.storeAction=active_samples>1U?
            MTLStoreActionMultisampleResolve:MTLStoreActionStore;
        scene_pass.depthAttachment.clearDepth=0.0;
        if(active_samples>1U)
          scene_pass.depthAttachment.depthResolveFilter=
              MTLMultisampleDepthResolveFilterMax;

        MTLRenderPassDescriptor* display_pass=
            [MTLRenderPassDescriptor renderPassDescriptor];
        display_pass.colorAttachments[0].texture=drawable.texture;
        display_pass.colorAttachments[0].loadAction=MTLLoadActionClear;
        display_pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        display_pass.colorAttachments[0].clearColor=
            MTLClearColorMake(0.035,0.055,0.085,1.0);

        if(pointer_captured)io.ConfigFlags|=ImGuiConfigFlags_NoMouse;
        else io.ConfigFlags&=~ImGuiConfigFlags_NoMouse;
        ImGui_ImplMetal_NewFrame(display_pass);
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(14.0F,14.0F),ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.78F);
        ImGui::Begin("World status",nullptr,
            ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings|
            ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("Native Metal: %s",device.name.UTF8String);
        ImGui::TextUnformatted("WASD move  Shift sprint  Space jump");
        ImGui::TextUnformatted("Ctrl super speed  Ctrl+Shift 10x super speed");
        ImGui::TextUnformatted("Mouse look  Esc releases pointer  Click captures");
        ImGui::Separator();
        const float frame_rate=io.Framerate;
        if(vsync)ImGui::Text("VSync %d Hz   %.2f ms frame budget",
                            display_refresh_hz,
                            1000.0/static_cast<double>(display_refresh_hz));
        else ImGui::Text("Render loop %.2f ms   %.1f FPS",
                         frame_rate>0.0F?1000.0F/frame_rate:0.0F,frame_rate);
        if(ImGui::Checkbox("VSync",&vsync))layer.displaySyncEnabled=vsync;

        if(ImGui::CollapsingHeader("Render resolution")){
          constexpr std::array<const char*,3> modes{"Native","Fixed","Auto"};
          ImGui::SetNextItemWidth(120.0F);
          if(ImGui::Combo("Mode",&render_resolution_mode,modes.data(),
                          modes.size())){
            automatic_quality_controller=tetra_viewer::MetalQualityController(
                automatic_quality_profiles,0U,
                1000.0/static_cast<double>(automatic_target_fps));
            automatic_render_scale=automatic_quality_controller.profile().render_scale;
            automatic_stable_frames=0U;
          }
          constexpr std::array<float,4> scale_values{
              0.5F,2.0F/3.0F,0.75F,1.0F};
          constexpr std::array<const char*,4> scale_names{
              "50%","67%","75%","100%"};
          const auto scale_combo=[&](const char* label,float& value){
            std::size_t selected{};
            for(std::size_t index=1U;index<scale_values.size();++index)
              if(std::abs(value-scale_values[index])<
                 std::abs(value-scale_values[selected]))selected=index;
            ImGui::SetNextItemWidth(120.0F);
            if(!ImGui::BeginCombo(label,scale_names[selected]))return false;
            bool changed=false;
            for(std::size_t index=0;index<scale_values.size();++index){
              const bool current=index==selected;
              if(ImGui::Selectable(scale_names[index],current)){
                value=scale_values[index];changed=true;
              }
              if(current)ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
            return changed;
          };
          if(render_resolution_mode==1)
            scale_combo("Render scale",fixed_render_scale);
          else if(render_resolution_mode==2){
            const std::string display_target_name="Display ("+
                std::to_string(display_refresh_hz)+" Hz)";
            const std::array<const char*,4> targets{
                display_target_name.c_str(),"60 FPS","90 FPS","120 FPS"};
            int selected_target=automatic_target_display?0:
                (automatic_target_fps==60?1:
                 (automatic_target_fps==90?2:3));
            ImGui::SetNextItemWidth(120.0F);
            if(ImGui::Combo("Target",&selected_target,targets.data(),
                            targets.size())){
              automatic_target_display=selected_target==0;
              automatic_target_fps=automatic_target_display?
                  display_refresh_hz:(selected_target==1?60:
                  (selected_target==2?90:120));
              automatic_quality_controller.set_target_milliseconds(
                  1000.0/static_cast<double>(automatic_target_fps));
              automatic_stable_frames=0U;
            }
            ImGui::TextDisabled("Qualified ladder: 50%% / 70%%, 2x MSAA");
            ImGui::TextDisabled("60-frame p95, 180-frame dwell");
          }
          if(!metalfx_temporal_supported)ImGui::BeginDisabled();
          if(ImGui::Checkbox("MetalFX temporal",&metalfx_temporal_enabled)){
            metalfx_resources.history_valid=false;
            previous_temporal_projection.reset();
          }
          if(!metalfx_temporal_supported)ImGui::EndDisabled();
          if(render_resolution_mode==0||metalfx_temporal_enabled)
            ImGui::BeginDisabled();
          ImGui::SetNextItemWidth(120.0F);
          ImGui::SliderFloat("Sharpening",&upscale_sharpening,
                             0.0F,1.0F,"%.2f");
          if(render_resolution_mode==0||metalfx_temporal_enabled)
            ImGui::EndDisabled();
          int logical_width{},logical_height{};
          glfwGetWindowSize(window,&logical_width,&logical_height);
          float scale_x{},scale_y{};
          glfwGetWindowContentScale(window,&scale_x,&scale_y);
          ImGui::Text("Window %dx%d points  %.2fx%.2f scale",
                      logical_width,logical_height,scale_x,scale_y);
          ImGui::Text("Drawable %dx%d  internal %dx%d (%.0f%%)",
                      width,height,render_width,render_height,
                      active_render_scale*100.0F);
          if(metalfx_temporal_active){
            ImGui::TextDisabled("MetalFX temporal active  frames %llu  resets %llu",
                static_cast<unsigned long long>(metalfx_resources.encoded_frames),
                static_cast<unsigned long long>(metalfx_resources.history_resets));
            if(gpu_stage_timings->metalfx_valid.load(
                   std::memory_order_relaxed))
              ImGui::TextDisabled("MetalFX %.2f ms",
                  gpu_stage_timings->metalfx_milliseconds.load(
                      std::memory_order_relaxed));
          }else if(metalfx_temporal_enabled&&!metalfx_temporal_supported)
            ImGui::TextDisabled("MetalFX temporal unavailable");
          else if(metalfx_requested&&!metalfx_resources.failure.empty())
            ImGui::TextDisabled("MetalFX fallback: %s",
                                metalfx_resources.failure.c_str());
          else if(metalfx_temporal_enabled)
            ImGui::TextDisabled("MetalFX waits for sub-native resolution");
          if(gpu_milliseconds>0.0)
            ImGui::Text("GPU %.2f ms",gpu_milliseconds);
          if(render_resolution_mode==2)
            ImGui::TextDisabled("Auto latest %.2f ms  p95 %.2f ms  stable %zu",
                automatic_gpu_median_milliseconds,
                automatic_gpu_percentile_95_milliseconds,
                automatic_stable_frames);
            ImGui::TextDisabled("Mode changes %llu (%s)",
                static_cast<unsigned long long>(automatic_quality_changes),
                automatic_last_change==tetra_viewer::MetalQualityChange::upgrade?
                    "upgrade":(automatic_last_change==
                    tetra_viewer::MetalQualityChange::downgrade?
                        "downgrade":"none"));
        }

        if(!runtime)ImGui::TextUnformatted("Terrain loading...");
        else{
          ImGui::Text("Terrain %s",diagnostics.busy?"updating...":"ready");
          ImGui::Text("Cells %zu   tetrahedra %zu",diagnostics.logical_cells,
                      diagnostics.active_tetrahedra);
          ImGui::Text("Mesh revision %llu   %.2f ms",
              static_cast<unsigned long long>(diagnostics.mesh_revision),
              diagnostics.last_update_milliseconds);
          ImGui::Text("World %.0f units   revision %llu",diagnostics.world_extent,
              static_cast<unsigned long long>(diagnostics.world_revision));
          promote_completed_terrain_acceleration_structure(
              terrain_acceleration_structure);
          ImGui::Text("Terrain RT structure %s   generation %llu   %.1f MiB",
              terrain_acceleration_structure.active!=nil?"ready":
                  (terrain_acceleration_structure.pending!=nil?"building":"waiting"),
              static_cast<unsigned long long>(
                  terrain_acceleration_structure.active_generation),
              static_cast<double>(terrain_acceleration_structure.resident_bytes)/
                  (1024.0*1024.0));
          if(terrain_acceleration_structure.last_build_timing_valid->load(
                 std::memory_order_acquire))
            ImGui::TextDisabled("RT builds %llu   last sampled %.3f ms",
                static_cast<unsigned long long>(
                    terrain_acceleration_structure.build_count),
                terrain_acceleration_structure.last_build_milliseconds->load(
                    std::memory_order_relaxed));
          else ImGui::TextDisabled("RT builds %llu   timing unavailable",
              static_cast<unsigned long long>(
                  terrain_acceleration_structure.build_count));
          ImGui::Text("Blocks %zu   surface blocks %zu",
                      diagnostics.hierarchy_blocks,diagnostics.surface_blocks);
        }
        const auto& camera_state=controller.state();
        ImGui::Text("Position %.3f  %.3f  %.3f",camera_state.feet.x,
                    camera_state.feet.y,camera_state.feet.z);
        ImGui::Text("Rotation yaw %.1f deg   pitch %.1f deg",
            std::remainder(camera_state.yaw*180.0/std::numbers::pi,360.0),
            camera_state.pitch*180.0/std::numbers::pi);
        ImGui::Separator();
        checkbox_with_hotkey("Pause simulation","P",ImGuiKey_P,&paused);
        ImGui::SameLine();
        if(ImGui::Button("Single step"))single_step=true;
        checkbox_with_hotkey("Free fly","F",ImGuiKey_F,&free_fly);
        if(checkbox_with_hotkey("Lock terrain LOD camera","G",ImGuiKey_G,
                                &lock_lod_camera)&&!lock_lod_camera)
          force_runtime_camera=true;
        if(!free_fly)ImGui::TextDisabled("Lock applies while free flying");
        checkbox_with_hotkey("Triangle wireframe","T",ImGuiKey_T,
                             &show_surface_edges);
        checkbox_with_hotkey("Smooth terrain normals","M",ImGuiKey_M,
                             &smooth_normals);
        const bool supports_msaa=scene_pipeline_2!=nil||scene_pipeline_4!=nil;
        if(!supports_msaa)ImGui::BeginDisabled();
        ImGui::Checkbox("Terrain MSAA",&terrain_msaa);
        if(!supports_msaa)ImGui::EndDisabled();
        if(terrain_msaa){
        ImGui::SameLine();
        ImGui::SetNextItemWidth(72.0F);
          if(ImGui::BeginCombo("##terrain-msaa-samples",
                               terrain_sample_count==4?"4x":"2x")){
            if(scene_pipeline_2!=nil&&ImGui::Selectable(
                   "2x",terrain_sample_count==2))terrain_sample_count=2;
            if(scene_pipeline_4!=nil&&ImGui::Selectable(
                   "4x",terrain_sample_count==4))terrain_sample_count=4;
            ImGui::EndCombo();
          }
          ImGui::SameLine();
          ImGui::TextDisabled("Metal resolve");
        }
        checkbox_with_hotkey("Capsule diagnostic","K",ImGuiKey_K,
                             &show_capsule);
        checkbox_with_hotkey("Contact normal","N",ImGuiKey_N,
                             &show_contact_normal);
        checkbox_with_hotkey("LOD zones","L",ImGuiKey_L,&show_lod_zones);

        ImGui::SeparatorText("Sun");
        if(checkbox_with_hotkey("Animate sun","Y",ImGuiKey_Y,&animate_sun)){
          sun_orbit_azimuth=sun_azimuth;
          sun_orbit_phase=sun_elevation;
        }
        if(animate_sun){
          constexpr double minimum_cycle=0.25;
          constexpr double maximum_cycle=600.0;
          ImGui::SetNextItemWidth(190.0F);
          ImGui::SliderScalar("Cycle (seconds)",ImGuiDataType_Double,
              &sun_cycle_seconds,&minimum_cycle,&maximum_cycle,"%.1f");
          ImGui::Text("Azimuth %.1f deg",sun_azimuth*180.0F/
                      std::numbers::pi_v<float>);
          ImGui::Text("Elevation %.1f deg",sun_elevation*180.0F/
                      std::numbers::pi_v<float>);
          ImGui::TextDisabled("Fast live atmospheric shadows");
        }else{
          ImGui::SetNextItemWidth(190.0F);
          ImGui::SliderAngle("Azimuth",&sun_azimuth,-180.0F,180.0F);
          ImGui::SetNextItemWidth(190.0F);
          ImGui::SliderAngle("Elevation",&sun_elevation,-90.0F,90.0F);
        }
        if(ImGui::Button("Reset sun")){
          sun_azimuth=tetra_viewer::default_world_sun_azimuth_radians;
          sun_elevation=tetra_viewer::default_world_sun_elevation_radians;
          sun_orbit_azimuth=sun_azimuth;
          sun_orbit_phase=sun_elevation;
        }

        ImGui::SeparatorText("Atmosphere");
        ImGui::TextDisabled("Native Metal physical atmosphere");
        checkbox_with_hotkey("Atmosphere","H",ImGuiKey_H,
                             &atmosphere_enabled);
        ImGui::SetNextItemWidth(190.0F);
        if(ImGui::BeginCombo("Transport",atmosphere_transport==0?
             "Qualified baseline":atmosphere_transport==2?
             "Reference Hillaire 2020":"Faithful Hillaire")){
          if(ImGui::Selectable("Qualified baseline",atmosphere_transport==0)){
            atmosphere_transport=0;atmosphere_optical_dirty=true;
          }
          if(ImGui::Selectable("Faithful Hillaire",atmosphere_transport==1)){
            atmosphere_transport=1;atmosphere_optical_dirty=true;
          }
          if(ImGui::Selectable("Reference Hillaire 2020",
                               atmosphere_transport==2)){
            atmosphere_transport=2;
            atmosphere_renderer=3;
            atmosphere_resources.history_valid=false;
            atmosphere_optical_dirty=true;
          }
          ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(190.0F);
        if(ImGui::BeginCombo("Atmosphere renderer",
             atmosphere_renderer==2?"Deterministic half resolution":
             atmosphere_renderer==3?"Temporal half resolution":
             atmosphere_renderer==4?"Deterministic shadowed froxels":
             atmosphere_renderer==1?"Native screen oracle":
                                      "Current qualified")){
          if(ImGui::Selectable("Current qualified",atmosphere_renderer==0))
            {atmosphere_renderer=0;atmosphere_resources.history_valid=false;}
          if(ImGui::Selectable("Native screen oracle",atmosphere_renderer==1))
            {atmosphere_renderer=1;atmosphere_resources.history_valid=false;}
          if(ImGui::Selectable("Deterministic half resolution",
                               atmosphere_renderer==2))
            {atmosphere_renderer=2;atmosphere_resources.history_valid=false;}
          if(ImGui::Selectable("Temporal half resolution",
                               atmosphere_renderer==3))
            {atmosphere_renderer=3;atmosphere_resources.history_valid=false;}
          if(ImGui::Selectable("Deterministic shadowed froxels",
                               atmosphere_renderer==4))
            {atmosphere_renderer=4;atmosphere_resources.history_valid=false;}
          ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(190.0F);
        constexpr std::array<const char*,5> shadow_integrator_names{
            "Fixed 32","Adaptive transition","Minmax segments",
            "Moment hybrid","Epipolar minmax"};
        constexpr std::array<int,5> shadow_integrator_values{0,1,2,4,5};
        const auto selected_shadow_integrator=std::ranges::find(
            shadow_integrator_values,shadow_integration);
        const std::size_t selected_shadow_integrator_index=
            selected_shadow_integrator==shadow_integrator_values.end()?0U:
            static_cast<std::size_t>(selected_shadow_integrator-
                                     shadow_integrator_values.begin());
        if(ImGui::BeginCombo("Shadow integration",
              shadow_integrator_names[selected_shadow_integrator_index])){
          for(std::size_t index=0;index<shadow_integrator_names.size();++index){
            if(ImGui::Selectable(shadow_integrator_names[index],
                    shadow_integration==shadow_integrator_values[index]))
              shadow_integration=shadow_integrator_values[index];
          }
          ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(190.0F);
        ImGui::Combo("Surface shadow bias",&shadow_bias,
                     "Slope scaled\0Receiver plane\0");
        ImGui::SetNextItemWidth(190.0F);
        ImGui::Combo("Shadow filtering",&shadow_filter,
                     "Unfiltered\0Fixed tent\0Physical footprint\0");
        constexpr std::array<const char*,6> atmosphere_preset_names{
            "Gameplay planet","Earth","Mars-like","Dense haze",
            "Nearly airless","Custom"};
        ImGui::SetNextItemWidth(190.0F);
        if(ImGui::BeginCombo("Preset",
                             atmosphere_preset_names[atmosphere_preset_index])){
          for(int index=0;index<5;++index){
            const bool selected=atmosphere_preset_index==index;
            if(ImGui::Selectable(atmosphere_preset_names[index],selected)){
              const double metres_per_world_unit=
                  atmosphere_parameters.metres_per_world_unit;
              atmosphere_parameters=tetra_viewer::atmosphere_preset(
                  static_cast<tetra_viewer::AtmospherePreset>(index));
              atmosphere_parameters.metres_per_world_unit=metres_per_world_unit;
              if(index==0)
                atmosphere_parameters=
                    tetra_viewer::adapt_compact_atmosphere_to_relief(
                        atmosphere_parameters,maximum_terrain_relief_metres);
              atmosphere_preset_index=index;
              atmosphere_optical_dirty=true;
            }
            if(selected)ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
        if(ImGui::CollapsingHeader("Physical parameters")){
          const auto drag_double=[&](const char* label,double& value,
                                      double speed,double minimum,
                                      double maximum,const char* format){
            return ImGui::DragScalar(label,ImGuiDataType_Double,&value,
                static_cast<float>(speed),&minimum,&maximum,format,
                ImGuiSliderFlags_AlwaysClamp|
                ImGuiSliderFlags_Logarithmic);
          };
          constexpr double zero=0.0;
          constexpr double coefficient_maximum=1.0e-3;
          constexpr double anisotropy_minimum=-0.95;
          constexpr double anisotropy_maximum=0.95;
          constexpr double one=1.0;
          bool changed=false;
          changed|=drag_double("Ground radius (m)",
              atmosphere_parameters.ground_radius_metres,
              1000.0,1000.0,1.0e9,"%.0f");
          changed|=drag_double("Atmosphere height (m)",
              atmosphere_parameters.atmosphere_height_metres,
              100.0,100.0,1.0e7,"%.0f");
          changed|=drag_double("Rayleigh height (m)",
              atmosphere_parameters.rayleigh_scale_height_metres,
              20.0,100.0,1.0e6,"%.0f");
          changed|=drag_double("Aerosol height (m)",
              atmosphere_parameters.mie_scale_height_metres,
              10.0,10.0,1.0e6,"%.0f");
          changed|=ImGui::DragScalarN("Rayleigh scattering",ImGuiDataType_Double,
              atmosphere_parameters.rayleigh_scattering_per_metre.data(),3,
              1.0e-7F,&zero,&coefficient_maximum,"%.3e",
              ImGuiSliderFlags_AlwaysClamp);
          changed|=ImGui::DragScalarN("Aerosol scattering",ImGuiDataType_Double,
              atmosphere_parameters.mie_scattering_per_metre.data(),3,
              1.0e-7F,&zero,&coefficient_maximum,"%.3e",
              ImGuiSliderFlags_AlwaysClamp);
          changed|=ImGui::DragScalarN("Aerosol absorption",ImGuiDataType_Double,
              atmosphere_parameters.mie_absorption_per_metre.data(),3,
              1.0e-7F,&zero,&coefficient_maximum,"%.3e",
              ImGuiSliderFlags_AlwaysClamp);
          changed|=ImGui::DragScalarN("Upper-air absorption",ImGuiDataType_Double,
              atmosphere_parameters.absorption_per_metre.data(),3,
              1.0e-7F,&zero,&coefficient_maximum,"%.3e",
              ImGuiSliderFlags_AlwaysClamp);
          changed|=ImGui::DragScalar("Aerosol anisotropy",ImGuiDataType_Double,
              &atmosphere_parameters.mie_anisotropy,0.005F,
              &anisotropy_minimum,&anisotropy_maximum,"%.3f",
              ImGuiSliderFlags_AlwaysClamp);
          changed|=ImGui::DragScalarN("Ground albedo",ImGuiDataType_Double,
              atmosphere_parameters.ground_albedo.data(),3,0.005F,&zero,&one,
              "%.3f",ImGuiSliderFlags_AlwaysClamp);
          constexpr double altitude_maximum=1.0e7;
          changed|=ImGui::DragScalar("Absorption peak (m)",ImGuiDataType_Double,
              &atmosphere_parameters.absorption_peak_altitude_metres,
              100.0F,&zero,&altitude_maximum,"%.0f",
              ImGuiSliderFlags_AlwaysClamp);
          changed|=drag_double("Absorption width (m)",
              atmosphere_parameters.absorption_half_width_metres,
              100.0,1.0,1.0e7,"%.0f");
          constexpr double irradiance_maximum=20.0;
          changed|=ImGui::DragScalarN("Solar irradiance",ImGuiDataType_Double,
              atmosphere_parameters.solar_irradiance.data(),3,0.01F,&zero,
              &irradiance_maximum,"%.3f",ImGuiSliderFlags_AlwaysClamp);
          constexpr double sun_radius_minimum=0.0001;
          constexpr double sun_radius_maximum=0.1;
          changed|=ImGui::DragScalar("Solar radius (rad)",ImGuiDataType_Double,
              &atmosphere_parameters.solar_angular_radius_radians,
              0.00005F,&sun_radius_minimum,&sun_radius_maximum,"%.5f",
              ImGuiSliderFlags_AlwaysClamp);
          if(changed){
            atmosphere_preset_index=5;
            atmosphere_optical_dirty=true;
          }
        }
        if(ImGui::CollapsingHeader("Atmosphere quality")){
          constexpr std::array<const char*,3> visibility_backend_names{
              "Automatic","Ray traced","Fitted/min-max"};
          ImGui::SetNextItemWidth(190.0F);
          if(ImGui::Combo("Atmosphere visibility",&atmosphere_visibility_backend,
                          visibility_backend_names.data(),
                          visibility_backend_names.size()))
            static_cast<void>(apply_visibility_settings());
          if(ImGui::Checkbox("iOS performance mode",&ios_performance_mode))
            static_cast<void>(apply_visibility_settings());
          const auto visibility_plan=
              tetra_viewer::resolve_atmosphere_visibility_plan(
                  {.requested=static_cast<tetra_viewer::AtmosphereVisibilityBackend>(
                       atmosphere_visibility_backend),
                   .ios_performance_mode=ios_performance_mode},
                  metal_ray_tracing_supported&&
                      terrain_acceleration_structure.active!=nil&&
                      terrain_acceleration_structure.active_generation==
                          terrain_display_front.render_generation&&
                      atmosphere_resources.ray_visibility_pipeline!=nil);
          ImGui::TextDisabled("Metal ray tracing: %s; backend: %s",
              metal_ray_tracing_supported?"supported":"not supported",
              tetra_viewer::atmosphere_visibility_backend_name(
                  visibility_plan.effective).data());
          if(!visibility_plan.requested_backend_available)
            ImGui::TextDisabled(
                "Ray tracing requested; waiting for a completed terrain structure.");
          ImGui::TextDisabled("Visibility preset: %ux, %u intervals/frame, 4 rays/interval",
              visibility_plan.screen_divisor,
              visibility_plan.rotating_queries_per_pixel);
          if(ImGui::Combo("Profile",&atmosphere_quality_index,
                          "Low\0Default\0High\0")){
            if(!apply_atmosphere_quality(atmosphere_quality_index))
              std::fprintf(stderr,
                  "Unable to recreate Metal atmosphere quality resources.\n");
          }
          int integration_index=atmosphere_screen_divisor-1;
          const bool integration_locked=ios_performance_mode||
              visibility_plan.effective==
                  tetra_viewer::AtmosphereVisibilityBackend::ray_traced;
          if(integration_locked)ImGui::BeginDisabled();
          if(ImGui::Combo("Screen integration",&integration_index,
                          "Full\0Half\0One third\0One quarter\0")){
            atmosphere_screen_divisor=integration_index+1;
            static_cast<void>(ensure_screen_atmosphere_resources(
                device,atmosphere_resources,render_width,render_height,
                atmosphere_screen_divisor,atmosphere_transport!=2));
          }
          if(integration_locked)ImGui::EndDisabled();
          constexpr double minimum_aerial_range=10'000.0;
          constexpr double maximum_aerial_range=10'000'000.0;
          ImGui::DragScalar("Aerial range (m)",ImGuiDataType_Double,
              &atmosphere_aerial_range,1'000.0F,&minimum_aerial_range,
              &maximum_aerial_range,"%.0f",
              ImGuiSliderFlags_AlwaysClamp|
              ImGuiSliderFlags_Logarithmic);
        }
        if(ImGui::CollapsingHeader("Atmosphere diagnostics")){
          constexpr std::array<const char*,31> debug_names{
              "Final composition","Transmittance lookup",
              "Multiple scattering lookup","Sky-view lookup",
              "Aerial scattering slice","Aerial transmittance slice",
              "Reversed depth","Shadow cascade 0","Shadow cascade 1",
              "Shadow cascade 2","Shadow cascade 3",
              "Long-path shadow coverage","Long-path direct loss",
              "Full-sky before terrain shadow",
              "Full-sky after terrain shadow",
              "Receiver-fitted atmosphere shadow",
              "Full-resolution direct scattering",
              "Full-resolution multiple scattering",
              "Direct scattering after terrain shadow",
              "HDR terrain before atmosphere",
              "Surface-truncated direct after shadow",
              "Surface-truncated multiple scattering",
              "Raw directional shadow loss",
              "Long-shadow epipolar classification",
              "Long-shadow epipolar traversal",
              "Terrain direct-shadow visibility",
              "Terrain indirect lighting","Terrain direct lighting",
              "Terrain selected local shadow","Terrain fitted shadow",
              "Terrain outer-cascade coverage"};
          ImGui::SetNextItemWidth(190.0F);
          if(ImGui::BeginCombo("Debug view",
                               debug_names[atmosphere_debug_view])){
            for(std::size_t index=0;index<debug_names.size();++index){
              if(ImGui::Selectable(debug_names[index],
                                   atmosphere_debug_view==
                                       static_cast<int>(index)))
                atmosphere_debug_view=static_cast<int>(index);
            }
            ImGui::EndCombo();
          }
          if(gpu_stage_timings->valid.load(std::memory_order_relaxed)){
            ImGui::Text("GPU shadow %.2f ms  atmosphere %.2f ms",
                gpu_stage_timings->shadows_milliseconds.load(
                    std::memory_order_relaxed),
                gpu_stage_timings->atmosphere_milliseconds.load(
                    std::memory_order_relaxed));
            ImGui::Text("GPU terrain %.2f ms  composite %.2f ms",
                gpu_stage_timings->terrain_milliseconds.load(
                    std::memory_order_relaxed),
                gpu_stage_timings->composite_milliseconds.load(
                    std::memory_order_relaxed));
            if(gpu_stage_timings->screen_stages_valid.load(
                   std::memory_order_relaxed))
              ImGui::Text("GPU depth %.2f  integrate %.2f  temporal %.2f ms",
                  gpu_stage_timings->depth_reduction_milliseconds.load(
                      std::memory_order_relaxed),
                  gpu_stage_timings->screen_integration_milliseconds.load(
                      std::memory_order_relaxed),
                  gpu_stage_timings->temporal_reconstruction_milliseconds.load(
                      std::memory_order_relaxed));
          }else if(gpu_milliseconds>0.0)
            ImGui::Text("GPU frame %.2f ms",gpu_milliseconds);
          else ImGui::TextDisabled("GPU timings pending");
          ImGui::Text("Atmosphere allocation %.1f MiB",
              static_cast<double>(live_atmosphere_allocation_bytes(
                  atmosphere_resources))/(1024.0*1024.0));
          const auto scene_target_bytes=static_cast<double>(render_width)*
              render_height*12.0*static_cast<double>(1U+(
                  active_samples>1U?active_samples:0U));
          ImGui::Text("HDR/depth allocation %.1f MiB",
                      scene_target_bytes/(1024.0*1024.0));
          const auto& dispatches=atmosphere_resources.dispatch_counts;
          ImGui::Text("LUT dispatch T %llu M %llu S %llu I %llu A %llu L %llu",
              static_cast<unsigned long long>(dispatches[0]),
              static_cast<unsigned long long>(dispatches[1]),
              static_cast<unsigned long long>(dispatches[2]),
              static_cast<unsigned long long>(dispatches[4]),
              static_cast<unsigned long long>(dispatches[3]),
              static_cast<unsigned long long>(dispatches[6]));
          ImGui::Text("Screen dispatch E %llu I %llu T %llu P %llu F %llu",
              static_cast<unsigned long long>(dispatches[12]),
              static_cast<unsigned long long>(dispatches[13]),
              static_cast<unsigned long long>(dispatches[14]),
              static_cast<unsigned long long>(dispatches[15]),
              static_cast<unsigned long long>(dispatches[16]));
          ImGui::Text("Temporal attempts %llu  compatible %llu  invalid %llu",
              static_cast<unsigned long long>(
                  atmosphere_resources.temporal_history_attempts),
              static_cast<unsigned long long>(
                  atmosphere_resources.temporal_history_compatible),
              static_cast<unsigned long long>(
                  atmosphere_resources.temporal_history_invalidations));
          ImGui::Text("Temporal camera visibility refreshes %llu",
              static_cast<unsigned long long>(
                  atmosphere_resources.temporal_camera_refreshes));
          ImGui::Text("Shadow hierarchy 2D %llu  epipolar %llu/%llu/%llu",
              static_cast<unsigned long long>(dispatches[8]),
              static_cast<unsigned long long>(dispatches[9]),
              static_cast<unsigned long long>(dispatches[10]),
              static_cast<unsigned long long>(dispatches[11]));
          ImGui::Text("Shadow refresh local %llu  fitted %llu",
              static_cast<unsigned long long>(shadow_cascade_refreshes),
              static_cast<unsigned long long>(fitted_shadow_refreshes));
        }
        constexpr double exposure_minimum=-6.0;
        constexpr double exposure_maximum=6.0;
        ImGui::SetNextItemWidth(190.0F);
        ImGui::SliderScalar("Exposure (EV)",ImGuiDataType_Double,&exposure_ev,
            &exposure_minimum,&exposure_maximum,"%.2f");

        if(runtime&&ImGui::CollapsingHeader("Terrain diagnostics")){
          if(ImGui::Checkbox("Experimental fast terrain preview",&preview_enabled)&&
             !preview_enabled)
            preview_surface_worker.cancel();
          ImGui::Text("Scene generation %llu   triangles %zu",
              static_cast<unsigned long long>(uploaded_generation),
              scene_vertex_count/3U);
          const auto& preview_state=terrain_front_coordinator.state();
          ImGui::Text("Preview CPU %s   GPU %s   upload %.2f MiB",
              preview_state.preview_awaiting_upload?"ready":
                  (preview_state.preview_requested?"pending":"idle"),
              terrain_display_front.preview_cpu?"visible":"exact",
              static_cast<double>(terrain_display_front.upload_bytes)/
                  (1024.0*1024.0));
          ImGui::Text("Peak display transition %.2f MiB",
              static_cast<double>(peak_terrain_display_transition_bytes)/
                  (1024.0*1024.0));
          ImGui::Text("Display generation %llu   exact/preview %zu / %zu",
              static_cast<unsigned long long>(
                  terrain_display_front.render_generation),
              (terrain_display_front.indexed_exact_selection?
                   terrain_display_front.exact_index_count:
                   terrain_display_front.exact_vertex_count)/3U,
              terrain_display_front.preview_index_count/3U);
          if(metal_gpu_terrain_diagnostic||metal_gpu_terrain_native_diagnostic)ImGui::Text(
              "GPU terrain slots D %llu C %llu A %llu stale %llu fail %llu over %llu",
              static_cast<unsigned long long>(
                  gpu_terrain_counters->dispatched.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(
                  gpu_terrain_counters->completed.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(
                  gpu_terrain_counters->accepted.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(
                  gpu_terrain_counters->stale_rejected.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(
                  gpu_terrain_counters->failed.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(
                  gpu_terrain_counters->overflow.load(std::memory_order_relaxed)));
          // P7e4 is the user-facing renderer choice.  The older P6 mesh
          // emission experiment stays available through its explicit
          // diagnostic environment route, but must not masquerade as this
          // GPU-resident path: it still consumes CPU-built terrain packets.
          const bool device_front_toggle_locked=
              metal_gpu_terrain_device_front_explicit||
              metal_gpu_terrain_live_selection_requested||
              metal_gpu_terrain_diagnostic||metal_gpu_terrain_native_diagnostic||
              gpu_terrain_renderer_selected;
          if(device_front_toggle_locked)ImGui::BeginDisabled();
          if(ImGui::Checkbox("Use GPU-resident terrain",
                             &metal_gpu_terrain_device_front)){
            metal_gpu_terrain_live_selection=metal_gpu_terrain_device_front||
                metal_gpu_terrain_live_selection_requested;
            gpu_terrain_renderer_available=false;
            if(!metal_gpu_terrain_device_front){
              // Return to a complete CPU front immediately.  The normal CPU
              // publication branch resumes on the next frame for the current
              // camera; no incomplete or failed GPU candidate is exposed.
              if(device_front_bootstrap_display.ready()){
                terrain_display_front=device_front_bootstrap_display;
                scene_vertices=terrain_display_front.exact_vertices;
                scene_vertex_count=terrain_display_front.triangle_count()*3U;
              }
              force_runtime_camera=true;
            }
          }
          if(device_front_toggle_locked)ImGui::EndDisabled();
          if(metal_gpu_terrain_device_front_explicit)
            ImGui::TextDisabled("Startup override: GPU-resident terrain is %s",
                metal_gpu_terrain_device_front?"forced on":"forced off");
          const bool device_private_front_active=
              metal_gpu_terrain_device_front&&gpu_terrain_active_front.promoted&&
              terrain_display_front.exact_vertices==gpu_terrain_active_front.vertices&&
              terrain_display_front.exact_indirect_arguments==
                  gpu_terrain_active_front.indirect_arguments&&
              !terrain_display_front.indexed_exact_selection;
          if(device_private_front_active)
            ImGui::Text("GPU-resident terrain: active (direct private front)");
          else if(metal_gpu_terrain_device_front)
            ImGui::Text("GPU-resident terrain: starting (CPU bootstrap visible)");
          else
            ImGui::Text("CPU terrain fallback: active");
          ImGui::TextDisabled("Set TETWORLD_METAL_GPU_TERRAIN_DEVICE_FRONT=0 "
                              "to force CPU terrain at startup.");
          if(metal_gpu_terrain_live_selection)
            ImGui::Text("GPU selection marks %llu/%llu; CPU bootstrap front retained",
                static_cast<unsigned long long>(gpu_hierarchy_live_selection.accepted),
                static_cast<unsigned long long>(gpu_hierarchy_live_selection.submitted));
          ImGui::Text("Resident %.1f MiB   cache %.1f MiB",
              static_cast<double>(diagnostics.resident_bytes)/(1024.0*1024.0),
              static_cast<double>(diagnostics.retained_cache_bytes)/
                  (1024.0*1024.0));
          ImGui::Text("Demand visible/guard/predicted %zu / %zu / %zu",
              diagnostics.visible_hierarchy_blocks,
              diagnostics.guard_hierarchy_blocks,
              diagnostics.predicted_hierarchy_blocks);
          ImGui::Text("Demand recent/cold %zu / %zu",
              diagnostics.recent_hierarchy_blocks,
              diagnostics.cold_hierarchy_blocks);
          ImGui::Text("Render blocks reused/rebuilt %zu / %zu",
              diagnostics.reused_render_blocks,
              diagnostics.rebuilt_render_blocks);
          ImGui::Text("CPU high water %.1f MiB   triangle high water %zu",
              static_cast<double>(diagnostics.cpu_high_water_bytes)/
                  (1024.0*1024.0),diagnostics.triangle_high_water);
          ImGui::Text("Shadow cascades %s   refreshes %llu",
              std::ranges::all_of(shadow_initialized,
                                  [](bool ready){return ready;})?
                  "ready":"warming",
              static_cast<unsigned long long>(shadow_cascade_refreshes));
          ImGui::Text("Budget %s",diagnostics.budget_exceeded?
                      "EXCEEDED":"within limits");
        }
        if(!pointer_captured&&ImGui::Button("Capture pointer")){
          pointer_captured=true;
          glfwSetInputMode(window,GLFW_CURSOR,GLFW_CURSOR_DISABLED);
          glfwGetCursorPos(window,&previous_cursor_x,&previous_cursor_y);
        }
        ImGui::End();
        if(!pointer_captured&&glfwGetMouseButton(
               window,GLFW_MOUSE_BUTTON_LEFT)==GLFW_PRESS&&!io.WantCaptureMouse){
          pointer_captured=true;
          glfwSetInputMode(window,GLFW_CURSOR,GLFW_CURSOR_DISABLED);
          glfwGetCursorPos(window,&previous_cursor_x,&previous_cursor_y);
        }
        ImGui::Render();

        id<MTLCommandBuffer> command_buffer=[command_queue commandBuffer];
        command_buffer.label=@"TetWorld frame";
        // Acquire before native terrain work so a counter interval can bracket
        // its full private generation and publication path.
        MetalTimestampFlight* gpu_timestamp_flight=nullptr;
        for(auto& flight:gpu_timestamp_flights){
          bool available=false;
          if(flight.samples!=nil&&flight.in_use->compare_exchange_strong(
                 available,true,std::memory_order_acq_rel)){
            gpu_timestamp_flight=&flight;
            break;
          }
        }
        id<MTLCounterSampleBuffer> gpu_timestamp_samples=
            gpu_timestamp_flight==nullptr?nil:gpu_timestamp_flight->samples;
        id<MTLBuffer> gpu_timestamp_results=
            gpu_timestamp_flight==nullptr?nil:gpu_timestamp_flight->results;
        id<MTLBuffer> gpu_timestamp_scratch=
            gpu_timestamp_flight==nullptr?nil:gpu_timestamp_flight->scratch;
        bool owner_direct_generation_encoded_this_frame=false;
        if(metal_gpu_terrain_device_front&&
           gpu_terrain_active_front.seed_pending&&
           gpu_hierarchy_live_selection.submitted!=0U){
          // P7e4a1 stops at its compact closure output.  It neither seeds nor
          // promotes the private terrain front: P8 owns that later boundary.
          // Keep the CPU display front selected without treating an unused
          // private allocation as a CPU-derived GPU candidate.
          gpu_terrain_active_front.seed_pending=false;
        }else if(gpu_terrain_active_front.seed_pending&&
           gpu_terrain_active_front.vertices!=nil&&
           gpu_terrain_active_front.indirect_arguments!=nil&&
           gpu_terrain_active_front.seed_vertices!=nil&&
           gpu_terrain_active_front.seed_arguments!=nil){
          id<MTLBlitCommandEncoder> seed=[command_buffer blitCommandEncoder];
          [seed copyFromBuffer:gpu_terrain_active_front.seed_vertices
                   sourceOffset:0U toBuffer:gpu_terrain_active_front.vertices
              destinationOffset:0U size:gpu_terrain_active_front.seed_vertices.length];
          [seed copyFromBuffer:gpu_terrain_active_front.seed_arguments
                   sourceOffset:0U toBuffer:gpu_terrain_active_front.indirect_arguments
              destinationOffset:0U size:gpu_terrain_active_front.seed_arguments.length];
          [seed endEncoding];
          const auto active_complete=gpu_terrain_active_front.completed;
          [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> command){
            active_complete->store(command.status==MTLCommandBufferStatusCompleted,
                                   std::memory_order_release);
          }];
          gpu_terrain_active_front.seed_pending=false;
        }
        if(gpu_hierarchy_live_selection_tuple&&
           gpu_lod_live_selection_pipeline!=nil){
          retire_metal_gpu_hierarchy_live_selection(
              gpu_hierarchy_live_selection);
          // P7e4b: a compact P8 flight is allowed to replace the CPU
          // bootstrap display front only after its command buffer has retired
          // and its scalar private-commit audit is valid.  This deliberately
          // copies no candidate payload or count: the completed P8 status is
          // the sole host-visible admission record, while the actual vertex
          // and indirect buffers stay private and are passed directly to draw.
          if(metal_gpu_terrain_device_front&&runtime!=nullptr&&
             gpu_hierarchy_live_selection.compact_p8_private_commits>
                 gpu_terrain_active_front.displayed_p8_commits){
            const auto& p8=gpu_hierarchy_live_selection.compact_p8_last_audit;
            const auto* current_directory=runtime->world_cut_directory();
            const auto current_source=current_directory==nullptr?0U:
                current_directory->revision();
            const auto current_field=runtime->published_view_identity().field_revision;
            const auto current_origin=runtime->render_origin();
            const bool audit_valid=p8[0U]==1U&&p8[1U]==0U&&p8[2U]!=0U&&
                p8[3U]==0U&&p8[4U]!=0U&&p8[2U]%3U==0U&&
                p8[2U]<=gpu_terrain_active_front.vertex_capacity;
            // A private buffer is not sufficient admission evidence: the
            // candidate must contain every vertex in the complete CPU front
            // it proposes to replace.
            const bool complete_front_vertex_parity=
                device_front_bootstrap_display.exact_vertex_count!=0U&&
                p8[2U]==device_front_bootstrap_display.exact_vertex_count;
            const bool current_bootstrap=
                !terrain_display_front.preview_cpu&&terrain_display_front.ready()&&
                gpu_terrain_active_front.vertices!=nil&&
                gpu_terrain_active_front.indirect_arguments!=nil&&
                gpu_terrain_active_front.identity==terrain_display_front.identity&&
                terrain_display_front.identity.exact_generation==
                    gpu_hierarchy_live_selection.bootstrap_scene_generation&&
                terrain_display_front.identity.exact_view.field_revision==
                    gpu_hierarchy_live_selection.field_revision&&
                current_source==gpu_hierarchy_live_selection.source_revision&&
                current_field==gpu_hierarchy_live_selection.field_revision&&
                terrain_display_front.identity.render_origin.x==current_origin.x&&
                terrain_display_front.identity.render_origin.y==current_origin.y&&
                terrain_display_front.identity.render_origin.z==current_origin.z;
            if(audit_valid&&complete_front_vertex_parity&&current_bootstrap){
              // Commit every coupled display handle as one new publication;
              // consumers never observe a GPU vertex pointer with old CPU
              // indexing/count metadata.
              auto promoted=terrain_display_front;
              promoted.exact_vertices=gpu_terrain_active_front.vertices;
              promoted.exact_indices=nil;
              promoted.exact_indirect_arguments=
                  gpu_terrain_active_front.indirect_arguments;
              promoted.indexed_exact_selection=false;
              promoted.exact_vertex_count=p8[2U];
              promoted.exact_index_count=0U;
              promoted.render_generation=next_terrain_render_generation++;
              if(next_terrain_render_generation==0U)
                next_terrain_render_generation=1U;
              terrain_display_front=std::move(promoted);
              scene_vertices=terrain_display_front.exact_vertices;
              scene_vertex_count=terrain_display_front.exact_vertex_count;
              gpu_terrain_active_front.promoted=true;
              gpu_terrain_active_front.displayed_p8_commits=
                  gpu_hierarchy_live_selection.compact_p8_private_commits;
              gpu_terrain_renderer_available=true;
              gpu_terrain_counters->device_front_display_promotions.fetch_add(
                  1U,std::memory_order_relaxed);
            }
          }
          if(!metal_gpu_terrain_device_front||
             !gpu_hierarchy_live_selection.closure_pending)
            if(encode_metal_gpu_hierarchy_live_selection(
                   command_buffer,gpu_lod_live_selection_pipeline,
                   metal_gpu_terrain_device_front?
                       gpu_hierarchy_compact_worklist_pipeline:nil,
                   gpu_hierarchy_live_selection,
                   *gpu_hierarchy_live_selection_tuple)&&
               metal_gpu_terrain_device_front&&
               encode_metal_gpu_hierarchy_live_compact_closure(
                  command_buffer,gpu_hierarchy_canonicalize_pipeline,
                  gpu_hierarchy_compact_green_pipeline,
                  gpu_hierarchy_compact_red_pipeline,
                  gpu_hierarchy_compact_red_scan_pipeline,
                  gpu_hierarchy_live_selection,
                  metal_gpu_terrain_device_front_inject_green_budget_failure)&&
               encode_metal_gpu_hierarchy_compact_owner_materialize(
                  command_buffer,gpu_hierarchy_compact_owner_materialize_pipeline,
                  gpu_hierarchy_live_selection)&&
               runtime!=nullptr&&gpu_terrain_active_front.vertices!=nil&&
               gpu_terrain_active_front.indirect_arguments!=nil&&
               ensure_metal_gpu_hierarchy_compact_p8_workspace(device,
                  gpu_hierarchy_live_selection,
                  gpu_terrain_active_front.vertex_capacity)&&
               encode_metal_gpu_hierarchy_compact_owner_p8(command_buffer,
                  gpu_hierarchy_compact_owner_p8_pipelines,
                  gpu_hierarchy_live_selection.compact_owner_stream,
                  gpu_hierarchy_live_selection.compact_owner_header,
                  gpu_hierarchy_live_selection.compact_p8_field,
                  gpu_hierarchy_live_selection.compact_p8_templates,
                  gpu_hierarchy_live_selection.compact_p8_counts,
                  gpu_hierarchy_live_selection.compact_p8_offsets,
                  gpu_hierarchy_live_selection.compact_p8_block_totals,
                  gpu_hierarchy_live_selection.compact_p8_block_offsets,
                  gpu_hierarchy_live_selection.compact_p8_level_totals,
                  gpu_hierarchy_live_selection.compact_p8_level_offsets,
                  gpu_hierarchy_live_selection.compact_p8_signs,
                  gpu_hierarchy_live_selection.compact_p8_candidate,
                  gpu_hierarchy_live_selection.compact_p8_status,
                  gpu_hierarchy_live_selection.compact_p8_dispatches,
                  gpu_terrain_active_front.vertices,
                  gpu_terrain_active_front.indirect_arguments,
                  gpu_hierarchy_live_selection.compact_p8_vertex_capacity,
                  runtime->render_origin(),
                  gpu_hierarchy_live_selection.source_revision)) {
              id<MTLBlitCommandEncoder> p8_audit=[command_buffer blitCommandEncoder];
              [p8_audit copyFromBuffer:gpu_hierarchy_live_selection.compact_p8_status
                   sourceOffset:0U toBuffer:gpu_hierarchy_live_selection.compact_p8_audit
              destinationOffset:0U size:2U*sizeof(std::uint32_t)];
              [p8_audit copyFromBuffer:gpu_hierarchy_live_selection.compact_p8_candidate
                   sourceOffset:0U toBuffer:gpu_hierarchy_live_selection.compact_p8_audit
              destinationOffset:2U*sizeof(std::uint32_t) size:4U*sizeof(std::uint32_t)];
              [p8_audit copyFromBuffer:gpu_hierarchy_live_selection.compact_owner_header
                   sourceOffset:0U toBuffer:gpu_hierarchy_live_selection.compact_p8_audit
              destinationOffset:6U*sizeof(std::uint32_t) size:4U*sizeof(std::uint32_t)];
              [p8_audit endEncoding];
              ++gpu_hierarchy_live_selection.compact_p8_encoded;
              gpu_hierarchy_live_selection.device_front_p8_at=
                  std::chrono::steady_clock::now();
              gpu_terrain_counters->device_closure_submitted.fetch_add(1U,
                  std::memory_order_relaxed);
              gpu_terrain_counters->device_owner_submitted.fetch_add(1U,
                  std::memory_order_relaxed);
            }
        }
        if(metal_gpu_terrain_diagnostic&&gpu_terrain_extract_pipeline!=nil&&runtime&&
           !terrain_display_front.preview_cpu&&terrain_display_front.ready()){
          // This is the authority assertion for P5c2: the diagnostic buffers
          // are never substituted into the CPU-published display front.
          gpu_terrain_counters->cpu_front_frames.fetch_add(1U,
              std::memory_order_relaxed);
          if(scene_vertices!=terrain_display_front.exact_vertices||
             terrain_display_front.indexed_exact_selection||
             terrain_display_front.preview_cpu)
            gpu_terrain_counters->cpu_front_violations.fetch_add(1U,
                std::memory_order_relaxed);
          auto& slot=gpu_terrain_slots[gpu_terrain_slot_cursor++%gpu_terrain_slots.size()];
          const auto cells=runtime->world_surface_gpu_cells();
          const auto generation=diagnostics.scene_generation;
          const auto origin=runtime->scene().render_origin;
          if(!slot.pending&&!cells.empty()&&generation!=0U&&
             cells.size()<=std::numeric_limits<std::uint32_t>::max()&&
             terrain_display_front.exact_vertex_count<=std::numeric_limits<std::uint32_t>::max()){
            const auto vertex_count=terrain_display_front.exact_vertex_count;
            const auto expected_fingerprint=metal_terrain_fingerprint(
                runtime->scene().triangle_vertices);
            slot.cells=[device newBufferWithBytes:cells.data()
                length:cells.size()*sizeof(tetra::GpuTerrainCellRecord)
                options:MTLResourceStorageModeShared];
            slot.output=[device newBufferWithLength:sizeof(std::uint32_t)*8U+
                vertex_count*sizeof(tetra_viewer::SceneVertex)
                options:MTLResourceStorageModeShared];
            slot.indices=[device newBufferWithLength:vertex_count*sizeof(std::uint32_t)
                options:MTLResourceStorageModeShared];
            if(slot.cells!=nil&&slot.output!=nil&&slot.indices!=nil){
              std::memset(slot.output.contents,0,slot.output.length);
              std::memset(slot.indices.contents,0,slot.indices.length);
              slot.scene_generation=generation;slot.render_origin=origin;
              slot.expected_vertices=static_cast<std::uint32_t>(vertex_count);
              slot.expected_fingerprint=expected_fingerprint;
              slot.completed->store(false,std::memory_order_release);
              slot.succeeded->store(false,std::memory_order_release);slot.pending=true;
              const std::array<std::uint32_t,4> params{static_cast<std::uint32_t>(cells.size()),slot.expected_vertices,slot.expected_vertices,1U};
              id<MTLComputeCommandEncoder> compute=[command_buffer computeCommandEncoder];
              [compute setComputePipelineState:gpu_terrain_extract_pipeline];
              [compute setBuffer:slot.output offset:0 atIndex:0U];[compute setBuffer:slot.indices offset:0 atIndex:1U];
              [compute setBytes:params.data() length:sizeof(params) atIndex:2U];[compute setBuffer:slot.cells offset:0 atIndex:3U];
              [compute dispatchThreads:MTLSizeMake(cells.size(),1U,1U) threadsPerThreadgroup:MTLSizeMake(64U,1U,1U)];
              [compute endEncoding];
              const auto complete=slot.completed,success=slot.succeeded;
              const auto counters=gpu_terrain_counters;
              id<MTLBuffer> output=slot.output;
              id<MTLBuffer> indices=slot.indices;
              counters->dispatched.fetch_add(1U,std::memory_order_relaxed);
              [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> command){
                const auto* header=static_cast<const std::uint32_t*>(output.contents);
                const bool command_complete=
                    command.status==MTLCommandBufferStatusCompleted;
                const bool header_valid=header!=nullptr&&header[0]==vertex_count&&
                    header[1]==1U&&header[6]==vertex_count;
                const bool overflown=header!=nullptr&&header[5]!=0U;
                if(overflown)counters->overflow.fetch_add(1U,
                    std::memory_order_relaxed);
                bool index_valid=header_valid&&!overflown;
                const auto* result_indices=static_cast<const std::uint32_t*>(
                    indices.contents);
                if(result_indices==nullptr)index_valid=false;
                for(std::uint32_t index=0U;index_valid&&index<vertex_count;++index)
                  index_valid=result_indices[index]==index;
                const auto* vertices=header==nullptr?nullptr:
                    reinterpret_cast<const tetra_viewer::SceneVertex*>(header+8U);
                const bool fingerprint_valid=vertices!=nullptr&&index_valid&&
                    metal_terrain_fingerprint(std::span{vertices,
                        static_cast<std::size_t>(vertex_count)})==
                        expected_fingerprint;
                const bool passed=command_complete&&header_valid&&!overflown&&
                    index_valid&&fingerprint_valid;
                if(!passed)counters->failed.fetch_add(1U,
                    std::memory_order_relaxed);
                counters->completed.fetch_add(1U,std::memory_order_relaxed);
                success->store(passed,std::memory_order_release);
                complete->store(true,std::memory_order_release);
              }];
            }
          }
        }
        if((metal_gpu_terrain_native_diagnostic||gpu_terrain_renderer_selected)&&runtime&&
           !terrain_display_front.preview_cpu&&terrain_display_front.ready()){
          // The legacy native route captures a self-contained P6 packet from a
          // complete publication. P7e4a instead receives only the just-built
          // device P7e3c owner stream and its device header; it never asks the
          // runtime for P6 or CPU surface payloads.
          gpu_terrain_counters->cpu_front_frames.fetch_add(1U,
              std::memory_order_relaxed);
          if(scene_vertices!=terrain_display_front.exact_vertices||
             terrain_display_front.indexed_exact_selection||
             terrain_display_front.preview_cpu)
            gpu_terrain_counters->cpu_front_violations.fetch_add(1U,
                std::memory_order_relaxed);
          auto& slot=gpu_terrain_native_slots[
              gpu_terrain_native_slot_cursor++%gpu_terrain_native_slots.size()];
          const auto* directory=runtime->world_cut_directory();
          // The P6 sidecar was constructed by the private terrain
          // publication.  Never reconstruct closure (or its packet) from the
          // presenter: a missing/stale sidecar simply retains this front.
          const auto* packet=[&]{
            gpu_terrain_counters->p6_requests.fetch_add(1U,
                std::memory_order_relaxed);
            return runtime->gpu_green_mask_packet();
          }();
          const auto origin=runtime->render_origin();
          const auto field_revision=runtime->published_view_identity().field_revision;
          const auto source_revision=directory==nullptr?0U:directory->revision();
          const auto vertex_count=terrain_display_front.exact_vertex_count;
          if(!slot.pending&&directory!=nullptr&&
             packet!=nullptr&&
             (!packet||packet->header.source_revision==source_revision)&&source_revision!=0U&&
             diagnostics.scene_generation!=0U&&field_revision!=0U&&
             vertex_count>=12U&&
             vertex_count<=std::numeric_limits<std::uint32_t>::max()&&
             gpu_terrain_active_front.vertices!=nil&&
             gpu_terrain_active_front.indirect_arguments!=nil&&
             gpu_terrain_active_front.vertex_capacity>=vertex_count&&
             gpu_terrain_active_front.identity==terrain_display_front.identity&&
             true){
            try {
              tetra::GpuTerrainFieldTupleParameters parameters;
              parameters.field=runtime->field();
              parameters.domain=runtime->profile().domain;
              parameters.source_revision=source_revision;
              parameters.field_revision=field_revision;
              const auto tuple=tetra::make_gpu_terrain_field_tuple(parameters);
              const auto templates=tetra::make_gpu_green_template_table();
              // A production ground cut currently carries roughly a million
              // candidate vertices.  Keep an explicit per-flight ceiling
              // rather than silently truncating a valid P6 packet.  Three
              // diagnostic flights remain bounded to 768 MiB and are opt-in
              // only; P8c owns replacing the root-expanded live route.
              constexpr std::size_t maximum_slot_bytes=256U*1024U*1024U;
              const std::size_t owner_count=metal_gpu_terrain_device_front?
                  gpu_hierarchy_live_selection.record_count:packet->owners.size();
              const bool owner_direct=gpu_terrain_renderer_selected||
                  metal_gpu_terrain_device_front;
              const bool expected_rejection=metal_gpu_terrain_device_front&&
                  (metal_gpu_terrain_device_front_inject_failure||
                   metal_gpu_terrain_device_front_inject_capacity_failure||
                   metal_gpu_terrain_device_front_inject_green_budget_failure);
              const std::size_t root_slots=owner_direct?owner_count:owner_count*24U;
              const std::size_t compaction_blocks=(root_slots+255U)/256U;
              const std::size_t super_blocks=(compaction_blocks+255U)/256U;
              const std::size_t triangle_capacity=vertex_count/12U;
              // P7e4a reuses the bootstrap private-front allocation only as
              // a bounded candidate capacity.  A larger device result must
              // fail P8 validation and retain that front; it is never clipped
              // or resized from CPU candidate data.  The test switch forces
              // precisely that failure mode without changing normal capacity.
              const std::size_t vertex_capacity=
                  metal_gpu_terrain_device_front_inject_capacity_failure?0U:
                  vertex_count;
              const auto words_bytes=[](std::size_t words)->std::optional<std::size_t>{
                if(words>(std::numeric_limits<std::size_t>::max()/sizeof(std::uint32_t)))
                  return std::nullopt;
                return words*sizeof(std::uint32_t);
              };
              const auto roots_bytes=words_bytes(owner_direct?0U:4U+root_slots*24U);
              const auto counts_bytes=words_bytes(root_slots);
              const auto signs_bytes=words_bytes(owner_direct?owner_count*3U:0U);
              const auto block_bytes=words_bytes(compaction_blocks);
              const auto super_block_bytes=words_bytes(super_blocks);
              const auto triangles_bytes=words_bytes(owner_direct?0U:4U+triangle_capacity*16U);
              const auto projected_bytes=words_bytes(owner_direct?0U:4U+triangle_capacity*36U);
              const auto vertices_bytes=words_bytes(4U+vertex_capacity*18U);
              const bool fits_slot_budget=roots_bytes&&counts_bytes&&signs_bytes&&block_bytes&&super_block_bytes&&triangles_bytes&&
                  projected_bytes&&vertices_bytes&&
                  *roots_bytes+3U * *counts_bytes+*signs_bytes+2U * *block_bytes+2U * *super_block_bytes+
                      4U*sizeof(std::uint32_t)<=maximum_slot_bytes&&
                  *triangles_bytes<=maximum_slot_bytes-*roots_bytes-
                      3U * *counts_bytes-*signs_bytes-2U * *block_bytes-2U * *super_block_bytes-4U*sizeof(std::uint32_t)&&
                  *projected_bytes<=maximum_slot_bytes-*roots_bytes-
                      3U * *counts_bytes-*signs_bytes-2U * *block_bytes-2U * *super_block_bytes-4U*sizeof(std::uint32_t)-
                      *triangles_bytes&&
                  *vertices_bytes<=maximum_slot_bytes-*roots_bytes-
                      3U * *counts_bytes-*signs_bytes-2U * *block_bytes-2U * *super_block_bytes-4U*sizeof(std::uint32_t)-
                      *triangles_bytes-*projected_bytes;
              const bool capacity_valid=owner_count!=0U&&
                  owner_count<=std::numeric_limits<std::uint32_t>::max()&&
                  root_slots<=std::numeric_limits<std::uint32_t>::max()&&
                  compaction_blocks<=std::numeric_limits<std::uint32_t>::max()&&
                  super_blocks<=std::numeric_limits<std::uint32_t>::max()&&
                  triangle_capacity<=std::numeric_limits<std::uint32_t>::max()&&
                  fits_slot_budget;
              if(!capacity_valid){
                gpu_terrain_counters->overflow.fetch_add(1U,
                    std::memory_order_relaxed);
              }else{
                const auto shared=[](id<MTLDevice> metal,const void* data,NSUInteger length){
                  return [metal newBufferWithBytes:data length:length
                      options:MTLResourceStorageModeShared];
                };
                slot.field=shared(device,&tuple,sizeof(tuple));
                if(metal_gpu_terrain_device_front){
                  // Device closure output is already the 12-word P8 owner
                  // shape. Its [failure, ..., emitted-count] header remains
                  // private and is bound to the owner passes directly.
                  slot.owners=gpu_hierarchy_live_selection.closure_owners;
                  slot.owner_header=gpu_hierarchy_live_selection.closure_status;
                  if(gpu_terrain_packet_upload.templates==nil)
                    gpu_terrain_packet_upload.templates=shared(
                        device,templates.data(),sizeof(templates));
                  slot.templates=gpu_terrain_packet_upload.templates;
                }else{
                  const std::array<std::uint32_t,4> owner_header{0U,0U,0U,
                      static_cast<std::uint32_t>(owner_count)};
                  slot.owner_header=owner_direct?shared(device,owner_header.data(),
                      sizeof(owner_header)):nil;
                  if(gpu_terrain_packet_upload.source_revision!=source_revision||
                     gpu_terrain_packet_upload.candidate_identity!=
                         packet->header.candidate_identity||
                     gpu_terrain_packet_upload.owner_count!=owner_count){
                    gpu_terrain_packet_upload.owners=shared(device,
                        packet->owners.data(),packet->owners.size()*
                            sizeof(packet->owners.front()));
                    gpu_terrain_packet_upload.templates=shared(
                        device,templates.data(),sizeof(templates));
                    gpu_terrain_packet_upload.source_revision=source_revision;
                    gpu_terrain_packet_upload.candidate_identity=
                        packet->header.candidate_identity;
                    gpu_terrain_packet_upload.owner_count=owner_count;
                  }
                  slot.owners=gpu_terrain_packet_upload.owners;
                  slot.templates=gpu_terrain_packet_upload.templates;
                }
                slot.roots=owner_direct?nil:[device newBufferWithLength:*roots_bytes
                    options:MTLResourceStorageModePrivate];
                slot.counts=[device newBufferWithLength:*counts_bytes options:MTLResourceStorageModePrivate];
                slot.signs=owner_direct?[device newBufferWithLength:*signs_bytes options:MTLResourceStorageModePrivate]:nil;
                slot.offsets=[device newBufferWithLength:*counts_bytes options:MTLResourceStorageModePrivate];
                slot.added_offsets=[device newBufferWithLength:*counts_bytes options:MTLResourceStorageModePrivate];
                slot.block_totals=[device newBufferWithLength:*block_bytes options:MTLResourceStorageModePrivate];
                slot.block_offsets=[device newBufferWithLength:*block_bytes options:MTLResourceStorageModePrivate];
                slot.block_totals2=[device newBufferWithLength:*super_block_bytes options:MTLResourceStorageModePrivate];
                slot.block_offsets2=[device newBufferWithLength:*super_block_bytes options:MTLResourceStorageModePrivate];
                slot.compaction_status=[device newBufferWithLength:4U*sizeof(std::uint32_t) options:MTLResourceStorageModePrivate];
                slot.triangles=owner_direct?nil:[device newBufferWithLength:*triangles_bytes
                    options:MTLResourceStorageModePrivate];
                slot.projected=owner_direct?nil:[device newBufferWithLength:*projected_bytes
                    options:MTLResourceStorageModePrivate];
                slot.vertices=[device newBufferWithLength:*vertices_bytes
                    options:MTLResourceStorageModePrivate];
                slot.commit_control=[device newBufferWithLength:
                    2U*sizeof(std::uint32_t) options:MTLResourceStorageModePrivate];
                slot.readback=(!metal_gpu_terrain_device_front&&
                    metal_gpu_terrain_qualification)?
                    [device newBufferWithLength:sizeof(std::uint32_t)*4U
                        options:MTLResourceStorageModeShared]:nil;
                slot.control_audit=metal_gpu_terrain_device_front?
                    [device newBufferWithLength:2U*sizeof(std::uint32_t)
                        options:MTLResourceStorageModeShared]:nil;
                if(slot.field&&slot.owners&&slot.templates&&
                   (owner_direct||slot.roots!=nil)&&slot.counts&&
                   (!owner_direct||(slot.signs!=nil&&slot.owner_header!=nil))&&
                   slot.offsets&&slot.added_offsets&&slot.block_totals&&
                   slot.block_offsets&&slot.block_totals2&&slot.block_offsets2&&slot.compaction_status&&
                   (owner_direct||(slot.triangles!=nil&&slot.projected!=nil))&&
                   slot.vertices&&slot.commit_control&&
                   (!metal_gpu_terrain_qualification||metal_gpu_terrain_device_front||
                    slot.readback!=nil)&&(!metal_gpu_terrain_device_front||
                    slot.control_audit!=nil)){
                  if(slot.readback!=nil)
                    std::memset(slot.readback.contents,0,slot.readback.length);
                  if(slot.control_audit!=nil)
                    std::memset(slot.control_audit.contents,0,
                                slot.control_audit.length);
                  slot.tuple=tuple;slot.scene_generation=diagnostics.scene_generation;
                  slot.source_revision=source_revision;slot.field_revision=field_revision;
                  slot.candidate_identity=packet==nullptr?
                      gpu_hierarchy_live_selection.slots[
                          gpu_hierarchy_live_selection.closure_slot_index].tuple_identity:
                      packet->header.candidate_identity;
                  slot.render_origin=origin;
                  slot.vertex_capacity=static_cast<std::uint32_t>(vertex_capacity);
                  slot.expected_rejection=expected_rejection;
                  slot.completed->store(false,std::memory_order_release);
                  slot.succeeded->store(false,std::memory_order_release);slot.pending=true;
                  id<MTLBlitCommandEncoder> clear=[command_buffer blitCommandEncoder];
                  for(id<MTLBuffer> buffer: {slot.roots,slot.counts,slot.signs,slot.offsets,
                      slot.added_offsets,slot.block_totals,slot.block_offsets,
                      slot.block_totals2,slot.block_offsets2,
                      slot.compaction_status,slot.triangles,slot.projected,slot.vertices,
                      slot.commit_control})
                    // The owner-direct P8c route deliberately has no root,
                    // triangle, or projected intermediate buffers.  Metal's
                    // blit encoder must not be handed a nil resource even for
                    // a zero-length range.
                    if(buffer!=nil)
                      [clear fillBuffer:buffer range:NSMakeRange(0U,buffer.length) value:0U];
                  [clear endEncoding];
                  if(metal_gpu_terrain_device_front_inject_failure){
                    // Poison P7e3c's private header after closure and before
                    // P8 count.  The owner-count shader propagates this to
                    // P8 validation, so commit cannot replace the old front.
                    clear=[command_buffer blitCommandEncoder];
                    [clear fillBuffer:gpu_hierarchy_live_selection.closure_status
                        range:NSMakeRange(0U,sizeof(std::uint32_t)) value:1U];
                    [clear endEncoding];
                    gpu_terrain_counters->device_front_injections.fetch_add(
                        1U,std::memory_order_relaxed);
                  }
                  if(metal_gpu_terrain_device_front_inject_capacity_failure)
                    gpu_terrain_counters->device_front_injections.fetch_add(
                        1U,std::memory_order_relaxed);
                  if(metal_gpu_terrain_device_front_inject_green_budget_failure)
                    gpu_terrain_counters->device_front_injections.fetch_add(
                        1U,std::memory_order_relaxed);
                  id<MTLComputeCommandEncoder> compute=nil;
                  if(!owner_direct){
                  const std::array<std::uint32_t,4> classify_parameters{
                    static_cast<std::uint32_t>(owner_count),static_cast<std::uint32_t>(root_slots),
                    static_cast<std::uint32_t>(source_revision),static_cast<std::uint32_t>(source_revision>>32U)};
                  compute=[command_buffer computeCommandEncoder];
                  [compute setComputePipelineState:gpu_terrain_classify_pipeline];
                  [compute setBuffer:slot.field offset:0U atIndex:0U];
                  [compute setBytes:classify_parameters.data() length:sizeof(classify_parameters) atIndex:1U];
                  [compute setBuffer:slot.owners offset:0U atIndex:2U];[compute setBuffer:slot.roots offset:0U atIndex:3U];
                  [compute setBuffer:slot.templates offset:0U atIndex:4U];
                  [compute dispatchThreads:MTLSizeMake(owner_count,1U,1U) threadsPerThreadgroup:MTLSizeMake(64U,1U,1U)];[compute endEncoding];
                  // Ordered count/scan/finalize/scatter is the only live
                  // compaction route.  The serial P7b2 kernel is deliberately
                  // absent: every stage derives its bounds from this slot.
                  const std::array<std::uint32_t,2> count_parameters{
                    static_cast<std::uint32_t>(owner_count),static_cast<std::uint32_t>(root_slots)};
                  compute=[command_buffer computeCommandEncoder];[compute setComputePipelineState:gpu_terrain_count_pipeline];
                  [compute setBuffer:slot.roots offset:0U atIndex:0U];[compute setBytes:count_parameters.data() length:sizeof(count_parameters) atIndex:1U];
                  [compute setBuffer:slot.owners offset:0U atIndex:2U];[compute setBuffer:slot.templates offset:0U atIndex:3U];[compute setBuffer:slot.compaction_status offset:0U atIndex:4U];[compute setBuffer:slot.counts offset:0U atIndex:5U];
                  [compute dispatchThreads:MTLSizeMake(root_slots,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[compute endEncoding];
                  const std::array<std::uint32_t,2> scan_parameters{static_cast<std::uint32_t>(root_slots),0U};
                  compute=[command_buffer computeCommandEncoder];[compute setComputePipelineState:gpu_terrain_scan_pipeline];[compute setBytes:scan_parameters.data() length:sizeof(scan_parameters) atIndex:0U];
                  [compute setBuffer:slot.offsets offset:0U atIndex:1U];[compute setBuffer:slot.counts offset:0U atIndex:2U];[compute setBuffer:slot.block_totals offset:0U atIndex:3U];
                  [compute dispatchThreads:MTLSizeMake(compaction_blocks*256U,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[compute endEncoding];
                  const std::array<std::uint32_t,2> block_scan_parameters{static_cast<std::uint32_t>(compaction_blocks),0U};
                  compute=[command_buffer computeCommandEncoder];[compute setComputePipelineState:gpu_terrain_scan_pipeline];[compute setBytes:block_scan_parameters.data() length:sizeof(block_scan_parameters) atIndex:0U];
                  [compute setBuffer:slot.block_offsets offset:0U atIndex:1U];[compute setBuffer:slot.block_totals offset:0U atIndex:2U];[compute setBuffer:slot.compaction_status offset:0U atIndex:3U];
                  [compute dispatchThreads:MTLSizeMake(256U,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[compute endEncoding];
                  const std::array<std::uint32_t,2> add_parameters{static_cast<std::uint32_t>(root_slots),1U};
                  compute=[command_buffer computeCommandEncoder];[compute setComputePipelineState:gpu_terrain_scan_pipeline];[compute setBytes:add_parameters.data() length:sizeof(add_parameters) atIndex:0U];
                  [compute setBuffer:slot.added_offsets offset:0U atIndex:1U];[compute setBuffer:slot.offsets offset:0U atIndex:2U];[compute setBuffer:slot.block_offsets offset:0U atIndex:3U];
                  [compute dispatchThreads:MTLSizeMake(compaction_blocks*256U,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[compute endEncoding];
                  const std::array<std::uint32_t,2> compact_parameters{static_cast<std::uint32_t>(root_slots),static_cast<std::uint32_t>(triangle_capacity)};
                  compute=[command_buffer computeCommandEncoder];[compute setComputePipelineState:gpu_terrain_finalize_pipeline];[compute setBytes:compact_parameters.data() length:sizeof(compact_parameters) atIndex:0U];
                  [compute setBuffer:slot.triangles offset:0U atIndex:1U];[compute setBuffer:slot.compaction_status offset:0U atIndex:2U];[compute setBuffer:slot.added_offsets offset:0U atIndex:3U];[compute setBuffer:slot.counts offset:0U atIndex:4U];
                  [compute dispatchThreads:MTLSizeMake(1U,1U,1U) threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];[compute endEncoding];
                  compute=[command_buffer computeCommandEncoder];[compute setComputePipelineState:gpu_terrain_scatter_pipeline];[compute setBytes:compact_parameters.data() length:sizeof(compact_parameters) atIndex:0U];
                  [compute setBuffer:slot.triangles offset:0U atIndex:1U];[compute setBuffer:slot.counts offset:0U atIndex:2U];[compute setBuffer:slot.added_offsets offset:0U atIndex:3U];[compute setBuffer:slot.roots offset:0U atIndex:4U];
                  [compute dispatchThreads:MTLSizeMake(root_slots,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[compute endEncoding];
                  MetalGpuTerrainGeometryParameters projection_parameters{
                    std::numeric_limits<std::uint32_t>::max(),static_cast<std::uint32_t>(triangle_capacity),0U,0U,
                    {static_cast<float>(origin.x),static_cast<float>(origin.y),static_cast<float>(origin.z),0.0F},
                    static_cast<std::uint32_t>(source_revision),static_cast<std::uint32_t>(source_revision>>32U),0U,0U};
                  compute=[command_buffer computeCommandEncoder];[compute setComputePipelineState:gpu_terrain_project_pipeline];
                  [compute setBuffer:slot.field offset:0U atIndex:0U];[compute setBytes:&projection_parameters length:sizeof(projection_parameters) atIndex:1U];[compute setBuffer:slot.projected offset:0U atIndex:2U];[compute setBuffer:slot.triangles offset:0U atIndex:3U];
                  [compute dispatchThreads:MTLSizeMake(1U,1U,1U) threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];[compute endEncoding];
                  auto draw_parameters=projection_parameters;draw_parameters.capacity=slot.vertex_capacity;
                  compute=[command_buffer computeCommandEncoder];[compute setComputePipelineState:gpu_terrain_draw_pipeline];
                  [compute setBuffer:slot.field offset:0U atIndex:0U];[compute setBytes:&draw_parameters length:sizeof(draw_parameters) atIndex:1U];[compute setBuffer:slot.vertices offset:0U atIndex:2U];[compute setBuffer:slot.projected offset:0U atIndex:3U];
                  [compute dispatchThreads:MTLSizeMake(1U,1U,1U) threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];[compute endEncoding];
                  }else{
                    encode_timestamp_marker(command_buffer,gpu_timestamp_samples,
                                            gpu_timestamp_scratch,25U);
                    const std::array<std::uint32_t,4> owner_parameters{
                        static_cast<std::uint32_t>(owner_count),
                        static_cast<std::uint32_t>(source_revision),
                        static_cast<std::uint32_t>(source_revision>>32U),0U};
                    id<MTLComputeCommandEncoder> owner_compute=[command_buffer computeCommandEncoder];
                    [owner_compute setComputePipelineState:gpu_terrain_owner_count_pipeline];
                    // Header-first helpers make SPIRV-Cross emit this Metal
                    // ABI: header/status/parameters/field/owners/signs/
                    // templates/counts. Keep the host binding order explicit.
                    [owner_compute setBuffer:slot.owner_header offset:0U atIndex:0U];
                    [owner_compute setBuffer:slot.compaction_status offset:0U atIndex:1U];
                    [owner_compute setBytes:owner_parameters.data() length:sizeof(owner_parameters) atIndex:2U];
                    [owner_compute setBuffer:slot.field offset:0U atIndex:3U];
                    [owner_compute setBuffer:slot.owners offset:0U atIndex:4U];
                    [owner_compute setBuffer:slot.signs offset:0U atIndex:5U];
                    [owner_compute setBuffer:slot.templates offset:0U atIndex:6U];
                    [owner_compute setBuffer:slot.counts offset:0U atIndex:7U];
                    [owner_compute dispatchThreads:MTLSizeMake(owner_count,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[owner_compute endEncoding];
                    const std::array<std::uint32_t,2> owner_scan{static_cast<std::uint32_t>(owner_count),0U};
                    owner_compute=[command_buffer computeCommandEncoder];[owner_compute setComputePipelineState:gpu_terrain_scan_pipeline];[owner_compute setBytes:owner_scan.data() length:sizeof(owner_scan) atIndex:0U];[owner_compute setBuffer:slot.offsets offset:0U atIndex:1U];[owner_compute setBuffer:slot.counts offset:0U atIndex:2U];[owner_compute setBuffer:slot.block_totals offset:0U atIndex:3U];[owner_compute dispatchThreads:MTLSizeMake(compaction_blocks*256U,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[owner_compute endEncoding];
                    const std::array<std::uint32_t,2> owner_block_scan{static_cast<std::uint32_t>(compaction_blocks),0U};
                    owner_compute=[command_buffer computeCommandEncoder];[owner_compute setComputePipelineState:gpu_terrain_scan_pipeline];[owner_compute setBytes:owner_block_scan.data() length:sizeof(owner_block_scan) atIndex:0U];[owner_compute setBuffer:slot.block_offsets offset:0U atIndex:1U];[owner_compute setBuffer:slot.block_totals offset:0U atIndex:2U];[owner_compute setBuffer:slot.block_totals2 offset:0U atIndex:3U];[owner_compute dispatchThreads:MTLSizeMake(super_blocks*256U,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[owner_compute endEncoding];
                    const std::array<std::uint32_t,2> owner_super_scan{static_cast<std::uint32_t>(super_blocks),0U};
                    owner_compute=[command_buffer computeCommandEncoder];[owner_compute setComputePipelineState:gpu_terrain_scan_pipeline];[owner_compute setBytes:owner_super_scan.data() length:sizeof(owner_super_scan) atIndex:0U];[owner_compute setBuffer:slot.block_offsets2 offset:0U atIndex:1U];[owner_compute setBuffer:slot.block_totals2 offset:0U atIndex:2U];[owner_compute setBuffer:slot.compaction_status offset:0U atIndex:3U];[owner_compute dispatchThreads:MTLSizeMake(256U,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[owner_compute endEncoding];
                    const std::array<std::uint32_t,2> owner_block_add{static_cast<std::uint32_t>(compaction_blocks),1U};
                    owner_compute=[command_buffer computeCommandEncoder];[owner_compute setComputePipelineState:gpu_terrain_scan_pipeline];[owner_compute setBytes:owner_block_add.data() length:sizeof(owner_block_add) atIndex:0U];[owner_compute setBuffer:slot.block_offsets offset:0U atIndex:1U];[owner_compute setBuffer:slot.block_offsets offset:0U atIndex:2U];[owner_compute setBuffer:slot.block_offsets2 offset:0U atIndex:3U];[owner_compute dispatchThreads:MTLSizeMake(super_blocks*256U,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[owner_compute endEncoding];
                    const std::array<std::uint32_t,2> owner_add{static_cast<std::uint32_t>(owner_count),1U};
                    owner_compute=[command_buffer computeCommandEncoder];[owner_compute setComputePipelineState:gpu_terrain_scan_pipeline];[owner_compute setBytes:owner_add.data() length:sizeof(owner_add) atIndex:0U];[owner_compute setBuffer:slot.added_offsets offset:0U atIndex:1U];[owner_compute setBuffer:slot.offsets offset:0U atIndex:2U];[owner_compute setBuffer:slot.block_offsets offset:0U atIndex:3U];[owner_compute dispatchThreads:MTLSizeMake(compaction_blocks*256U,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[owner_compute endEncoding];
                    const std::array<std::uint32_t,2> owner_final{static_cast<std::uint32_t>(owner_count),static_cast<std::uint32_t>(vertex_capacity)};
                    owner_compute=[command_buffer computeCommandEncoder];[owner_compute setComputePipelineState:gpu_terrain_owner_finalize_pipeline];[owner_compute setBuffer:slot.counts offset:0U atIndex:0U];[owner_compute setBuffer:slot.added_offsets offset:0U atIndex:1U];[owner_compute setBuffer:slot.compaction_status offset:0U atIndex:2U];[owner_compute setBuffer:slot.vertices offset:0U atIndex:3U];[owner_compute setBytes:owner_final.data() length:sizeof(owner_final) atIndex:4U];[owner_compute dispatchThreads:MTLSizeMake(1U,1U,1U) threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];[owner_compute endEncoding];
                    MetalGpuTerrainGeometryParameters owner_emit{static_cast<std::uint32_t>(owner_count),static_cast<std::uint32_t>(vertex_capacity),0U,0U,{static_cast<float>(origin.x),static_cast<float>(origin.y),static_cast<float>(origin.z),0.0F},static_cast<std::uint32_t>(source_revision),static_cast<std::uint32_t>(source_revision>>32U),0U,0U};
                    owner_compute=[command_buffer computeCommandEncoder];
                    [owner_compute setComputePipelineState:gpu_terrain_owner_emit_pipeline];
                    // Header/parameters/field/owners/vertices/templates/
                    // offsets/counts is the generated owner-emit Metal ABI.
                    [owner_compute setBuffer:slot.owner_header offset:0U atIndex:0U];
                    [owner_compute setBytes:&owner_emit length:sizeof(owner_emit) atIndex:1U];
                    [owner_compute setBuffer:slot.field offset:0U atIndex:2U];
                    [owner_compute setBuffer:slot.owners offset:0U atIndex:3U];
                    [owner_compute setBuffer:slot.vertices offset:0U atIndex:4U];
                    [owner_compute setBuffer:slot.templates offset:0U atIndex:5U];
                    [owner_compute setBuffer:slot.added_offsets offset:0U atIndex:6U];
                    [owner_compute setBuffer:slot.counts offset:0U atIndex:7U];
                    [owner_compute dispatchThreads:MTLSizeMake(owner_count,1U,1U)
                        threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];
                    [owner_compute endEncoding];
                  }
                  // A candidate never becomes a CPU-visible payload.  These
                  // three passes validate, copy, and publish its indirect
                  // arguments wholly in private memory; validation failure
                  // leaves the preceding complete active front untouched.
                  const std::array<std::uint32_t,1> commit_parameters{slot.vertex_capacity};
                  compute=[command_buffer computeCommandEncoder];[compute setComputePipelineState:gpu_terrain_commit_validate_pipeline];
                  [compute setBuffer:slot.vertices offset:0U atIndex:0U];[compute setBuffer:slot.commit_control offset:0U atIndex:1U];[compute setBytes:commit_parameters.data() length:sizeof(commit_parameters) atIndex:2U];
                  [compute dispatchThreads:MTLSizeMake(1U,1U,1U) threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];[compute endEncoding];
                  compute=[command_buffer computeCommandEncoder];[compute setComputePipelineState:gpu_terrain_commit_copy_pipeline];
                  [compute setBuffer:slot.vertices offset:0U atIndex:0U];[compute setBuffer:slot.commit_control offset:0U atIndex:1U];[compute setBuffer:gpu_terrain_active_front.vertices offset:0U atIndex:2U];
                  [compute dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(slot.vertex_capacity)*18U,1U,1U) threadsPerThreadgroup:MTLSizeMake(256U,1U,1U)];[compute endEncoding];
                  compute=[command_buffer computeCommandEncoder];[compute setComputePipelineState:gpu_terrain_commit_publish_pipeline];
                  [compute setBuffer:slot.commit_control offset:0U atIndex:0U];[compute setBuffer:gpu_terrain_active_front.indirect_arguments offset:0U atIndex:1U];
                  [compute dispatchThreads:MTLSizeMake(1U,1U,1U) threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];[compute endEncoding];
                  if(slot.readback!=nil){
                    gpu_terrain_counters->candidate_payload_readback_requests.fetch_add(
                        1U,std::memory_order_relaxed);
                    id<MTLBlitCommandEncoder> read=[command_buffer blitCommandEncoder];
                    [read copyFromBuffer:slot.vertices sourceOffset:0U toBuffer:slot.readback destinationOffset:0U size:slot.readback.length];[read endEncoding];
                  }
                  if(slot.control_audit!=nil){
                    // P7e4a may observe only P8's two-word private commit
                    // control. No terrain vertex/owner payload crosses to
                    // host memory on this route.
                    id<MTLBlitCommandEncoder> audit=[command_buffer blitCommandEncoder];
                    [audit copyFromBuffer:slot.commit_control sourceOffset:0U
                        toBuffer:slot.control_audit destinationOffset:0U
                        size:slot.control_audit.length];
                    [audit endEncoding];
                  }
                  if(owner_direct){
                    encode_timestamp_marker(command_buffer,gpu_timestamp_samples,
                                            gpu_timestamp_scratch,26U);
                    owner_direct_generation_encoded_this_frame=true;
                    if(metal_gpu_terrain_device_front)
                      gpu_terrain_counters->device_owner_submitted.fetch_add(1U,
                          std::memory_order_relaxed);
                  }
                  const auto complete=slot.completed,success=slot.succeeded;
                  const auto counters=gpu_terrain_counters;id<MTLBuffer> readback=slot.readback;
                  id<MTLBuffer> control_audit=slot.control_audit;
                  const auto capacity=slot.vertex_capacity;
                  const auto expected_rejection=slot.expected_rejection;
                  const auto completed_vertex_count=slot.completed_vertex_count;
                  counters->dispatched.fetch_add(1U,std::memory_order_relaxed);
                  [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> command){
                    const auto* header=readback==nil?nullptr:
                        static_cast<const std::uint32_t*>(readback.contents);
                    const bool qualified=readback==nil||
                        (header!=nullptr&&header[1U]==0U&&header[0U]!=0U&&
                         header[0U]<=capacity&&header[0U]%3U==0U&&
                         header[2U]*12U==header[0U]);
                    const auto* control=control_audit==nil?nullptr:
                        static_cast<const std::uint32_t*>(control_audit.contents);
                    const bool rejected=control!=nullptr&&control[0U]==0U&&
                        control[1U]==0U;
                    const bool private_committed=control!=nullptr&&
                        control[1U]==1U&&control[0U]!=0U&&
                        control[0U]<=capacity&&control[0U]%3U==0U;
                    const bool passed=command.status==MTLCommandBufferStatusCompleted&&
                        (expected_rejection?rejected:
                         (control_audit!=nil?private_committed:qualified));
                    if(!passed)counters->failed.fetch_add(1U,std::memory_order_relaxed);
                    if(passed&&expected_rejection)
                      counters->device_front_rejections.fetch_add(
                          1U,std::memory_order_relaxed);
                    if(passed&&!expected_rejection&&control_audit!=nil)
                      counters->device_front_private_commits.fetch_add(
                          1U,std::memory_order_relaxed);
                    if(passed&&header!=nullptr)completed_vertex_count->store(header[0U],std::memory_order_release);
                    counters->completed.fetch_add(1U,std::memory_order_relaxed);
                    success->store(passed,std::memory_order_release);
                    complete->store(true,std::memory_order_release);
                  }];
                  // A scalar crosses only in explicit qualification mode.
                  slot.completed_vertex_count->store(0U,std::memory_order_release);
                }else gpu_terrain_counters->failed.fetch_add(1U,std::memory_order_relaxed);
              }
            }catch(const std::exception&){
              gpu_terrain_counters->failed.fetch_add(1U,std::memory_order_relaxed);
            }
          }
        }
        bool optical_lookup_encoded_this_frame=false;
        bool reference_lookup_encoded_this_frame=false;
        bool aerial_lookup_encoded_this_frame=false;
        bool reference_screen_integration_encoded_this_frame=false;
        bool acceleration_structure_build_encoded=false;
        if(metal_ray_tracing_supported&&terrain_display_front.ready())
          static_cast<void>(encode_terrain_acceleration_structure_build(
              device,command_buffer,terrain_acceleration_structure,
              terrain_display_front.exact_vertices,
              terrain_display_front.exact_vertex_count,
              terrain_display_front.indexed_exact_selection,
              terrain_display_front.exact_indices,
              terrain_display_front.exact_index_count,
              terrain_display_front.preview_vertices,
              terrain_display_front.preview_vertex_count,
              terrain_display_front.preview_indices,
              terrain_display_front.preview_index_count,
              terrain_display_front.render_generation,gpu_timestamp_samples,
              acceleration_structure_build_encoded));
        // This qualification deliberately uses the currently published terrain
        // buffer.  It samples real triangle centroids on both sides of the
        // solar half-ray and compares Metal traversal against an independent
        // CPU Moller--Trumbore walk of all published triangles.
        if(terrain_ray_oracle_test&&!terrain_ray_oracle_encoded&&runtime&&
           (!preview_enabled||terrain_display_front.preview_cpu)&&
           terrain_acceleration_structure.active!=nil&&
           terrain_acceleration_structure.active_generation==
               terrain_display_front.render_generation){
          std::vector<tetra_viewer::SceneVertex> displayed_terrain_storage;
          std::span<const tetra_viewer::SceneVertex> terrain=
              runtime->scene().triangle_vertices;
          if(terrain_display_front.preview_cpu){
            const auto composition=tetra_viewer::compose_terrain_display(
                runtime->scene().triangle_vertices,
                terrain_display_front.identity.render_origin,
                runtime->field(),*terrain_display_front.preview_cpu);
            displayed_terrain_storage.reserve(
                composition.exact_indices.size()+
                composition.preview_vertices.size());
            for(const auto index:composition.exact_indices)
              displayed_terrain_storage.push_back(
                  runtime->scene().triangle_vertices[index]);
            displayed_terrain_storage.insert(displayed_terrain_storage.end(),
                composition.preview_vertices.begin(),
                composition.preview_vertices.end());
            terrain=displayed_terrain_storage;
          }
          const auto sun_world=tetra_viewer::world_sun_direction(
              sun_azimuth,sun_elevation);
          const simd_float3 sun=simd_normalize(simd_make_float3(
              static_cast<float>(sun_world.x),static_cast<float>(sun_world.y),
              static_cast<float>(sun_world.z)));
          std::vector<RayVisibilityInput> rays;
          constexpr std::size_t requested_triangles=192U;
          const std::size_t triangle_count=terrain.size()/3U;
          const std::size_t stride=std::max<std::size_t>(1U,
              triangle_count/requested_triangles);
          for(std::size_t triangle=0U;triangle<triangle_count;
              triangle+=stride){
            const auto& a=terrain[triangle*3U];
            const auto& b=terrain[triangle*3U+1U];
            const auto& c=terrain[triangle*3U+2U];
            const simd_float3 centre=(simd_make_float3(a.position[0],a.position[1],a.position[2])+
                simd_make_float3(b.position[0],b.position[1],b.position[2])+
                simd_make_float3(c.position[0],c.position[1],c.position[2]))/3.0F;
            // A centimetre-scale offset is sufficient to distinguish the two
            // sides without turning this into a receiver-normal bias test.
            rays.push_back({centre-sun*0.02F,sun,0.001F,4096.0F});
            rays.push_back({centre+sun*0.02F,sun,0.001F,4096.0F});
            if(rays.size()>=requested_triangles*2U)break;
          }
          terrain_ray_oracle_expected.clear();
          terrain_ray_oracle_expected.reserve(rays.size());
          for(const auto& ray:rays)
            terrain_ray_oracle_expected.push_back(
                cpu_terrain_visibility(terrain,ray)?1U:0U);
          terrain_ray_oracle_triangles=triangle_count;
          terrain_ray_oracle_inputs=[device newBufferWithBytes:rays.data()
              length:rays.size()*sizeof(RayVisibilityInput)
              options:MTLResourceStorageModeShared];
          terrain_ray_oracle_outputs=[device newBufferWithLength:
              rays.size()*sizeof(std::uint32_t)
              options:MTLResourceStorageModeShared];
          if(terrain_ray_oracle_inputs!=nil&&terrain_ray_oracle_outputs!=nil&&
             !rays.empty()){
            id<MTLComputeCommandEncoder> oracle=[command_buffer computeCommandEncoder];
            [oracle setComputePipelineState:make_ray_visibility_pipeline(device)];
            [oracle setBuffer:terrain_ray_oracle_inputs offset:0U atIndex:0U];
            [oracle setBuffer:terrain_ray_oracle_outputs offset:0U atIndex:1U];
            [oracle setAccelerationStructure:terrain_acceleration_structure.active
                                atBufferIndex:2U];
            [oracle dispatchThreads:MTLSizeMake(rays.size(),1U,1U)
                 threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
            [oracle endEncoding];
            terrain_ray_oracle_encoded=true;
          }
        }
        encode_timestamp_marker(command_buffer,gpu_timestamp_samples,
                                gpu_timestamp_scratch,0U);
        std::optional<tetra_viewer::CameraProjection>
            frame_projection;
        std::array<float,16> frame_render_matrix{};
        const float temporal_jitter_x=metalfx_temporal_active?
            halton(metalfx_frame_index%1024U+1U,2U)-0.5F:0.0F;
        const float temporal_jitter_y=metalfx_temporal_active?
            halton(metalfx_frame_index%1024U+1U,3U)-0.5F:0.0F;
        const float temporal_jitter_ndc_x=render_width>0?
            2.0F*temporal_jitter_x/static_cast<float>(render_width):0.0F;
        const float temporal_jitter_ndc_y=render_height>0?
            2.0F*temporal_jitter_y/static_cast<float>(render_height):0.0F;
        TemporalMotionUniforms temporal_motion_uniforms;
        bool temporal_history_reset=false;
        std::uint64_t temporal_visual_signature{};
        if(runtime){
          frame_projection=tetra_viewer::make_infinite_reversed_projection(
              camera.position,(terrain_display_front.ready()?
                  terrain_display_front.identity.render_origin:
                  runtime->render_origin()),camera.forward,camera.up,
              camera.vertical_fov_radians,camera.aspect_ratio);
          frame_render_matrix=frame_projection->matrix;
          for(std::size_t column=0;column<4U;++column){
            frame_render_matrix[column*4U]+=
                temporal_jitter_ndc_x*frame_render_matrix[column*4U+3U];
            frame_render_matrix[column*4U+1U]+=
                temporal_jitter_ndc_y*frame_render_matrix[column*4U+3U];
          }
          const auto sun=tetra_viewer::world_sun_direction(
              sun_azimuth,sun_elevation);
          const double planet_radius_world=
              atmosphere_parameters.ground_radius_metres/
              atmosphere_parameters.metres_per_world_unit;
          const tetra::Vec3 planet_centre_world{
              0.5,0.5-planet_radius_world,0.5};
          const auto planet_centre_relative=
              planet_centre_world-(terrain_display_front.ready()?
                  terrain_display_front.identity.render_origin:
                  runtime->render_origin());
          const auto jittered_atmosphere_forward=frame_projection->forward-
              frame_projection->right*(temporal_jitter_ndc_x*
                  frame_projection->tangent*frame_projection->aspect_ratio)-
              frame_projection->up*(temporal_jitter_ndc_y*
                  frame_projection->tangent);
          atmosphere_uniform=make_live_atmosphere_uniform(
              atmosphere_parameters,frame_projection->camera_relative,
              planet_centre_relative,frame_projection->right,
              frame_projection->up,jittered_atmosphere_forward,sun,
              frame_projection->tangent,frame_projection->aspect_ratio,
              atmosphere_aerial_range,
              static_cast<float>(std::exp2(exposure_ev)),
              atmosphere_debug_view,atmosphere_transport,
              atmosphere_renderer,shadow_filter,shadow_integration,
              atmosphere_screen_divisor,atmosphere_enabled,
              render_width,render_height);
          // Sky, aerial, irradiance, and long-shadow lookup atlases are
          // parameterized by the physical camera pose, not by MetalFX's
          // subpixel screen jitter. A jittered cache key forced every expensive
          // view lookup to rebuild each frame for a stationary camera.
          stable_atmosphere_lookup_uniform=make_live_atmosphere_uniform(
              atmosphere_parameters,frame_projection->camera_relative,
              planet_centre_relative,frame_projection->right,
              frame_projection->up,frame_projection->forward,sun,
              frame_projection->tangent,frame_projection->aspect_ratio,
              atmosphere_aerial_range,
              static_cast<float>(std::exp2(exposure_ev)),
              atmosphere_debug_view,atmosphere_transport,
              atmosphere_renderer,shadow_filter,shadow_integration,
              atmosphere_screen_divisor,atmosphere_enabled,
              render_width,render_height);
          temporal_visual_signature=1469598103934665603ULL;
          const auto signature_value=[&](std::uint32_t value){
            temporal_visual_signature^=value;
            temporal_visual_signature*=1099511628211ULL;
          };
          for(std::size_t index=0U;index<28U;++index)
            signature_value(std::bit_cast<std::uint32_t>(
                atmosphere_uniform[index]));
          for(std::size_t index=44U;index<48U;++index)
            signature_value(std::bit_cast<std::uint32_t>(
                atmosphere_uniform[index]));
          for(std::size_t index=52U;index<56U;++index)
            signature_value(std::bit_cast<std::uint32_t>(
                atmosphere_uniform[index]));
          signature_value(show_surface_edges?1U:0U);
          signature_value(smooth_normals?1U:0U);
          signature_value(atmosphere_enabled?1U:0U);
          const auto& previous=previous_temporal_projection.value_or(
              *frame_projection);
          const auto render_origin=terrain_display_front.ready()?
              terrain_display_front.identity.render_origin:
              runtime->render_origin();
          const auto origin_delta=render_origin-previous_temporal_render_origin;
          const auto current_world_camera=
              frame_projection->camera_relative+render_origin;
          const auto previous_world_camera=
              previous.camera_relative+previous_temporal_render_origin;
          const auto camera_delta=current_world_camera-previous_world_camera;
          const double camera_delta_squared=camera_delta.x*camera_delta.x+
              camera_delta.y*camera_delta.y+camera_delta.z*camera_delta.z;
          const double direction_alignment=
              frame_projection->forward.x*previous.forward.x+
              frame_projection->forward.y*previous.forward.y+
              frame_projection->forward.z*previous.forward.z;
          temporal_history_reset=!previous_temporal_projection.has_value()||
              camera_delta_squared>4.0||direction_alignment<0.5||
              (previous_temporal_visual_signature.has_value()&&
               *previous_temporal_visual_signature!=temporal_visual_signature);
          temporal_motion_uniforms.current_camera_near={
              static_cast<float>(frame_projection->camera_relative.x),
              static_cast<float>(frame_projection->camera_relative.y),
              static_cast<float>(frame_projection->camera_relative.z),
              static_cast<float>(frame_projection->near_plane)};
          temporal_motion_uniforms.current_forward_tangent={
              static_cast<float>(frame_projection->forward.x),
              static_cast<float>(frame_projection->forward.y),
              static_cast<float>(frame_projection->forward.z),
              static_cast<float>(frame_projection->tangent)};
          temporal_motion_uniforms.current_right_aspect={
              static_cast<float>(frame_projection->right.x),
              static_cast<float>(frame_projection->right.y),
              static_cast<float>(frame_projection->right.z),
              static_cast<float>(frame_projection->aspect_ratio)};
          temporal_motion_uniforms.current_down_jitter_x={
              static_cast<float>(frame_projection->up.x),
              static_cast<float>(frame_projection->up.y),
              static_cast<float>(frame_projection->up.z),
              temporal_jitter_ndc_x};
          temporal_motion_uniforms.current_jitter_y_extent={
              temporal_jitter_ndc_y,static_cast<float>(render_width),
              static_cast<float>(render_height),
              previous_temporal_scene_generation!=0U&&
                      previous_temporal_scene_generation!=
                          terrain_display_front.render_generation?
                  1.0F:0.0F};
          if(metalfx_test&&
             temporal_motion_uniforms.current_jitter_y_extent[3]>0.5F)
            ++metalfx_generation_changes;
          temporal_motion_uniforms.previous_camera_tangent={
              static_cast<float>(previous.camera_relative.x),
              static_cast<float>(previous.camera_relative.y),
              static_cast<float>(previous.camera_relative.z),
              static_cast<float>(previous.tangent)};
          temporal_motion_uniforms.previous_forward_tangent={
              static_cast<float>(previous.forward.x),
              static_cast<float>(previous.forward.y),
              static_cast<float>(previous.forward.z),
              static_cast<float>(previous.tangent)};
          temporal_motion_uniforms.previous_right_aspect={
              static_cast<float>(previous.right.x),
              static_cast<float>(previous.right.y),
              static_cast<float>(previous.right.z),
              static_cast<float>(previous.aspect_ratio)};
          temporal_motion_uniforms.previous_down={
              static_cast<float>(previous.up.x),
              static_cast<float>(previous.up.y),
              static_cast<float>(previous.up.z),0.0F};
          temporal_motion_uniforms.origin_delta={
              static_cast<float>(origin_delta.x),
              static_cast<float>(origin_delta.y),
              static_cast<float>(origin_delta.z),0.0F};
          if(atmosphere_enabled){
            const bool aerial_lookup_consumed=atmosphere_transport!=2||
                atmosphere_debug_view==4||atmosphere_debug_view==5;
            const auto aerial_dispatches_before=
                atmosphere_resources.dispatch_counts[3U];
            optical_lookup_encoded_this_frame=
                encode_live_atmosphere_lookups(
                    device,command_buffer,atmosphere_resources,
                    stable_atmosphere_lookup_uniform,atmosphere_optical_dirty,
                    aerial_lookup_consumed,gpu_timestamp_samples);
            aerial_lookup_encoded_this_frame=
                atmosphere_resources.dispatch_counts[3U]!=
                aerial_dispatches_before;
            atmosphere_optical_dirty=false;
          }
        }
        encode_timestamp_marker(command_buffer,gpu_timestamp_samples,
                                gpu_timestamp_scratch,1U);
        const auto draw_terrain=[&](id<MTLRenderCommandEncoder> encoder,
                                    NSUInteger vertex_buffer_index){
          if(!terrain_display_front.ready())return;
          if(metal_gpu_terrain_device_front){
            const bool private_front_expected=
                gpu_hierarchy_live_selection.compact_p8_private_commits!=0U;
            const bool private_front_bound=private_front_expected&&
                gpu_terrain_active_front.promoted&&
                terrain_display_front.exact_vertices==
                    gpu_terrain_active_front.vertices&&
                terrain_display_front.exact_indirect_arguments==
                    gpu_terrain_active_front.indirect_arguments&&
                !terrain_display_front.indexed_exact_selection&&
                terrain_display_front.exact_vertex_count!=0U;
            const bool fallback_expected=!private_front_expected&&
                gpu_hierarchy_live_selection.compact_p8_rejected!=0U;
            const bool bootstrap_front_bound=fallback_expected&&
                terrain_display_front.exact_vertices==
                    device_front_bootstrap_vertices&&
                terrain_display_front.exact_indirect_arguments==
                    device_front_bootstrap_indirect_arguments&&
                !terrain_display_front.indexed_exact_selection;
            if(private_front_expected){
              if(private_front_bound)
                gpu_terrain_counters->device_front_display_frames.fetch_add(
                    1U,std::memory_order_relaxed);
              else
                gpu_terrain_counters->device_front_display_binding_violations.fetch_add(
                    1U,std::memory_order_relaxed);
            }else if(fallback_expected){
              if(bootstrap_front_bound)
                gpu_terrain_counters->device_front_bootstrap_fallback_frames.fetch_add(
                    1U,std::memory_order_relaxed);
              else
                gpu_terrain_counters->device_front_display_binding_violations.fetch_add(
                    1U,std::memory_order_relaxed);
            }
          }
          [encoder setVertexBuffer:terrain_display_front.exact_vertices
                            offset:0 atIndex:vertex_buffer_index];
          if(terrain_display_front.indexed_exact_selection){
            if(terrain_display_front.exact_index_count!=0U)
              [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                  indexCount:terrain_display_front.exact_index_count
                                   indexType:MTLIndexTypeUInt32
                                 indexBuffer:terrain_display_front.exact_indices
                           indexBufferOffset:0];
          }else if(terrain_display_front.exact_indirect_arguments!=nil)
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                     indirectBuffer:terrain_display_front.exact_indirect_arguments
               indirectBufferOffset:0U];
          else [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
                            vertexCount:terrain_display_front.exact_vertex_count];
          if(terrain_display_front.preview_vertices!=nil&&
             terrain_display_front.preview_indices!=nil&&
             terrain_display_front.preview_index_count!=0U){
            [encoder setVertexBuffer:terrain_display_front.preview_vertices
                              offset:0 atIndex:vertex_buffer_index];
            [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:terrain_display_front.preview_index_count
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:terrain_display_front.preview_indices
                         indexBufferOffset:0];
          }
        };
        ShadowUniforms shadow_uniforms;
        std::optional<tetra_viewer::AtmosphereShadowMapFit> fitted_shadow_fit;
        double fitted_receiver_distance{};
        if(scene_vertices!=nil&&scene_vertex_count!=0U&&runtime){
          const auto sun=tetra_viewer::world_sun_direction(
              sun_azimuth,sun_elevation);
          const auto relative_camera=camera.position-
              terrain_display_front.identity.render_origin;
          const auto cascades=tetra_viewer::make_stable_shadow_cascades(
              relative_camera,camera.forward,sun,
              static_cast<std::uint32_t>(shadow_texture_resolution));
          shadow_uniforms.camera_position={
              static_cast<float>(relative_camera.x),
              static_cast<float>(relative_camera.y),
              static_cast<float>(relative_camera.z),0.0F};
          for(std::size_t index=0;
              index<tetra_viewer::shadow_cascade_count;++index){
            shadow_uniforms.matrices[index]=cascades.cascades[index].matrix;
            shadow_uniforms.splits[index]=static_cast<float>(
                cascades.cascades[index].split_distance);
            shadow_uniforms.depth_spans[index]=static_cast<float>(
                2.0*cascades.cascades[index].depth_half_range);
            if(shadow_test&&shadow_test_frames==0U){
              const auto& vertices=runtime->scene().triangle_vertices;
              for(const auto& vertex:vertices){
                const auto projected=tetra_viewer::transform_shadow_point(
                    cascades.cascades[index].matrix,
                    {vertex.position[0],vertex.position[1],vertex.position[2]});
                if(std::abs(projected.x)<=1.0&&std::abs(projected.y)<=1.0&&
                   projected.z>=0.0&&projected.z<=1.0)
                  ++shadow_cpu_candidates[index];
              }
            }
            if(!tetra_viewer::local_shadow_cascade_requires_refresh(
                   shadow_initialized[index],cached_shadow_matrices[index],
                   cached_shadow_generations[index],
                   cascades.cascades[index].matrix,
                   terrain_display_front.render_generation))
              continue;
            MTLRenderPassDescriptor* shadow_pass=
                [MTLRenderPassDescriptor renderPassDescriptor];
            shadow_pass.depthAttachment.texture=shadow_texture;
            shadow_pass.depthAttachment.slice=index;
            shadow_pass.depthAttachment.loadAction=MTLLoadActionClear;
            shadow_pass.depthAttachment.storeAction=MTLStoreActionStore;
            shadow_pass.depthAttachment.clearDepth=1.0;
            id<MTLRenderCommandEncoder> shadow_encoder=
                [command_buffer renderCommandEncoderWithDescriptor:shadow_pass];
            [shadow_encoder setRenderPipelineState:shadow_pipeline];
            [shadow_encoder setDepthStencilState:shadow_depth_state];
            [shadow_encoder setCullMode:MTLCullModeNone];
            [shadow_encoder setDepthBias:0.8125F slopeScale:1.21875F
                                     clamp:0.0F];
            [shadow_encoder setVertexBytes:
                shadow_uniforms.matrices[index].data()
                                      length:sizeof(std::array<float,16>)
                                     atIndex:1];
            draw_terrain(shadow_encoder,0U);
            [shadow_encoder endEncoding];
            cached_shadow_matrices[index]=cascades.cascades[index].matrix;
            cached_shadow_generations[index]=
                terrain_display_front.render_generation;
            shadow_initialized[index]=true;
            ++shadow_cascade_refreshes;
          }
          const auto quality=tetra_viewer::atmosphere_quality_settings(
              static_cast<tetra_viewer::AtmosphereQuality>(
                  atmosphere_quality_index));
          const double metres_per_world_unit=std::max(
              atmosphere_parameters.metres_per_world_unit,1.0e-12);
          const double camera_altitude_world=std::max(
              static_cast<double>(atmosphere_uniform[59])/metres_per_world_unit,
              0.0);
          fitted_receiver_distance=
              tetra_viewer::elevated_shadow_receiver_distance(
                  atmosphere_aerial_range/metres_per_world_unit,
                  tetra_viewer::default_shadow_cascade_half_widths.back(),
                  camera_altitude_world,8192.0);
          const auto fitted_request=
              tetra_viewer::make_atmosphere_shadow_front_request(
                  relative_camera,frame_projection->forward,
                  frame_projection->right,frame_projection->up,
                  frame_projection->tangent,frame_projection->aspect_ratio,
                  fitted_receiver_distance,1.15,sun,
                  fitted_receiver_distance,{},1U);
          fitted_shadow_fit=tetra_viewer::fit_atmosphere_shadow_map(
              fitted_request,quality.atmosphere_shadow_resolution);
          if(!fitted_shadow_initialized||
             cached_fitted_shadow_matrix!=fitted_shadow_fit->matrix||
             cached_fitted_shadow_generation!=
                 terrain_display_front.render_generation){
            MTLRenderPassDescriptor* fitted_pass=
                [MTLRenderPassDescriptor renderPassDescriptor];
            fitted_pass.depthAttachment.texture=shadow_texture;
            fitted_pass.depthAttachment.slice=
                tetra_viewer::shadow_cascade_count;
            fitted_pass.depthAttachment.loadAction=MTLLoadActionClear;
            fitted_pass.depthAttachment.storeAction=MTLStoreActionStore;
            fitted_pass.depthAttachment.clearDepth=1.0;
            id<MTLRenderCommandEncoder> fitted_encoder=
                [command_buffer renderCommandEncoderWithDescriptor:fitted_pass];
            [fitted_encoder setRenderPipelineState:shadow_pipeline];
            [fitted_encoder setDepthStencilState:shadow_depth_state];
            [fitted_encoder setCullMode:MTLCullModeNone];
            [fitted_encoder setViewport:MTLViewport{
                0.0,0.0,static_cast<double>(quality.atmosphere_shadow_resolution),
                static_cast<double>(quality.atmosphere_shadow_resolution),
                0.0,1.0}];
            [fitted_encoder setScissorRect:MTLScissorRect{
                0U,0U,quality.atmosphere_shadow_resolution,
                quality.atmosphere_shadow_resolution}];
            [fitted_encoder setDepthBias:0.8125F slopeScale:1.21875F
                                      clamp:0.0F];
            [fitted_encoder setVertexBytes:fitted_shadow_fit->matrix.data()
                                    length:sizeof(fitted_shadow_fit->matrix)
                                   atIndex:1];
            draw_terrain(fitted_encoder,0U);
            [fitted_encoder endEncoding];
            cached_fitted_shadow_matrix=fitted_shadow_fit->matrix;
            cached_fitted_shadow_generation=
                terrain_display_front.render_generation;
            fitted_shadow_initialized=true;
            ++fitted_shadow_refreshes;
            atmosphere_resources.minmax_scene_generation=0U;
          }
        }
        encode_timestamp_marker(command_buffer,gpu_timestamp_samples,
                                gpu_timestamp_scratch,2U);
        const auto frame_atmosphere_quality=
            tetra_viewer::atmosphere_quality_settings(
                static_cast<tetra_viewer::AtmosphereQuality>(
                    atmosphere_quality_index));
        ProductionShadowUniforms production_shadows=
            make_production_shadow_uniforms(
                shadow_uniforms,fitted_shadow_fit,fitted_shadow_initialized,
                fitted_receiver_distance,frame_atmosphere_quality,
                atmosphere_resources.minmax_element_count,
                shadow_texture_resolution);
        if(atmosphere_enabled&&atmosphere_transport==2&&
           scene_vertex_count!=0U)
          reference_lookup_encoded_this_frame=encode_reference_sky_lookup(
              command_buffer,atmosphere_resources,
              stable_atmosphere_lookup_uniform,production_shadows,
              shadow_texture,terrain_display_front.render_generation,
              gpu_timestamp_samples);
        encode_timestamp_marker(command_buffer,gpu_timestamp_samples,
                                gpu_timestamp_scratch,3U);
        id<MTLRenderCommandEncoder> scene_encoder=
            [command_buffer renderCommandEncoderWithDescriptor:scene_pass];
        if(scene_vertices!=nil&&scene_vertex_count!=0U&&runtime){
          const auto& projection=*frame_projection;
          ProductionCameraUniforms uniforms;
          uniforms.view_projection=frame_render_matrix;
          for(std::size_t index:std::array<std::size_t,4>{1U,5U,9U,13U})
            uniforms.view_projection[index]=-uniforms.view_projection[index];
          const auto sun=tetra_viewer::world_sun_direction(
              sun_azimuth,sun_elevation);
          uniforms.light_direction={static_cast<float>(sun.x),
                                    static_cast<float>(sun.y),
                                    static_cast<float>(sun.z),
                                    shadow_bias==1?1.0F:0.0F};
          uniforms.rendering={4.0F,show_surface_edges?1.0F:0.0F,
                              1.0F,2.0F};
          const auto relative_camera=camera.position-
              terrain_display_front.identity.render_origin;
          uniforms.view_position={static_cast<float>(relative_camera.x),
                                  static_cast<float>(relative_camera.y),
                                  static_cast<float>(relative_camera.z),
                                  smooth_normals?1.0F:0.0F};
          id<MTLRenderPipelineState> active_pipeline=active_samples==4U?
              scene_pipeline_4:(active_samples==2U?scene_pipeline_2:
                                                    scene_pipeline_1);
          [scene_encoder setRenderPipelineState:active_pipeline];
          [scene_encoder setDepthStencilState:depth_state];
          [scene_encoder setCullMode:MTLCullModeNone];
          [scene_encoder setVertexBytes:&uniforms length:sizeof(uniforms)
                                atIndex:0];
          [scene_encoder setFragmentBytes:&production_shadows
                                   length:sizeof(production_shadows) atIndex:0];
          [scene_encoder setFragmentBytes:&uniforms length:sizeof(uniforms)
                                  atIndex:1];
          [scene_encoder setFragmentBytes:atmosphere_uniform.data()
                                   length:atmosphere_uniform.size()*sizeof(float)
                                  atIndex:2];
          [scene_encoder setFragmentTexture:shadow_texture atIndex:0];
          [scene_encoder setFragmentTexture:atmosphere_resources.sky_irradiance
                                       atIndex:1];
          [scene_encoder setFragmentTexture:atmosphere_resources.transmittance
                                       atIndex:2];
          [scene_encoder setFragmentTexture:atmosphere_resources.multiple_scattering
                                       atIndex:3];
          for(NSUInteger index=0U;index<4U;++index)
            [scene_encoder setFragmentSamplerState:atmosphere_resources.sampler
                                            atIndex:index];
          draw_terrain(scene_encoder,1U);
          if(show_surface_edges){
            id<MTLRenderPipelineState> active_wire_pipeline=active_samples==4U?
                wire_pipeline_4:(active_samples==2U?wire_pipeline_2:
                                                      wire_pipeline_1);
            [scene_encoder setRenderPipelineState:active_wire_pipeline];
            [scene_encoder setDepthStencilState:overlay_depth_state];
            [scene_encoder setFragmentBytes:&uniforms length:sizeof(uniforms)
                                    atIndex:0];
            [scene_encoder setTriangleFillMode:MTLTriangleFillModeLines];
            draw_terrain(scene_encoder,1U);
            ++wireframe_draws;
            [scene_encoder setTriangleFillMode:MTLTriangleFillModeFill];
          }
        }
        if((player_overlay_vertex_count!=0U||lod_overlay_vertex_count!=0U)&&
           runtime){
          const auto& projection=*frame_projection;
          CameraUniforms uniforms;
          uniforms.view_projection=frame_render_matrix;
          id<MTLRenderPipelineState> overlay_pipeline=active_samples==4U?
              overlay_pipeline_4:(active_samples==2U?overlay_pipeline_2:
                                                        overlay_pipeline_1);
          [scene_encoder setRenderPipelineState:overlay_pipeline];
          [scene_encoder setDepthStencilState:overlay_depth_state];
          [scene_encoder setDepthBias:1.0e-5F slopeScale:0.0F clamp:0.0F];
          [scene_encoder setVertexBytes:&uniforms length:sizeof(uniforms)
                                atIndex:1];
          const auto draw_lines=[&](id<MTLBuffer> vertices,std::size_t count){
            if(vertices==nil||count==0U)return;
            [scene_encoder setVertexBuffer:vertices offset:0 atIndex:0];
            [scene_encoder drawPrimitives:MTLPrimitiveTypeLine
                              vertexStart:0 vertexCount:count];
          };
          draw_lines(lod_overlay_vertices,lod_overlay_vertex_count);
          draw_lines(player_overlay_vertices,player_overlay_vertex_count);
          [scene_encoder setDepthBias:0.0F slopeScale:0.0F clamp:0.0F];
        }
        [scene_encoder endEncoding];
        encode_timestamp_marker(command_buffer,gpu_timestamp_samples,
                                gpu_timestamp_scratch,4U);
        const auto frame_visibility_plan=
            tetra_viewer::resolve_atmosphere_visibility_plan(
                {.requested=static_cast<tetra_viewer::AtmosphereVisibilityBackend>(
                     atmosphere_visibility_backend),
                 .ios_performance_mode=ios_performance_mode},
                metal_ray_tracing_supported&&
                    terrain_acceleration_structure.active!=nil&&
                    terrain_acceleration_structure.active_generation==
                        terrain_display_front.render_generation&&
                    atmosphere_resources.ray_visibility_pipeline!=nil);
        const bool ray_traced_screen_visibility_active=atmosphere_enabled&&
            atmosphere_transport!=2&&
            (atmosphere_renderer==2||atmosphere_renderer==3)&&
            frame_visibility_plan.effective==
                tetra_viewer::AtmosphereVisibilityBackend::ray_traced;
        // This bit is consumed by the compositor as an ownership contract:
        // when set, only the ray-integrated screen textures may supply direct
        // atmospheric visibility. Cascades and the long-shadow atlas remain
        // exclusively available to the raster compatibility backend.
        atmosphere_uniform[83]=ray_traced_screen_visibility_active?1.0F:0.0F;
        const std::uint32_t desired_screen_divisor=
            ray_traced_screen_visibility_active?
                frame_visibility_plan.screen_divisor:
                (ios_performance_mode?4U:2U);
        const bool needs_visibility_history=atmosphere_transport!=2;
        if(atmosphere_screen_divisor!=desired_screen_divisor||
           (needs_visibility_history&&
            atmosphere_resources.terrain_ray_visibility==nil)){
          atmosphere_screen_divisor=desired_screen_divisor;
          if(!ensure_screen_atmosphere_resources(
                 device,atmosphere_resources,render_width,render_height,
                 atmosphere_screen_divisor,needs_visibility_history)){
            std::fprintf(stderr,
                "Unable to allocate atmosphere visibility resources.\n");
            result=1;
            glfwSetWindowShouldClose(window,GLFW_TRUE);
          }
        }
        // The visibility plan can switch the desktop RT path from the
        // previously allocated half-resolution target to native resolution
        // after this frame's atmosphere uniform was assembled. Keep the
        // shader's active extent synchronized with the textures that will be
        // dispatched and sampled below; a stale half-resolution extent left
        // three quarters of the native target unwritten and produced a hard
        // rectangular/circular compositing artifact.
        atmosphere_uniform[86]=static_cast<float>(
            atmosphere_resources.screen_width);
        atmosphere_uniform[87]=static_cast<float>(
            atmosphere_resources.screen_height);
        if(atmosphere_enabled&&
           !ray_traced_screen_visibility_active&&
           (shadow_integration==2||shadow_integration==4)&&
           scene_vertex_count!=0U)
          encode_shadow_minmax_hierarchy(
              device,command_buffer,atmosphere_resources,shadow_texture,
              terrain_display_front.render_generation);
        if(atmosphere_enabled&&!ray_traced_screen_visibility_active&&
           shadow_integration==5&&
           scene_vertex_count!=0U)
          encode_shadow_epipolar_hierarchy(
              device,command_buffer,atmosphere_resources,atmosphere_uniform,
              production_shadows,shadow_texture,
              terrain_display_front.render_generation);
        // The faithful screen renderers return their reconstructed transport
        // before the FAITHFUL_SHADOW_SPLIT lookup is reached.  Refreshing the
        // directional atlas for those paths was therefore entirely dead work:
        // their per-ray terrain visibility already owns the direct-light term.
        // The native faithful marcher still consumes it, as do the explicit
        // long-shadow comparison and diagnostic views.
        const bool long_shadow_diagnostic=
            (atmosphere_debug_view>=11&&atmosphere_debug_view<=14)||
            (atmosphere_debug_view>=22&&atmosphere_debug_view<=24);
        const bool long_shadow_consumed=long_shadow_diagnostic||
            (atmosphere_transport==1&&
             (atmosphere_renderer==0||atmosphere_renderer==1));
        if(atmosphere_enabled&&!ray_traced_screen_visibility_active&&
           atmosphere_transport!=0&&long_shadow_consumed&&
           scene_vertex_count!=0U)
          encode_long_shadow_atmosphere(
              device,command_buffer,atmosphere_resources,
              stable_atmosphere_lookup_uniform,
              production_shadows,shadow_texture,
              terrain_display_front.render_generation);
        if(atmosphere_enabled&&
           (atmosphere_renderer==2||atmosphere_renderer==3)&&
           scene_vertex_count!=0U){
          const auto history_identity=make_metal_atmosphere_history_identity(
              stable_atmosphere_lookup_uniform,atmosphere_parameters,
              terrain_display_front.render_generation,
              terrain_display_front.identity.render_origin,
              static_cast<std::uint32_t>(render_width),
              static_cast<std::uint32_t>(render_height),
              atmosphere_screen_divisor,atmosphere_transport,
              atmosphere_renderer,true);
          encode_deterministic_screen_atmosphere(
              command_buffer,atmosphere_resources,atmosphere_uniform,
              history_identity,
              production_shadows,depth_texture,shadow_texture,
              terrain_acceleration_structure.active,
              terrain_acceleration_structure.active_generation,
              static_cast<float>(std::sqrt(
                  atmosphere_uniform[48]*atmosphere_uniform[48]+
                  atmosphere_uniform[49]*atmosphere_uniform[49]+
                  atmosphere_uniform[50]*atmosphere_uniform[50])+
                  atmosphere_uniform[43]/std::max(atmosphere_uniform[25],1.0e-6F)+
                  terrain_acceleration_structure.maximum_vertex_radius_world+0.01F),
              frame_visibility_plan.effective==
                  tetra_viewer::AtmosphereVisibilityBackend::ray_traced,
              frame_visibility_plan.rotating_queries_per_pixel,
              atmosphere_renderer==3,gpu_timestamp_samples,
              legacy_native_depth_scan,
              atmosphere_transport==2&&atmosphere_renderer==3&&
              atmosphere_debug_view==0&&elide_reference_sky_transport);
          reference_screen_integration_encoded_this_frame=
              atmosphere_transport==2;
        }
        if(atmosphere_enabled&&atmosphere_renderer==4&&
           scene_vertex_count!=0U)
          encode_shadowed_froxel_atmosphere(
              device,command_buffer,atmosphere_resources,atmosphere_uniform,
              production_shadows,shadow_texture,depth_texture);
        encode_timestamp_marker(command_buffer,gpu_timestamp_samples,
                                gpu_timestamp_scratch,5U);
        MTLRenderPassDescriptor* composite_pass=display_pass;
        if(metalfx_temporal_active){
          composite_pass=[MTLRenderPassDescriptor renderPassDescriptor];
          composite_pass.colorAttachments[0].texture=
              metalfx_resources.input_colour;
          composite_pass.colorAttachments[0].loadAction=MTLLoadActionClear;
          composite_pass.colorAttachments[0].storeAction=MTLStoreActionStore;
          composite_pass.colorAttachments[0].clearColor=
              MTLClearColorMake(0.0,0.0,0.0,1.0);
        }
        id<MTLRenderCommandEncoder> composite_encoder=
            [command_buffer renderCommandEncoderWithDescriptor:composite_pass];
        const std::array<float,4> composite_settings{
            render_resolution_mode==0||metalfx_temporal_active?
                0.0F:upscale_sharpening,
            render_resolution_mode==0||metalfx_temporal_active?
                0.0F:1.0F,0.0F,0.0F};
        const bool faithful_composite=atmosphere_transport!=0;
        [composite_encoder setRenderPipelineState:metalfx_temporal_active?
            (faithful_composite?temporal_faithful_composite_pipeline:
                                temporal_composite_pipeline):
            (faithful_composite?
                 atmosphere_resources.faithful_composite_pipeline:
                 atmosphere_resources.composite_pipeline)];
        [composite_encoder setFragmentTexture:scene_colour_texture atIndex:0];
        [composite_encoder setFragmentTexture:depth_texture atIndex:1];
        [composite_encoder setFragmentTexture:atmosphere_resources.sky_view
                                       atIndex:2];
        const bool direct_history_presentation=atmosphere_renderer==3&&
            atmosphere_resources.history_present_valid;
        id<MTLTexture> composite_scattering=direct_history_presentation?
            atmosphere_resources.history_scattering[
                atmosphere_resources.history_present_index]:
            atmosphere_resources.screen_scattering;
        id<MTLTexture> composite_transmittance=direct_history_presentation?
            atmosphere_resources.history_transmittance[
                atmosphere_resources.history_present_index]:
            atmosphere_resources.screen_transmittance;
        if(faithful_composite){
          [composite_encoder setFragmentTexture:atmosphere_resources.long_shadow
                                         atIndex:3];
          [composite_encoder setFragmentTexture:shadow_texture atIndex:4];
          [composite_encoder setFragmentTexture:atmosphere_resources.sky_irradiance
                                         atIndex:5];
          [composite_encoder setFragmentTexture:atmosphere_resources.froxel_scattering
                                         atIndex:6];
          [composite_encoder setFragmentTexture:atmosphere_resources.froxel_transmittance
                                         atIndex:7];
          [composite_encoder setFragmentTexture:atmosphere_resources.transmittance
                                         atIndex:8];
          [composite_encoder setFragmentTexture:atmosphere_resources.multiple_scattering
                                         atIndex:9];
          [composite_encoder setFragmentTexture:atmosphere_resources.screen_endpoint
                                         atIndex:10];
          [composite_encoder setFragmentTexture:composite_scattering
                                         atIndex:11];
          [composite_encoder setFragmentTexture:composite_transmittance
                                         atIndex:12];
          [composite_encoder setFragmentTexture:atmosphere_resources.aerial_scattering
                                         atIndex:13];
          [composite_encoder setFragmentTexture:atmosphere_resources.aerial_transmittance
                                         atIndex:14];
          [composite_encoder setFragmentBytes:&production_shadows
                                     length:sizeof(production_shadows) atIndex:2];
        }else{
          [composite_encoder setFragmentTexture:atmosphere_resources.sky_irradiance
                                         atIndex:3];
          [composite_encoder setFragmentTexture:atmosphere_resources.froxel_scattering
                                         atIndex:4];
          [composite_encoder setFragmentTexture:atmosphere_resources.froxel_transmittance
                                         atIndex:5];
          [composite_encoder setFragmentTexture:atmosphere_resources.transmittance
                                         atIndex:6];
          [composite_encoder setFragmentTexture:atmosphere_resources.multiple_scattering
                                         atIndex:7];
          [composite_encoder setFragmentTexture:atmosphere_resources.screen_endpoint
                                         atIndex:8];
          [composite_encoder setFragmentTexture:composite_scattering
                                         atIndex:9];
          [composite_encoder setFragmentTexture:composite_transmittance
                                         atIndex:10];
          [composite_encoder setFragmentTexture:atmosphere_resources.aerial_scattering
                                         atIndex:11];
          [composite_encoder setFragmentTexture:atmosphere_resources.aerial_transmittance
                                         atIndex:12];
          [composite_encoder setFragmentTexture:shadow_texture atIndex:13];
        }
        for(NSUInteger index=0U;index<(faithful_composite?15U:14U);++index)
          [composite_encoder setFragmentSamplerState:atmosphere_resources.sampler
                                            atIndex:index];
        [composite_encoder setFragmentBytes:atmosphere_uniform.data()
                                  length:atmosphere_uniform.size()*sizeof(float)
                                 atIndex:0];
        [composite_encoder setFragmentBytes:composite_settings.data()
                                  length:sizeof(composite_settings)
                                 atIndex:1];
        [composite_encoder drawPrimitives:MTLPrimitiveTypeTriangle
                            vertexStart:0 vertexCount:3];
        if(metalfx_temporal_active){
          [composite_encoder endEncoding];
          MTLRenderPassDescriptor* motion_pass=
              [MTLRenderPassDescriptor renderPassDescriptor];
          motion_pass.colorAttachments[0].texture=metalfx_resources.motion;
          motion_pass.colorAttachments[0].loadAction=MTLLoadActionClear;
          motion_pass.colorAttachments[0].storeAction=MTLStoreActionStore;
          motion_pass.colorAttachments[0].clearColor=
              MTLClearColorMake(0.0,0.0,0.0,0.0);
          motion_pass.colorAttachments[1].texture=metalfx_resources.reactive;
          motion_pass.colorAttachments[1].loadAction=MTLLoadActionClear;
          motion_pass.colorAttachments[1].storeAction=MTLStoreActionStore;
          motion_pass.colorAttachments[1].clearColor=
              MTLClearColorMake(1.0,0.0,0.0,0.0);
          id<MTLRenderCommandEncoder> motion_encoder=
              [command_buffer renderCommandEncoderWithDescriptor:motion_pass];
          [motion_encoder setRenderPipelineState:temporal_motion_pipeline];
          [motion_encoder setFragmentBytes:&temporal_motion_uniforms
                                     length:sizeof(temporal_motion_uniforms)
                                    atIndex:0];
          [motion_encoder setFragmentTexture:depth_texture atIndex:0];
          [motion_encoder drawPrimitives:MTLPrimitiveTypeTriangle
                              vertexStart:0 vertexCount:3];
          [motion_encoder endEncoding];
          if(metalfx_test&&metalfx_test_frames>=10U&&
             metalfx_test_frames<20U&&metalfx_motion_probe_buffer==nil){
            metalfx_motion_probe_row_bytes=
                (static_cast<NSUInteger>(render_width)*4U+255U)&~255U;
            metalfx_reactive_probe_row_bytes=
                (static_cast<NSUInteger>(render_width)+255U)&~255U;
            metalfx_motion_probe_buffer=[device newBufferWithLength:
                metalfx_motion_probe_row_bytes*
                    static_cast<NSUInteger>(render_height)
                options:MTLResourceStorageModeShared];
            metalfx_reactive_probe_buffer=[device newBufferWithLength:
                metalfx_reactive_probe_row_bytes*
                    static_cast<NSUInteger>(render_height)
                options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> temporal_probe=
                [command_buffer blitCommandEncoder];
            [temporal_probe copyFromTexture:metalfx_resources.motion
                               sourceSlice:0U sourceLevel:0U
                              sourceOrigin:MTLOriginMake(0U,0U,0U)
                                sourceSize:MTLSizeMake(
                                    render_width,render_height,1U)
                                  toBuffer:metalfx_motion_probe_buffer
                         destinationOffset:0U
                    destinationBytesPerRow:metalfx_motion_probe_row_bytes
                  destinationBytesPerImage:metalfx_motion_probe_row_bytes*
                      static_cast<NSUInteger>(render_height)];
            [temporal_probe copyFromTexture:metalfx_resources.reactive
                               sourceSlice:0U sourceLevel:0U
                              sourceOrigin:MTLOriginMake(0U,0U,0U)
                                sourceSize:MTLSizeMake(
                                    render_width,render_height,1U)
                                  toBuffer:metalfx_reactive_probe_buffer
                         destinationOffset:0U
                    destinationBytesPerRow:metalfx_reactive_probe_row_bytes
                  destinationBytesPerImage:metalfx_reactive_probe_row_bytes*
                      static_cast<NSUInteger>(render_height)];
            [temporal_probe endEncoding];
          }

          id<MTLFXTemporalScaler> scaler=metalfx_resources.scaler;
          scaler.inputContentWidth=static_cast<NSUInteger>(render_width);
          scaler.inputContentHeight=static_cast<NSUInteger>(render_height);
          scaler.colorTexture=metalfx_resources.input_colour;
          scaler.depthTexture=depth_texture;
          scaler.motionTexture=metalfx_resources.motion;
          scaler.reactiveMaskTexture=metalfx_resources.reactive;
          scaler.outputTexture=metalfx_resources.direct_output?
              drawable.texture:metalfx_resources.output_colour;
          scaler.exposureTexture=metalfx_resources.exposure;
          scaler.preExposure=1.0F;
          scaler.jitterOffsetX=temporal_jitter_x;
          scaler.jitterOffsetY=temporal_jitter_y;
          scaler.motionVectorScaleX=1.0F;
          scaler.motionVectorScaleY=1.0F;
          scaler.depthReversed=YES;
          scaler.reset=temporal_history_reset||
              !metalfx_resources.history_valid;
          if(scaler.reset)++metalfx_resources.history_resets;
          encode_timestamp_marker(command_buffer,gpu_timestamp_samples,
                                  gpu_timestamp_scratch,13U);
          [scaler encodeToCommandBuffer:command_buffer];
          encode_timestamp_marker(command_buffer,gpu_timestamp_samples,
                                  gpu_timestamp_scratch,14U);
          metalfx_resources.history_valid=true;
          ++metalfx_resources.encoded_frames;

          if(metalfx_resources.direct_output)
            display_pass.colorAttachments[0].loadAction=MTLLoadActionLoad;
          id<MTLRenderCommandEncoder> display_encoder=
              [command_buffer renderCommandEncoderWithDescriptor:display_pass];
          if(!metalfx_resources.direct_output){
            [display_encoder setRenderPipelineState:temporal_present_pipeline];
            [display_encoder setFragmentTexture:metalfx_resources.output_colour
                                         atIndex:0];
            [display_encoder drawPrimitives:MTLPrimitiveTypeTriangle
                                vertexStart:0 vertexCount:3];
          }
          if(!capture_test&&!any_atmosphere_frame_test)
            ImGui_ImplMetal_RenderDrawData(
                ImGui::GetDrawData(),command_buffer,display_encoder);
          [display_encoder endEncoding];
          previous_temporal_projection=frame_projection;
          previous_temporal_render_origin=
              terrain_display_front.identity.render_origin;
          previous_temporal_scene_generation=
              terrain_display_front.render_generation;
          previous_temporal_visual_signature=temporal_visual_signature;
          ++metalfx_frame_index;
        }else{
          if(!capture_test&&!any_atmosphere_frame_test)
            ImGui_ImplMetal_RenderDrawData(
                ImGui::GetDrawData(),command_buffer,composite_encoder);
          [composite_encoder endEncoding];
          metalfx_resources.history_valid=false;
          previous_temporal_projection.reset();
          previous_temporal_visual_signature.reset();
          previous_temporal_scene_generation=0U;
          metalfx_frame_index=0U;
        }
        encode_timestamp_marker(command_buffer,gpu_timestamp_samples,
                                gpu_timestamp_scratch,6U);
        if(gpu_timestamp_samples!=nil){
          id<MTLBlitCommandEncoder> timestamp_resolve=
              [command_buffer blitCommandEncoder];
          [timestamp_resolve resolveCounters:gpu_timestamp_samples
                                      inRange:NSMakeRange(0U,gpu_timestamp_count)
                             destinationBuffer:gpu_timestamp_results
                            destinationOffset:0U];
          [timestamp_resolve endEncoding];
        }
        id<MTLBuffer> capture_buffer=nil;
        id<MTLBuffer> capture_depth_buffer=nil;
        id<MTLBuffer> shadow_probe_buffer=nil;
        id<MTLBuffer> fitted_shadow_probe_buffer=nil;
        NSUInteger capture_row_bytes{};
        // Motion and MetalFX finish only after the exact world settles.  That
        // deliberately retires a compatible preview, so requiring one at the
        // same instant would make their preview-enabled completion condition
        // unreachable. Preview image gates still require a visible preview;
        // exact-handoff timing keeps its stronger explicit event check.
        const bool accepts_exact_preview_handoff=motion_test||metalfx_test||soak_test;
        const bool requested_preview_capture_ready=!preview_enabled||
            !automated_test||accepts_exact_preview_handoff||
            (require_exact_handoff_capture?
                (timing_profile_test&&
                 timing_profile_class==TimingProfileClass::exact_handoff?
                     timing_profile_exact_handoff_observed:
                 (terrain_display_front.preview_cpu==nullptr&&
                  terrain_front_coordinator.state().preview_retirement_reason==
                      tetra_viewer::PreviewRetirementReason::exact_handoff)):
                terrain_display_front.preview_cpu!=nullptr);
        const bool requested_rt_capture_ready=
            !(any_atmosphere_frame_test&&atmosphere_transport!=2&&
              metal_ray_tracing_supported)||
            (terrain_acceleration_structure.active!=nil&&
             terrain_acceleration_structure.active_generation==
                 terrain_display_front.render_generation);
        const bool requested_profile_capture_ready=
            !profile_interactive_rendering||!atmosphere_capture||
            raster_profile_qualification||
            (automatic_stable_frames>=60U&&
             automatic_render_scale<=0.501F);
        if((capture_test||any_atmosphere_frame_test||metalfx_test)&&
           scene_vertex_count!=0U&&requested_preview_capture_ready&&
           requested_profile_capture_ready){
          capture_row_bytes=(static_cast<NSUInteger>(width)*4U+255U)&~255U;
          capture_buffer=[device
              newBufferWithLength:capture_row_bytes*static_cast<NSUInteger>(height)
                          options:MTLResourceStorageModeShared];
          if(capture_test)
            capture_depth_buffer=[device newBufferWithLength:
                capture_row_bytes*static_cast<NSUInteger>(height)
                options:MTLResourceStorageModeShared];
          id<MTLBlitCommandEncoder> blit=[command_buffer blitCommandEncoder];
          [blit copyFromTexture:drawable.texture
                    sourceSlice:0
                    sourceLevel:0
                   sourceOrigin:MTLOriginMake(0,0,0)
                     sourceSize:MTLSizeMake(width,height,1)
                       toBuffer:capture_buffer
              destinationOffset:0
         destinationBytesPerRow:capture_row_bytes
       destinationBytesPerImage:capture_row_bytes*static_cast<NSUInteger>(height)];
          if(capture_test)[blit copyFromTexture:depth_texture
                    sourceSlice:0
                    sourceLevel:0
                   sourceOrigin:MTLOriginMake(0,0,0)
                     sourceSize:MTLSizeMake(width,height,1)
                       toBuffer:capture_depth_buffer
              destinationOffset:0
         destinationBytesPerRow:capture_row_bytes
       destinationBytesPerImage:capture_row_bytes*static_cast<NSUInteger>(height)
                        options:MTLBlitOptionDepthFromDepthStencil];
          [blit endEncoding];
        }
        if(shadow_test&&scene_vertex_count!=0U&&
           std::ranges::all_of(shadow_initialized,
                               [](bool ready){return ready;})){
          constexpr NSUInteger shadow_row_bytes=
              tetra_viewer::shadow_map_resolution*sizeof(float);
          shadow_probe_buffer=[device newBufferWithLength:
              shadow_row_bytes*tetra_viewer::shadow_map_resolution*
                  tetra_viewer::shadow_cascade_count
              options:MTLResourceStorageModeShared];
          id<MTLBlitCommandEncoder> blit=[command_buffer blitCommandEncoder];
          for(NSUInteger slice=0;slice<tetra_viewer::shadow_cascade_count;
              ++slice)
            [blit copyFromTexture:shadow_texture
                      sourceSlice:slice sourceLevel:0
                     sourceOrigin:MTLOriginMake(0,0,0)
                       sourceSize:MTLSizeMake(
                           tetra_viewer::shadow_map_resolution,
                           tetra_viewer::shadow_map_resolution,1)
                         toBuffer:shadow_probe_buffer
                destinationOffset:slice*shadow_row_bytes*
                                      tetra_viewer::shadow_map_resolution
           destinationBytesPerRow:shadow_row_bytes
         destinationBytesPerImage:shadow_row_bytes*
                                      tetra_viewer::shadow_map_resolution
                          options:MTLBlitOptionDepthFromDepthStencil];
          [blit endEncoding];
        }
        if(any_atmosphere_frame_test&&fitted_shadow_initialized){
          constexpr NSUInteger fitted_row_bytes=
              tetra_viewer::shadow_map_resolution*sizeof(float);
          fitted_shadow_probe_buffer=[device newBufferWithLength:
              fitted_row_bytes*tetra_viewer::shadow_map_resolution
              options:MTLResourceStorageModeShared];
          id<MTLBlitCommandEncoder> blit=[command_buffer blitCommandEncoder];
          [blit copyFromTexture:shadow_texture
                    sourceSlice:tetra_viewer::shadow_cascade_count
                    sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                     sourceSize:MTLSizeMake(
                         tetra_viewer::shadow_map_resolution,
                         tetra_viewer::shadow_map_resolution,1)
                       toBuffer:fitted_shadow_probe_buffer
              destinationOffset:0 destinationBytesPerRow:fitted_row_bytes
            destinationBytesPerImage:fitted_row_bytes*
                tetra_viewer::shadow_map_resolution
                        options:MTLBlitOptionDepthFromDepthStencil];
          [blit endEncoding];
        }
        const auto timing_destination=gpu_frame_milliseconds;
        const auto timing_sequence=gpu_frame_sequence;
        const auto maintenance_destination=gpu_frame_maintenance;
        const auto moving_destination=gpu_frame_moving;
        const auto stage_destination=gpu_stage_timings;
        const auto profile_destination=timing_profile_samples;
        const auto acceleration_structure_milliseconds=
            terrain_acceleration_structure.last_build_milliseconds;
        const auto acceleration_structure_timing_valid=
            terrain_acceleration_structure.last_build_timing_valid;
        const bool timing_includes_acceleration_structure=
            acceleration_structure_build_encoded;
        const MetalTimingIdentity timing_identity{
            .terrain_generation=terrain_display_front.render_generation,
            .output_width=static_cast<std::uint32_t>(width),
            .output_height=static_cast<std::uint32_t>(height),
            .render_width=static_cast<std::uint32_t>(render_width),
            .render_height=static_cast<std::uint32_t>(render_height),
            .atmosphere_divisor=static_cast<std::uint32_t>(
                atmosphere_screen_divisor),
            .samples=static_cast<std::uint32_t>(active_samples),
            .transport=atmosphere_transport,
            .renderer=atmosphere_renderer,
            .metalfx=metalfx_temporal_active};
        // Startup uploads and empty drawables are not representative rendered
        // frames.  In particular, including them hid forced lookup refreshes
        // behind the asynchronous terrain startup in early P2 runs.
        const bool timing_profile_sample_eligible=(timing_profile_test||soak_test||
            gpu_terrain_performance_smoke_test)&&
            scene_vertex_count!=0U&&requested_preview_capture_ready&&
            requested_rt_capture_ready&&requested_profile_capture_ready;
        const bool timing_includes_metalfx=metalfx_temporal_active;
        const bool timing_maintenance_frame=acceleration_structure_build_encoded||
            optical_lookup_encoded_this_frame||reference_lookup_encoded_this_frame||
            aerial_lookup_encoded_this_frame;
        const bool timing_moving_frame=runtime_camera_interactive;
        id<MTLBuffer> stage_results=gpu_timestamp_results;
        const auto timestamp_flight_in_use=gpu_timestamp_flight==nullptr?
            std::shared_ptr<std::atomic<bool>>{}:
            gpu_timestamp_flight->in_use;
        [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> completed){
          if(completed.status==MTLCommandBufferStatusCompleted&&
             completed.GPUEndTime>=completed.GPUStartTime){
            timing_destination->store(
                (completed.GPUEndTime-completed.GPUStartTime)*1000.0,
                std::memory_order_relaxed);
            maintenance_destination->store(timing_maintenance_frame,
                                           std::memory_order_relaxed);
            moving_destination->store(timing_moving_frame,
                                      std::memory_order_relaxed);
            if(timing_profile_sample_eligible)
              profile_destination->add(
                  (completed.GPUEndTime-completed.GPUStartTime)*1000.0);
            const std::uint64_t completed_sequence=
                timing_sequence->fetch_add(1U,std::memory_order_relaxed)+1U;
            if(stage_results!=nil){
              // Counter samples can be unavailable on a frame. Preserve the
              // most recent coherent sample rather than relabelling it with
              // an incoherent successor; consumers must treat it as a
              // sampled breakdown, not a per-frame timer.
              const auto* timestamps=static_cast<const MTLCounterResultTimestamp*>(
                  stage_results.contents);
              const auto usable_sample=[&](NSUInteger index){
                return timestamps[index].timestamp!=0U&&
                    timestamps[index].timestamp!=
                        std::numeric_limits<std::uint64_t>::max();
              };
              // Lookup encoders are independent timing intervals. Keep their
              // evidence even when the coarse frame partition cannot be
              // composed on this counter flight (for example around MetalFX
              // driver-owned work).
              const auto sampled_milliseconds=[&](NSUInteger first,
                                                   NSUInteger second){
                return static_cast<double>(
                    timestamps[second].timestamp-timestamps[first].timestamp)*
                    counter_timestamp_milliseconds;
              };
              const bool terrain_generation_timing_valid=
                  usable_sample(25U)&&usable_sample(26U)&&
                  timestamps[26U].timestamp>=timestamps[25U].timestamp;
              if(owner_direct_generation_encoded_this_frame&&
                 terrain_generation_timing_valid){
                const double generation_milliseconds=
                    sampled_milliseconds(25U,26U);
                stage_destination->terrain_generation_milliseconds.store(
                    generation_milliseconds,std::memory_order_relaxed);
                if(gpu_terrain_performance_smoke_test)
                  profile_destination->add_terrain_generation(
                      generation_milliseconds);
              }
              const bool optical_lookup_timing_valid=usable_sample(21U)&&
                  usable_sample(22U)&&
                  timestamps[22U].timestamp>=timestamps[21U].timestamp;
              if(optical_lookup_encoded_this_frame&&
                 optical_lookup_timing_valid){
                stage_destination->optical_lookup_milliseconds.store(
                    sampled_milliseconds(21U,22U),std::memory_order_relaxed);
                if(timing_profile_sample_eligible)
                  profile_destination->add_optical_lookup(
                      sampled_milliseconds(21U,22U));
              }
              const bool sky_view_lookup_timing_valid=usable_sample(17U)&&
                  usable_sample(18U)&&
                  timestamps[18U].timestamp>=timestamps[17U].timestamp;
              if(reference_lookup_encoded_this_frame&&
                 sky_view_lookup_timing_valid){
                stage_destination->sky_view_lookup_milliseconds.store(
                    sampled_milliseconds(17U,18U),std::memory_order_relaxed);
                if(timing_profile_sample_eligible)
                  profile_destination->add_sky_view_lookup(
                      sampled_milliseconds(17U,18U));
              }
              const bool irradiance_lookup_timing_valid=usable_sample(19U)&&
                  usable_sample(20U)&&
                  timestamps[20U].timestamp>=timestamps[19U].timestamp;
              if(reference_lookup_encoded_this_frame&&
                 irradiance_lookup_timing_valid){
                stage_destination->irradiance_lookup_milliseconds.store(
                    sampled_milliseconds(19U,20U),std::memory_order_relaxed);
                if(timing_profile_sample_eligible)
                  profile_destination->add_irradiance_lookup(
                      sampled_milliseconds(19U,20U));
              }
              const bool aerial_lookup_timing_valid=usable_sample(23U)&&
                  usable_sample(24U)&&
                  timestamps[24U].timestamp>=timestamps[23U].timestamp;
              if(aerial_lookup_encoded_this_frame&&
                 aerial_lookup_timing_valid){
                stage_destination->aerial_lookup_milliseconds.store(
                    sampled_milliseconds(23U,24U),std::memory_order_relaxed);
                if(timing_profile_sample_eligible)
                  profile_destination->add_aerial_lookup(
                      sampled_milliseconds(23U,24U));
              }
              // Like the lookup encoders, the reference screen marcher has an
              // independently bracketed interval. MetalFX may make the
              // surrounding coarse partition non-composable, which must not
              // discard an otherwise valid, current integration sample.
              const bool screen_integration_timing_valid=usable_sample(9U)&&
                  usable_sample(10U)&&
                  timestamps[10U].timestamp>=timestamps[9U].timestamp;
              if(timing_profile_sample_eligible&&
                 reference_screen_integration_encoded_this_frame&&
                 screen_integration_timing_valid)
                profile_destination->add_screen_integration(
                    sampled_milliseconds(9U,10U));
              bool valid=true;
              for(NSUInteger index=0U;index<gpu_base_timestamp_count;++index)
                valid=valid&&usable_sample(index);
              for(NSUInteger index=1U;index<gpu_base_timestamp_count;++index)
                valid=valid&&timestamps[index].timestamp>=
                    timestamps[index-1U].timestamp;
              if(valid){
                const double frame_milliseconds=
                    (completed.GPUEndTime-completed.GPUStartTime)*1000.0;
                const auto milliseconds=[&](NSUInteger first,
                                             NSUInteger second){
                  return static_cast<double>(
                      timestamps[second].timestamp-
                      timestamps[first].timestamp)*
                      counter_timestamp_milliseconds;
                };
                const auto ordered_milliseconds=[&](NSUInteger first,
                                                     NSUInteger second){
                  return timestamps[second].timestamp>=
                         timestamps[first].timestamp?
                      milliseconds(first,second):0.0;
                };
                const double shadows=milliseconds(1U,2U);
                const double terrain=milliseconds(3U,4U);
                const double composite=milliseconds(5U,6U);
                const double atmosphere=ordered_milliseconds(0U,1U)+
                    ordered_milliseconds(2U,3U)+
                    ordered_milliseconds(4U,5U);
                const double consistency_tolerance=
                    std::max(0.02,frame_milliseconds*0.01);
                valid=atmosphere+shadows+terrain+composite<=
                    frame_milliseconds+consistency_tolerance;
                if(!valid){
                  if(timestamp_flight_in_use)
                    timestamp_flight_in_use->store(
                        false,std::memory_order_release);
                  return;
                }
                if(timing_includes_acceleration_structure&&
                   usable_sample(15U)&&usable_sample(16U)&&
                   timestamps[16].timestamp>=timestamps[15].timestamp){
                  const double acceleration_structure_milliseconds_value=
                      milliseconds(15U,16U);
                  if(acceleration_structure_milliseconds_value<=
                     frame_milliseconds+consistency_tolerance){
                    acceleration_structure_milliseconds->store(
                        acceleration_structure_milliseconds_value,
                        std::memory_order_relaxed);
                    acceleration_structure_timing_valid->store(
                        true,std::memory_order_release);
                  }
                }
                stage_destination->atmosphere_milliseconds.store(
                    atmosphere,std::memory_order_relaxed);
                stage_destination->shadows_milliseconds.store(
                    shadows,std::memory_order_relaxed);
                stage_destination->terrain_milliseconds.store(
                    terrain,std::memory_order_relaxed);
                stage_destination->composite_milliseconds.store(
                    composite,std::memory_order_relaxed);
                const bool screen_valid=usable_sample(7U)&&usable_sample(8U)&&
                    usable_sample(9U)&&usable_sample(10U)&&
                    timestamps[7].timestamp>=timestamps[4].timestamp&&
                    timestamps[8].timestamp>=timestamps[7].timestamp&&
                    timestamps[9].timestamp>=timestamps[8].timestamp&&
                    timestamps[10].timestamp>=timestamps[9].timestamp;
                const bool temporal_valid=usable_sample(11U)&&
                    usable_sample(12U)&&
                    timestamps[11].timestamp>=timestamps[10].timestamp&&
                    timestamps[12].timestamp>=timestamps[11].timestamp&&
                    timestamps[12].timestamp<=timestamps[5].timestamp;
                const double screen_sum=screen_valid?
                    milliseconds(7U,8U)+milliseconds(9U,10U)+
                        (temporal_valid?milliseconds(11U,12U):0.0):0.0;
                const bool screen_consistent=screen_valid&&valid&&
                    screen_sum<=ordered_milliseconds(4U,5U)+
                        consistency_tolerance;
                if(screen_consistent){
                  stage_destination->depth_reduction_milliseconds.store(
                      milliseconds(7U,8U),std::memory_order_relaxed);
                  stage_destination->screen_integration_milliseconds.store(
                      milliseconds(9U,10U),std::memory_order_relaxed);
                stage_destination->temporal_reconstruction_milliseconds.store(
                    temporal_valid?milliseconds(11U,12U):0.0,
                    std::memory_order_relaxed);
                }
                stage_destination->screen_stages_valid.store(
                    screen_consistent,std::memory_order_relaxed);
                const bool metalfx_valid=valid&&usable_sample(13U)&&
                    usable_sample(14U)&&
                    timestamps[14].timestamp>=timestamps[13].timestamp&&
                    milliseconds(13U,14U)<=frame_milliseconds+
                        consistency_tolerance;
                if(metalfx_valid){
                  stage_destination->metalfx_milliseconds.store(
                      milliseconds(13U,14U),std::memory_order_relaxed);
                  stage_destination->metalfx_valid.store(
                      true,std::memory_order_relaxed);
                }
                stage_destination->terrain_generation.store(
                    timing_identity.terrain_generation,std::memory_order_relaxed);
                stage_destination->output_width.store(
                    timing_identity.output_width,std::memory_order_relaxed);
                stage_destination->output_height.store(
                    timing_identity.output_height,std::memory_order_relaxed);
                stage_destination->render_width.store(
                    timing_identity.render_width,std::memory_order_relaxed);
                stage_destination->render_height.store(
                    timing_identity.render_height,std::memory_order_relaxed);
                stage_destination->atmosphere_divisor.store(
                    timing_identity.atmosphere_divisor,std::memory_order_relaxed);
                stage_destination->samples.store(
                    timing_identity.samples,std::memory_order_relaxed);
                stage_destination->transport.store(
                    timing_identity.transport,std::memory_order_relaxed);
                stage_destination->renderer.store(
                    timing_identity.renderer,std::memory_order_relaxed);
                stage_destination->metalfx.store(
                    timing_identity.metalfx,std::memory_order_relaxed);
                stage_destination->frame_sequence.store(
                    completed_sequence,std::memory_order_release);
                stage_destination->valid.store(true,std::memory_order_release);
              }
            }
          }
          if(timestamp_flight_in_use)
            timestamp_flight_in_use->store(false,std::memory_order_release);
        }];
        [command_buffer presentDrawable:drawable];
        const auto submission_started=std::chrono::steady_clock::now();
        [command_buffer commit];
        cpu_submission_milliseconds->store(
            std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-submission_started).count(),
            std::memory_order_relaxed);
        if(serial_timestamp_collection)[command_buffer waitUntilCompleted];
        if((motion_test||gpu_terrain_performance_smoke_test)&&
           scene_vertex_count!=0U)++motion_rendered_frames;
        if(render_test&&scene_vertex_count!=0U)++render_test_frames;
        if(metalfx_test&&scene_vertex_count!=0U&&capture_buffer!=nil)
          ++metalfx_test_frames;
        if(soak_test&&scene_vertex_count!=0U)++soak_rendered_frames;
        if(auto_resolution_test&&scene_vertex_count!=0U)
          ++auto_resolution_test_frames;
        if(overlay_test&&lod_overlay_vertex_count!=0U&&
           player_overlay_vertex_count!=0U)++overlay_test_frames;
        if(shadow_test&&shadow_probe_buffer!=nil)++shadow_test_frames;
        if(any_atmosphere_frame_test&&capture_buffer!=nil)++atmosphere_test_frames;
        if(atmosphere_quality_test&&scene_vertex_count!=0U)
          ++atmosphere_quality_test_frames;
        const bool automated_frame_complete=scene_vertex_count!=0U&&
            requested_preview_capture_ready&&
            requested_rt_capture_ready&&
            requested_profile_capture_ready&&
            (!capture_test||capture_buffer!=nil)&&
            (!terrain_ray_oracle_test||terrain_ray_oracle_encoded)&&
            (!motion_test||(motion_rendered_frames>=30U&&
                            !runtime_camera_interactive&&
                            diagnostics.converged&&!diagnostics.busy))&&
            (!metal_gpu_terrain_diagnostic||(
                gpu_terrain_counters->dispatched.load(std::memory_order_acquire)!=0U&&
                gpu_terrain_counters->completed.load(std::memory_order_acquire)!=0U&&
                gpu_terrain_counters->accepted.load(std::memory_order_acquire)!=0U&&
                (!motion_test||gpu_terrain_counters->stale_rejected.load(
                    std::memory_order_acquire)!=0U)&&
                gpu_terrain_counters->failed.load(std::memory_order_acquire)==0U&&
                gpu_terrain_counters->overflow.load(std::memory_order_acquire)==0U&&
                  gpu_terrain_counters->cpu_front_violations.load(
                    std::memory_order_acquire)==0U))&&
            (!metal_gpu_terrain_live_selection||
             (gpu_hierarchy_live_selection.submitted!=0U&&
              gpu_hierarchy_live_selection.completed!=0U&&
              gpu_hierarchy_live_selection.accepted!=0U&&
              gpu_hierarchy_live_selection.failed==0U&&
              gpu_hierarchy_live_selection.cpu_generation_violations==0U&&
              // The motion test's final contract requires two completed,
              // accepted selections.  Do not tear down the asynchronous
              // live-selection harness after the first completion and then
              // reject that very same otherwise healthy run below.
              (!motion_test||
               (gpu_hierarchy_live_selection.submitted>=2U&&
                gpu_hierarchy_live_selection.completed>=2U&&
                gpu_hierarchy_live_selection.accepted>=2U))) )&&
            (!metal_gpu_terrain_device_front||
             (gpu_terrain_counters->device_closure_submitted.load(
                  std::memory_order_acquire)>=2U&&
              gpu_hierarchy_live_selection.compact_closure_encoded>=2U&&
              (metal_gpu_terrain_device_front_inject_green_budget_failure?
               gpu_hierarchy_live_selection.compact_red_encoded==0U:
               gpu_hierarchy_live_selection.compact_red_encoded>=2U)&&
              (metal_gpu_terrain_device_front_inject_green_budget_failure?
               gpu_hierarchy_live_selection.compact_closure_rejected>=2U:
               gpu_hierarchy_live_selection.compact_closure_completed>=2U)&&
              (metal_gpu_terrain_device_front_inject_green_budget_failure?
               gpu_hierarchy_live_selection.compact_closure_rejected>=1U:
               gpu_hierarchy_live_selection.compact_quiescent>=1U)&&
              gpu_hierarchy_live_selection.compact_owner_materialization_encoded>=2U&&
              gpu_hierarchy_live_selection.compact_p8_encoded>=2U&&
              gpu_hierarchy_live_selection.compact_p8_completed>=2U&&
              gpu_terrain_counters->p6_requests.load(
                  std::memory_order_acquire)==0U&&
              gpu_terrain_counters->cpu_surface_build_requests.load(
                  std::memory_order_acquire)==0U&&
              gpu_terrain_counters->immutable_snapshot_builds.load(
                  std::memory_order_acquire)==1U&&
              gpu_terrain_counters->candidate_payload_readback_requests.load(
                  std::memory_order_acquire)==0U&&
              gpu_terrain_counters->post_bootstrap_seed_attempts.load(
                  std::memory_order_acquire)==0U&&
              gpu_terrain_counters->failed.load(
                  std::memory_order_acquire)==0U&&
              (metal_gpu_terrain_device_front_inject_green_budget_failure?
               (gpu_hierarchy_live_selection.compact_closure_rejected>=1U&&
                gpu_hierarchy_live_selection.compact_p8_private_commits==0U&&
                gpu_hierarchy_live_selection.compact_p8_rejected>=1U&&
                gpu_terrain_counters->device_front_bootstrap_fallback_frames.load(
                    std::memory_order_acquire)!=0U&&
                gpu_terrain_counters->device_front_display_binding_violations.load(
                    std::memory_order_acquire)==0U):
               (gpu_hierarchy_live_selection.compact_closure_rejected==0U&&
                gpu_hierarchy_live_selection.compact_p8_private_commits>=1U&&
                gpu_hierarchy_live_selection.compact_p8_rejected==0U&&
                gpu_terrain_counters->device_front_display_promotions.load(
                    std::memory_order_acquire)!=0U&&
                gpu_terrain_counters->device_front_display_frames.load(
                    std::memory_order_acquire)!=0U&&
                gpu_terrain_counters->device_front_display_binding_violations.load(
                    std::memory_order_acquire)==0U))))&&
            (!gpu_terrain_renderer_selected||
             (gpu_terrain_renderer_available&&gpu_terrain_active_front.promoted&&
              terrain_display_front.exact_indirect_arguments==
                  gpu_terrain_active_front.indirect_arguments))&&
            (!metal_gpu_terrain_private_front_qualification||
             gpu_terrain_counters->cpu_front_violations.load(
                 std::memory_order_acquire)==0U)&&
            (!render_test||render_test_frames>=40U)&&
            (!metalfx_test||(metalfx_test_frames>=45U&&
                             diagnostics.converged&&!diagnostics.busy))&&
            (!soak_test||(soak_rendered_frames>=soak_simulated_frames&&
                          diagnostics.converged&&!diagnostics.busy))&&
            (!auto_resolution_test||auto_resolution_test_frames>=
                 auto_resolution_required_frames)&&
            (!timing_profile_test||timing_profile_samples->size()>=300U)&&
            (!gpu_terrain_performance_smoke_test||
             (timing_profile_samples->size()>=300U&&
              timing_profile_samples->ordered_terrain_generation().size()>=30U))&&
            (!overlay_test||overlay_test_frames>=10U)&&
            (!shadow_test||shadow_test_frames>=3U)&&
            (!any_atmosphere_frame_test||atmosphere_test_frames>=12U);
        const bool quality_test_complete=!atmosphere_quality_test||
            atmosphere_quality_test_frames>=70U;
        if(automated_test&&automated_frame_complete&&quality_test_complete){
          [command_buffer waitUntilCompleted];
          if(command_buffer.status==MTLCommandBufferStatusCompleted){
            if(capture_test){
              std::vector<std::uint8_t> packed(
                  static_cast<std::size_t>(width)*height*4U);
              const auto* source=static_cast<const std::uint8_t*>(
                  capture_buffer.contents);
              for(int row=0;row<height;++row)
                std::memcpy(packed.data()+static_cast<std::size_t>(row)*width*4U,
                            source+static_cast<std::size_t>(row)*capture_row_bytes,
                            static_cast<std::size_t>(width)*4U);
              tetra_viewer::Rgb8Image image;
              std::string error;
              if(!tetra_viewer::make_rgb8_image(
                     packed,static_cast<std::uint32_t>(width),
                     static_cast<std::uint32_t>(height),true,false,image,error)||
                 (write_capture&&
                  !tetra_viewer::write_ppm(argv[2],image,error))){
                std::fprintf(stderr,"Metal capture failed: %s\n",error.c_str());
                result=1;
              }else{
                std::vector<float> depth(
                    static_cast<std::size_t>(width)*height);
                const auto* depth_source=static_cast<const std::uint8_t*>(
                    capture_depth_buffer.contents);
                for(int row=0;row<height;++row)
                  std::memcpy(depth.data()+static_cast<std::size_t>(row)*width,
                              depth_source+
                                  static_cast<std::size_t>(row)*capture_row_bytes,
                              static_cast<std::size_t>(width)*sizeof(float));
                std::vector<std::uint8_t> geometry_mask;
                std::filesystem::path geometry_path(argv[2]);
                geometry_path.replace_extension(".geometry.pgm");
                if(!tetra_viewer::make_reversed_depth_mask(
                       depth,static_cast<std::uint32_t>(width),
                       static_cast<std::uint32_t>(height),geometry_mask,error)){
                  std::fprintf(stderr,"Metal depth capture failed: %s\n",
                               error.c_str());
                  result=1;
                }else if(write_capture){
                  if(!tetra_viewer::write_pgm(
                         geometry_path.string(),
                         static_cast<std::uint32_t>(width),
                         static_cast<std::uint32_t>(height),geometry_mask,error)){
                    std::fprintf(stderr,"Metal depth capture failed: %s\n",
                                 error.c_str());
                    result=1;
                  }
                  const auto analysis=tetra_viewer::analyse_rgb8_image(image);
                  std::printf("{\"event\":\"metal_capture\",\"path\":\"%s\","
                              "\"geometry_mask_path\":\"%s\","
                              "\"scene_generation\":%llu,\"display_generation\":%llu,"
                              "\"preview_visible\":%s,\"triangles\":%zu,"
                              "\"exact_triangles\":%zu,\"preview_triangles\":%zu,"
                              "\"rgb_hash\":%llu,\"luminance_mean\":%.6f,"
                              "\"luminance_stddev\":%.6f}\n",
                              argv[2],geometry_path.string().c_str(),
                              static_cast<unsigned long long>(uploaded_generation),
                              static_cast<unsigned long long>(
                                  terrain_display_front.render_generation),
                              terrain_display_front.preview_cpu?"true":"false",
                              scene_vertex_count/3U,
                              (terrain_display_front.indexed_exact_selection?
                                   terrain_display_front.exact_index_count:
                                   terrain_display_front.exact_vertex_count)/3U,
                              terrain_display_front.preview_index_count/3U,
                              static_cast<unsigned long long>(
                                  tetra_viewer::rgb8_hash(image)),
                              analysis.luminance_mean,
                              analysis.luminance_standard_deviation);
                }else{
                  std::uint32_t reference_width{},reference_height{};
                  std::vector<std::uint8_t> reference;
                  if(!tetra_viewer::read_pgm(
                         argv[2],reference_width,reference_height,reference,
                         error)||reference_width!=static_cast<std::uint32_t>(width)||
                     reference_height!=static_cast<std::uint32_t>(height)){
                    if(error.empty())error="reference mask extent differs from Metal";
                    std::fprintf(stderr,"Metal geometry validation failed: %s\n",
                                 error.c_str());
                    result=1;
                  }else{
                    const auto differences=static_cast<std::size_t>(
                        std::ranges::count_if(
                            std::views::iota(std::size_t{},reference.size()),
                            [&](std::size_t index){
                              return reference[index]!=geometry_mask[index];
                            }));
                    const double fraction=static_cast<double>(differences)/
                        static_cast<double>(reference.size());
                    constexpr double maximum_difference_fraction=0.002;
                    std::printf("{\"event\":\"metal_geometry_validation\","
                                "\"reference\":\"%s\",\"different_pixels\":%zu,"
                                "\"different_fraction\":%.8f,\"limit\":%.8f,"
                                "\"triangles\":%zu,\"passed\":%s}\n",
                                argv[2],differences,fraction,
                                maximum_difference_fraction,
                                scene_vertex_count/3U,
                                fraction<=maximum_difference_fraction?
                                    "true":"false");
                    if(fraction>maximum_difference_fraction)result=1;
                  }
                }
              }
            }else if(terrain_ray_oracle_test){
              const auto* actual=static_cast<const std::uint32_t*>(
                  terrain_ray_oracle_outputs.contents);
              const std::size_t query_count=terrain_ray_oracle_expected.size();
              const std::size_t mismatches=actual==nullptr?query_count:
                  static_cast<std::size_t>(std::count_if(
                      terrain_ray_oracle_expected.begin(),
                      terrain_ray_oracle_expected.end(),
                      [actual,index=std::size_t{}](std::uint32_t expected)mutable{
                        return actual[index++]!=expected;
                      }));
              const std::size_t blocked=actual==nullptr?0U:
                  static_cast<std::size_t>(std::count(actual,actual+query_count,0U));
              std::size_t cpu_only_blocked{},gpu_only_blocked{};
              if(actual!=nullptr)for(std::size_t index=0U;index<query_count;
                                     ++index){
                if(terrain_ray_oracle_expected[index]==0U&&actual[index]!=0U)
                  ++cpu_only_blocked;
                if(terrain_ray_oracle_expected[index]!=0U&&actual[index]==0U)
                  ++gpu_only_blocked;
              }
              const bool passed=terrain_acceleration_structure.active!=nil&&
                  terrain_acceleration_structure.active_generation==
                      terrain_display_front.render_generation&&
                  terrain_ray_oracle_triangles!=0U&&query_count>=128U&&
                  mismatches==0U&&blocked!=0U&&blocked!=query_count;
              std::printf("{\"event\":\"metal_terrain_ray_oracle\","
                          "\"generation\":%llu,\"triangles\":%zu,"
                          "\"queries\":%zu,\"blocked\":%zu,"
                          "\"cpu_gpu_mismatches\":%zu,\"cpu_only_blocked\":%zu,"
                          "\"gpu_only_blocked\":%zu,\"passed\":%s}\n",
                          static_cast<unsigned long long>(uploaded_generation),
                          terrain_ray_oracle_triangles,query_count,blocked,
                          mismatches,cpu_only_blocked,gpu_only_blocked,
                          passed?"true":"false");
              if(!passed)result=1;
            }else if(metalfx_test){
              std::vector<std::uint8_t> packed(
                  static_cast<std::size_t>(width)*height*4U);
              const auto* source=static_cast<const std::uint8_t*>(
                  capture_buffer.contents);
              for(int row=0;row<height;++row)
                std::memcpy(packed.data()+static_cast<std::size_t>(row)*width*4U,
                            source+static_cast<std::size_t>(row)*capture_row_bytes,
                            static_cast<std::size_t>(width)*4U);
              tetra_viewer::Rgb8Image image;
              std::string error;
              const bool converted=tetra_viewer::make_rgb8_image(
                  packed,static_cast<std::uint32_t>(width),
                  static_cast<std::uint32_t>(height),true,false,image,error);
              const auto analysis=converted?
                  tetra_viewer::analyse_rgb8_image(image):
                  tetra_viewer::Rgb8ImageAnalysis{};
              const char* metalfx_capture_path=
                  std::getenv("TETWORLD_METALFX_SMOKE_CAPTURE");
              bool metalfx_capture_written=true;
              if(converted&&metalfx_capture_path!=nullptr&&
                 metalfx_capture_path[0]!='\0')
                metalfx_capture_written=tetra_viewer::write_ppm(
                    metalfx_capture_path,image,error);
              std::size_t finite_motion_pixels{};
              std::size_t moving_pixels{};
              double maximum_motion_pixels{};
              std::size_t reactive_pixels{};
              const std::size_t temporal_pixel_count=
                  static_cast<std::size_t>(render_width)*render_height;
              if(metalfx_motion_probe_buffer!=nil&&
                 metalfx_reactive_probe_buffer!=nil){
                const auto* motion_bytes=static_cast<const std::uint8_t*>(
                    metalfx_motion_probe_buffer.contents);
                const auto* reactive_bytes=static_cast<const std::uint8_t*>(
                    metalfx_reactive_probe_buffer.contents);
                for(int row=0;row<render_height;++row){
                  const auto* motion=reinterpret_cast<const std::uint16_t*>(
                      motion_bytes+static_cast<std::size_t>(row)*
                          metalfx_motion_probe_row_bytes);
                  const auto* reactive=reactive_bytes+
                      static_cast<std::size_t>(row)*
                          metalfx_reactive_probe_row_bytes;
                  for(int column=0;column<render_width;++column){
                    const float x=half_to_float(motion[column*2U]);
                    const float y=half_to_float(motion[column*2U+1U]);
                    if(std::isfinite(x)&&std::isfinite(y)){
                      ++finite_motion_pixels;
                      const double magnitude=std::hypot(
                          static_cast<double>(x),static_cast<double>(y));
                      if(magnitude>0.05)++moving_pixels;
                      maximum_motion_pixels=std::max(
                          maximum_motion_pixels,magnitude);
                    }
                    if(reactive[column]!=0U)++reactive_pixels;
                  }
                }
              }
              const bool passed=metalfx_temporal_supported&&
                  metalfx_resources.scaler!=nil&&
                  metalfx_resources.encoded_frames>=metalfx_test_frames&&
                  metalfx_resources.history_resets>=3U&&
                  metalfx_resources.input_width==render_width&&
                  metalfx_resources.input_height==render_height&&
                  metalfx_resources.output_width==width&&
                  metalfx_resources.output_height==height&&converted&&
                  analysis.luminance_mean>0.01&&
                  analysis.luminance_standard_deviation>0.005&&
                  finite_motion_pixels==temporal_pixel_count&&
                  moving_pixels>temporal_pixel_count/20U&&
                  maximum_motion_pixels>0.1&&
                  reactive_pixels!=0U&&
                  metalfx_generation_changes!=0U&&
                  metalfx_capture_written&&
                  atmosphere_resources.temporal_history_attempts>1U&&
                  atmosphere_resources.temporal_history_compatible!=0U&&
                  (atmosphere_transport==2?
                       atmosphere_resources.ray_visibility_dispatches==0U:
                       (!metal_ray_tracing_supported||
                        atmosphere_resources.ray_visibility_dispatches!=0U))&&
                  (!gpu_stage_timestamps_enabled||
                   gpu_timestamp_counter_set==nil||
                   (gpu_stage_timings->metalfx_valid.load(
                        std::memory_order_relaxed)&&
                    gpu_stage_timings->metalfx_milliseconds.load(
                        std::memory_order_relaxed)>0.0))&&
                  gpu_frame_milliseconds->load(std::memory_order_relaxed)>0.0;
              std::printf("{\"event\":\"metal_metalfx_smoke\","
                          "\"rendered_frames\":%zu,\"encoded_frames\":%llu,"
                          "\"history_resets\":%llu,"
                          "\"input\":\"%dx%d\",\"output\":\"%dx%d\","
                          "\"motion_finite_pixels\":%zu,"
                          "\"motion_moving_pixels\":%zu,"
                          "\"motion_max_pixels\":%.4f,"
                          "\"reactive_pixels\":%zu,"
                          "\"lod_generation_changes\":%zu,"
                          "\"depth_reversed\":true,"
                          "\"exposure\":\"manual_unity_after_tonemap\","
                          "\"ray_visibility_dispatches\":%llu,"
                          "\"ray_visibility_queries_per_pixel\":%u,"
                          "\"atmosphere_history_attempts\":%llu,"
                          "\"atmosphere_history_compatible\":%llu,"
                          "\"atmosphere_history_invalidations\":%llu,"
                          "\"atmosphere_camera_refreshes\":%llu,"
                          "\"gpu_ms\":%.4f,\"metalfx_gpu_ms\":%.4f,"
                          "\"luminance_mean\":%.6f,"
                          "\"luminance_stddev\":%.6f,\"passed\":%s}\n",
                          metalfx_test_frames,
                          static_cast<unsigned long long>(
                              metalfx_resources.encoded_frames),
                          static_cast<unsigned long long>(
                              metalfx_resources.history_resets),
                          render_width,render_height,width,height,
                          finite_motion_pixels,moving_pixels,
                          maximum_motion_pixels,reactive_pixels,
                          metalfx_generation_changes,
                          static_cast<unsigned long long>(
                              atmosphere_resources.ray_visibility_dispatches),
                          atmosphere_resources.last_ray_visibility_query_count,
                          static_cast<unsigned long long>(
                              atmosphere_resources.temporal_history_attempts),
                          static_cast<unsigned long long>(
                              atmosphere_resources.temporal_history_compatible),
                          static_cast<unsigned long long>(
                              atmosphere_resources.temporal_history_invalidations),
                          static_cast<unsigned long long>(
                              atmosphere_resources.temporal_camera_refreshes),
                          gpu_frame_milliseconds->load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->metalfx_milliseconds.load(
                              std::memory_order_relaxed),
                          analysis.luminance_mean,
                          analysis.luminance_standard_deviation,
                          passed?"true":"false");
              if(!passed)result=1;
            }else if(any_atmosphere_frame_test){
              std::vector<std::uint8_t> packed(
                  static_cast<std::size_t>(width)*height*4U);
              const auto* source=static_cast<const std::uint8_t*>(
                  capture_buffer.contents);
              for(int row=0;row<height;++row)
                std::memcpy(packed.data()+static_cast<std::size_t>(row)*width*4U,
                            source+static_cast<std::size_t>(row)*capture_row_bytes,
                            static_cast<std::size_t>(width)*4U);
              tetra_viewer::Rgb8Image image;
              std::string error;
              const bool converted=tetra_viewer::make_rgb8_image(
                  packed,static_cast<std::uint32_t>(width),
                  static_cast<std::uint32_t>(height),true,false,image,error);
              const auto analysis=converted?
                  tetra_viewer::analyse_rgb8_image(image):
                  tetra_viewer::Rgb8ImageAnalysis{};
              std::vector<float> endpoint_values(
                  atmosphere_resources.screen_width*
                  atmosphere_resources.screen_height*4U);
              [atmosphere_resources.screen_endpoint getBytes:endpoint_values.data()
                  bytesPerRow:atmosphere_resources.screen_width*4U*sizeof(float)
                   fromRegion:MTLRegionMake2D(
                       0U,0U,atmosphere_resources.screen_width,
                       atmosphere_resources.screen_height)
                  mipmapLevel:0U];
              std::size_t endpoint_sky_pixels{};
              std::size_t endpoint_surface_pixels{};
              for(std::size_t pixel=0U;pixel<endpoint_values.size()/4U;++pixel)
                if(endpoint_values[pixel*4U+1U]>0.5F)++endpoint_surface_pixels;
                else ++endpoint_sky_pixels;
              std::size_t fitted_coverage{};
              if(fitted_shadow_probe_buffer!=nil){
                const auto* depths=static_cast<const float*>(
                    fitted_shadow_probe_buffer.contents);
                constexpr std::size_t depth_count=
                    static_cast<std::size_t>(
                        tetra_viewer::shadow_map_resolution)*
                    tetra_viewer::shadow_map_resolution;
                fitted_coverage=static_cast<std::size_t>(std::count_if(
                    depths,depths+depth_count,[](float depth){
                      return std::isfinite(depth)&&depth<1.0F;
                    }));
              }
              const NSUInteger long_row_floats=
                  atmosphere_resources.long_shadow.width*4U;
              std::vector<float> long_shadow_values(
                  long_row_floats*atmosphere_resources.long_shadow.height);
              [atmosphere_resources.long_shadow
                  getBytes:long_shadow_values.data()
               bytesPerRow:long_row_floats*sizeof(float)
                fromRegion:MTLRegionMake2D(
                    0U,0U,atmosphere_resources.long_shadow.width,
                    atmosphere_resources.long_shadow.height)
               mipmapLevel:0U];
              std::size_t long_shadow_finite{};
              std::size_t long_shadow_occluded{};
              for(std::size_t pixel=0;pixel<long_shadow_values.size()/4U;
                  ++pixel){
                const auto* value=long_shadow_values.data()+pixel*4U;
                if(std::ranges::all_of(std::span(value,4U),
                                      [](float channel){
                                        return std::isfinite(channel);
                                      }))++long_shadow_finite;
                if(value[3]>1.0e-8F&&
                   std::min({value[0],value[1],value[2]})<0.999F)
                  ++long_shadow_occluded;
              }
              const std::size_t long_shadow_pixels=
                  long_shadow_values.size()/4U;
              const bool stage_timing_valid=gpu_stage_timings->valid.load(
                  std::memory_order_relaxed);
              const double atmosphere_gpu_milliseconds=
                  gpu_stage_timings->atmosphere_milliseconds.load(
                      std::memory_order_relaxed);
              const bool screen_stage_timing_valid=
                  gpu_stage_timings->screen_stages_valid.load(
                      std::memory_order_relaxed);
              const bool reference_screen_dispatched=atmosphere_transport==2&&
                  atmosphere_resources.dispatch_counts[12]!=0U&&
                  atmosphere_resources.dispatch_counts[13]!=0U&&
                  atmosphere_resources.dispatch_counts[14]!=0U&&
                  atmosphere_resources.dispatch_counts[15]==0U;
              const bool timing_passed=atmosphere_renderer==4?
                  atmosphere_resources.dispatch_counts[16]!=0U:
                  (reference_screen_dispatched||
                   !gpu_stage_timestamps_enabled||
                   gpu_timestamp_counter_set==nil||
                   (stage_timing_valid&&screen_stage_timing_valid&&
                    atmosphere_gpu_milliseconds>0.0)||
                   gpu_frame_milliseconds->load(std::memory_order_relaxed)>
                       0.0);
              const bool temporal_accounting_passed=atmosphere_renderer!=3||
                  (atmosphere_resources.temporal_history_attempts>1U&&
                   atmosphere_resources.temporal_history_compatible!=0U);
              // This smoke is deliberately parameterised by the public
              // transport/renderer/debug environment controls.  Keep the
              // liveness assertion next to the image qualification so each
              // route proves that an inactive atlas was not merely ignored by
              // its shader after being encoded.
              const bool long_shadow_dispatch_required=long_shadow_consumed&&
                  !ray_traced_screen_visibility_active&&
                  atmosphere_transport!=0&&scene_vertex_count!=0U;
              const bool long_shadow_dispatch_passed=
                  long_shadow_dispatch_required?
                      atmosphere_resources.dispatch_counts[6U]!=0U:
                      atmosphere_resources.dispatch_counts[6U]==0U;
              const bool lookup_invalidation_passed=
                  !atmosphere_lookup_invalidation_test||
                  (lookup_invalidation_sun_changed&&
                   atmosphere_resources.dispatch_counts[2U]==2U&&
                   atmosphere_resources.dispatch_counts[4U]==2U&&
                   atmosphere_resources.reference_lookup_attempts==12U&&
                   atmosphere_resources.reference_lookup_skips==10U);
              const bool reference_visibility_resources_absent=
                  atmosphere_transport!=2||
                  (atmosphere_resources.terrain_ray_visibility==nil&&
                   atmosphere_resources.history_visibility[0]==nil&&
                   atmosphere_resources.history_visibility[1]==nil);
              const bool froxel_resources_lazy=atmosphere_renderer==4||
                  (atmosphere_resources.froxel_scattering==
                       atmosphere_resources.dummy_froxel_scattering&&
                   atmosphere_resources.froxel_transmittance==
                       atmosphere_resources.dummy_froxel_transmittance);
              const bool long_shadow_resources_lazy=long_shadow_consumed||
                  atmosphere_resources.long_shadow==
                      atmosphere_resources.dummy_long_shadow;
              const bool minmax_resources_lazy=long_shadow_consumed||
                  shadow_integration==2||shadow_integration==4||
                  shadow_integration==5||atmosphere_resources.minmax==
                      atmosphere_resources.dummy_minmax;
              const bool aerial_resources_lazy=
                  atmosphere_transport!=2||atmosphere_debug_view==4||
                  atmosphere_debug_view==5||
                  (atmosphere_resources.aerial_scattering==
                       atmosphere_resources.dummy_aerial_scattering&&
                   atmosphere_resources.aerial_transmittance==
                       atmosphere_resources.dummy_aerial_transmittance);
              const bool passed=converted&&analysis.luminance_mean>0.01&&
                  analysis.luminance_standard_deviation>0.005&&
                  // The named flight/top/orbit fixtures deliberately look
                  // beyond the local shadow map's footprint.  Their image is
                  // still guarded by real geometry/readback and the physical
                  // atmosphere checks above; requiring a local fitted-depth
                  // texel here would reject a valid whole-planet view.
                  (atmosphere_capture_pose!=nullptr||fitted_coverage!=0U)&&
                  (!long_shadow_consumed||
                   (long_shadow_finite==long_shadow_pixels&&
                    (ray_traced_screen_visibility_active||
                     long_shadow_occluded!=0U)))&&
                  (ray_traced_screen_visibility_active?
                      atmosphere_resources.ray_visibility_dispatches!=0U:
                      atmosphere_resources.ray_visibility_dispatches==0U)&&
                  (!atmosphere_quarter_test||
                   (atmosphere_resources.screen_divisor==4U&&
                    atmosphere_resources.last_ray_visibility_query_count==1U))&&
                  timing_passed&&temporal_accounting_passed&&
                  long_shadow_dispatch_passed&&lookup_invalidation_passed&&
                  reference_visibility_resources_absent&&froxel_resources_lazy&&
                  long_shadow_resources_lazy&&minmax_resources_lazy&&
                  aerial_resources_lazy;
              if(atmosphere_capture&&converted&&
                 !tetra_viewer::write_ppm(argv[2],image,error)){
                std::fprintf(stderr,"Metal atmosphere capture failed: %s\n",
                             error.c_str());
                result=1;
              }
              std::printf("{\"event\":\"metal_atmosphere_frame_smoke\","
                          "\"path\":\"%s\",\"rendered_frames\":%zu,"
                          "\"fitted_shadow_pixels\":%zu,"
                          "\"long_shadow_pixels\":%zu,"
                          "\"long_shadow_occluded\":%zu,"
                          "\"luminance_mean\":%.6f,"
                          "\"luminance_stddev\":%.6f,"
                          "\"frame_gpu_ms\":%.4f,\"composite_gpu_ms\":%.4f,"
                          "\"atmosphere_gpu_ms\":%.4f,"
                          "\"depth_gpu_ms\":%.4f,\"integration_gpu_ms\":%.4f,"
                          "\"temporal_gpu_ms\":%.4f,"
                          "\"endpoint_sky_pixels\":%zu,"
                          "\"endpoint_surface_pixels\":%zu,"
                          "\"sky_view_dispatches\":%llu,"
                          "\"sky_irradiance_dispatches\":%llu,"
                          "\"sky_lookup_attempts\":%llu,"
                          "\"sky_lookup_skips\":%llu,"
                          "\"lookup_invalidation_sun_changed\":%s,"
                          "\"long_shadow_dispatches\":%llu,"
                          "\"long_shadow_dispatch_required\":%s,"
                          "\"aerial_scattering_dispatches\":%llu,"
                          "\"froxel_dispatches\":%llu,"
                          "\"reference_visibility_resources_absent\":%s,"
                          "\"froxel_resources_lazy\":%s,"
                          "\"long_shadow_resources_lazy\":%s,"
                          "\"minmax_resources_lazy\":%s,"
                          "\"aerial_resources_lazy\":%s,"
                          "\"atmosphere_allocation_bytes\":%zu,"
                          "\"ray_visibility_dispatches\":%llu,"
                          "\"ray_visibility_queries_per_pixel\":%u,"
                          "\"history_attempts\":%llu,"
                          "\"history_compatible\":%llu,"
                          "\"history_invalidations\":%llu,"
                          "\"camera_visibility_refreshes\":%llu,"
                          "\"screen_divisor\":%u,"
                          "\"render_scale\":%.3f,"
                          "\"terrain_samples\":%lu,"
                          "\"metalfx\":%s,"
                          "\"rt_visibility_owner\":%s,"
                          "\"passed\":%s}\n",
                          atmosphere_capture?argv[2]:"",atmosphere_test_frames,
                          fitted_coverage,long_shadow_pixels,
                          long_shadow_occluded,
                          analysis.luminance_mean,
                          analysis.luminance_standard_deviation,
                          gpu_frame_milliseconds->load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->composite_milliseconds.load(
                              std::memory_order_relaxed),
                          atmosphere_gpu_milliseconds,
                          gpu_stage_timings->depth_reduction_milliseconds.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->screen_integration_milliseconds.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->temporal_reconstruction_milliseconds.load(
                              std::memory_order_relaxed),
                          endpoint_sky_pixels,endpoint_surface_pixels,
                          static_cast<unsigned long long>(
                              atmosphere_resources.dispatch_counts[2]),
                          static_cast<unsigned long long>(
                              atmosphere_resources.dispatch_counts[4]),
                          static_cast<unsigned long long>(
                              atmosphere_resources.reference_lookup_attempts),
                          static_cast<unsigned long long>(
                              atmosphere_resources.reference_lookup_skips),
                          lookup_invalidation_sun_changed?"true":"false",
                          static_cast<unsigned long long>(
                              atmosphere_resources.dispatch_counts[6U]),
                          long_shadow_dispatch_required?"true":"false",
                          static_cast<unsigned long long>(
                              atmosphere_resources.dispatch_counts[3U]),
                          static_cast<unsigned long long>(
                              atmosphere_resources.dispatch_counts[16U]),
                          reference_visibility_resources_absent?"true":"false",
                          froxel_resources_lazy?"true":"false",
                          long_shadow_resources_lazy?"true":"false",
                          minmax_resources_lazy?"true":"false",
                          aerial_resources_lazy?"true":"false",
                          live_atmosphere_allocation_bytes(atmosphere_resources),
                          static_cast<unsigned long long>(
                              atmosphere_resources.ray_visibility_dispatches),
                          atmosphere_resources.last_ray_visibility_query_count,
                          static_cast<unsigned long long>(
                              atmosphere_resources.temporal_history_attempts),
                          static_cast<unsigned long long>(
                              atmosphere_resources.temporal_history_compatible),
                          static_cast<unsigned long long>(
                              atmosphere_resources.temporal_history_invalidations),
                          static_cast<unsigned long long>(
                              atmosphere_resources.temporal_camera_refreshes),
                          atmosphere_resources.screen_divisor,
                          active_render_scale,
                          static_cast<unsigned long>(active_samples),
                          metalfx_temporal_active?"true":"false",
                          ray_traced_screen_visibility_active?"true":"false",
                          passed?"true":"false");
              if(!passed)result=1;
            }else if(shadow_test){
              const auto* samples=static_cast<const float*>(
                  shadow_probe_buffer.contents);
              constexpr std::size_t sample_count=
                  static_cast<std::size_t>(
                      tetra_viewer::shadow_map_resolution)*
                  tetra_viewer::shadow_map_resolution*
                  tetra_viewer::shadow_cascade_count;
              std::array<std::size_t,tetra_viewer::shadow_cascade_count>
                  cascade_coverage{};
              constexpr std::size_t pixels_per_cascade=sample_count/
                  tetra_viewer::shadow_cascade_count;
              for(std::size_t cascade=0;
                  cascade<tetra_viewer::shadow_cascade_count;++cascade)
                cascade_coverage[cascade]=static_cast<std::size_t>(
                    std::count_if(samples+cascade*pixels_per_cascade,
                        samples+(cascade+1U)*pixels_per_cascade,
                        [](float depth){
                          return std::isfinite(depth)&&depth<1.0F;
                        }));
              const auto covered=std::accumulate(
                  cascade_coverage.begin(),cascade_coverage.end(),
                  std::size_t{});
              const bool passed=covered!=0U;
              std::printf("{\"event\":\"metal_shadow_smoke\","
                          "\"rendered_frames\":%zu,\"covered_pixels\":%zu,"
                          "\"cascade_coverage\":[%zu,%zu,%zu,%zu],"
                          "\"cpu_candidates\":[%zu,%zu,%zu,%zu],"
                          "\"cascade_refreshes\":%llu,\"passed\":%s}\n",
                          shadow_test_frames,covered,cascade_coverage[0],
                          cascade_coverage[1],cascade_coverage[2],
                          cascade_coverage[3],shadow_cpu_candidates[0],
                          shadow_cpu_candidates[1],shadow_cpu_candidates[2],
                          shadow_cpu_candidates[3],
                          static_cast<unsigned long long>(
                              shadow_cascade_refreshes),
                          passed?"true":"false");
              if(!passed)result=1;
            }else if(overlay_test){
              const bool passed=wireframe_draws!=0U;
              std::printf("{\"event\":\"metal_overlay_smoke\","
                          "\"rendered_frames\":%zu,\"line_vertices\":%zu,"
                          "\"capsule\":true,\"contact_normal\":true,"
                          "\"lod_zones\":true,\"wireframe_draws\":%llu,"
                          "\"passed\":%s}\n",
                          overlay_test_frames,lod_overlay_vertex_count+
                              player_overlay_vertex_count,
                          static_cast<unsigned long long>(wireframe_draws),
                          passed?"true":"false");
              if(!passed)result=1;
            }else if(render_test){
              const bool stage_timing_valid=gpu_stage_timings->valid.load(
                  std::memory_order_acquire);
              const auto stage_sequence=gpu_stage_timings->frame_sequence.load(
                  std::memory_order_acquire);
              const bool passed=gpu_frame_milliseconds->load(
                  std::memory_order_relaxed)>0.0&&
                  (!gpu_stage_timestamps_enabled||
                   gpu_timestamp_counter_set==nil||stage_timing_valid);
              std::printf("{\"event\":\"metal_render_smoke\","
                          "\"rendered_frames\":%zu,\"drawable\":\"%dx%d\","
                          "\"internal\":\"%dx%d\",\"samples\":%lu,"
                          "\"gpu_milliseconds\":%.4f,"
                          "\"cpu_submission_ms\":%.4f,"
                          "\"stage_timing_valid\":%s,"
                          "\"stage_sequence\":%llu,"
                          "\"stage_generation\":%llu,"
                          "\"stage_output\":\"%ux%u\","
                          "\"stage_internal\":\"%ux%u\","
                          "\"stage_samples\":%u,"
                          "\"stage_transport\":%d,"
                          "\"stage_renderer\":%d,"
                          "\"stage_divisor\":%u,"
                          "\"stage_metalfx\":%s,"
                          "\"shadow_ms\":%.4f,\"atmosphere_ms\":%.4f,"
                          "\"terrain_ms\":%.4f,\"composite_ms\":%.4f,"
                          "\"depth_ms\":%.4f,\"integration_ms\":%.4f,"
                          "\"temporal_ms\":%.4f,\"metalfx_ms\":%.4f,"
                          "\"rt_builds\":%llu,\"rt_build_ms\":%.4f,"
                          "\"rt_build_timing_valid\":%s,"
                          "\"display_generation\":%llu,\"rt_generation\":%llu,"
                          "\"passed\":%s}\n",
                          render_test_frames,width,height,render_width,
                          render_height,static_cast<unsigned long>(
                              allocated_samples),
                          gpu_frame_milliseconds->load(
                              std::memory_order_relaxed),
                          cpu_submission_milliseconds->load(
                              std::memory_order_relaxed),
                          stage_timing_valid?"true":"false",
                          static_cast<unsigned long long>(stage_sequence),
                          static_cast<unsigned long long>(
                              gpu_stage_timings->terrain_generation.load(
                                  std::memory_order_relaxed)),
                          gpu_stage_timings->output_width.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->output_height.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->render_width.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->render_height.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->samples.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->transport.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->renderer.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->atmosphere_divisor.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->metalfx.load(
                              std::memory_order_relaxed)?"true":"false",
                          gpu_stage_timings->shadows_milliseconds.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->atmosphere_milliseconds.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->terrain_milliseconds.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->composite_milliseconds.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->depth_reduction_milliseconds.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->screen_integration_milliseconds.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->temporal_reconstruction_milliseconds.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->metalfx_milliseconds.load(
                              std::memory_order_relaxed),
                          static_cast<unsigned long long>(
                              terrain_acceleration_structure.build_count),
                          terrain_acceleration_structure.last_build_milliseconds->load(
                              std::memory_order_relaxed),
                          terrain_acceleration_structure.last_build_timing_valid->load(
                              std::memory_order_acquire)?"true":"false",
                          static_cast<unsigned long long>(
                              terrain_display_front.render_generation),
                          static_cast<unsigned long long>(
                              terrain_acceleration_structure.active_generation),
                          passed?"true":"false");
              if(!passed)result=1;
            }else if(gpu_terrain_performance_smoke_test){
              const auto frames=timing_profile_samples->ordered();
              const auto generations=
                  timing_profile_samples->ordered_terrain_generation();
              // P8c2 only qualifies CPU-seeded owner-direct emission, not the
              // future P7e GPU terrain-generation route. Its GPU timestamp
              // p95 varies substantially under normal desktop scheduling, so
              // use the observed 58.8695 ms worst p95 as a 60 ms regression
              // ceiling rather than pretending this is a repeatable latency
              // target. P7e4 must instead prove a material camera-to-front
              // improvement against the CPU baseline on the actual route.
              constexpr double generation_limit_milliseconds=60.0;
              constexpr double frame_limit_milliseconds=1000.0/30.0;
              const double generation_p95=timing_percentile(generations,0.95);
              const double frame_p95=timing_percentile(frames,0.95);
              const bool passed=gpu_stage_timestamps_enabled&&
                  generations.size()==30U&&frames.size()==300U&&
                  generations.front()>0.0&&std::isfinite(generations.back())&&
                  frames.front()>0.0&&std::isfinite(frames.back())&&
                  generation_p95<=generation_limit_milliseconds&&
                  frame_p95<=frame_limit_milliseconds&&
                  gpu_terrain_renderer_available&&
                  gpu_terrain_active_front.promoted&&
                  gpu_terrain_counters->failed.load(std::memory_order_acquire)==0U&&
                  gpu_terrain_counters->overflow.load(std::memory_order_acquire)==0U&&
                  gpu_terrain_counters->cpu_front_violations.load(
                      std::memory_order_acquire)==0U;
              std::printf("{\"event\":\"metal_gpu_terrain_performance\","
                          "\"generation_samples\":%zu,\"generation_median_ms\":%.4f,"
                          "\"generation_p95_ms\":%.4f,\"generation_max_ms\":%.4f,"
                          "\"generation_limit_ms\":%.4f,\"frame_samples\":%zu,"
                          "\"frame_median_ms\":%.4f,\"frame_p95_ms\":%.4f,"
                          "\"frame_max_ms\":%.4f,\"frame_limit_ms\":%.4f,"
                          "\"stage_timestamps\":%s,\"selected\":true,"
                          "\"dispatched\":%llu,\"accepted\":%llu,"
                          "\"failed\":%llu,\"overflow\":%llu,"
                          "\"cpu_front_violations\":%llu,\"passed\":%s}\n",
                          generations.size(),timing_percentile(generations,0.50),
                          generation_p95,generations.empty()?0.0:generations.back(),
                          generation_limit_milliseconds,frames.size(),
                          timing_percentile(frames,0.50),frame_p95,
                          frames.empty()?0.0:frames.back(),frame_limit_milliseconds,
                          gpu_stage_timestamps_enabled?"true":"false",
                          static_cast<unsigned long long>(gpu_terrain_counters->dispatched.load(std::memory_order_acquire)),
                          static_cast<unsigned long long>(gpu_terrain_counters->accepted.load(std::memory_order_acquire)),
                          static_cast<unsigned long long>(gpu_terrain_counters->failed.load(std::memory_order_acquire)),
                          static_cast<unsigned long long>(gpu_terrain_counters->overflow.load(std::memory_order_acquire)),
                          static_cast<unsigned long long>(gpu_terrain_counters->cpu_front_violations.load(std::memory_order_acquire)),
                          passed?"true":"false");
              if(!passed)result=1;
            }else if(timing_profile_test){
              const auto ordered=timing_profile_samples->ordered();
              const auto [ordered_optical,ordered_sky,ordered_irradiance,
                          ordered_aerial,ordered_screen_integration]=
                  timing_profile_samples->ordered_lookups();
              const bool all_positive=!ordered.empty()&&
                  ordered.front()>0.0&&std::isfinite(ordered.back());
              const bool exact_handoff_ready=
                  timing_profile_class!=TimingProfileClass::exact_handoff||
                  timing_profile_exact_handoff_observed;
              const bool ray_trace_ready=
                  timing_profile_class!=TimingProfileClass::ray_tracing||
                  (terrain_acceleration_structure.active!=nil&&
                   terrain_acceleration_structure.build_count!=0U);
              const bool aerial_profile_ready=
                  timing_profile_class!=TimingProfileClass::aerial_refresh||
                  ordered_aerial.size()==300U;
              const bool shadow_profile_ready=
                  timing_profile_class!=TimingProfileClass::shadow_lookup||
                  ordered_screen_integration.size()==300U;
              const bool passed=ordered.size()==300U&&all_positive&&
                  exact_handoff_ready&&ray_trace_ready&&aerial_profile_ready&&
                  shadow_profile_ready;
              std::printf("{\"event\":\"metal_timing_profile\","
                          "\"class\":\"%s\",\"samples\":%zu,"
                          "\"median_ms\":%.4f,\"p95_ms\":%.4f,"
                          "\"p99_ms\":%.4f,\"max_ms\":%.4f,"
                          "\"stage_timestamps\":%s,"
                          "\"drawable\":\"%dx%d\",\"internal\":\"%dx%d\","
                          "\"samples_per_pixel\":%lu,\"transport\":%d,"
                          "\"renderer\":%d,\"metalfx\":%s,"
                          "\"sky_view\":\"%lux%lu\","
                          "\"preview\":%s,\"exact_handoff\":%s,"
                          "\"optical_lookup_samples\":%zu,"
                          "\"optical_lookup_ms\":%.4f,"
                          "\"optical_lookup_p95_ms\":%.4f,"
                          "\"lookup_samples\":%zu,"
                          "\"sky_view_lookup_ms\":%.4f,"
                          "\"irradiance_lookup_ms\":%.4f,"
                          "\"sky_view_lookup_p95_ms\":%.4f,"
                          "\"irradiance_lookup_p95_ms\":%.4f,"
                          "\"aerial_lookup_samples\":%zu,"
                          "\"aerial_lookup_ms\":%.4f,"
                          "\"aerial_lookup_p95_ms\":%.4f,"
                          "\"screen_integration_samples\":%zu,"
                          "\"screen_integration_ms\":%.4f,"
                          "\"screen_integration_p95_ms\":%.4f,"
                          "\"rt_builds\":%llu,\"sky_view_dispatches\":%llu,"
                          "\"irradiance_dispatches\":%llu,\"passed\":%s}\n",
                          timing_profile_class_name,ordered.size(),
                          timing_percentile(ordered,0.50),
                          timing_percentile(ordered,0.95),
                          timing_percentile(ordered,0.99),ordered.back(),
                          gpu_stage_timestamps_enabled?"true":"false",
                          width,height,render_width,render_height,
                          static_cast<unsigned long>(allocated_samples),
                          atmosphere_transport,atmosphere_renderer,
                          metalfx_temporal_active?"true":"false",
                          static_cast<unsigned long>(atmosphere_resources.sky_view.width),
                          static_cast<unsigned long>(atmosphere_resources.sky_view.height),
                          preview_enabled?"true":"false",
                          exact_handoff_ready?"true":"false",
                          ordered_optical.size(),
                          timing_percentile(ordered_optical,0.50),
                          timing_percentile(ordered_optical,0.95),
                          ordered_sky.size(),
                          timing_percentile(ordered_sky,0.50),
                          timing_percentile(ordered_irradiance,0.50),
                          timing_percentile(ordered_sky,0.95),
                          timing_percentile(ordered_irradiance,0.95),
                          ordered_aerial.size(),
                          timing_percentile(ordered_aerial,0.50),
                          timing_percentile(ordered_aerial,0.95),
                          ordered_screen_integration.size(),
                          timing_percentile(ordered_screen_integration,0.50),
                          timing_percentile(ordered_screen_integration,0.95),
                          static_cast<unsigned long long>(
                              terrain_acceleration_structure.build_count),
                          static_cast<unsigned long long>(
                              atmosphere_resources.dispatch_counts[2]),
                          static_cast<unsigned long long>(
                              atmosphere_resources.dispatch_counts[4]),
                          passed?"true":"false");
              if(!passed)result=1;
            }else if(auto_resolution_test){
              const bool passed=automatic_gpu_median_milliseconds>0.0&&
                  automatic_gpu_percentile_95_milliseconds>0.0&&
                  (automatic_render_scale>0.5F||
                   profile_interactive_rendering)&&render_width<=width&&
                  render_height<=height&&
                  (profile_interactive_rendering||automatic_quality_changes!=0U);
              std::printf("{\"event\":\"metal_auto_resolution_smoke\","
                          "\"rendered_frames\":%zu,\"display_hz\":%d,"
                          "\"scale\":%.2f,\"internal\":\"%dx%d\","
                          "\"median_ms\":%.4f,\"p95_ms\":%.4f,"
                          "\"profile_interactive\":%s,"
                          "\"shadow_ms\":%.4f,\"atmosphere_ms\":%.4f,"
                          "\"terrain_ms\":%.4f,\"composite_ms\":%.4f,"
                          "\"samples\":%lu,\"view_lookup_dispatches\":%llu,"
                          "\"irradiance_lookup_dispatches\":%llu,"
                          "\"aerial_dispatches\":%llu,"
                          "\"long_shadow_dispatches\":%llu,"
                          "\"stable_frames\":%zu,\"quality_profile\":%zu,"
                          "\"quality_changes\":%llu,\"last_change\":\"%s\","
                          "\"passed\":%s}\n",
                          auto_resolution_test_frames,display_refresh_hz,
                          automatic_render_scale,render_width,render_height,
                          automatic_gpu_median_milliseconds,
                          automatic_gpu_percentile_95_milliseconds,
                          profile_interactive_rendering?"true":"false",
                          gpu_stage_timings->shadows_milliseconds.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->atmosphere_milliseconds.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->terrain_milliseconds.load(
                              std::memory_order_relaxed),
                          gpu_stage_timings->composite_milliseconds.load(
                              std::memory_order_relaxed),
                          static_cast<unsigned long>(allocated_samples),
                          static_cast<unsigned long long>(
                              atmosphere_resources.dispatch_counts[2]),
                          static_cast<unsigned long long>(
                              atmosphere_resources.dispatch_counts[4]),
                          static_cast<unsigned long long>(
                              atmosphere_resources.dispatch_counts[3]),
                          static_cast<unsigned long long>(
                              atmosphere_resources.dispatch_counts[6]),
                          automatic_stable_frames,
                          automatic_quality_controller.profile_index(),
                          static_cast<unsigned long long>(automatic_quality_changes),
                          automatic_last_change==tetra_viewer::MetalQualityChange::upgrade?
                              "upgrade":(automatic_last_change==
                              tetra_viewer::MetalQualityChange::downgrade?
                                  "downgrade":"none"),
                          passed?"true":"false");
              if(!passed)result=1;
            }else if(atmosphere_quality_test){
              const bool passed=atmosphere_quality_switches_ok&&
                  low_atmosphere_allocation!=0U&&
                  low_atmosphere_allocation<default_atmosphere_allocation&&
                  default_atmosphere_allocation<high_atmosphere_allocation&&
                  atmosphere_quality_index==1&&
                  live_atmosphere_resources_valid(atmosphere_resources);
              std::printf("{\"event\":\"metal_atmosphere_quality_smoke\","
                          "\"rendered_frames\":%zu,\"low_bytes\":%zu,"
                          "\"default_bytes\":%zu,\"high_bytes\":%zu,"
                          "\"passed\":%s}\n",
                          atmosphere_quality_test_frames,
                          low_atmosphere_allocation,
                          default_atmosphere_allocation,
                          high_atmosphere_allocation,
                          passed?"true":"false");
              if(!passed)result=1;
            }else if(soak_test){
              const auto samples=timing_profile_samples->ordered();
              const bool passed=soak_rendered_frames>=soak_simulated_frames&&
                  diagnostics.converged&&!diagnostics.busy&&samples.size()==300U&&
                  atmosphere_resources.temporal_history_attempts>1U&&
                  atmosphere_resources.temporal_history_compatible!=0U;
              std::printf("{\"event\":\"metal_soak_smoke\","
                          "\"simulated_seconds\":300,\"rendered_frames\":%zu,"
                          "\"median_ms\":%.4f,\"p95_ms\":%.4f,\"p99_ms\":%.4f,"
                          "\"history_attempts\":%llu,\"history_compatible\":%llu,"
                          "\"history_invalidations\":%llu,\"settled\":%s,"
                          "\"passed\":%s}\n",
                          soak_rendered_frames,timing_percentile(samples,0.50),
                          timing_percentile(samples,0.95),timing_percentile(samples,0.99),
                          static_cast<unsigned long long>(atmosphere_resources.temporal_history_attempts),
                          static_cast<unsigned long long>(atmosphere_resources.temporal_history_compatible),
                          static_cast<unsigned long long>(atmosphere_resources.temporal_history_invalidations),
                          diagnostics.converged&&!diagnostics.busy?"true":"false",
                          passed?"true":"false");
              if(!passed)result=1;
            }else if(motion_test){
              const auto delta=controller.state().feet-motion_start;
              const double distance=std::sqrt(
                  delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
              const auto published_delta=
                  diagnostics.published_camera_position-camera.position;
              const double published_distance=std::sqrt(
                  published_delta.x*published_delta.x+
                  published_delta.y*published_delta.y+
                  published_delta.z*published_delta.z);
              const auto dispatched=gpu_terrain_counters->dispatched.load(
                  std::memory_order_acquire);
              const auto completed=gpu_terrain_counters->completed.load(
                  std::memory_order_acquire);
              const auto accepted=gpu_terrain_counters->accepted.load(
                  std::memory_order_acquire);
              const auto stale_rejected=gpu_terrain_counters->stale_rejected.load(
                  std::memory_order_acquire);
              const auto failed=gpu_terrain_counters->failed.load(
                  std::memory_order_acquire);
              const auto overflow=gpu_terrain_counters->overflow.load(
                  std::memory_order_acquire);
              const auto cpu_front_frames=gpu_terrain_counters->cpu_front_frames.load(
                  std::memory_order_acquire);
              const auto cpu_front_violations=
                  gpu_terrain_counters->cpu_front_violations.load(
                      std::memory_order_acquire);
              const auto device_closures=
                  gpu_terrain_counters->device_closure_submitted.load(
                      std::memory_order_acquire);
              const auto p6_requests=
                  gpu_terrain_counters->p6_requests.load(std::memory_order_acquire);
              const auto cpu_surface_build_requests=
                  gpu_terrain_counters->cpu_surface_build_requests.load(
                      std::memory_order_acquire);
              const auto immutable_snapshot_builds=
                  gpu_terrain_counters->immutable_snapshot_builds.load(
                      std::memory_order_acquire);
              const auto candidate_payload_readbacks=
                  gpu_terrain_counters->candidate_payload_readback_requests.load(
                      std::memory_order_acquire);
              const auto post_bootstrap_seeds=
                  gpu_terrain_counters->post_bootstrap_seed_attempts.load(
                      std::memory_order_acquire);
              const auto device_front_display_promotions=
                  gpu_terrain_counters->device_front_display_promotions.load(
                      std::memory_order_acquire);
              const auto device_front_display_frames=
                  gpu_terrain_counters->device_front_display_frames.load(
                      std::memory_order_acquire);
              const auto device_front_display_binding_violations=
                  gpu_terrain_counters->device_front_display_binding_violations.load(
                      std::memory_order_acquire);
              const auto device_front_bootstrap_fallback_frames=
                  gpu_terrain_counters->device_front_bootstrap_fallback_frames.load(
                      std::memory_order_acquire);
              const auto compact_p8_encoded=
                  gpu_hierarchy_live_selection.compact_p8_encoded;
              const auto compact_p8_completed=
                  gpu_hierarchy_live_selection.compact_p8_completed;
              const auto compact_p8_commits=
                  gpu_hierarchy_live_selection.compact_p8_private_commits;
              const auto compact_p8_rejected=
                  gpu_hierarchy_live_selection.compact_p8_rejected;
              const auto device_front_phase=
                  metal_gpu_hierarchy_device_front_phase(
                      gpu_hierarchy_live_selection);
              const auto stage_milliseconds=[](
                  std::chrono::steady_clock::time_point begin,
                  std::chrono::steady_clock::time_point end){
                if(begin.time_since_epoch().count()==0||
                   end.time_since_epoch().count()==0)return -1.0;
                return std::chrono::duration<double,std::milli>(end-begin).count();
              };
              const auto bootstrap_to_selector_ms=stage_milliseconds(
                  gpu_hierarchy_live_selection.device_front_bootstrap_at,
                  gpu_hierarchy_live_selection.device_front_selector_at);
              const auto selector_to_closure_ms=stage_milliseconds(
                  gpu_hierarchy_live_selection.device_front_selector_at,
                  gpu_hierarchy_live_selection.device_front_closure_at);
              const auto closure_to_materializer_ms=stage_milliseconds(
                  gpu_hierarchy_live_selection.device_front_closure_at,
                  gpu_hierarchy_live_selection.device_front_materializer_at);
              const auto materializer_to_p8_ms=stage_milliseconds(
                  gpu_hierarchy_live_selection.device_front_materializer_at,
                  gpu_hierarchy_live_selection.device_front_p8_at);
              const auto p8_completion_ms=
                  gpu_hierarchy_live_selection.device_front_last_p8_completion_milliseconds;
              const auto cpu_bootstrap_vertices=
                  device_front_bootstrap_display.exact_vertex_count;
              const auto gpu_p8_vertices=
                  gpu_hierarchy_live_selection.compact_p8_last_audit[2U];
              const bool complete_front_vertex_parity=
                  !metal_gpu_terrain_device_front||
                  (cpu_bootstrap_vertices!=0U&&gpu_p8_vertices==cpu_bootstrap_vertices);
              const bool gpu_slots_passed=!(metal_gpu_terrain_diagnostic||
                  metal_gpu_terrain_native_diagnostic)||
                  (dispatched!=0U&&completed!=0U&&accepted!=0U&&
                   stale_rejected!=0U&&failed==0U&&overflow==0U&&
                   cpu_front_frames!=0U&&cpu_front_violations==0U);
              const bool gpu_live_selection_passed=
                  !metal_gpu_terrain_live_selection||
                  (gpu_hierarchy_live_selection.submitted>=2U&&
                   gpu_hierarchy_live_selection.completed>=2U&&
                   gpu_hierarchy_live_selection.accepted>=2U&&
                   gpu_hierarchy_live_selection.failed==0U&&
                   gpu_hierarchy_live_selection.cpu_generation_violations==0U);
              const bool gpu_compact_closure_passed=!metal_gpu_terrain_device_front||
                  (device_closures>=2U&&
                  gpu_hierarchy_live_selection.compact_closure_encoded>=2U&&
                  (metal_gpu_terrain_device_front_inject_green_budget_failure?
                   gpu_hierarchy_live_selection.compact_red_encoded==0U:
                   gpu_hierarchy_live_selection.compact_red_encoded>=2U)&&
                  (metal_gpu_terrain_device_front_inject_green_budget_failure?
                   gpu_hierarchy_live_selection.compact_closure_rejected>=2U:
                   gpu_hierarchy_live_selection.compact_closure_completed>=2U)&&
                  (metal_gpu_terrain_device_front_inject_green_budget_failure?
                   gpu_hierarchy_live_selection.compact_closure_rejected>=1U:
                   gpu_hierarchy_live_selection.compact_quiescent>=1U)&&
                  compact_p8_encoded>=2U&&compact_p8_completed>=2U&&
                  p6_requests==0U&&cpu_surface_build_requests==0U&&
                  immutable_snapshot_builds==1U&&
                  candidate_payload_readbacks==0U&&post_bootstrap_seeds==0U&&
                   (metal_gpu_terrain_device_front_inject_green_budget_failure?
                    (gpu_hierarchy_live_selection.compact_closure_rejected>=1U&&
                     compact_p8_commits==0U&&compact_p8_rejected>=1U):
                    (gpu_hierarchy_live_selection.compact_closure_rejected==0U&&
                     compact_p8_commits>=1U&&compact_p8_rejected==0U))&&
                  (metal_gpu_terrain_device_front_inject_green_budget_failure?
                   (device_front_bootstrap_fallback_frames!=0U&&
                    device_front_display_binding_violations==0U):
                   (device_front_display_promotions!=0U&&
                    device_front_display_frames!=0U&&
                    device_front_display_binding_violations==0U))&&
                   complete_front_vertex_parity&&
                   failed==0U&&overflow==0U&&cpu_front_violations==0U);
              const bool passed=distance>0.001&&
                  (metal_gpu_terrain_live_selection||published_distance<1.0e-8)&&
                  diagnostics.converged&&!diagnostics.busy&&
                  !runtime_camera_interactive&&gpu_slots_passed&&
                  gpu_live_selection_passed&&gpu_compact_closure_passed&&
                  (!metal_gpu_terrain_private_front_qualification||
                   (gpu_terrain_renderer_available&&
                    gpu_terrain_active_front.promoted&&
                    terrain_display_front.exact_indirect_arguments==
                        gpu_terrain_active_front.indirect_arguments&&
                    cpu_front_violations==0U));
              std::printf("{\"event\":\"metal_motion_smoke\","
                          "\"rendered_frames\":%zu,\"distance\":%.8f,"
                          "\"published_pose_error\":%.12f,"
                          "\"settled\":true,\"triangles\":%zu,"
                          "\"gpu_slots\":{\"dispatched\":%llu,"
                          "\"completed\":%llu,\"accepted\":%llu,"
                          "\"stale_rejected\":%llu,\"failed\":%llu,"
                          "\"overflow\":%llu,\"cpu_front_frames\":%llu,"
                          "\"cpu_front_violations\":%llu},"
                          "\"gpu_live_selection\":{\"enabled\":%s,"
                          "\"submitted\":%llu,\"completed\":%llu,"
                          "\"accepted\":%llu,\"failed\":%llu,"
                          "\"cpu_generation_violations\":%llu},"
                          "\"gpu_compact_closure\":{\"enabled\":%s,\"closures\":%llu,"
                          "\"compact_encoded\":%llu,\"compact_completed\":%llu,"
                          "\"direct_red_closure\":%llu,\"compact_quiescent\":%llu,"
                          "\"compact_rejected\":%llu,"
                          "\"p8_owner_materialization\":%s,\"p8_encoded\":%llu,"
                          "\"p8_completed\":%llu,\"p8_rejected\":%llu,"
                          "\"p8_private_commit\":%s,"
                          "\"device_front_default_selected\":%s,"
                          "\"complete_front_vertices\":{\"cpu_bootstrap\":%zu,"
                          "\"gpu_p8\":%u,\"exact\":%s},"
                          "\"display_front\":{\"promotions\":%llu,"
                          "\"private_frames\":%llu,"
                          "\"bootstrap_fallback_frames\":%llu,"
                          "\"binding_violations\":%llu,"
                          "\"private_vertices_bound\":%s,"
                          "\"private_indirect_bound\":%s,"
                          "\"indexed_exact_selection\":%s,"
                          "\"bootstrap_vertices_retained\":%s,"
                          "\"bootstrap_indirect_retained\":%s},"
                          "\"device_front_progress\":{\"phase\":\"%s\","
                          "\"bootstrap_to_selector_ms\":%.3f,"
                          "\"selector_to_closure_ms\":%.3f,"
                          "\"closure_to_materializer_ms\":%.3f,"
                          "\"materializer_to_p8_ms\":%.3f,"
                          "\"p8_completion_ms\":%.3f},"
                          "\"p6_requests\":%llu,"
                          "\"cpu_surface_build_requests\":%llu,"
                          "\"immutable_snapshot_builds\":%llu,"
                          "\"candidate_payload_readbacks\":%llu,"
                          "\"post_bootstrap_seed_attempts\":%llu,"
                          "\"indirect_zero_grids\":%llu},"
                          "\"passed\":%s}\n",
                          motion_rendered_frames,distance,
                          published_distance,scene_vertex_count/3U,
                          static_cast<unsigned long long>(dispatched),
                          static_cast<unsigned long long>(completed),
                          static_cast<unsigned long long>(accepted),
                          static_cast<unsigned long long>(stale_rejected),
                          static_cast<unsigned long long>(failed),
                          static_cast<unsigned long long>(overflow),
                          static_cast<unsigned long long>(cpu_front_frames),
                          static_cast<unsigned long long>(cpu_front_violations),
                          metal_gpu_terrain_live_selection?"true":"false",
                          static_cast<unsigned long long>(
                              gpu_hierarchy_live_selection.submitted),
                          static_cast<unsigned long long>(
                              gpu_hierarchy_live_selection.completed),
                          static_cast<unsigned long long>(
                              gpu_hierarchy_live_selection.accepted),
                          static_cast<unsigned long long>(
                              gpu_hierarchy_live_selection.failed),
                          static_cast<unsigned long long>(
                              gpu_hierarchy_live_selection.cpu_generation_violations),
                          metal_gpu_terrain_device_front?"true":"false",
                          static_cast<unsigned long long>(device_closures),
                          static_cast<unsigned long long>(
                              gpu_hierarchy_live_selection.compact_closure_encoded),
                          static_cast<unsigned long long>(
                              gpu_hierarchy_live_selection.compact_closure_completed),
                          static_cast<unsigned long long>(
                              gpu_hierarchy_live_selection.compact_red_encoded),
                          static_cast<unsigned long long>(
                              gpu_hierarchy_live_selection.compact_quiescent),
                          static_cast<unsigned long long>(
                              gpu_hierarchy_live_selection.compact_closure_rejected),
                          compact_p8_encoded!=0U?"true":"false",
                          static_cast<unsigned long long>(compact_p8_encoded),
                          static_cast<unsigned long long>(compact_p8_completed),
                          static_cast<unsigned long long>(compact_p8_rejected),
                          compact_p8_commits!=0U?"true":"false",
                          (!metal_gpu_terrain_device_front_explicit&&
                           device_front_default_test)?"true":"false",
                          cpu_bootstrap_vertices,gpu_p8_vertices,
                          complete_front_vertex_parity?"true":"false",
                          static_cast<unsigned long long>(
                              device_front_display_promotions),
                          static_cast<unsigned long long>(
                              device_front_display_frames),
                          static_cast<unsigned long long>(
                              device_front_bootstrap_fallback_frames),
                          static_cast<unsigned long long>(
                              device_front_display_binding_violations),
                          terrain_display_front.exact_vertices==
                              gpu_terrain_active_front.vertices?"true":"false",
                          terrain_display_front.exact_indirect_arguments==
                              gpu_terrain_active_front.indirect_arguments?"true":"false",
                          terrain_display_front.indexed_exact_selection?"true":"false",
                          terrain_display_front.exact_vertices==
                              device_front_bootstrap_vertices?"true":"false",
                          terrain_display_front.exact_indirect_arguments==
                              device_front_bootstrap_indirect_arguments?"true":"false",
                          device_front_phase,bootstrap_to_selector_ms,
                          selector_to_closure_ms,closure_to_materializer_ms,
                          materializer_to_p8_ms,p8_completion_ms,
                          static_cast<unsigned long long>(p6_requests),
                          static_cast<unsigned long long>(cpu_surface_build_requests),
                          static_cast<unsigned long long>(immutable_snapshot_builds),
                          static_cast<unsigned long long>(candidate_payload_readbacks),
                          static_cast<unsigned long long>(post_bootstrap_seeds),
                          static_cast<unsigned long long>(
                              gpu_hierarchy_live_selection.indirect_zero_grid_observations),
                          passed?"true":"false");
              if(!passed)result=1;
            }else{
              const auto dispatched=gpu_terrain_counters->dispatched.load(
                  std::memory_order_acquire);
              const auto completed=gpu_terrain_counters->completed.load(
                  std::memory_order_acquire);
              const auto accepted=gpu_terrain_counters->accepted.load(
                  std::memory_order_acquire);
              const auto failed=gpu_terrain_counters->failed.load(
                  std::memory_order_acquire);
              const auto overflow=gpu_terrain_counters->overflow.load(
                  std::memory_order_acquire);
              const auto cpu_front_violations=
                  gpu_terrain_counters->cpu_front_violations.load(
                      std::memory_order_acquire);
              std::printf("{\"event\":\"metal_smoke\",\"device\":\"%s\","
                          "\"scene_generation\":%llu,\"triangles\":%zu,"
                          "\"gpu_renderer_requested\":%s,"
                          "\"gpu_renderer_available\":%s,"
                          "\"gpu_live_selection\":{\"enabled\":%s,"
                          "\"submitted\":%llu,\"completed\":%llu,"
                          "\"accepted\":%llu,\"failed\":%llu,"
                          "\"cpu_generation_violations\":%llu},"
                          "\"gpu_slots\":{\"dispatched\":%llu,"
                          "\"completed\":%llu,\"accepted\":%llu,"
                          "\"failed\":%llu,\"overflow\":%llu,"
                          "\"cpu_front_violations\":%llu}}\n",
                          device.name.UTF8String,
                          static_cast<unsigned long long>(uploaded_generation),
                          scene_vertex_count/3U,
                          gpu_terrain_renderer_selected?"true":"false",
                          gpu_terrain_renderer_available?"true":"false",
                          metal_gpu_terrain_live_selection?"true":"false",
                          static_cast<unsigned long long>(
                              gpu_hierarchy_live_selection.submitted),
                          static_cast<unsigned long long>(
                              gpu_hierarchy_live_selection.completed),
                          static_cast<unsigned long long>(
                              gpu_hierarchy_live_selection.accepted),
                          static_cast<unsigned long long>(
                              gpu_hierarchy_live_selection.failed),
                          static_cast<unsigned long long>(
                              gpu_hierarchy_live_selection.cpu_generation_violations),
                          static_cast<unsigned long long>(dispatched),
                          static_cast<unsigned long long>(completed),
                          static_cast<unsigned long long>(accepted),
                          static_cast<unsigned long long>(failed),
                          static_cast<unsigned long long>(overflow),
                          static_cast<unsigned long long>(cpu_front_violations));
            }
          }else{
            std::fprintf(stderr,"Metal smoke frame failed: %s\n",
                         command_buffer.error.localizedDescription.UTF8String);
            result=1;
          }
          glfwSetWindowShouldClose(window,GLFW_TRUE);
        }else if(automated_test&&now>=smoke_deadline){
          const auto p8_started=gpu_hierarchy_live_selection.device_front_p8_at;
          const auto p8_age_milliseconds=p8_started.time_since_epoch().count()==0?
              -1.0:std::chrono::duration<double,std::milli>(now-p8_started).count();
          std::fprintf(stderr,
              "Metal automated test timed out waiting for terrain "
              "(scene=%llu requested_view=%llu published_view=%llu "
              "submitted=%zu canceled=%zu budget_exceeded=%s "
              "rejected_cpu=%zu rejected_triangles=%zu rejected_work=%zu "
              "rejected_upload=%zu rejected_hierarchy=%zu rejected_volume=%zu "
              "busy=%s converged=%s interactive=%s motion_frames=%zu "
              "gpu_dispatched=%llu gpu_completed=%llu gpu_accepted=%llu "
              "gpu_stale=%llu gpu_failed=%llu gpu_overflow=%llu "
              "gpu_cpu_front_violations=%llu gpu_available=%s "
              "device_front_phase=%s bootstrap=%s selector=%s closure=%s "
              "materializer=%s p8_encoded=%llu p8_completed=%llu "
              "p8_commits=%llu p8_rejected=%llu p8_audit=%u/%u/%u/%u/%u/%u "
              "owner_header=%u/%u/%u/%u "
              "selected_header=%u/%u/%u/%u final_active_header=%u/%u/%u/%u "
              "final_masks_header=%u/%u/%u/%u "
              "closure_audit=%u/%u/%u/%u/%u red_status=%u/%u/%u/%u "
              "p8_age_ms=%.3f).\n",
              static_cast<unsigned long long>(diagnostics.scene_generation),
              static_cast<unsigned long long>(
                  diagnostics.exact_requested_view_epoch),
              static_cast<unsigned long long>(
                  diagnostics.exact_published_view_epoch),
              diagnostics.submitted_builds,diagnostics.canceled_builds,
              diagnostics.budget_exceeded?"true":"false",
              diagnostics.rejected_proposed_cpu_bytes,
              diagnostics.rejected_proposed_triangles,
              diagnostics.rejected_proposed_work_units,
              diagnostics.rejected_proposed_upload_bytes,
              diagnostics.rejected_proposed_hierarchy_blocks,
              diagnostics.rejected_proposed_volume_blocks,
              diagnostics.busy?"true":"false",
              diagnostics.converged?"true":"false",
              runtime_camera_interactive?"true":"false",motion_rendered_frames,
              static_cast<unsigned long long>(gpu_terrain_counters->dispatched.load(
                  std::memory_order_acquire)),
              static_cast<unsigned long long>(gpu_terrain_counters->completed.load(
                  std::memory_order_acquire)),
              static_cast<unsigned long long>(gpu_terrain_counters->accepted.load(
                  std::memory_order_acquire)),
              static_cast<unsigned long long>(gpu_terrain_counters->stale_rejected.load(
                  std::memory_order_acquire)),
              static_cast<unsigned long long>(gpu_terrain_counters->failed.load(
                  std::memory_order_acquire)),
              static_cast<unsigned long long>(gpu_terrain_counters->overflow.load(
                  std::memory_order_acquire)),
              static_cast<unsigned long long>(gpu_terrain_counters->cpu_front_violations.load(
                  std::memory_order_acquire)),
              gpu_terrain_renderer_available?"true":"false",
              metal_gpu_hierarchy_device_front_phase(gpu_hierarchy_live_selection),
              gpu_hierarchy_live_selection.device_front_bootstrap_at.time_since_epoch().count()!=0?
                  "ready":"pending",
              gpu_hierarchy_live_selection.device_front_selector_at.time_since_epoch().count()!=0?
                  "encoded":"pending",
              gpu_hierarchy_live_selection.device_front_closure_at.time_since_epoch().count()!=0?
                  "encoded":"pending",
              gpu_hierarchy_live_selection.device_front_materializer_at.time_since_epoch().count()!=0?
                  "encoded":"pending",
              static_cast<unsigned long long>(gpu_hierarchy_live_selection.compact_p8_encoded),
              static_cast<unsigned long long>(gpu_hierarchy_live_selection.compact_p8_completed),
              static_cast<unsigned long long>(gpu_hierarchy_live_selection.compact_p8_private_commits),
              static_cast<unsigned long long>(gpu_hierarchy_live_selection.compact_p8_rejected),
              gpu_hierarchy_live_selection.compact_p8_last_audit[0U],
              gpu_hierarchy_live_selection.compact_p8_last_audit[1U],
              gpu_hierarchy_live_selection.compact_p8_last_audit[2U],
              gpu_hierarchy_live_selection.compact_p8_last_audit[3U],
              gpu_hierarchy_live_selection.compact_p8_last_audit[4U],
              gpu_hierarchy_live_selection.compact_p8_last_audit[5U],
              gpu_hierarchy_live_selection.compact_p8_last_owner_header[0U],
              gpu_hierarchy_live_selection.compact_p8_last_owner_header[1U],
              gpu_hierarchy_live_selection.compact_p8_last_owner_header[2U],
              gpu_hierarchy_live_selection.compact_p8_last_owner_header[3U],
              gpu_hierarchy_live_selection.compact_last_selected_header[0U],
              gpu_hierarchy_live_selection.compact_last_selected_header[1U],
              gpu_hierarchy_live_selection.compact_last_selected_header[2U],
              gpu_hierarchy_live_selection.compact_last_selected_header[3U],
              gpu_hierarchy_live_selection.compact_last_final_active_header[0U],
              gpu_hierarchy_live_selection.compact_last_final_active_header[1U],
              gpu_hierarchy_live_selection.compact_last_final_active_header[2U],
              gpu_hierarchy_live_selection.compact_last_final_active_header[3U],
              gpu_hierarchy_live_selection.compact_last_final_masks_header[0U],
              gpu_hierarchy_live_selection.compact_last_final_masks_header[1U],
              gpu_hierarchy_live_selection.compact_last_final_masks_header[2U],
              gpu_hierarchy_live_selection.compact_last_final_masks_header[3U],
              gpu_hierarchy_live_selection.compact_last_closure_audit[0U],
              gpu_hierarchy_live_selection.compact_last_closure_audit[1U],
              gpu_hierarchy_live_selection.compact_last_closure_audit[2U],
              gpu_hierarchy_live_selection.compact_last_closure_audit[3U],
              gpu_hierarchy_live_selection.compact_last_closure_audit[4U],
              gpu_hierarchy_live_selection.compact_last_closure_audit[5U],
              gpu_hierarchy_live_selection.compact_last_closure_audit[6U],
              gpu_hierarchy_live_selection.compact_last_closure_audit[7U],
              gpu_hierarchy_live_selection.compact_last_closure_audit[8U],
              p8_age_milliseconds);
          result=1;
          glfwSetWindowShouldClose(window,GLFW_TRUE);
        }
      }
    }

    if(runtime_startup.valid())runtime_startup.wait();
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
  }
  return result;
}
