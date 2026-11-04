#version 450
// Bezel and menu overlay: the texture as it is (alpha blending is set in the pipeline).
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(tex, vUV);
}
