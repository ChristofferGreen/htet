#version 450

layout(location = 0) out vec2 texture_coordinate;

void main() {
  const vec2 coordinate=vec2(
      float((gl_VertexIndex << 1) & 2),float(gl_VertexIndex & 2));
  texture_coordinate=coordinate;
  gl_Position=vec4(coordinate*2.0-1.0,0.0,1.0);
}
