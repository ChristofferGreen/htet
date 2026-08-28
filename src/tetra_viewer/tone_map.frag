#version 450

layout(location = 0) in vec2 texture_coordinate;
layout(location = 0) out vec4 out_colour;
layout(set = 0,binding = 0) uniform sampler2D hdr_scene;
layout(set = 0,binding = 1) uniform sampler2D scene_depth;

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

void main() {
  // Manual fixed exposure is the deterministic qualification default. Scene
  // depth is deliberately bound here as the contract for the following
  // depth-aware atmosphere composition gate.
  const float fixed_exposure=0.65;
  const vec3 hdr=max(texture(hdr_scene,texture_coordinate).rgb,vec3(0.0));
  out_colour=vec4(linear_to_srgb(aces_fitted(hdr*fixed_exposure)),1.0);
}
