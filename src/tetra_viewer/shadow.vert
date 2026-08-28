#version 450

layout(location = 0) in vec3 in_position;

layout(push_constant) uniform Camera {
  mat4 view_projection;
  vec4 light_direction;
  vec4 rendering;
  vec4 view_position;
} camera;

// Terrain vertices are already relative to the snapped render origin. This
// stable orthographic sun volume covers the complete current world prototype;
// planet-scale rendering will replace it with cascades anchored to pages.
void main() {
  const vec3 sun_direction=normalize(camera.light_direction.xyz);
  const vec3 sun_right=normalize(cross(vec3(0.0,1.0,0.0),sun_direction));
  const vec3 sun_up=cross(sun_direction,sun_right);
  const float radius=64.0;
  const float x=dot(in_position,sun_right)/radius;
  const float y=dot(in_position,sun_up)/radius;
  const float depth=(radius-dot(in_position,sun_direction))/(2.0*radius);
  gl_Position=vec4(x,y,depth,1.0);
}
