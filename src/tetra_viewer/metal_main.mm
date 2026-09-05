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
#import <MetalFX/MetalFX.h>
#import <QuartzCore/CAMetalLayer.h>
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
  bool indexed_exact_selection{};
  std::size_t exact_vertex_count{};
  std::size_t exact_index_count{};
  std::size_t preview_vertex_count{};
  std::size_t preview_index_count{};
  std::size_t upload_bytes{};
  std::uint64_t render_generation{};

  [[nodiscard]] bool ready() const noexcept {
    const bool exact_ready=exact_vertices!=nil&&exact_vertex_count!=0U;
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
  if(hierarchy==nil||children==nil||inputs==nil)return false;
  id<MTLCommandQueue> queue=[device newCommandQueue];
  if(queue==nil)return false;

  constexpr std::uint32_t output_capacity=65536U;
  const auto make_tuple=[](float edge,float field,float limb){
    tetra::GpuHierarchySelectionTupleParameters p;
    p.camera.position={0.5,0.5,3.0};p.camera.forward={0.0,0.0,-1.0};
    p.camera.up={0.0,1.0,0.0};p.camera.viewport_height_pixels=800.0;
    p.camera.aspect_ratio=1.0;p.render_origin={};p.field_centre={0.5,0.5,0.5};
    p.planet_radius=2.0;p.terrain_height_bound=0.1;p.field_lipschitz=1.0;
    p.edge_threshold=edge;p.field_threshold=field;p.limb_threshold=limb;
    p.merge_ratio=0.5;p.source_revision=41U;p.field_revision=43U;
    return tetra::make_gpu_hierarchy_selection_tuple(p);
  };
  struct SelectorCase {
    const char* name;
    float edge,field,limb;
    std::uint32_t capacity;
  };
  constexpr std::array cases{
      SelectorCase{"coarse",1.0e6F,1.0e6F,1.0e6F,output_capacity},
      SelectorCase{"edge",0.5F,1.0e6F,1.0e6F,output_capacity},
      SelectorCase{"field",1.0e6F,0.05F,1.0e6F,output_capacity},
      SelectorCase{"limb",1.0e6F,1.0e6F,0.02F,output_capacity},
      // A full selector result with zero writable entries is the hardware
      // overflow/fail-closed contract; the interactive Metal renderer still
      // draws its CPU front because this diagnostic can never be promoted.
      SelectorCase{"overflow",1.0e6F,1.0e6F,1.0e6F,0U}};
  std::size_t completed{};
  for(const auto& selector_case:cases){
    const auto tuple=make_tuple(selector_case.edge,selector_case.field,
                                selector_case.limb);
    const auto oracle=tetra::gpu_hierarchy_traverse(snapshot,
        tetra::gpu_hierarchy_traversal_parameters(tuple));
    if(oracle.selected_records.size()>output_capacity)return false;
    id<MTLBuffer> tuple_buffer=make_buffer(&tuple,sizeof(tuple));
    std::vector<std::uint32_t> zeroed(4U+selector_case.capacity,0U);
    id<MTLBuffer> output=make_buffer(zeroed.data(),
        zeroed.size()*sizeof(zeroed.front()));
    const std::array<std::uint32_t,2> parameters{
        static_cast<std::uint32_t>(snapshot.records.size()),selector_case.capacity};
    if(tuple_buffer==nil||output==nil)return false;
    id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:hierarchy offset:0U atIndex:0U];
    [encoder setBuffer:children offset:0U atIndex:1U];
    [encoder setBuffer:inputs offset:0U atIndex:2U];
    [encoder setBuffer:tuple_buffer offset:0U atIndex:3U];
    [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:4U];
    [encoder setBuffer:output offset:0U atIndex:5U];
    [encoder dispatchThreads:MTLSizeMake(snapshot.records.size(),1U,1U)
         threadsPerThreadgroup:MTLSizeMake(1U,1U,1U)];
    [encoder endEncoding];[command commit];[command waitUntilCompleted];
    if(command.status!=MTLCommandBufferStatusCompleted)return false;
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
    ++completed;
  }
  std::printf("{\"event\":\"metal_gpu_lod_selector\","
              "\"cases\":%zu,\"records\":%zu,\"passed\":true}\n",
              completed,snapshot.records.size());
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
constexpr NSUInteger gpu_timestamp_count=25U;
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
  const bool gpu_terrain_extract_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-terrain-extract-smoke-test")==0;
  const bool gpu_terrain_runtime_smoke_test=argc==2&&
      std::strcmp(argv[1],"--metal-gpu-terrain-runtime-smoke-test")==0;
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
  const bool motion_test=argc==2&&
      std::strcmp(argv[1],"--metal-motion-smoke-test")==0;
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
  if(argc>1&&!device_check&&!ray_visibility_smoke_test&&!terrain_ray_oracle_test&&!atmosphere_compiler_check&&!gpu_lod_selector_smoke_test&&!gpu_terrain_extract_smoke_test&&!gpu_terrain_runtime_smoke_test&&
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
                        "--metal-gpu-terrain-extract-smoke-test|"
                        "--metal-gpu-terrain-runtime-smoke-test|"
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
    if(gpu_terrain_extract_smoke_test)
      return run_metal_gpu_terrain_extract_smoke_test(device)?0:1;
    if(gpu_terrain_runtime_smoke_test)
      return run_metal_gpu_terrain_runtime_smoke_test(device)?0:1;
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
                                        ((smoke_test||motion_test||render_test||metalfx_test||
                                          overlay_test||shadow_test||
                                          any_atmosphere_frame_test||soak_test)?
                                             (interactive_capture_resolution?1440:960):1440),
                                        capture_test?480:
                                        ((smoke_test||motion_test||render_test||metalfx_test||
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
    const bool gpu_stage_timestamps_enabled=[] {
      const char* value=std::getenv("TETWORLD_METAL_STAGE_TIMESTAMPS");
      return value!=nullptr&&std::strcmp(value,"0")!=0;
    }();
    // Diagnostic-only serialization gives a stage study one valid counter
    // flight per rendered frame. It is intentionally opt-in: normal timing
    // profiles retain their production-style asynchronous submission.
    const bool serial_timestamp_profile=timing_profile_test&&
        std::getenv("TETWORLD_METAL_SERIAL_STAGE_TIMESTAMPS")!=nullptr;
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
    const auto smoke_deadline=previous_time+std::chrono::seconds(
        any_atmosphere_frame_test||metalfx_test||timing_profile_test||motion_test||soak_test?300:
        basic_automation_timeout_seconds);
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
        if(((motion_test&&motion_rendered_frames<30U)||
            (timing_profile_test&&
             timing_profile_class==TimingProfileClass::moving&&
             timing_profile_samples->size()<300U))&&
           scene_vertex_count!=0U)
          // This smoke exercises interactive-to-settled publication, not a
          // terrain-detail stress path. Keep its deterministic displacement
          // above the success threshold while remaining inside the published
          // production resource envelope; large travel belongs to the
          // dedicated camera-path benchmark.
          movement.forward=motion_test?0.05:1.0;
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
        }else if(runtime&&!(motion_test&&scene_vertex_count!=0U&&
                             motion_rendered_frames>=30U)){
          const auto previous_feet=controller.state().feet;
          controller.advance(elapsed,movement,runtime->field());
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
        if(runtime){
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
          }else if(motion_test&&scene_vertex_count!=0U&&
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
        bool optical_lookup_encoded_this_frame=false;
        bool reference_lookup_encoded_this_frame=false;
        bool aerial_lookup_encoded_this_frame=false;
        bool reference_screen_integration_encoded_this_frame=false;
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
          [encoder setVertexBuffer:terrain_display_front.exact_vertices
                            offset:0 atIndex:vertex_buffer_index];
          if(terrain_display_front.indexed_exact_selection){
            if(terrain_display_front.exact_index_count!=0U)
              [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                  indexCount:terrain_display_front.exact_index_count
                                   indexType:MTLIndexTypeUInt32
                                 indexBuffer:terrain_display_front.exact_indices
                           indexBufferOffset:0];
          }else [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
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
        const bool timing_profile_sample_eligible=(timing_profile_test||soak_test)&&
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
        if(serial_timestamp_profile)[command_buffer waitUntilCompleted];
        if(motion_test&&scene_vertex_count!=0U)++motion_rendered_frames;
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
            (!render_test||render_test_frames>=40U)&&
            (!metalfx_test||(metalfx_test_frames>=45U&&
                             diagnostics.converged&&!diagnostics.busy))&&
            (!soak_test||(soak_rendered_frames>=soak_simulated_frames&&
                          diagnostics.converged&&!diagnostics.busy))&&
            (!auto_resolution_test||auto_resolution_test_frames>=
                 auto_resolution_required_frames)&&
            (!timing_profile_test||timing_profile_samples->size()>=300U)&&
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
              const bool passed=distance>0.001&&
                  published_distance<1.0e-8&&diagnostics.converged&&
                  !diagnostics.busy&&!runtime_camera_interactive;
              std::printf("{\"event\":\"metal_motion_smoke\","
                          "\"rendered_frames\":%zu,\"distance\":%.8f,"
                          "\"published_pose_error\":%.12f,"
                          "\"settled\":true,\"triangles\":%zu,"
                          "\"passed\":%s}\n",
                          motion_rendered_frames,distance,
                          published_distance,scene_vertex_count/3U,
                          passed?"true":"false");
              if(!passed)result=1;
            }else{
              std::printf("{\"event\":\"metal_smoke\",\"device\":\"%s\","
                          "\"scene_generation\":%llu,\"triangles\":%zu}\n",
                          device.name.UTF8String,
                          static_cast<unsigned long long>(uploaded_generation),
                          scene_vertex_count/3U);
            }
          }else{
            std::fprintf(stderr,"Metal smoke frame failed: %s\n",
                         command_buffer.error.localizedDescription.UTF8String);
            result=1;
          }
          glfwSetWindowShouldClose(window,GLFW_TRUE);
        }else if(automated_test&&now>=smoke_deadline){
          std::fprintf(stderr,
              "Metal automated test timed out waiting for terrain "
              "(scene=%llu requested_view=%llu published_view=%llu "
              "submitted=%zu canceled=%zu budget_exceeded=%s "
              "rejected_cpu=%zu rejected_triangles=%zu rejected_work=%zu "
              "rejected_upload=%zu rejected_hierarchy=%zu rejected_volume=%zu "
              "busy=%s converged=%s interactive=%s "
              "motion_frames=%zu).\n",
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
              runtime_camera_interactive?"true":"false",motion_rendered_frames);
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
