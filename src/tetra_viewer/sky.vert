#version 450

layout(location = 0) out vec2 disc_point;

layout(push_constant) uniform Camera {
  mat4 view_projection;
  vec4 light_direction;
  vec4 rendering;
  vec4 view_position;
} camera;

void main() {
  const vec2 corners[6]=vec2[6](
      vec2(-1.0,-1.0),vec2(1.0,-1.0),vec2(1.0,1.0),
      vec2(-1.0,-1.0),vec2(1.0,1.0),vec2(-1.0,1.0));
  const vec4 clip=camera.view_projection*vec4(
      normalize(camera.light_direction.xyz),0.0);
  const vec2 corner=corners[gl_VertexIndex];
  const float vertical_radius=0.070;
  const float horizontal_scale=length(vec3(
      camera.view_projection[0][0],camera.view_projection[1][0],
      camera.view_projection[2][0]));
  const float vertical_scale=length(vec3(
      camera.view_projection[0][1],camera.view_projection[1][1],
      camera.view_projection[2][1]));
  const vec2 radius=vec2(
      vertical_radius*horizontal_scale/max(vertical_scale,1.0e-6),
      vertical_radius);
  const bool enabled=int(camera.rendering.x+0.5)==4;
  const vec2 centre=enabled&&clip.w>0.0?clip.xy/clip.w:vec2(4.0);
  gl_Position=vec4(centre+corner*radius,0.0,1.0);
  disc_point=corner;
}
