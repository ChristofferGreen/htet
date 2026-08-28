#version 450

layout(location = 0) in vec3 in_position;

layout(push_constant) uniform Camera {
  mat4 view_projection;
  vec4 light_direction;
  vec4 rendering;
  vec4 view_position;
} camera;

void main() {
  gl_Position=camera.view_projection*vec4(in_position,1.0);
}
