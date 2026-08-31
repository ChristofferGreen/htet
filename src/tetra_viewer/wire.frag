#version 450

layout(location = 0) in vec3 colour;
layout(location = 2) flat in vec2 diagnostics;
layout(location = 4) in float world_x;
layout(location = 0) out vec4 out_colour;

layout(push_constant) uniform Camera {
  mat4 view_projection;
  vec4 light_direction;
  vec4 rendering;
} camera;

void main() {
  const bool connected_surface=diagnostics.x<-1.5;
  const bool volume_face=diagnostics.x<-0.5&&!connected_surface;
  if(!volume_face&&!connected_surface&&world_x>camera.rendering.w)discard;
  const bool enabled=volume_face
      ? camera.light_direction.w>0.5
      : camera.rendering.y>0.5;
  if(!enabled)discard;
  vec3 edge_colour;
  if(connected_surface)edge_colour=vec3(0.05,0.17,0.15);
  else if(volume_face&&colour.r>0.7)edge_colour=vec3(0.18,0.055,0.015);
  else edge_colour=vec3(0.045,0.13,0.16);
  out_colour=vec4(edge_colour,1.0);
}
