#version 450

layout(location = 0) in vec2 disc_point;
layout(location = 0) out vec4 out_colour;

void main() {
  const float radius=length(disc_point);
  if(radius>1.0)discard;
  const float limb=smoothstep(1.0,0.15,radius);
  const vec3 warm=vec3(1.0,0.64,0.12);
  const vec3 centre=vec3(1.0,0.96,0.72);
  out_colour=vec4(mix(warm,centre,limb),1.0);
}
