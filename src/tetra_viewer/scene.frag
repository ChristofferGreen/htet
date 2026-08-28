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
    // Conventional realtime dielectric BRDF: fixed world-space sun,
    // per-fragment view direction, GGX distribution, Smith masking, Schlick
    // Fresnel, and a stable diffuse environment term.
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
    const float sky_mix=clamp(unit_normal.y*0.5+0.5,0.0,1.0);
    const vec3 environment=mix(vec3(0.08,0.075,0.07),
        vec3(0.24,0.28,0.34),sky_mix);
    const vec3 ambient=albedo*environment;
    const float shadow=sun_visibility(fragment_position,n_dot_l);
    const vec3 linear_colour=ambient+
        (diffuse+specular)*n_dot_l*vec3(2.8,2.60,2.30)*shadow;
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
