#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_colour;
layout(location = 2) in vec3 in_normal;
layout(location = 3) in vec2 in_diagnostics;
layout(location = 4) in vec3 in_barycentric;
layout(location = 5) in float in_edge_flags;
layout(location = 6) in vec3 in_smooth_normal;
layout(location = 0) out vec3 colour;
layout(location = 1) flat out vec3 normal;
layout(location = 2) flat out vec2 diagnostics;
layout(location = 3) noperspective out vec3 barycentric;
layout(location = 4) out float world_x;
layout(location = 5) flat out float edge_flags;
layout(location = 6) out vec3 fragment_position;
layout(location = 7) out vec3 smooth_normal;

layout(push_constant) uniform Camera {
  mat4 view_projection;
  vec4 light_direction;
  vec4 rendering;
  vec4 view_position;
} camera;

void main() {
  gl_Position = camera.view_projection * vec4(in_position, 1.0);
  colour = in_colour;
  normal = in_normal;
  diagnostics = in_diagnostics;
  barycentric = in_barycentric;
  world_x = in_position.x;
  edge_flags = in_edge_flags;
  fragment_position = in_position;
  smooth_normal = in_smooth_normal;
}
