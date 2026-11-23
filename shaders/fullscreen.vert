#version 460
// One triangle covering the viewport; o_ndc in [-1, 1]
layout(location = 0) out vec2 o_ndc;

void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    o_ndc = p;
    gl_Position = vec4(p, 0.0, 1.0);
}
