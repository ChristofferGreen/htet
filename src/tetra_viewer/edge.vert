#version 450

layout(location = 0) in vec3 edge_start;
layout(location = 1) in vec3 edge_colour;
layout(location = 2) in vec3 edge_end;
layout(location = 3) in vec2 ribbon_corner;
layout(location = 0) out vec3 colour;
layout(location = 1) noperspective out float edge_distance;

layout(push_constant) uniform Camera {
  mat4 view_projection;
  vec4 light_direction;
  vec4 rendering;
  // Framebuffer width, height, and ribbon half-width in physical pixels.
  vec4 viewport;
} camera;

void main() {
  const vec4 start_clip = camera.view_projection * vec4(edge_start, 1.0);
  const vec4 end_clip = camera.view_projection * vec4(edge_end, 1.0);
  const vec2 start_ndc = start_clip.xy / start_clip.w;
  const vec2 end_ndc = end_clip.xy / end_clip.w;
  const vec2 pixel_delta = (end_ndc-start_ndc)*0.5*camera.viewport.xy;
  const float delta_length = length(pixel_delta);
  const vec2 perpendicular = delta_length > 1e-5
      ? vec2(-pixel_delta.y,pixel_delta.x)/delta_length : vec2(0.0,1.0);
  vec4 selected_clip = mix(start_clip,end_clip,ribbon_corner.x);
  const vec2 offset_ndc = perpendicular*ribbon_corner.y*
      (2.0*camera.viewport.z/camera.viewport.xy);
  selected_clip.xy += offset_ndc*selected_clip.w;
  // Main camera depth is reversed: larger values are closer.
  selected_clip.z+=5.0e-6*selected_clip.w;
  gl_Position = selected_clip;
  colour = edge_colour;
  edge_distance = ribbon_corner.y*camera.viewport.z;
}
