#version 450

layout(location = 0) in vec3 colour;
layout(location = 1) noperspective in float edge_distance;
layout(location = 0) out vec4 out_colour;

void main() {
  // The ribbon extends one pixel to either side of the mathematical edge.
  // Linear pixel coverage represents a one-pixel line plus its half-pixel
  // filter footprint; unlike smoothing inside a narrow quad, this cannot
  // disappear when the edge falls between sample centres.
  const float coverage = clamp(1.0-abs(edge_distance),0.0,1.0);
  out_colour = vec4(colour,coverage);
}
