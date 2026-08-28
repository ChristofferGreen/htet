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
  mat4 shadow_matrices[4];
  vec4 shadow_splits;
} shadow_cascades;

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

vec2 atmosphere_transmittance_uv(float altitude,float cosine_angle) {
  const float bottom=atmosphere.rayleigh_ground_radius.w;
  const float top=atmosphere.mie_scattering_top_radius.w;
  if(atmosphere.reserved1.y<0.5)
    return vec2(clamp(cosine_angle*0.5+0.5,0.0,1.0),
                clamp(altitude/(top-bottom),0.0,1.0));
  const float radius=bottom+clamp(altitude,0.0,top-bottom);
  const float horizon=sqrt(max(0.0,(top-bottom)*(top+bottom)));
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
  const vec3 local_up=normalize(atmosphere.camera_position_near.xyz);
  const vec3 sun_direction=normalize(atmosphere.sun_direction_exposure.xyz);
  vec3 sun_tangent=sun_direction-local_up*dot(sun_direction,local_up);
  if(dot(sun_tangent,sun_tangent)<1.0e-10){
    const vec3 reference=abs(local_up.z)<0.9?vec3(0.0,0.0,1.0):
        vec3(1.0,0.0,0.0);
    sun_tangent=reference-local_up*dot(reference,local_up);
  }
  sun_tangent=normalize(sun_tangent);
  const vec3 longitude_tangent=normalize(cross(local_up,sun_tangent));
  const float latitude=asin(clamp(dot(direction,local_up),-1.0,1.0));
  const float normalized_latitude=latitude/(0.5*3.14159265358979323846);
  const float mapped_latitude=abs(normalized_latitude)<1.0e-6?0.0:
      sign(normalized_latitude)*sqrt(abs(normalized_latitude));
  const float longitude=atan(dot(direction,longitude_tangent),
                             dot(direction,sun_tangent));
  return vec2(longitude/(2.0*3.14159265358979323846)+0.5,
              mapped_latitude*0.5+0.5);
}

vec3 sample_sky_view(vec3 direction,vec2 screen_uv) {
  if(atmosphere.reserved1.y<0.5)return texture(sky_view_lut,screen_uv).rgb;
  return texture(sky_view_lut,atmosphere_full_sky_uv(direction)).rgb;
}

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

vec2 atmosphere_sphere_roots(vec3 origin,vec3 direction,float radius) {
  const float projection=dot(origin,direction);
  const float discriminant=projection*projection-
      ((length(origin)-radius)*(length(origin)+radius));
  if(discriminant<0.0)return vec2(1.0,-1.0);
  const float root=sqrt(max(discriminant,0.0));
  return vec2(-projection-root,-projection+root);
}

bool atmosphere_medium_segment(vec3 origin,vec3 direction,
                               float maximum_distance,
                               out float begin,out float end) {
  const float ground=atmosphere.rayleigh_ground_radius.w;
  const float top=atmosphere.mie_scattering_top_radius.w;
  const vec2 outer=atmosphere_sphere_roots(origin,direction,top);
  begin=max(0.0,outer.x);
  end=min(maximum_distance,outer.y);
  if(end<=begin)return false;
  const vec2 ground_roots=atmosphere_sphere_roots(origin,direction,ground);
  if(ground_roots.y>=ground_roots.x)
    for(int root_index=0;root_index<2;++root_index){
      const float root=root_index==0?ground_roots.x:ground_roots.y;
      if(root>begin+1.0e-3)end=min(end,root);
    }
  return end>begin;
}

vec3 atmosphere_densities(float altitude) {
  const float rayleigh=exp(-max(altitude,0.0)/
      atmosphere.mie_absorption_rayleigh_scale.w);
  const float mie=exp(-max(altitude,0.0)/
      atmosphere.absorption_mie_scale.w);
  const float absorption=max(0.0,1.0-abs(altitude-
      atmosphere.solar_absorption_peak.w)/
      max(atmosphere.profile_and_mode.x,1.0));
  return altitude<0.0||altitude>
      atmosphere.mie_scattering_top_radius.w-
          atmosphere.rayleigh_ground_radius.w?
      vec3(0.0):vec3(rayleigh,mie,absorption);
}

vec3 atmosphere_extinction(vec3 density) {
  return atmosphere.rayleigh_ground_radius.rgb*density.x+
      (atmosphere.mie_scattering_top_radius.rgb+
       atmosphere.mie_absorption_rayleigh_scale.rgb)*density.y+
      atmosphere.absorption_mie_scale.rgb*density.z;
}

float atmosphere_rayleigh_phase(float cosine_angle) {
  return 3.0*(1.0+cosine_angle*cosine_angle)/
      (16.0*3.14159265358979323846);
}

float atmosphere_mie_phase(float cosine_angle) {
  const float g=atmosphere.ground_albedo_mie_anisotropy.w;
  return (1.0-g*g)/(4.0*3.14159265358979323846*pow(
      max(1.0+g*g-2.0*g*cosine_angle,1.0e-4),1.5));
}

float atmosphere_sun_visibility(vec3 point,vec3 sun_direction) {
  const float radial_distance=max(length(point),1.0);
  const float radius=atmosphere.rayleigh_ground_radius.w;
  const float altitude=max(radial_distance-radius,0.0);
  const float horizon_cosine=-sqrt(max(0.0,
      altitude*(radial_distance+radius)/(radial_distance*radial_distance)));
  const float cosine_width=max(sin(atmosphere.profile_and_mode.z),1.0e-5);
  return smoothstep(horizon_cosine-cosine_width,
                    horizon_cosine+cosine_width,
                    dot(point/radial_distance,sun_direction));
}

float atmosphere_cascade_visibility(vec3 point_world,int cascade) {
  const vec3 projected=(shadow_cascades.shadow_matrices[cascade]*
      vec4(point_world,1.0)).xyz;
  if(any(greaterThan(abs(projected.xy),vec2(1.0)))||
     projected.z<0.0||projected.z>1.0)return 1.0;
  const vec2 uv=projected.xy*0.5+0.5;
  const vec2 texel=1.0/vec2(textureSize(sun_shadow_map,0).xy);
  const float bias=0.00045*(1.0+float(cascade)*0.45);
  float visibility=0.0;
  for(int y=0;y<2;++y)for(int x=0;x<2;++x){
    const vec2 offset=(vec2(x,y)-0.5)*texel;
    const float blocker=texture(sun_shadow_map,
        vec3(uv+offset,float(cascade))).r;
    visibility+=projected.z-bias<=blocker?1.0:0.0;
  }
  const float footprint=max(abs(projected.x),abs(projected.y));
  const float footprint_fade=1.0-smoothstep(0.88,0.98,footprint);
  return mix(1.0,visibility*0.25,footprint_fade);
}

float atmosphere_terrain_shadow_visibility(vec3 point_metres) {
  const float metres_per_world=atmosphere.profile_and_mode.y;
  const vec3 point_world=atmosphere.reserved0.xyz+
      (point_metres-atmosphere.camera_position_near.xyz)/metres_per_world;
  const float distance_world=length(point_world-atmosphere.reserved0.xyz);
  int cascade=3;
  if(distance_world<shadow_cascades.shadow_splits.x)cascade=0;
  else if(distance_world<shadow_cascades.shadow_splits.y)cascade=1;
  else if(distance_world<shadow_cascades.shadow_splits.z)cascade=2;
  const float current=atmosphere_cascade_visibility(point_world,cascade);
  if(cascade==3){
    const float outer_fade=smoothstep(shadow_cascades.shadow_splits.w*0.80,
        shadow_cascades.shadow_splits.w,distance_world);
    return mix(current,1.0,outer_fade);
  }
  const float split=shadow_cascades.shadow_splits[cascade];
  const float previous=cascade==0?0.0:
      shadow_cascades.shadow_splits[cascade-1];
  const float blend=smoothstep(mix(previous,split,0.85),split,distance_world);
  return mix(current,atmosphere_cascade_visibility(point_world,cascade+1),
             blend);
}

vec3 atmosphere_multiple_source(vec3 point,vec3 sun_direction) {
  const float altitude=length(point)-atmosphere.rayleigh_ground_radius.w;
  const float sun_cosine=dot(normalize(point),sun_direction);
  const float atmosphere_height=atmosphere.mie_scattering_top_radius.w-
      atmosphere.rayleigh_ground_radius.w;
  const vec2 uv=vec2(clamp(sun_cosine*0.5+0.5,0.0,1.0),
      clamp(altitude/max(atmosphere_height,1.0),0.0,1.0));
  const vec3 density=atmosphere_densities(altitude);
  const vec3 scattering=atmosphere.rayleigh_ground_radius.rgb*density.x+
      atmosphere.mie_scattering_top_radius.rgb*density.y;
  return scattering*texture(multiple_scattering_lut,uv).rgb*
      atmosphere.solar_absorption_peak.rgb;
}

vec3 integrate_long_aerial(vec3 direction,float maximum_distance,
                           out vec3 path_transmittance) {
  const vec3 origin=atmosphere.camera_position_near.xyz;
  float begin,end;
  path_transmittance=vec3(1.0);
  if(!atmosphere_medium_segment(origin,direction,maximum_distance,begin,end))
    return vec3(0.0);
  const vec3 sun_direction=normalize(atmosphere.sun_direction_exposure.xyz);
  const float phase_cosine=dot(direction,sun_direction);
  const float rayleigh_phase=atmosphere_rayleigh_phase(phase_cosine);
  const float mie_phase=atmosphere_mie_phase(phase_cosine);
  vec3 radiance=vec3(0.0);
  vec3 transmittance=vec3(1.0);
  const int steps=32;
  const float segment_length=end-begin;
  for(int index=0;index<steps;++index){
    const float fraction_begin=float(index)/float(steps);
    const float fraction_end=float(index+1)/float(steps);
    const float interval_begin=begin+segment_length*fraction_begin*fraction_begin;
    const float interval_end=begin+segment_length*fraction_end*fraction_end;
    const float step_length=interval_end-interval_begin;
    const vec3 point=origin+direction*(0.5*(interval_begin+interval_end));
    const float altitude=length(point)-atmosphere.rayleigh_ground_radius.w;
    const vec3 density=atmosphere_densities(altitude);
    const vec3 local_extinction=atmosphere_extinction(density);
    const vec3 segment_transmittance=exp(-local_extinction*step_length);
    const float sun_cosine=dot(normalize(point),sun_direction);
    const vec3 solar_transmittance=texture(transmittance_lut,
        atmosphere_transmittance_uv(altitude,sun_cosine)).rgb;
    const vec3 direct_source=(
        atmosphere.rayleigh_ground_radius.rgb*density.x*rayleigh_phase+
        atmosphere.mie_scattering_top_radius.rgb*density.y*mie_phase)*
        solar_transmittance*atmosphere.solar_absorption_peak.rgb*
        atmosphere_sun_visibility(point,sun_direction)*
        atmosphere_terrain_shadow_visibility(point);
    const vec3 source=direct_source+
        atmosphere_multiple_source(point,sun_direction);
    const vec3 integral=(vec3(1.0)-segment_transmittance)/
        max(local_extinction,vec3(1.0e-9));
    radiance+=transmittance*source*integral;
    transmittance*=segment_transmittance;
  }
  path_transmittance=transmittance;
  return radiance;
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
  const vec3 average_radiance=texture(
      multiple_scattering_lut,scattering_uv).rgb;
  const vec3 vertical_transmittance=texture(
      transmittance_lut,atmosphere_transmittance_uv(1.0,1.0)).rgb;
  const float daylight=smoothstep(-0.12,0.20,sun_cosine);
  const vec3 single_scattered_fill=atmosphere.solar_absorption_peak.rgb*
      (vec3(1.0)-vertical_transmittance)*daylight*0.60;
  const vec3 albedo=atmosphere.ground_albedo_mie_anisotropy.rgb;
  if(atmosphere.reserved1.y>0.5){
    const vec3 ambient=albedo*sample_sky_irradiance(normal);
    const vec3 direct=albedo/3.14159265359*
        atmosphere.solar_absorption_peak.rgb*solar_transmittance*n_dot_l*2.8;
    return ambient+direct;
  }

  // Keep the fitted ground model as the qualified baseline only.
  const vec3 environment=(average_radiance+single_scattered_fill)*2.0;
  const vec3 ambient=0.96*albedo*environment;
  const vec3 direct=0.96*albedo/3.14159265359*
      atmosphere.solar_absorption_peak.rgb*solar_transmittance*n_dot_l*2.8;
  return ambient+direct;
}

vec3 composite_aerial(vec3 surface_radiance,float distance_metres) {
  const float local_distance=atmosphere.camera_forward_maximum_distance.w;
  const float slice=pow(clamp(distance_metres/local_distance,0.0,1.0),
      1.0/3.0);
  const vec3 lookup=vec3(texture_coordinate,slice);
  vec3 scattering=texture(aerial_scattering_lut,lookup).rgb;
  const vec3 transmittance=texture(aerial_transmittance_lut,lookup).rgb;
  const vec3 local_result=surface_radiance*transmittance+scattering;
  if(atmosphere.reserved1.y>0.5&&distance_metres>local_distance*0.75){
    const vec3 direction=atmosphere_view_direction(texture_coordinate);
    // The faithful full-sky table is already a camera-position-dependent,
    // terrain-shadowed integration over the complete atmospheric segment.
    // Reuse it for far opaque geometry instead of repeating the same 32-step
    // march at every full-resolution fragment.  The endpoint/top ratio gives
    // transmittance only over camera-to-surface, including elevated terrain.
    const vec3 endpoint=atmosphere.camera_position_near.xyz+
        direction*distance_metres;
    const vec3 reverse_direction=-direction;
    const float endpoint_altitude=length(endpoint)-
        atmosphere.rayleigh_ground_radius.w;
    const float camera_altitude=length(atmosphere.camera_position_near.xyz)-
        atmosphere.rayleigh_ground_radius.w;
    const vec3 endpoint_to_top=texture(transmittance_lut,
        atmosphere_transmittance_uv(endpoint_altitude,
            dot(normalize(endpoint),reverse_direction))).rgb;
    const vec3 camera_to_top=texture(transmittance_lut,
        atmosphere_transmittance_uv(camera_altitude,
            dot(normalize(atmosphere.camera_position_near.xyz),
                reverse_direction))).rgb;
    const vec3 long_transmittance=clamp(endpoint_to_top/
        max(camera_to_top,vec3(1.0e-6)),vec3(0.0),vec3(1.0));
    const vec3 long_scattering=sample_sky_view(
        direction,texture_coordinate);
    const vec3 long_result=surface_radiance*long_transmittance+long_scattering;
    const float handoff=smoothstep(local_distance*0.75,local_distance,
                                   distance_metres);
    return mix(local_result,long_result,handoff);
  }
  if(atmosphere.reserved1.y>0.5)return local_result;
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
      const vec3 view_direction=atmosphere_view_direction(texture_coordinate);
      const vec3 sun_direction=normalize(atmosphere.sun_direction_exposure.xyz);
      const float ground_distance=ground_intersection_distance(view_direction);
      if(ground_distance>0.0){
        hdr=composite_aerial(
            analytic_ground_radiance(view_direction,ground_distance),
            ground_distance);
      }else{
        hdr=sample_sky_view(view_direction,texture_coordinate);
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
        hdr+=atmosphere.solar_absorption_peak.rgb*
            texture(transmittance_lut,transmittance_uv).rgb*24.0;
      }
    }else{
      const float distance_metres=atmosphere.camera_position_near.w/depth;
      hdr=composite_aerial(hdr,distance_metres);
    }
  }
  out_colour=vec4(linear_to_srgb(aces_fitted(
      hdr*atmosphere.sun_direction_exposure.w)),1.0);
}
