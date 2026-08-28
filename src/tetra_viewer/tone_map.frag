#version 450

layout(location = 0) in vec2 texture_coordinate;
layout(location = 0) out vec4 out_colour;
layout(set = 0,binding = 0) uniform sampler2D hdr_scene;
layout(set = 0,binding = 1) uniform sampler2D scene_depth;
layout(set = 0,binding = 2) uniform sampler2D transmittance_lut;
layout(set = 0,binding = 3) uniform sampler2D multiple_scattering_lut;
layout(set = 0,binding = 4) uniform sampler2D sky_view_lut;
layout(set = 0,binding = 5) uniform sampler3D aerial_scattering_lut;
layout(set = 0,binding = 6) uniform sampler3D aerial_transmittance_lut;

layout(std140,set=0,binding=7) uniform Atmosphere {
  vec4 rayleigh_ground_radius;
  vec4 mie_scattering_top_radius;
  vec4 mie_absorption_rayleigh_scale;
  vec4 absorption_mie_scale;
  vec4 ground_albedo_mie_anisotropy;
  vec4 solar_absorption_peak;
  vec4 profile_and_mode;
  vec4 camera_position_near;
  vec4 camera_right_tangent_x;
  vec4 camera_down_tangent_y;
  vec4 camera_forward_maximum_distance;
  vec4 sun_direction_exposure;
  vec4 reserved0;
  vec4 reserved1;
  vec4 reserved2;
  vec4 reserved3;
} atmosphere;

vec3 aces_fitted(vec3 value) {
  const float a=2.51;
  const float b=0.03;
  const float c=2.43;
  const float d=0.59;
  const float e=0.14;
  return clamp((value*(a*value+b))/(value*(c*value+d)+e),0.0,1.0);
}

vec3 linear_to_srgb(vec3 value) {
  const bvec3 low=lessThanEqual(value,vec3(0.0031308));
  const vec3 lower=value*12.92;
  const vec3 upper=1.055*pow(max(value,vec3(0.0)),vec3(1.0/2.4))-0.055;
  return mix(upper,lower,low);
}

vec3 atmosphere_view_direction(vec2 texture_coordinate) {
  const vec2 ndc=texture_coordinate*2.0-1.0;
  return normalize(atmosphere.camera_forward_maximum_distance.xyz+
      atmosphere.camera_right_tangent_x.xyz*ndc.x*
          atmosphere.camera_right_tangent_x.w+
      atmosphere.camera_down_tangent_y.xyz*ndc.y*
          atmosphere.camera_down_tangent_y.w);
}

bool planet_blocks_view(vec3 direction) {
  const vec3 origin=atmosphere.camera_position_near.xyz;
  const float projection=dot(origin,direction);
  const float radius=atmosphere.rayleigh_ground_radius.w;
  const float discriminant=projection*projection-
      (dot(origin,origin)-radius*radius);
  return projection<0.0&&discriminant>=0.0;
}

void main() {
  // Manual fixed exposure is the deterministic qualification default. Scene
  // depth is deliberately bound here as the contract for the following
  // depth-aware atmosphere composition gate.
  vec3 hdr=max(texture(hdr_scene,texture_coordinate).rgb,vec3(0.0));
  if(atmosphere.profile_and_mode.w>0.5){
    const float depth=texture(scene_depth,texture_coordinate).r;
    if(depth<=1.0e-8){
      hdr=texture(sky_view_lut,texture_coordinate).rgb;
      const vec3 view_direction=atmosphere_view_direction(texture_coordinate);
      const vec3 sun_direction=normalize(atmosphere.sun_direction_exposure.xyz);
      if(dot(view_direction,sun_direction)>cos(atmosphere.profile_and_mode.z)&&
         !planet_blocks_view(view_direction)){
        const float altitude=length(atmosphere.camera_position_near.xyz)-
            atmosphere.rayleigh_ground_radius.w;
        const float cosine_angle=dot(
            normalize(atmosphere.camera_position_near.xyz),view_direction);
        const vec2 transmittance_uv=vec2(
            clamp(cosine_angle*0.5+0.5,0.0,1.0),
            clamp(altitude/(atmosphere.mie_scattering_top_radius.w-
                atmosphere.rayleigh_ground_radius.w),0.0,1.0));
        hdr+=atmosphere.solar_absorption_peak.rgb*
            texture(transmittance_lut,transmittance_uv).rgb*24.0;
      }
    }else{
      const float distance_metres=atmosphere.camera_position_near.w/depth;
      const float slice=sqrt(clamp(distance_metres/
          atmosphere.camera_forward_maximum_distance.w,0.0,1.0));
      const vec3 lookup=vec3(texture_coordinate,slice);
      const vec3 scattering=texture(aerial_scattering_lut,lookup).rgb;
      const vec3 transmittance=texture(aerial_transmittance_lut,lookup).rgb;
      hdr=hdr*transmittance+scattering;
    }
  }
  out_colour=vec4(linear_to_srgb(aces_fitted(
      hdr*atmosphere.sun_direction_exposure.w)),1.0);
}
