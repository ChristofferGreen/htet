#version 450

layout(location = 0) in vec3 colour;
layout(location = 1) flat in vec3 normal;
layout(location = 2) flat in vec2 diagnostics;
layout(location = 3) noperspective in vec3 barycentric;
layout(location = 4) in float world_x;
layout(location = 5) flat in float in_edge_flags;
layout(location = 6) in vec3 fragment_position;
layout(location = 7) in vec3 smooth_normal;
layout(location = 0) out vec4 out_colour;
layout(set = 0, binding = 0) uniform sampler2DArray sun_shadow_map;
layout(std140,set=0,binding=1) uniform ShadowCascades {
  mat4 shadow_matrices[4];
  vec4 shadow_splits;
} shadow_cascades;
layout(set=0,binding=2) uniform sampler2D transmittance_lut;
layout(set=0,binding=3) uniform sampler2D multiple_scattering_lut;
layout(std140,set=0,binding=4) uniform Atmosphere {
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
layout(set=0,binding=5) uniform sampler2D sky_irradiance_lut;

layout(push_constant) uniform Camera {
  mat4 view_projection;
  vec4 light_direction;
  vec4 rendering;
  vec4 view_position;
} camera;

float cascade_visibility(vec3 position,float n_dot_l,int cascade) {
  const vec3 projected=(shadow_cascades.shadow_matrices[cascade]*
      vec4(position,1.0)).xyz;
  if(any(greaterThan(abs(projected.xy),vec2(1.0)))||
     projected.z<0.0||projected.z>1.0)return 1.0;
  const vec2 uv=projected.xy*0.5+0.5;
  const float bias=mix(0.00018,0.0012,1.0-n_dot_l)*
      (1.0+float(cascade)*0.45);
  const vec2 texel=1.0/vec2(textureSize(sun_shadow_map,0).xy);
  float visibility=0.0;
  for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){
    const float blocker=texture(sun_shadow_map,
        vec3(uv+vec2(x,y)*texel,float(cascade))).r;
    visibility+=projected.z-bias<=blocker?1.0:0.0;
  }
  return visibility/9.0;
}

float sun_visibility(vec3 position,float n_dot_l) {
  const float distance_from_camera=length(position-camera.view_position.xyz);
  int cascade=3;
  if(distance_from_camera<shadow_cascades.shadow_splits.x)cascade=0;
  else if(distance_from_camera<shadow_cascades.shadow_splits.y)cascade=1;
  else if(distance_from_camera<shadow_cascades.shadow_splits.z)cascade=2;
  const float current=cascade_visibility(position,n_dot_l,cascade);
  if(cascade==3)return current;
  const float split=shadow_cascades.shadow_splits[cascade];
  const float previous=cascade==0?0.0:
      shadow_cascades.shadow_splits[cascade-1];
  const float blend_start=mix(previous,split,0.85);
  const float blend=smoothstep(blend_start,split,distance_from_camera);
  return mix(current,cascade_visibility(position,n_dot_l,cascade+1),blend);
}

float atmosphere_sun_visibility(float radial_distance,float sun_cosine) {
  const float radius=atmosphere.rayleigh_ground_radius.w;
  const float altitude=max(radial_distance-radius,0.0);
  const float horizon_sine_squared=max(0.0,
      altitude*(radial_distance+radius)/(radial_distance*radial_distance));
  const float horizon_cosine=-sqrt(horizon_sine_squared);
  const float cosine_width=max(sin(atmosphere.profile_and_mode.z),1.0e-5);
  return smoothstep(horizon_cosine-cosine_width,
                    horizon_cosine+cosine_width,sun_cosine);
}

vec2 atmosphere_transmittance_uv(float altitude,float cosine_angle) {
  if(atmosphere.reserved1.y<0.5)
    return vec2(clamp(cosine_angle*0.5+0.5,0.0,1.0),
                clamp(altitude/(atmosphere.mie_scattering_top_radius.w-
                    atmosphere.rayleigh_ground_radius.w),0.0,1.0));
  const float bottom=atmosphere.rayleigh_ground_radius.w;
  const float top=atmosphere.mie_scattering_top_radius.w;
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

vec3 atmosphere_terrain_lighting(vec3 position,vec3 surface_normal,
                                 out vec3 direct_sun) {
  if(atmosphere.profile_and_mode.w<0.5){
    const float sky_mix=clamp(surface_normal.y*0.5+0.5,0.0,1.0);
    direct_sun=vec3(2.8,2.60,2.30);
    return mix(vec3(0.08,0.075,0.07),vec3(0.24,0.28,0.34),sky_mix);
  }
  const float metres=atmosphere.profile_and_mode.y;
  const vec3 relative_metres=(position-camera.view_position.xyz)*metres;
  const vec3 planet_position=atmosphere.camera_position_near.xyz+
      relative_metres;
  const float radial_distance=max(length(planet_position),1.0);
  const vec3 local_up=planet_position/radial_distance;
  const float atmosphere_height=atmosphere.mie_scattering_top_radius.w-
      atmosphere.rayleigh_ground_radius.w;
  const float altitude=clamp(radial_distance-
      atmosphere.rayleigh_ground_radius.w,0.0,atmosphere_height);
  const vec3 sun_direction=normalize(atmosphere.sun_direction_exposure.xyz);
  const float sun_cosine=dot(local_up,sun_direction);
  const vec2 scattering_uv=vec2(clamp(sun_cosine*0.5+0.5,0.0,1.0),
      clamp(altitude/max(atmosphere_height,1.0),0.0,1.0));
  const vec2 transmittance_uv=atmosphere_transmittance_uv(
      altitude,sun_cosine);

  const float upward=clamp(dot(surface_normal,local_up),-1.0,1.0);
  const vec3 solar_transmittance=texture(
      transmittance_lut,transmittance_uv).rgb;
  const float planet_visibility=atmosphere_sun_visibility(
      radial_distance,sun_cosine);
  direct_sun=atmosphere.solar_absorption_peak.rgb*solar_transmittance*
      planet_visibility*2.8;
  if(atmosphere.reserved1.y>0.5)
    return sample_sky_irradiance(surface_normal);

  // The qualified baseline retains its fitted readability terms. The
  // faithful path above consumes the explicit cosine-convolved sky lookup.
  // Turn the retained incident-sky spherical average into a cosine-weighted
  // diffuse approximation for this surface normal.
  const vec3 average_radiance=texture(
      multiple_scattering_lut,scattering_uv).rgb;
  const float sky_hemisphere_weight=mix(0.35,2.0,upward*0.5+0.5);
  // The isotropic closure deliberately contains little first-order sky
  // energy. Recover a conservative coloured share of the vertical beam loss
  // so blue-hour terrain remains readable without a constant ambient floor.
  const vec3 vertical_transmittance=texture(transmittance_lut,
      atmosphere_transmittance_uv(altitude,1.0)).rgb;
  const float daylight=smoothstep(-0.12,0.20,sun_cosine);
  const vec3 single_scattered_fill=atmosphere.solar_absorption_peak.rgb*
      (vec3(1.0)-vertical_transmittance)*daylight*0.60;
  const vec3 sky_irradiance_over_pi=(average_radiance+
      single_scattered_fill)*sky_hemisphere_weight;
  const float ground_facing=max(-upward,0.0);
  const vec3 ground_bounce=atmosphere.ground_albedo_mie_anisotropy.rgb*
      direct_sun*max(sun_cosine,0.0)*ground_facing*0.25;
  return sky_irradiance_over_pi+ground_bounce;
}

vec3 angle_colour(float angle_degrees) {
  const vec3 blue = vec3(0.08, 0.20, 0.82);
  const vec3 cyan = vec3(0.05, 0.78, 0.92);
  const vec3 green = vec3(0.12, 0.78, 0.30);
  const vec3 yellow = vec3(0.96, 0.88, 0.12);
  const vec3 orange = vec3(1.00, 0.42, 0.06);
  const vec3 red = vec3(0.88, 0.04, 0.04);
  const vec3 magenta = vec3(0.82, 0.05, 0.70);
  const vec3 white = vec3(1.00, 0.96, 1.00);
  if (angle_degrees < 0.5) return blue;
  if (angle_degrees < 1.0) return mix(blue, cyan, (angle_degrees-0.5)/0.5);
  if (angle_degrees < 2.0) return mix(cyan, green, angle_degrees-1.0);
  if (angle_degrees < 5.0) return mix(green, yellow, (angle_degrees-2.0)/3.0);
  if (angle_degrees < 10.0) return mix(yellow, orange, (angle_degrees-5.0)/5.0);
  if (angle_degrees < 20.0) return mix(orange, red, (angle_degrees-10.0)/10.0);
  if (angle_degrees < 45.0) return mix(red, magenta, (angle_degrees-20.0)/25.0);
  return mix(magenta, white, clamp((angle_degrees-45.0)/45.0, 0.0, 1.0));
}

void main() {
  const bool volume_cut = diagnostics.x < -0.5;
  const bool connected_surface = diagnostics.x < -1.5;
  // Surface and hierarchy geometry use a geometric clip. The volume layer is
  // selected per cell on the CPU, so every retained tetrahedron stays whole
  // and lies completely on the visible side of the plane.
  if (!volume_cut && world_x > camera.rendering.w) discard;
  const int triangle_edge_flags = int(in_edge_flags+0.5);
  const bool wire_only_volume = volume_cut && (triangle_edge_flags & 8) != 0;
  const bool show_solid_faces = wire_only_volume ? false :
      (connected_surface ? camera.rendering.z > 0.5 :
       (volume_cut || camera.rendering.z > 0.5));
  const vec3 hidden_face_colour = vec3(0.06,0.08,0.11);
  const vec3 light_direction = normalize(camera.light_direction.xyz);
  const float normal_length = length(normal);
  if (normal_length <= 0.0001) {
    // Degenerate or deliberately unlit geometry must still obey the face
    // visibility controls. Lighting validity is not a draw-control escape.
    out_colour = vec4(show_solid_faces ? colour : hidden_face_colour, 1.0);
    return;
  }
  const int shading_model = int(camera.rendering.x+0.5);
  const bool smooth_surface = camera.view_position.w>0.5 &&
      shading_model==4 && (!volume_cut || connected_surface) &&
      length(smooth_normal)>0.0001;
  const vec3 unit_normal = normalize(smooth_surface?smooth_normal:normal);
  const vec3 up_seed = abs(light_direction.y) < 0.9 ? vec3(0.0,1.0,0.0) : vec3(1.0,0.0,0.0);
  const vec3 camera_right = normalize(cross(up_seed,light_direction));
  const vec3 camera_up = cross(light_direction,camera_right);
  const vec3 diagnostic_light = normalize(light_direction+0.55*camera_right+0.35*camera_up);
  const float diagnostic_relief = 0.78+0.22*max(dot(unit_normal,diagnostic_light),0.0);
  vec3 shaded_colour;
  if (shading_model == 1 && !volume_cut) {
    shaded_colour = angle_colour(diagnostics.x)*diagnostic_relief;
  } else if (shading_model == 2 && !volume_cut) {
    shaded_colour = angle_colour(diagnostics.y)*diagnostic_relief;
  } else if (shading_model == 3 && !volume_cut) {
    const vec3 reflected = reflect(-light_direction, unit_normal);
    const vec3 stripe_axis = normalize(vec3(0.35, 0.82, 0.45));
    const float wave = 0.5+0.5*cos(dot(reflected, stripe_axis)*56.5486678);
    const float stripe = smoothstep(0.18, 0.82, wave);
    shaded_colour = mix(vec3(0.025,0.055,0.10),vec3(0.88,0.96,1.0),stripe);
  } else if (shading_model == 4 && (!volume_cut || connected_surface)) {
    // Conventional realtime dielectric BRDF: world-space sun,
    // per-fragment view direction, GGX distribution, Smith masking, Schlick
    // Fresnel, and atmosphere-derived indirect illumination.
    const vec3 albedo=vec3(0.32,0.33,0.34);
    const float roughness=0.82;
    const float metallic=0.0;
    const vec3 sun_direction=light_direction;
    const vec3 view_delta=camera.view_position.xyz-fragment_position;
    const vec3 view_direction=length(view_delta)>1.0e-5?
        normalize(view_delta):sun_direction;
    const vec3 half_direction=normalize(sun_direction+view_direction);
    const float n_dot_l=max(dot(unit_normal,sun_direction),0.0);
    const float n_dot_v=max(dot(unit_normal,view_direction),0.0);
    const float n_dot_h=max(dot(unit_normal,half_direction),0.0);
    const float v_dot_h=max(dot(view_direction,half_direction),0.0);
    const float alpha=roughness*roughness;
    const float alpha_squared=alpha*alpha;
    const float denominator=n_dot_h*n_dot_h*(alpha_squared-1.0)+1.0;
    const float distribution=alpha_squared/
        max(3.14159265359*denominator*denominator,1.0e-5);
    const float k=(roughness+1.0)*(roughness+1.0)/8.0;
    const float geometry_v=n_dot_v/max(n_dot_v*(1.0-k)+k,1.0e-5);
    const float geometry_l=n_dot_l/max(n_dot_l*(1.0-k)+k,1.0e-5);
    const vec3 f0=mix(vec3(0.04),albedo,metallic);
    const vec3 fresnel=f0+(1.0-f0)*pow(1.0-v_dot_h,5.0);
    const vec3 specular=distribution*geometry_v*geometry_l*fresnel/
        max(4.0*n_dot_v*n_dot_l,1.0e-5);
    const vec3 diffuse=(1.0-fresnel)*(1.0-metallic)*albedo/3.14159265359;
    vec3 direct_sun;
    const vec3 environment=atmosphere_terrain_lighting(
        fragment_position,unit_normal,direct_sun);
    const vec3 ambient=(1.0-f0)*(1.0-metallic)*albedo*environment;
    const float shadow=sun_visibility(fragment_position,n_dot_l);
    const vec3 linear_colour=ambient+
        (diffuse+specular)*n_dot_l*direct_sun*shadow;
    // Keep scene radiance linear. The HDR composite owns exposure, tone
    // mapping, and the single display transfer for every material.
    shaded_colour=linear_colour;
  } else {
    // Cutaway faces are deliberately viewed from either side. One-sided
    // lighting makes intact back-facing tetrahedron faces look like holes.
    const float facing = dot(unit_normal, light_direction);
    const float diffuse = volume_cut ? abs(facing) : max(facing, 0.0);
    const float illumination = volume_cut ? 0.48+0.52*diffuse : 0.22+0.78*diffuse;
    shaded_colour = colour*illumination;
  }

  // Geometry establishes an opaque visibility buffer. Wire-only modes retain
  // a background-coloured depth mask; the dedicated screen-space edge pass
  // can then draw the front mesh without revealing rear edges.
  out_colour=vec4(show_solid_faces ? shaded_colour : hidden_face_colour,1.0);
}
