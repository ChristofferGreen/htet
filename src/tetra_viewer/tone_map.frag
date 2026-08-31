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
layout(set = 0,binding = 9) uniform sampler2D sky_irradiance_lut;
layout(std140,set=0,binding=10) uniform ShadowCascades {
  mat4 shadow_matrices[5];
  vec4 shadow_splits;
  vec4 atmosphere_shadow_metadata;
  vec4 epipolar_metadata;
} shadow_cascades;
#ifdef FAITHFUL_SHADOW_SPLIT
layout(set = 0,binding = 11) uniform sampler2D long_shadow_lut;
#endif
layout(set = 0,binding = 12) uniform sampler2D screen_scattering;
layout(set = 0,binding = 13) uniform sampler2D screen_transmittance;
layout(set = 0,binding = 14) uniform sampler2D screen_endpoint;
layout(set = 0,binding = 15) uniform sampler3D froxel_scattering;
layout(set = 0,binding = 16) uniform sampler3D froxel_transmittance;

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
  vec4 previous_camera_position_near;
  vec4 previous_camera_right_tangent_x;
  vec4 previous_camera_down_tangent_y;
  vec4 previous_camera_forward;
  vec4 temporal_control;
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

vec2 atmosphere_transmittance_uv(float altitude,float cosine_angle) {
  const float bottom=atmosphere.rayleigh_ground_radius.w;
  const float top=atmosphere.mie_scattering_top_radius.w;
  if(atmosphere.reserved1.y<0.5)
    return vec2(clamp(cosine_angle*0.5+0.5,0.0,1.0),
                clamp(altitude/(top-bottom),0.0,1.0));
  const float radius=bottom+clamp(altitude,0.0,top-bottom);
  const float horizon=atmosphere.reserved3.w;
  const float rho=sqrt(max(0.0,(radius-bottom)*(radius+bottom)));
  cosine_angle=clamp(cosine_angle,-1.0,1.0);
  const float discriminant=max(0.0,
      radius*radius*(cosine_angle*cosine_angle-1.0)+top*top);
  const float distance=max(0.0,-radius*cosine_angle+sqrt(discriminant));
  const float minimum=top-radius;
  const float maximum=rho+horizon;
  return vec2(clamp((distance-minimum)/max(maximum-minimum,1.0e-6),0.0,1.0),
              clamp(rho/max(horizon,1.0e-6),0.0,1.0));
}

vec2 atmosphere_full_sky_uv(vec3 direction) {
  const vec3 local_up=atmosphere.reserved2.xyz;
  const vec3 sun_tangent=atmosphere.reserved3.xyz;
  const vec3 longitude_tangent=cross(local_up,sun_tangent);
  const float tangent_x=dot(direction,sun_tangent);
  const float tangent_y=dot(direction,longitude_tangent);
  float perimeter=0.0;
  if(tangent_x>=0.0){
    const float denominator=abs(tangent_x)+abs(tangent_y);
    perimeter=denominator>0.0?tangent_y/denominator:0.0;
  }else if(tangent_y>=0.0){
    const float denominator=-tangent_x+tangent_y;
    perimeter=2.0-tangent_y/denominator;
  }else{
    const float denominator=-tangent_x-tangent_y;
    perimeter=-1.0+tangent_x/denominator;
  }
  const float vertical=clamp(dot(direction,local_up),-1.0,1.0);
  if(atmosphere.reserved1.y>=9.5){
    const float bottom=atmosphere.rayleigh_ground_radius.w;
    const float view_height=bottom+max(atmosphere.reserved2.w,0.0);
    const float horizon_cosine=sqrt(max(0.0,
        (view_height-bottom)*(view_height+bottom)))/
        max(view_height,1.0e-6);
    const float beta=acos(clamp(horizon_cosine,0.0,1.0));
    const float zenith_horizon=3.14159265358979323846-beta;
    const float angle=acos(vertical);
    float vertical_uv;
    if(angle<=zenith_horizon){
      float coordinate=1.0-angle/max(zenith_horizon,1.0e-6);
      coordinate=1.0-sqrt(max(coordinate,0.0));
      vertical_uv=coordinate*0.5;
    }else{
      float coordinate=(angle-zenith_horizon)/max(beta,1.0e-6);
      vertical_uv=0.5+0.5*sqrt(max(coordinate,0.0));
    }
    return vec2(perimeter*0.25+0.5,clamp(vertical_uv,0.0,1.0));
  }
  const float latitude_shape=0.7853981633974483-1.0;
  const float root=sqrt(max(0.0,1.0-abs(vertical)));
  const float latitude_proxy=(1.0-root)/(1.0+latitude_shape*root);
  const float mapped_latitude=abs(vertical)<1.0e-6?0.0:
      sign(vertical)*sqrt(latitude_proxy);
  return vec2(perimeter*0.25+0.5,
              mapped_latitude*0.5+0.5);
}

vec2 atmosphere_sun_focused_sky_uv(vec3 direction) {
  vec2 uv=atmosphere_full_sky_uv(direction);
  const float perimeter=(uv.x-0.5)*4.0;
  const float focused=sign(perimeter)*sqrt(abs(perimeter)*0.5);
  uv.x=focused*0.5+0.5;
  return uv;
}

vec2 atmosphere_sun_shadow_sky_uv(vec3 direction) {
  vec2 uv=atmosphere_full_sky_uv(direction);
  const float perimeter=(uv.x-0.5)*4.0;
  const float magnitude=abs(perimeter);
  const float focused=magnitude<=0.25?magnitude:
      0.25+(magnitude-0.25)/7.0;
  uv.x=0.5+sign(perimeter)*focused;
  return uv;
}

vec3 sample_sky_view(vec3 direction,vec2 screen_uv) {
  if(atmosphere.reserved1.y<0.5)return texture(sky_view_lut,screen_uv).rgb;
  vec2 uv=atmosphere_full_sky_uv(direction);
  if(atmosphere.reserved1.y>=9.5){
    const vec2 resolution=vec2(textureSize(sky_view_lut,0));
    uv=(uv+0.5/resolution)*(resolution/(resolution+1.0));
  }
  return texture(sky_view_lut,uv).rgb;
}

vec2 aerial_direction_uv(vec3 direction,vec2 screen_uv) {
  return atmosphere.reserved1.y>0.5?
      atmosphere_sun_focused_sky_uv(direction):screen_uv;
}

#ifdef FAITHFUL_SHADOW_SPLIT
vec4 sample_long_shadow_bilinear(vec2 uv) {
  const ivec2 size=textureSize(long_shadow_lut,0);
  const vec2 half_texel=0.5/vec2(size);
  // The bound scene sampler already provides linear filtering.  Use it over
  // the regular interior so each reconstruction lobe costs one fetch; retain
  // the explicit wrapped path only at the longitude seam.
  if(uv.x>=half_texel.x&&uv.x<=1.0-half_texel.x&&
     uv.y>=half_texel.y&&uv.y<=1.0-half_texel.y)
    return max(texture(long_shadow_lut,uv),vec4(0.0));
  const vec2 texel=uv*vec2(size)-0.5;
  const ivec2 low=ivec2(floor(texel));
  const ivec2 high=low+1;
  const int low_x=(low.x%size.x+size.x)%size.x;
  const int high_x=(high.x%size.x+size.x)%size.x;
  const int low_y=clamp(low.y,0,size.y-1);
  const int high_y=clamp(high.y,0,size.y-1);
  const vec2 fraction=fract(texel);
  const vec4 lower=mix(texelFetch(long_shadow_lut,ivec2(low_x,low_y),0),
      texelFetch(long_shadow_lut,ivec2(high_x,low_y),0),fraction.x);
  const vec4 upper=mix(texelFetch(long_shadow_lut,ivec2(low_x,high_y),0),
      texelFetch(long_shadow_lut,ivec2(high_x,high_y),0),fraction.x);
  return max(mix(lower,upper,fraction.y),vec4(0.0));
}

vec4 sample_long_shadow(vec3 direction) {
  if(atmosphere.reserved1.y<0.5)return vec4(0.0);
  const vec2 uv=atmosphere_sun_shadow_sky_uv(direction);
  const int shadow_filter=int(mod(atmosphere.reserved1.z,10.0)+0.5);
  if(shadow_filter==0){
    const ivec2 size=textureSize(long_shadow_lut,0);
    ivec2 coordinate=ivec2(clamp(uv,vec2(0.0),vec2(1.0))*vec2(size));
    coordinate=clamp(coordinate,ivec2(0),size-1);
    return max(texelFetch(long_shadow_lut,coordinate,0),vec4(0.0));
  }
  if(shadow_filter==2)
    return sample_long_shadow_bilinear(uv);
  const vec2 texel=1.0/vec2(textureSize(long_shadow_lut,0));
  // The planet-relative atlas is deliberately much smaller than the output.
  // Plain bilinear reconstruction enlarged each source texel into a visible
  // stair step along a mountain's volumetric umbra.  A compact five-lobe tent
  // keeps the stable atlas while reconstructing one continuous penumbra.  The
  // centre-heavy weights retain the shadow energy and the exact solar core
  // below still supplies the sharp occulting silhouette.
  vec4 filtered=sample_long_shadow_bilinear(uv)*0.5;
  filtered+=sample_long_shadow_bilinear(uv+texel*vec2(-1.0,-1.0))*0.125;
  filtered+=sample_long_shadow_bilinear(uv+texel*vec2( 1.0,-1.0))*0.125;
  filtered+=sample_long_shadow_bilinear(uv+texel*vec2(-1.0, 1.0))*0.125;
  filtered+=sample_long_shadow_bilinear(uv+texel*vec2( 1.0, 1.0))*0.125;
  return filtered;
}

vec3 sample_long_shadow_visibility(vec3 direction) {
  // The producer already treats an unavailable fitted map as unoccluded and
  // evaluates the current local cascades.  Rejecting the complete atlas here
  // therefore also rejected valid local mountain shadows during asynchronous
  // front construction—the common startup/camera-settle state—and restored
  // the unshadowed Mie aureole.  Consume the coherent atlas generation; its
  // individual samples carry the conservative fallback.
  return sample_long_shadow(direction).rgb;
}

float primary_bilinear_shadow_visibility_single(
    vec2 uv,float layer,float receiver_depth,float bias) {
  const ivec2 size=textureSize(sun_shadow_map,0).xy;
  const vec2 texel_position=uv*vec2(size)-0.5;
  const ivec2 low=ivec2(floor(texel_position));
  const ivec2 high=low+1;
  const vec2 fraction=fract(texel_position);
  const ivec2 maximum=size-1;
  const float lower_left=receiver_depth-bias<=texelFetch(
      sun_shadow_map,ivec3(clamp(low,ivec2(0),maximum),int(layer)),0).r?1.0:0.0;
  const float lower_right=receiver_depth-bias<=texelFetch(
      sun_shadow_map,ivec3(clamp(ivec2(high.x,low.y),ivec2(0),maximum),
                           int(layer)),0).r?1.0:0.0;
  const float upper_left=receiver_depth-bias<=texelFetch(
      sun_shadow_map,ivec3(clamp(ivec2(low.x,high.y),ivec2(0),maximum),
                           int(layer)),0).r?1.0:0.0;
  const float upper_right=receiver_depth-bias<=texelFetch(
      sun_shadow_map,ivec3(clamp(high,ivec2(0),maximum),int(layer)),0).r?1.0:0.0;
  return mix(mix(lower_left,lower_right,fraction.x),
             mix(upper_left,upper_right,fraction.x),fraction.y);
}

float primary_bilinear_shadow_visibility(vec2 uv,float layer,
                                         float receiver_depth,float bias) {
  const vec2 texel=1.0/vec2(textureSize(sun_shadow_map,0).xy);
  // A one-texel PCF footprint preserves the caster mesh's faceted silhouette.
  // Projecting that silhouette through kilometres of haze magnifies it into
  // the stepped dark halo seen above a ridge.  Use a modest centre-weighted
  // solar penumbra for atmospheric receivers only; opaque terrain retains its
  // sharper scene-shadow filter.
  float visibility=primary_bilinear_shadow_visibility_single(
      uv,layer,receiver_depth,bias)*0.5;
  visibility+=primary_bilinear_shadow_visibility_single(
      uv+texel*vec2(-1.5,-1.5),layer,receiver_depth,bias)*0.125;
  visibility+=primary_bilinear_shadow_visibility_single(
      uv+texel*vec2( 1.5,-1.5),layer,receiver_depth,bias)*0.125;
  visibility+=primary_bilinear_shadow_visibility_single(
      uv+texel*vec2(-1.5, 1.5),layer,receiver_depth,bias)*0.125;
  visibility+=primary_bilinear_shadow_visibility_single(
      uv+texel*vec2( 1.5, 1.5),layer,receiver_depth,bias)*0.125;
  return visibility;
}

float primary_fitted_shadow_visibility(vec3 point_world) {
  if(shadow_cascades.atmosphere_shadow_metadata.x<0.999)return 1.0;
  const vec3 projected=(shadow_cascades.shadow_matrices[4]*
      vec4(point_world,1.0)).xyz;
  if(any(greaterThan(abs(projected.xy),vec2(1.0)))||
     projected.z<0.0||projected.z>1.0)return 1.0;
  const float scale=shadow_cascades.atmosphere_shadow_metadata.w;
  const float visibility=primary_bilinear_shadow_visibility(
      (projected.xy*0.5+0.5)*scale,4.0,projected.z,
      shadow_cascades.atmosphere_shadow_metadata.z);
  const float coverage=1.0-smoothstep(
      0.94,0.995,max(abs(projected.x),abs(projected.y)));
  return mix(1.0,visibility,coverage);
}

float primary_cascade_shadow_visibility(vec3 point_world,int cascade,
                                        float fallback_visibility) {
  const vec3 projected=(shadow_cascades.shadow_matrices[cascade]*
      vec4(point_world,1.0)).xyz;
  if(any(greaterThan(abs(projected.xy),vec2(1.0)))||
     projected.z<0.0||projected.z>1.0)return fallback_visibility;
  const float depth_span=max(
      shadow_cascades.shadow_splits[cascade]*(16.0/3.0),1.0e-6);
  const float visibility=primary_bilinear_shadow_visibility(
      projected.xy*0.5+0.5,float(cascade),projected.z,0.0036/depth_span);
  const float coverage=1.0-smoothstep(
      0.88,0.98,max(abs(projected.x),abs(projected.y)));
  const float local_visibility=mix(fallback_visibility,visibility,coverage);
  // Retain long-range fitted occluders inside local cascade footprints. At a
  // grazing sun angle a nearby receiver and its distant mountain caster need
  // not fit inside the same local cascade depth interval.
  return min(local_visibility,fallback_visibility);
}

float primary_terrain_sun_visibility(vec3 point_metres) {
  const float metres_per_world=atmosphere.profile_and_mode.y;
  const vec3 point_world=atmosphere.reserved0.xyz+
      (point_metres-atmosphere.camera_position_near.xyz)/metres_per_world;
  const float distance_world=length(point_world-atmosphere.reserved0.xyz);
  const float fitted=primary_fitted_shadow_visibility(point_world);
  int cascade=3;
  if(distance_world<shadow_cascades.shadow_splits.x)cascade=0;
  else if(distance_world<shadow_cascades.shadow_splits.y)cascade=1;
  else if(distance_world<shadow_cascades.shadow_splits.z)cascade=2;
  const float current=primary_cascade_shadow_visibility(
      point_world,cascade,fitted);
  if(cascade==3){
    if(shadow_cascades.atmosphere_shadow_metadata.x<0.999)return current;
    const float outer_fade=smoothstep(
        shadow_cascades.shadow_splits.w*0.80,
        shadow_cascades.shadow_splits.w,distance_world);
    return mix(current,fitted,outer_fade);
  }
  const float split=shadow_cascades.shadow_splits[cascade];
  const float previous=cascade==0?0.0:
      shadow_cascades.shadow_splits[cascade-1];
  const float blend=smoothstep(mix(previous,split,0.85),split,distance_world);
  return mix(current,primary_cascade_shadow_visibility(
      point_world,cascade+1,fitted),blend);
}

#endif

vec3 sample_sky_irradiance(vec3 normal) {
  const vec2 uv=atmosphere_full_sky_uv(normal);
  const ivec2 size=textureSize(sky_irradiance_lut,0);
  const vec2 texel=uv*vec2(size)-0.5;
  const ivec2 low=ivec2(floor(texel));
  const ivec2 high=low+1;
  const int low_x=(low.x%size.x+size.x)%size.x;
  const int high_x=(high.x%size.x+size.x)%size.x;
  const int low_y=clamp(low.y,0,size.y-1);
  const int high_y=clamp(high.y,0,size.y-1);
  const vec2 fraction=fract(texel);
  const vec3 lower=mix(
      texelFetch(sky_irradiance_lut,ivec2(low_x,low_y),0).rgb,
      texelFetch(sky_irradiance_lut,ivec2(high_x,low_y),0).rgb,fraction.x);
  const vec3 upper=mix(
      texelFetch(sky_irradiance_lut,ivec2(low_x,high_y),0).rgb,
      texelFetch(sky_irradiance_lut,ivec2(high_x,high_y),0).rgb,fraction.x);
  return mix(lower,upper,fraction.y);
}

bool planet_blocks_view(vec3 direction) {
  const vec3 origin=atmosphere.camera_position_near.xyz;
  const float projection=dot(origin,direction);
  const float radius=atmosphere.rayleigh_ground_radius.w;
  const float radial_distance=length(origin);
  const float discriminant=projection*projection-
      (radial_distance-radius)*(radial_distance+radius);
  return projection<0.0&&discriminant>=0.0;
}

float ground_intersection_distance(vec3 direction) {
  const vec3 origin=atmosphere.camera_position_near.xyz;
  const float projection=dot(origin,direction);
  const float radius=atmosphere.rayleigh_ground_radius.w;
  const float radial_distance=length(origin);
  const float discriminant=projection*projection-
      (radial_distance-radius)*(radial_distance+radius);
  if(discriminant<0.0)return -1.0;
  const float root=sqrt(max(discriminant,0.0));
  const float near_distance=-projection-root;
  const float far_distance=-projection+root;
  return near_distance>0.01?near_distance:
      (far_distance>0.01?far_distance:-1.0);
}

float orbital_sun_visibility(float radial_distance,float sun_cosine) {
  const float ground=atmosphere.rayleigh_ground_radius.w;
  const float altitude=max(radial_distance-ground,0.0);
  const float horizon_sine_squared=max(0.0,
      altitude*(radial_distance+ground)/(radial_distance*radial_distance));
  const float horizon_cosine=-sqrt(horizon_sine_squared);
  const float cosine_width=max(sin(atmosphere.profile_and_mode.z),1.0e-5);
  return smoothstep(horizon_cosine-cosine_width,
                    horizon_cosine+cosine_width,sun_cosine);
}

vec2 orbital_sphere_roots(vec3 origin,vec3 direction,float radius) {
  const float projection=dot(origin,direction);
  const float radial_distance=length(origin);
  const float offset=(radial_distance-radius)*(radial_distance+radius);
  const float discriminant=projection*projection-offset;
  if(discriminant<0.0)return vec2(1.0,-1.0);
  const float root=sqrt(max(discriminant,0.0));
  return vec2(-projection-root,-projection+root);
}

bool orbital_medium_segment(vec3 origin,vec3 direction,
                            out float begin,out float end) {
  const float ground=atmosphere.rayleigh_ground_radius.w;
  const float top=atmosphere.mie_scattering_top_radius.w;
  const vec2 outer=orbital_sphere_roots(origin,direction,top);
  if(outer.y<0.0)return false;
  begin=max(0.0,outer.x);
  end=outer.y;
  const float radial_distance=length(origin);
  if(radial_distance>=ground){
    const float altitude=radial_distance-ground;
    const float radial_cosine=dot(origin/radial_distance,direction);
    const float horizon_sine_squared=max(0.0,
        altitude*(radial_distance+ground)/
            (radial_distance*radial_distance));
    if(radial_cosine<0.0&&radial_cosine*radial_cosine>
       horizon_sine_squared*(1.0+2.0e-6)){
      const float hit=radial_distance*(-radial_cosine-
          sqrt(max(radial_cosine*radial_cosine-
                       horizon_sine_squared,0.0)));
      if(hit>begin+0.01)end=min(end,hit);
    }
  }
  return end>begin;
}

vec3 orbital_densities(float altitude) {
  const float height=atmosphere.mie_scattering_top_radius.w-
      atmosphere.rayleigh_ground_radius.w;
  if(altitude<0.0||altitude>height)return vec3(0.0);
  return vec3(
      exp(-altitude/atmosphere.mie_absorption_rayleigh_scale.w),
      exp(-altitude/atmosphere.absorption_mie_scale.w),
      max(0.0,1.0-abs(altitude-atmosphere.solar_absorption_peak.w)/
          atmosphere.profile_and_mode.x));
}

vec3 orbital_extinction(vec3 density) {
  return atmosphere.rayleigh_ground_radius.rgb*density.x+
      (atmosphere.mie_scattering_top_radius.rgb+
       atmosphere.mie_absorption_rayleigh_scale.rgb)*density.y+
      atmosphere.absorption_mie_scale.rgb*density.z;
}

int atmosphere_rendering_method() {
  const int encoded=int(atmosphere.reserved1.y+0.5);
  return encoded>=10?encoded-10:encoded;
}

bool reference_hillaire_transport() {
  return int(atmosphere.reserved1.y+0.5)>=10;
}

bool exact_screen_visibility() {
  return atmosphere_rendering_method()>=2;
}

bool deterministic_half_resolution() {
  const int method=atmosphere_rendering_method();
  return method==3||method==4;
}

bool deterministic_shadowed_froxels() {
  return atmosphere_rendering_method()==5;
}

void shadowed_froxel_atmosphere(float distance_metres,
                                out vec3 scattering,
                                out vec3 transmittance) {
  const float maximum=max(
      atmosphere.camera_forward_maximum_distance.w,1.0e-6);
  const float normalized_distance=clamp(distance_metres/maximum,0.0,1.0);
  const float slice=reference_hillaire_transport()?
      sqrt(normalized_distance):pow(normalized_distance,1.0/3.0);
  const vec3 coordinate=vec3(texture_coordinate,slice);
  scattering=texture(froxel_scattering,coordinate).rgb;
  transmittance=texture(froxel_transmittance,coordinate).rgb;
}

float orbital_rayleigh_phase(float cosine_angle) {
  return 3.0*(1.0+cosine_angle*cosine_angle)/(16.0*3.14159265359);
}

float orbital_mie_phase(float cosine_angle) {
  const float g=atmosphere.ground_albedo_mie_anisotropy.w;
  return (1.0-g*g)/(4.0*3.14159265359*pow(
      max(1.0+g*g-2.0*g*cosine_angle,1.0e-4),1.5));
}

vec3 orbital_primary_scattering_components(
    vec3 direction,float maximum_distance,float terrain_shadow_weight,
    vec3 cached_shadow_visibility,
    out vec3 path_transmittance,
    out vec3 direct_radiance,out vec3 multiple_radiance) {
  const vec3 origin=atmosphere.camera_position_near.xyz;
  float begin,end;
  path_transmittance=vec3(1.0);
  direct_radiance=vec3(0.0);
  multiple_radiance=vec3(0.0);
  if(!orbital_medium_segment(origin,direction,begin,end)||
     maximum_distance<=begin)return vec3(0.0);
  end=min(end,maximum_distance);
  const float closest=clamp(-dot(origin,direction),begin,end);
  const vec3 sun_direction=normalize(atmosphere.sun_direction_exposure.xyz);
  const float phase_cosine=dot(direction,sun_direction);
  const float rayleigh_phase=orbital_rayleigh_phase(phase_cosine);
  const float mie_phase=orbital_mie_phase(phase_cosine);
  const float atmosphere_height=atmosphere.mie_scattering_top_radius.w-
      atmosphere.rayleigh_ground_radius.w;
  vec3 transmittance=vec3(1.0);
  const float ground_radius=atmosphere.rayleigh_ground_radius.w;
  const float closest_radius=length(origin+direction*closest);
  const float closest_altitude=max(closest_radius-ground_radius,0.0);
  const float mie_scale=max(atmosphere.absorption_mie_scale.w,1.0);
  // Parameterize both halves by radial altitude rather than by their camera-
  // dependent distance. A distance-space grid changes the number of samples
  // inside a compact aerosol layer during ascent. Fifth-power
  // altitude spacing gives that layer several stable intervals at the limb;
  // rays whose closest approach is above the aerosol smoothly return to the
  // less concentrated quadratic distribution needed by Rayleigh scattering.
  const float altitude_power=mix(2.0,5.0,
      exp(-closest_altitude/(4.0*mie_scale)));
  for(int index=0;index<32;++index){
    const bool before=index<16;
    const int local_index=before?index:index-16;
    const float u0=float(local_index)/16.0;
    const float u1=float(local_index+1)/16.0;
    const float side_distance=before?begin:end;
    const float side_altitude=max(
        length(origin+direction*side_distance)-ground_radius,
        closest_altitude);
    const float altitude_begin=mix(closest_altitude,side_altitude,
        pow(before?1.0-u0:u0,altitude_power));
    const float altitude_end=mix(closest_altitude,side_altitude,
        pow(before?1.0-u1:u1,altitude_power));
    const float offset_begin=sqrt(max(
        (ground_radius+altitude_begin)*(ground_radius+altitude_begin)-
        closest_radius*closest_radius,0.0));
    const float offset_end=sqrt(max(
        (ground_radius+altitude_end)*(ground_radius+altitude_end)-
        closest_radius*closest_radius,0.0));
    const float distance_begin=closest+(before?-offset_begin:offset_begin);
    const float distance_end=closest+(before?-offset_end:offset_end);
    const float interval_length=distance_end-distance_begin;
    if(interval_length<=0.0)continue;
    int substeps=1;
#ifdef FAITHFUL_SHADOW_SPLIT
    // A single binary shadow lookup per radial interval stamps a displaced
    // copy of a ridge wherever that interval enters or leaves the mountain's
    // shadow volume. Locate those discontinuities before integrating. Smooth
    // intervals retain one quadrature point; only intervals crossing a
    // visibility boundary pay for eight ordered substeps.
    if(exact_screen_visibility()||terrain_shadow_weight>0.0){
      const vec3 first_point=origin+direction*mix(
          distance_begin,distance_end,0.125);
      const vec3 middle_point=origin+direction*0.5*(
          distance_begin+distance_end);
      const vec3 last_point=origin+direction*mix(
          distance_begin,distance_end,0.875);
      const float first_visibility=primary_terrain_sun_visibility(first_point);
      const float middle_visibility=primary_terrain_sun_visibility(middle_point);
      const float last_visibility=primary_terrain_sun_visibility(last_point);
      const float visibility_span=max(first_visibility,max(
          middle_visibility,last_visibility))-min(first_visibility,min(
          middle_visibility,last_visibility));
      if(visibility_span>0.02)substeps=8;
    }
#endif
    for(int substep=0;substep<8;++substep){
      if(substep>=substeps)break;
      const float sub_begin=mix(distance_begin,distance_end,
          float(substep)/float(substeps));
      const float sub_end=mix(distance_begin,distance_end,
          float(substep+1)/float(substeps));
      const float step_length=sub_end-sub_begin;
      const vec3 point=origin+direction*0.5*(sub_begin+sub_end);
      const float radial_distance=length(point);
      const float altitude=radial_distance-
          atmosphere.rayleigh_ground_radius.w;
      const vec3 density=orbital_densities(altitude);
      const vec3 local_extinction=orbital_extinction(density);
      const vec3 segment_transmittance=exp(-local_extinction*step_length);
      const float sun_cosine=dot(point/radial_distance,sun_direction);
      const vec3 solar_transmittance=texture(transmittance_lut,
          atmosphere_transmittance_uv(altitude,sun_cosine)).rgb*
          orbital_sun_visibility(radial_distance,sun_cosine);
      const vec3 scattering=
          atmosphere.rayleigh_ground_radius.rgb*density.x+
          atmosphere.mie_scattering_top_radius.rgb*density.y;
      vec3 direct_source=(
          atmosphere.rayleigh_ground_radius.rgb*density.x*rayleigh_phase+
          atmosphere.mie_scattering_top_radius.rgb*density.y*mie_phase)*
          solar_transmittance*atmosphere.solar_absorption_peak.rgb;
#ifdef FAITHFUL_SHADOW_SPLIT
      // The long-shadow atlas stores an integrated loss and is intentionally
      // inexpensive, but the forward Mie cone needs visibility at the actual
      // quadrature point so occlusion happens before integration.
      float exact_visibility=1.0;
      if(exact_screen_visibility()||
         (terrain_shadow_weight>0.0&&density.y>1.0e-4))
        exact_visibility=primary_terrain_sun_visibility(point);
      if(exact_screen_visibility())direct_source*=exact_visibility;
      else{
        const vec3 terrain_visibility=mix(cached_shadow_visibility,
            vec3(exact_visibility),terrain_shadow_weight);
        direct_source*=mix(vec3(1.0),terrain_visibility,0.25);
      }
#endif
      const vec2 multiple_uv=vec2(sun_cosine*0.5+0.5,
          clamp(altitude/max(atmosphere_height,1.0),0.0,1.0));
      const vec3 multiple_source=scattering*
          texture(multiple_scattering_lut,multiple_uv).rgb*
          atmosphere.solar_absorption_peak.rgb;
      const vec3 integral=(vec3(1.0)-segment_transmittance)/
          max(local_extinction,vec3(1.0e-9));
      direct_radiance+=transmittance*direct_source*integral;
      multiple_radiance+=transmittance*multiple_source*integral;
      transmittance*=segment_transmittance;
    }
  }
  path_transmittance=transmittance;
  return direct_radiance+multiple_radiance;
}

vec3 orbital_primary_scattering(vec3 direction,float maximum_distance,
                                float terrain_shadow_weight,
                                vec3 cached_shadow_visibility,
                                out vec3 path_transmittance) {
  vec3 direct_radiance,multiple_radiance;
  return orbital_primary_scattering_components(
      direction,maximum_distance,terrain_shadow_weight,
      cached_shadow_visibility,path_transmittance,direct_radiance,
      multiple_radiance);
}

void reconstructed_atmosphere(float native_depth,
                              out vec3 radiance,out vec3 transmittance) {
  const bool opaque=native_depth>1.0e-8;
  const vec3 native_direction=atmosphere_view_direction(texture_coordinate);
  const bool near_solar_mie_lobe=dot(native_direction,normalize(
      atmosphere.sun_direction_exposure.xyz))>cos(radians(45.0));
  const bool native_class_boundary=!opaque&&near_solar_mie_lobe&&
      textureOffset(scene_depth,texture_coordinate,ivec2(0,3)).r>1.0e-8;
  const float target_depth=opaque?
      atmosphere.camera_position_near.w/native_depth:0.0;
  if(native_class_boundary&&!reference_hillaire_transport()){
    radiance=orbital_primary_scattering(native_direction,
        opaque?target_depth:1.0e9,1.0,vec3(1.0),transmittance);
    return;
  }
  const ivec2 size=textureSize(screen_scattering,0);
  const vec2 texel=texture_coordinate*vec2(size)-0.5;
  const ivec2 low=ivec2(floor(texel));
  const vec2 fraction=fract(texel);
  radiance=vec3(0.0);
  transmittance=vec3(0.0);
  float weight_sum=0.0;
  bool saw_opaque=false;
  bool saw_sky=false;
  for(int y=0;y<2;++y)for(int x=0;x<2;++x){
    const ivec2 coordinate=clamp(low+ivec2(x,y),ivec2(0),size-1);
    const vec4 endpoint=texelFetch(screen_endpoint,coordinate,0);
    const bool tap_opaque=endpoint.y>0.5;
    saw_opaque=saw_opaque||tap_opaque;
    saw_sky=saw_sky||!tap_opaque;
    if(tap_opaque!=opaque)continue;
    float depth_weight=1.0;
    if(opaque){
      const float threshold=max(target_depth*0.05,1.0e-6);
      const float difference=abs(endpoint.x-target_depth);
      if(difference>=threshold)continue;
      depth_weight=1.0-difference/threshold;
    }
    const float spatial=(x==0?1.0-fraction.x:fraction.x)*
        (y==0?1.0-fraction.y:fraction.y);
    const float weight=spatial*depth_weight;
    radiance+=texelFetch(screen_scattering,coordinate,0).rgb*weight;
    transmittance+=texelFetch(screen_transmittance,coordinate,0).rgb*weight;
    weight_sum+=weight;
  }
  if(weight_sum>0.0&&
     (reference_hillaire_transport()||!(saw_opaque&&saw_sky))){
    radiance/=weight_sum;
    transmittance/=weight_sum;
    return;
  }
  // A mixed 2x2 block can leave a native silhouette pixel with no compatible
  // low-resolution tap. Evaluate that rare pixel directly instead of leaking
  // lit sky through terrain or smearing foreground depth into the sky.
  const vec3 direction=atmosphere_view_direction(texture_coordinate);
  radiance=orbital_primary_scattering(direction,
      opaque?target_depth:1.0e9,1.0,vec3(1.0),transmittance);
}

float primary_shadow_cone_weight(vec3 direction) {
#ifdef FAITHFUL_SHADOW_SPLIT
  const float sun_cosine=dot(direction,
      normalize(atmosphere.sun_direction_exposure.xyz));
  // The compact-planet Mie lobe that needs silhouette-accurate visibility is
  // confined to the solar neighbourhood.  A 7-degree exact core covers the
  // complete qualified mountain wedge; the five-degree shoulder returns
  // smoothly to the stable planet-relative atlas without paying per-ray
  // shadow-map cost over most of the frame.
  return smoothstep(cos(radians(12.0)),cos(radians(7.0)),sun_cosine);
#else
  return 0.0;
#endif
}

vec3 analytic_ground_radiance(vec3 direction,float distance_metres) {
  const vec3 point=atmosphere.camera_position_near.xyz+
      direction*distance_metres;
  const vec3 normal=normalize(point);
  const vec3 sun_direction=normalize(atmosphere.sun_direction_exposure.xyz);
  const float sun_cosine=dot(normal,sun_direction);
  const float n_dot_l=max(sun_cosine,0.0);
  const float atmosphere_height=atmosphere.mie_scattering_top_radius.w-
      atmosphere.rayleigh_ground_radius.w;
  const vec2 scattering_uv=vec2(clamp(sun_cosine*0.5+0.5,0.0,1.0),
      clamp(1.0/max(atmosphere_height,1.0),0.0,1.0));
  const vec2 lighting_uv=atmosphere_transmittance_uv(1.0,sun_cosine);
  const vec3 solar_transmittance=texture(
      transmittance_lut,lighting_uv).rgb;
  const vec3 albedo=atmosphere.ground_albedo_mie_anisotropy.rgb;
  if(atmosphere.reserved1.y>0.5&&atmosphere.reserved1.z<99.5){
    const vec3 ambient=albedo*sample_sky_irradiance(normal);
    const vec3 direct=albedo/3.14159265359*
        atmosphere.solar_absorption_peak.rgb*solar_transmittance*n_dot_l*2.8;
    return ambient+direct;
  }

  // Keep the fitted ground model as the qualified baseline only.
  const vec3 average_radiance=texture(
      multiple_scattering_lut,scattering_uv).rgb;
  const vec3 vertical_transmittance=texture(
      transmittance_lut,atmosphere_transmittance_uv(1.0,1.0)).rgb;
  const float daylight=smoothstep(-0.12,0.20,sun_cosine);
  const vec3 single_scattered_fill=atmosphere.solar_absorption_peak.rgb*
      (vec3(1.0)-vertical_transmittance)*daylight*0.60;
  const vec3 environment=(average_radiance+single_scattered_fill)*2.0;
  const vec3 ambient=0.96*albedo*environment;
  const vec3 direct=0.96*albedo/3.14159265359*
      atmosphere.solar_absorption_peak.rgb*solar_transmittance*n_dot_l*2.8;
  return ambient+direct;
}

vec3 composite_aerial(vec3 surface_radiance,float distance_metres) {
  const float local_distance=atmosphere.camera_forward_maximum_distance.w;
  const bool faithful=atmosphere.reserved1.y>0.5;
  const vec3 direction=atmosphere_view_direction(texture_coordinate);
  if(faithful){
    if(deterministic_shadowed_froxels()){
      vec3 scattering,transmittance;
      shadowed_froxel_atmosphere(distance_metres,scattering,transmittance);
      return surface_radiance*transmittance+scattering;
    }
    if(deterministic_half_resolution()){
      vec3 scattering,transmittance;
      reconstructed_atmosphere(
          atmosphere.camera_position_near.w/distance_metres,
          scattering,transmittance);
      return surface_radiance*transmittance+scattering;
    }
    // A retained camera-relative volume cannot vary continuously under
    // arbitrary translation: moving its angular or depth footprint across a
    // texel recreates the same Mie pulse in a different part of the image.
    // Use one current, surface-truncated primary ray for every final faithful
    // aerial pixel.  Lookup volumes remain available to diagnostics and the
    // qualified baseline, but no longer form visible faithful output.
    const float terrain_shadow_weight=primary_shadow_cone_weight(direction);
    vec3 cached_shadow_visibility=vec3(1.0);
#ifdef FAITHFUL_SHADOW_SPLIT
    cached_shadow_visibility=sample_long_shadow_visibility(direction);
#endif
    vec3 primary_transmittance;
    vec3 primary=orbital_primary_scattering(
        direction,distance_metres,terrain_shadow_weight,
        cached_shadow_visibility,primary_transmittance);
    return surface_radiance*primary_transmittance+primary;
  }
  const float slice=pow(clamp(distance_metres/local_distance,0.0,1.0),
      1.0/3.0);
  const vec3 lookup=vec3(aerial_direction_uv(direction,texture_coordinate),
                         slice);
  vec3 scattering=texture(aerial_scattering_lut,lookup).rgb;
  const vec3 transmittance=texture(aerial_transmittance_lut,lookup).rgb;
  // The compact 16-slice volume can under-resolve the rapidly changing
  // eye-level aerosol layer near the horizon. The full-path sky lookup is a
  // conservative directional airlight reference; scale it by the traversed
  // optical opacity so nearby geometry is unchanged while distant silhouettes
  // converge toward the horizon radiance instead of merely becoming dark.
  const float optical_opacity=1.0-dot(transmittance,
      vec3(0.2126,0.7152,0.0722));
  const vec3 directional_airlight=sample_sky_view(
      atmosphere_view_direction(texture_coordinate),texture_coordinate)*
      max(optical_opacity,0.0);
  scattering=max(scattering,directional_airlight);
  return surface_radiance*transmittance+scattering;
}

vec3 background_atmosphere() {
  const vec3 view_direction=atmosphere_view_direction(texture_coordinate);
  const vec3 sun_direction=normalize(atmosphere.sun_direction_exposure.xyz);
  const float ground_distance=ground_intersection_distance(view_direction);
  vec3 radiance;
  if(ground_distance>0.0){
    radiance=composite_aerial(
        analytic_ground_radiance(view_direction,ground_distance),
        ground_distance);
  }else{
    if(atmosphere.reserved1.y>0.5){
      vec3 primary_transmittance;
      const float terrain_shadow_weight=
          primary_shadow_cone_weight(view_direction);
      vec3 cached_shadow_visibility=vec3(1.0);
#ifdef FAITHFUL_SHADOW_SPLIT
      cached_shadow_visibility=sample_long_shadow_visibility(view_direction);
#endif
      if(reference_hillaire_transport())
        radiance=sample_sky_view(view_direction,texture_coordinate);
      else if(deterministic_shadowed_froxels())
        shadowed_froxel_atmosphere(
            atmosphere.camera_forward_maximum_distance.w,
            radiance,primary_transmittance);
      else if(deterministic_half_resolution())
        reconstructed_atmosphere(0.0,radiance,primary_transmittance);
      else
        radiance=orbital_primary_scattering(
            view_direction,1.0e9,terrain_shadow_weight,
            cached_shadow_visibility,primary_transmittance);
    }else{
      radiance=sample_sky_view(view_direction,texture_coordinate);
    }
  }
  if(ground_distance<0.0&&
     dot(view_direction,sun_direction)>cos(atmosphere.profile_and_mode.z)&&
     !planet_blocks_view(view_direction)){
    const float altitude=length(atmosphere.camera_position_near.xyz)-
        atmosphere.rayleigh_ground_radius.w;
    const float cosine_angle=dot(
        normalize(atmosphere.camera_position_near.xyz),view_direction);
    const vec2 transmittance_uv=atmosphere_transmittance_uv(
        altitude,cosine_angle);
    radiance+=atmosphere.solar_absorption_peak.rgb*
        texture(transmittance_lut,transmittance_uv).rgb*24.0;
  }
  return radiance;
}

void main() {
  // Manual fixed exposure is the deterministic qualification default. Scene
  // depth is deliberately bound here as the contract for the following
  // depth-aware atmosphere composition gate.
  const vec4 scene_sample=texture(hdr_scene,texture_coordinate);
  vec3 hdr=max(scene_sample.rgb,vec3(0.0));
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
      lookup=texture(aerial_scattering_lut,vec3(aerial_direction_uv(
          atmosphere_view_direction(texture_coordinate),texture_coordinate),
          0.5));
      lookup.rgb*=4.0;
    }else if(debug_view==5)
      lookup=texture(aerial_transmittance_lut,vec3(aerial_direction_uv(
          atmosphere_view_direction(texture_coordinate),texture_coordinate),
          0.5));
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
#ifdef FAITHFUL_SHADOW_SPLIT
    }else if(debug_view==11){
      lookup=sample_long_shadow(
          atmosphere_view_direction(texture_coordinate));
      // Alpha is the strongest cascade occlusion encountered by this ray.
      // Displaying it separates missed cascade samples from a valid but
      // radiometrically small direct-scattering loss.
      diagnostic=vec3(lookup.a);
    }else if(debug_view==12){
      diagnostic=(vec3(1.0)-sample_long_shadow_visibility(
          atmosphere_view_direction(texture_coordinate)))*8.0;
    }else if(debug_view==13){
      diagnostic=sample_sky_view(
          atmosphere_view_direction(texture_coordinate),texture_coordinate);
    }else if(debug_view==14){
      const vec3 direction=atmosphere_view_direction(texture_coordinate);
      diagnostic=sample_sky_view(direction,texture_coordinate)*
          sample_long_shadow_visibility(direction);
    }else if(debug_view==15){
      diagnostic=vec3(texture(sun_shadow_map,
          vec3(texture_coordinate*
              shadow_cascades.atmosphere_shadow_metadata.w,4.0)).r);
    }else if(debug_view>=16&&debug_view<=18){
      vec3 transmittance,direct_radiance,multiple_radiance;
      orbital_primary_scattering_components(
          atmosphere_view_direction(texture_coordinate),1.0e9,0.0,
          vec3(1.0),transmittance,direct_radiance,multiple_radiance);
      diagnostic=debug_view==16?direct_radiance:
          (debug_view==17?multiple_radiance:
           direct_radiance*sample_long_shadow_visibility(
               atmosphere_view_direction(texture_coordinate)));
    }else if(debug_view==19){
      diagnostic=texture(hdr_scene,texture_coordinate).rgb;
    }else if(debug_view==20||debug_view==21){
      const float depth=texture(scene_depth,texture_coordinate).r;
      if(depth>1.0e-8){
        vec3 transmittance,direct_radiance,multiple_radiance;
        orbital_primary_scattering_components(
            atmosphere_view_direction(texture_coordinate),
            atmosphere.camera_position_near.w/depth,
            primary_shadow_cone_weight(
                atmosphere_view_direction(texture_coordinate)),
            sample_long_shadow_visibility(
                atmosphere_view_direction(texture_coordinate)),
            transmittance,direct_radiance,multiple_radiance);
        diagnostic=debug_view==20?direct_radiance:multiple_radiance;
      }
    }else if(debug_view==22){
      // Raw directional loss before final reconstruction filtering.
      diagnostic=texture(long_shadow_lut,texture_coordinate).rgb;
    }else if(debug_view==23||debug_view==24){
      diagnostic=sample_long_shadow(
          atmosphere_view_direction(texture_coordinate)).rgb;
#endif
    }else if(debug_view>=25&&debug_view<=27){
      diagnostic=texture(hdr_scene,texture_coordinate).rgb;
    }
    out_colour=vec4(linear_to_srgb(aces_fitted(max(diagnostic,vec3(0.0)))),1.0);
    return;
  }
  if(atmosphere.profile_and_mode.w>0.5){
    const float depth=texture(scene_depth,texture_coordinate).r;
    if(depth<=1.0e-8){
      hdr=background_atmosphere();
    }else{
      const float distance_metres=atmosphere.camera_position_near.w/depth;
      const float coverage=clamp(scene_sample.a,0.0,1.0);
      const vec3 surface_radiance=hdr/max(coverage,0.25);
      const vec3 terrain_radiance=composite_aerial(
          surface_radiance,distance_metres);
      hdr=coverage<0.999?
          mix(background_atmosphere(),terrain_radiance,coverage):
          terrain_radiance;
    }
  }
  out_colour=vec4(linear_to_srgb(aces_fitted(
      hdr*atmosphere.sun_direction_exposure.w)),1.0);
}
