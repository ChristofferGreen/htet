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
layout(set = 0,binding = 8) uniform sampler2DArray sun_shadow_map;

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

float ground_intersection_distance(vec3 direction) {
  const vec3 origin=atmosphere.camera_position_near.xyz;
  const float projection=dot(origin,direction);
  const float radius=atmosphere.rayleigh_ground_radius.w;
  const float discriminant=projection*projection-
      (dot(origin,origin)-radius*radius);
  if(discriminant<0.0)return -1.0;
  const float root=sqrt(max(discriminant,0.0));
  const float near_distance=-projection-root;
  const float far_distance=-projection+root;
  return near_distance>0.0?near_distance:(far_distance>0.0?far_distance:-1.0);
}

float ground_disc_coverage(vec3 direction) {
  const vec3 origin=atmosphere.camera_position_near.xyz;
  const float projection=dot(origin,direction);
  const float radius=atmosphere.rayleigh_ground_radius.w;
  const float discriminant=projection*projection-
      (dot(origin,origin)-radius*radius);
  const float filter_width=max(fwidth(discriminant),1.0);
  return projection<0.0?
      smoothstep(-filter_width,filter_width,discriminant):0.0;
}

vec3 orbital_ground_radiance(vec3 direction,float distance_metres) {
  const vec3 point=atmosphere.camera_position_near.xyz+
      direction*distance_metres;
  const vec3 normal=normalize(point);
  const vec3 sun_direction=normalize(atmosphere.sun_direction_exposure.xyz);
  const float n_dot_l=max(dot(normal,sun_direction),0.0);
  const vec2 solar_uv=vec2(dot(normal,sun_direction)*0.5+0.5,0.0);
  const vec3 solar_transmittance=texture(transmittance_lut,solar_uv).rgb;
  const vec3 direct=atmosphere.ground_albedo_mie_anisotropy.rgb*
      atmosphere.solar_absorption_peak.rgb*solar_transmittance*n_dot_l/
      3.14159265359*2.8;
  // Compact multiple-scattering ground fill prevents the night side from
  // becoming numerically discontinuous while remaining visibly dark.
  const vec3 fill=atmosphere.ground_albedo_mie_anisotropy.rgb*
      texture(multiple_scattering_lut,vec2(solar_uv.x,0.0)).rgb*0.35;
  return direct+fill;
}

void main() {
  // Manual fixed exposure is the deterministic qualification default. Scene
  // depth is deliberately bound here as the contract for the following
  // depth-aware atmosphere composition gate.
  vec3 hdr=max(texture(hdr_scene,texture_coordinate).rgb,vec3(0.0));
  const int debug_view=int(atmosphere.reserved1.x+0.5);
  if(debug_view!=0){
    vec3 diagnostic=vec3(0.0);
    vec4 lookup=vec4(0.0,0.0,0.0,1.0);
    if(debug_view==1)lookup=texture(transmittance_lut,texture_coordinate);
    else if(debug_view==2){
      lookup=texture(multiple_scattering_lut,texture_coordinate);
      lookup.rgb*=12.0;
    }else if(debug_view==3)lookup=texture(sky_view_lut,texture_coordinate);
    else if(debug_view==4){
      lookup=texture(aerial_scattering_lut,vec3(texture_coordinate,0.5));
      lookup.rgb*=4.0;
    }else if(debug_view==5)
      lookup=texture(aerial_transmittance_lut,vec3(texture_coordinate,0.5));
    if(debug_view>=1&&debug_view<=5){
      diagnostic=lookup.a<0.99?vec3(8.0,0.0,8.0):lookup.rgb;
    }
    else if(debug_view==6){
      const float depth=texture(scene_depth,texture_coordinate).r;
      diagnostic=vec3(pow(clamp(depth,0.0,1.0),0.2));
    }else if(debug_view>=7&&debug_view<=10){
      const int cascade=debug_view-7;
      const float depth=texture(sun_shadow_map,
          vec3(texture_coordinate,float(cascade))).r;
      diagnostic=vec3(depth);
    }
    out_colour=vec4(linear_to_srgb(aces_fitted(max(diagnostic,vec3(0.0)))),1.0);
    return;
  }
  if(atmosphere.profile_and_mode.w>0.5){
    const float depth=texture(scene_depth,texture_coordinate).r;
    if(depth<=1.0e-8){
      hdr=texture(sky_view_lut,texture_coordinate).rgb;
      const vec3 view_direction=atmosphere_view_direction(texture_coordinate);
      const vec3 sun_direction=normalize(atmosphere.sun_direction_exposure.xyz);
      const float camera_altitude=length(atmosphere.camera_position_near.xyz)-
          atmosphere.rayleigh_ground_radius.w;
      const float ground_distance=ground_intersection_distance(view_direction);
      // The exact terrain remains authoritative near the playable surface.
      // The analytic sphere is only a far-field planet representation for
      // flight and orbit, where the finite local terrain front cannot cover a
      // planetary disc.
      if(ground_distance>0.0&&camera_altitude>5000.0){
        const vec3 camera_normal=normalize(
            atmosphere.camera_position_near.xyz);
        const vec2 view_transmittance_uv=vec2(
            dot(camera_normal,view_direction)*0.5+0.5,
            clamp(camera_altitude/(atmosphere.mie_scattering_top_radius.w-
                atmosphere.rayleigh_ground_radius.w),0.0,1.0));
        const vec3 ground_hdr=orbital_ground_radiance(
            view_direction,ground_distance)*
            texture(transmittance_lut,view_transmittance_uv).rgb+
            texture(sky_view_lut,texture_coordinate).rgb;
        hdr=mix(hdr,ground_hdr,ground_disc_coverage(view_direction));
      }
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
