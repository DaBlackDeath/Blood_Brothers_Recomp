#version 450
// CRT shader, the Vulkan twin of the GLSL 1.20 shader in gl_renderer.cpp (keep both in step).
// All effects are scaled by values in 0..1 (0 = off), so with everything at 0 the picture is just
// scaled with the selected filter. Colour mixing happens in linear light.
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform Params {
    vec2 srcSize;     // game picture size in pixels (256 x 224)
    vec2 outSize;     // size of the output rectangle in window pixels
    float scan, mask, curv, glow, vig, bright, sat;
    int filterMode;   // 0 sharp bilinear, 1 linear, 2 nearest
} pc;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 lin(vec3 c) { return pow(c, vec3(2.2)); }

void main() {
    // --- screen curvature: pushes the corners outwards so the picture bulges; outside = black bezel ---
    vec2 c = vUV * 2.0 - 1.0;
    c += c * vec2(c.y * c.y, c.x * c.x) * (0.30 * pc.curv);
    vec2 cuv = c * 0.5 + 0.5;
    if (cuv.x < 0.0 || cuv.x > 1.0 || cuv.y < 0.0 || cuv.y > 1.0) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec2 e = min(cuv, 1.0 - cuv);
    float edge = pc.curv > 0.0 ? smoothstep(0.0, 0.010, min(e.x, e.y)) : 1.0;

    // --- sampling position ---
    vec2 p = cuv * pc.srcSize;
    vec2 ip = floor(p);
    vec2 fp = p - ip;
    vec2 ff;
    if (pc.filterMode == 0) ff = clamp((fp - 0.5) * (pc.outSize / pc.srcSize) + 0.5, 0.0, 1.0);  // sharp bilinear
    else if (pc.filterMode == 1) ff = fp;                                                       // linear
    else ff = vec2(0.5);                                                                        // nearest
    vec2 suv = (ip + ff) / pc.srcSize;
    suv.y = mix(suv.y, (ip.y + 0.5) / pc.srcSize.y, pc.scan);   // with scanlines every picture line stays one beam

    vec3 col = lin(texture(tex, suv).rgb);

    // --- glow: a little light from the neighbouring pixels ---
    if (pc.glow > 0.0) {
        vec3 b = lin(texture(tex, suv + vec2( 1.5, 0.0) / pc.srcSize).rgb)
               + lin(texture(tex, suv + vec2(-1.5, 0.0) / pc.srcSize).rgb)
               + lin(texture(tex, suv + vec2(0.0,  1.5) / pc.srcSize).rgb)
               + lin(texture(tex, suv + vec2(0.0, -1.5) / pc.srcSize).rgb);
        col += b * 0.25 * pc.glow * 0.6;
    }

    // --- scanlines: the beam is fatter for bright pixels ---
    if (pc.scan > 0.0) {
        float fy = fp.y - 0.5;
        float lum = clamp(max(max(col.r, col.g), col.b), 0.0, 1.0);
        float sigma = mix(0.30, 0.55, lum);
        float beam = exp(-(fy * fy) / (2.0 * sigma * sigma));
        col *= mix(1.0, beam * 1.26, pc.scan);   // 1.26 keeps the average brightness
    }

    // --- aperture grille: RGB stripes, 3 window pixels wide ---
    if (pc.mask > 0.0) {
        float x = mod(floor(gl_FragCoord.x), 3.0);
        vec3 triad = vec3(x < 0.5 ? 1.0 : 0.0, (x > 0.5 && x < 1.5) ? 1.0 : 0.0, x > 1.5 ? 1.0 : 0.0);
        col *= mix(vec3(1.0), triad, pc.mask) / (1.0 - pc.mask * 0.6667);
    }

    float vgn = 1.0 - pc.vig * 0.85 * dot(c, c) * 0.5;
    col *= pc.bright * clamp(vgn, 0.0, 1.0) * edge;

    col = pow(max(col, 0.0), vec3(1.0 / 2.2));
    float l = dot(col, vec3(0.299, 0.587, 0.114));
    col = mix(vec3(l), col, pc.sat);
    outColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
