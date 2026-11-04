#version 450
// A rectangle covering the viewport, drawn as a 4-vertex triangle strip without vertex buffers.
// uv (0,0) is the top left corner (Vulkan: y = -1 is the top of the viewport).
layout(location = 0) out vec2 vUV;

void main() {
    vec2 uv = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    vUV = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
