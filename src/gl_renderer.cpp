#include "gl_renderer.h"

#include <SDL_opengl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// GL 2.0 entry points are not exported by opengl32.dll: load them through SDL.
PFNGLCREATESHADERPROC glCreateShader_;
PFNGLSHADERSOURCEPROC glShaderSource_;
PFNGLCOMPILESHADERPROC glCompileShader_;
PFNGLGETSHADERIVPROC glGetShaderiv_;
PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog_;
PFNGLCREATEPROGRAMPROC glCreateProgram_;
PFNGLATTACHSHADERPROC glAttachShader_;
PFNGLLINKPROGRAMPROC glLinkProgram_;
PFNGLGETPROGRAMIVPROC glGetProgramiv_;
PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog_;
PFNGLUSEPROGRAMPROC glUseProgram_;
PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation_;
PFNGLUNIFORM1FPROC glUniform1f_;
PFNGLUNIFORM2FPROC glUniform2f_;
PFNGLUNIFORM1IPROC glUniform1i_;
PFNGLDELETESHADERPROC glDeleteShader_;
PFNGLDELETEPROGRAMPROC glDeleteProgram_;

template <class T>
bool load(T& fn, const char* name) {
    fn = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
    return fn != nullptr;
}

bool load_gl_functions() {
    return load(glCreateShader_, "glCreateShader") && load(glShaderSource_, "glShaderSource") &&
           load(glCompileShader_, "glCompileShader") && load(glGetShaderiv_, "glGetShaderiv") &&
           load(glGetShaderInfoLog_, "glGetShaderInfoLog") && load(glCreateProgram_, "glCreateProgram") &&
           load(glAttachShader_, "glAttachShader") && load(glLinkProgram_, "glLinkProgram") &&
           load(glGetProgramiv_, "glGetProgramiv") && load(glGetProgramInfoLog_, "glGetProgramInfoLog") &&
           load(glUseProgram_, "glUseProgram") && load(glGetUniformLocation_, "glGetUniformLocation") &&
           load(glUniform1f_, "glUniform1f") && load(glUniform2f_, "glUniform2f") && load(glUniform1i_, "glUniform1i") &&
           load(glDeleteShader_, "glDeleteShader") && load(glDeleteProgram_, "glDeleteProgram");
}

const char* kVertex = R"(#version 120
varying vec2 vUV;
void main() {
    gl_Position = gl_Vertex;
    vUV = gl_MultiTexCoord0.xy;
}
)";

// CRT shader. All effects are scaled by uniforms in 0..1 (0 = off), so with everything at 0 the picture is just
// scaled with the selected filter. Colour mixing happens in linear light.
const char* kFragment = R"(#version 120
uniform sampler2D tex;
uniform vec2 srcSize;     // game picture size in pixels (256 x 224)
uniform vec2 outSize;     // size of the output rectangle in window pixels
uniform float scan, mask, curv, glow, vig, bright, sat;
uniform int filterMode;   // 0 sharp bilinear, 1 linear, 2 nearest
varying vec2 vUV;

vec3 lin(vec3 c) { return pow(c, vec3(2.2)); }

void main() {
    // --- screen curvature: pushes the corners outwards so the picture bulges; outside = black bezel ---
    vec2 c = vUV * 2.0 - 1.0;
    c += c * vec2(c.y * c.y, c.x * c.x) * (0.30 * curv);
    vec2 cuv = c * 0.5 + 0.5;
    if (cuv.x < 0.0 || cuv.x > 1.0 || cuv.y < 0.0 || cuv.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec2 e = min(cuv, 1.0 - cuv);
    float edge = curv > 0.0 ? smoothstep(0.0, 0.010, min(e.x, e.y)) : 1.0;

    // --- sampling position ---
    vec2 p = cuv * srcSize;
    vec2 ip = floor(p);
    vec2 fp = p - ip;
    vec2 ff;
    if (filterMode == 0) ff = clamp((fp - 0.5) * (outSize / srcSize) + 0.5, 0.0, 1.0);  // sharp bilinear
    else if (filterMode == 1) ff = fp;                                                    // linear
    else ff = vec2(0.5);                                                                  // nearest
    vec2 suv = (ip + ff) / srcSize;
    suv.y = mix(suv.y, (ip.y + 0.5) / srcSize.y, scan);   // with scanlines every picture line stays one beam

    vec3 col = lin(texture2D(tex, suv).rgb);

    // --- glow: a little light from the neighbouring pixels ---
    if (glow > 0.0) {
        vec3 b = lin(texture2D(tex, suv + vec2( 1.5, 0.0) / srcSize).rgb)
               + lin(texture2D(tex, suv + vec2(-1.5, 0.0) / srcSize).rgb)
               + lin(texture2D(tex, suv + vec2(0.0,  1.5) / srcSize).rgb)
               + lin(texture2D(tex, suv + vec2(0.0, -1.5) / srcSize).rgb);
        col += b * 0.25 * glow * 0.6;
    }

    // --- scanlines: the beam is fatter for bright pixels ---
    if (scan > 0.0) {
        float fy = fp.y - 0.5;
        float lum = clamp(max(max(col.r, col.g), col.b), 0.0, 1.0);
        float sigma = mix(0.30, 0.55, lum);
        float beam = exp(-(fy * fy) / (2.0 * sigma * sigma));
        col *= mix(1.0, beam * 1.26, scan);   // 1.26 keeps the average brightness
    }

    // --- aperture grille: RGB stripes, 3 window pixels wide ---
    if (mask > 0.0) {
        float x = mod(floor(gl_FragCoord.x), 3.0);
        vec3 triad = vec3(x < 0.5 ? 1.0 : 0.0, (x > 0.5 && x < 1.5) ? 1.0 : 0.0, x > 1.5 ? 1.0 : 0.0);
        col *= mix(vec3(1.0), triad, mask) / (1.0 - mask * 0.6667);
    }

    float vgn = 1.0 - vig * 0.85 * dot(c, c) * 0.5;
    col *= bright * clamp(vgn, 0.0, 1.0) * edge;

    col = pow(max(col, 0.0), vec3(1.0 / 2.2));
    float l = dot(col, vec3(0.299, 0.587, 0.114));
    col = mix(vec3(l), col, sat);
    gl_FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
)";

unsigned make_shader(unsigned type, const char* src, std::string& err) {
    unsigned s = glCreateShader_(type);
    glShaderSource_(s, 1, &src, nullptr);
    glCompileShader_(s);
    int ok = 0;
    glGetShaderiv_(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog_(s, sizeof log, nullptr, log);
        err = std::string(type == GL_VERTEX_SHADER ? "vertex" : "fragment") + " shader: " + log;
        glDeleteShader_(s);
        return 0;
    }
    return s;
}

void setup_texture(unsigned tex) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void upload(unsigned tex, int& cur_w, int& cur_h, const uint32_t* px, int w, int h) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (cur_w != w || cur_h != h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
        cur_w = w;
        cur_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
    }
}

void quad() {
    glBegin(GL_TRIANGLE_STRIP);
    glTexCoord2f(0.f, 0.f); glVertex2f(-1.f, 1.f);
    glTexCoord2f(1.f, 0.f); glVertex2f(1.f, 1.f);
    glTexCoord2f(0.f, 1.f); glVertex2f(-1.f, -1.f);
    glTexCoord2f(1.f, 1.f); glVertex2f(1.f, -1.f);
    glEnd();
}

}  // namespace

bool GlRenderer::init(SDL_Window* window, std::string& error) {
    window_ = window;
    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    if (!ctx) {
        error = std::string("no OpenGL context: ") + SDL_GetError();
        return false;
    }
    context_ = ctx;
    const char* ver = (const char*)glGetString(GL_VERSION);
    const char* rend = (const char*)glGetString(GL_RENDERER);
    info_ = std::string(rend ? rend : "?") + ", OpenGL " + (ver ? ver : "?");
    if (!load_gl_functions()) {
        error = "OpenGL 2.0 shader functions are not available (" + info_ + ")";
        shutdown();
        return false;
    }
    if (!compile(error)) {
        shutdown();
        return false;
    }
    SDL_GL_SetSwapInterval(1);
    glGenTextures(1, &tex_game_);
    glGenTextures(1, &tex_overlay_);
    setup_texture(tex_game_);
    setup_texture(tex_overlay_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    return true;
}

bool GlRenderer::compile(std::string& error) {
    unsigned vs = make_shader(GL_VERTEX_SHADER, kVertex, error);
    if (!vs) return false;
    unsigned fs = make_shader(GL_FRAGMENT_SHADER, kFragment, error);
    if (!fs) return false;
    prog_ = glCreateProgram_();
    glAttachShader_(prog_, vs);
    glAttachShader_(prog_, fs);
    glLinkProgram_(prog_);
    glDeleteShader_(vs);
    glDeleteShader_(fs);
    int ok = 0;
    glGetProgramiv_(prog_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog_(prog_, sizeof log, nullptr, log);
        error = std::string("shader link: ") + log;
        return false;
    }
    auto loc = [&](const char* n) { return glGetUniformLocation_(prog_, n); };
    u_.tex = loc("tex"); u_.src_size = loc("srcSize"); u_.out_size = loc("outSize"); u_.scan = loc("scan");
    u_.mask = loc("mask"); u_.curv = loc("curv"); u_.glow = loc("glow"); u_.vig = loc("vig");
    u_.bright = loc("bright"); u_.sat = loc("sat"); u_.filter = loc("filterMode");
    return true;
}

void GlRenderer::shutdown() {
    if (context_) {
        if (prog_) glDeleteProgram_(prog_);
        if (tex_game_) glDeleteTextures(1, &tex_game_);
        if (tex_overlay_) glDeleteTextures(1, &tex_overlay_);
        if (tex_bezel_) glDeleteTextures(1, &tex_bezel_);
        SDL_GL_DeleteContext((SDL_GLContext)context_);
        context_ = nullptr;
    }
    prog_ = tex_game_ = tex_overlay_ = tex_bezel_ = 0;
}

void GlRenderer::set_bezel(const uint32_t* argb, int w, int h) {
    if (!tex_bezel_) glGenTextures(1, &tex_bezel_);
    glBindTexture(GL_TEXTURE_2D, tex_bezel_);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);   // the frame is usually shown smaller than the image
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, argb);
}

void GlRenderer::draw(const uint32_t* game, int gw, int gh, const VideoSettings& vs, int win_w, int win_h,
                      const SDL_Rect& dst, const uint32_t* overlay, int ow, int oh, const SDL_Rect* bezel_dst, bool bezel_on_top) {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glViewport(0, 0, win_w, win_h);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    auto draw_bezel = [&]() {
        if (!bezel_dst || !tex_bezel_) return;
        glViewport(bezel_dst->x, win_h - bezel_dst->y - bezel_dst->h, bezel_dst->w, bezel_dst->h);
        glUseProgram_(0);
        glEnable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glBindTexture(GL_TEXTURE_2D, tex_bezel_);
        quad();
        glDisable(GL_BLEND);
    };
    if (!bezel_on_top) draw_bezel();   // a plain background picture goes first

    upload(tex_game_, game_w_, game_h_, afterglow(game, gw, gh, vs.persistence), gw, gh);
    glViewport(dst.x, win_h - dst.y - dst.h, dst.w, dst.h);
    glUseProgram_(prog_);
    glUniform1i_(u_.tex, 0);
    glUniform2f_(u_.src_size, (float)gw, (float)gh);
    glUniform2f_(u_.out_size, (float)dst.w, (float)dst.h);
    glUniform1f_(u_.scan, vs.scanlines / 100.f);
    glUniform1f_(u_.mask, vs.mask / 100.f);
    glUniform1f_(u_.curv, vs.curvature / 100.f);
    glUniform1f_(u_.glow, vs.glow / 100.f);
    glUniform1f_(u_.vig, vs.vignette / 100.f);
    glUniform1f_(u_.bright, vs.brightness / 100.f);
    glUniform1f_(u_.sat, vs.saturation / 100.f);
    glUniform1i_(u_.filter, vs.filter);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex_game_);
    quad();

    glUseProgram_(0);
    if (bezel_on_top) draw_bezel();

    if (overlay) {
        glViewport(dst.x, win_h - dst.y - dst.h, dst.w, dst.h);
        glUseProgram_(0);
        upload(tex_overlay_, overlay_w_, overlay_h_, overlay, ow, oh);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        quad();
        glDisable(GL_BLEND);
    }
    glUseProgram_(0);
    glDisable(GL_TEXTURE_2D);
}

// Each phosphor dot keeps glowing after the beam has passed: per channel the shown light is the new picture or the
// faded previous light, whichever is brighter. Done in linear light on the 256x224 picture, so it is cheap on the CPU.
const uint32_t* GlRenderer::afterglow(const uint32_t* game, int w, int h, int persistence) {
    const size_t n = (size_t)w * h;
    if (persistence <= 0) {
        glow_lin_.clear();
        return game;
    }
    static float to_lin[256];
    static uint8_t to_srgb[4096];
    static bool tables = false;
    if (!tables) {
        for (int i = 0; i < 256; i++) to_lin[i] = std::pow(i / 255.f, 2.2f);
        for (int i = 0; i < 4096; i++) to_srgb[i] = (uint8_t)(std::pow(i / 4095.f, 1.f / 2.2f) * 255.f + 0.5f);
        tables = true;
    }
    // fraction of the light left after one frame (60 Hz): 100% -> about 0.85, a trail of roughly half a second
    const float decay = 0.85f * std::pow(persistence / 100.f, 0.5f);
    if (glow_lin_.size() != n * 3) glow_lin_.assign(n * 3, 0.f);
    glow_px_.resize(n);
    float* g = glow_lin_.data();
    for (size_t i = 0; i < n; i++, g += 3) {
        const uint32_t p = game[i];
        uint32_t out = 0xFF000000u;
        for (int c = 0; c < 3; c++) {
            const int shift = 16 - 8 * c;
            const float v = std::max(to_lin[(p >> shift) & 0xFF], g[c] * decay);
            g[c] = v;
            out |= (uint32_t)to_srgb[(int)(v * 4095.f + 0.5f)] << shift;
        }
        glow_px_[i] = out;
    }
    return glow_px_.data();
}

bool GlRenderer::read_pixels(std::vector<uint32_t>& out, int& w, int& h) {
    SDL_GL_GetDrawableSize(window_, &w, &h);
    if (w <= 0 || h <= 0) return false;
    std::vector<uint32_t> raw((size_t)w * h);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, raw.data());
    out.resize(raw.size());
    for (int y = 0; y < h; y++)
        std::memcpy(&out[(size_t)y * w], &raw[(size_t)(h - 1 - y) * w], (size_t)w * 4);
    return true;
}

void GlRenderer::swap() { SDL_GL_SwapWindow(window_); }

void GlRenderer::set_vsync(bool on) { SDL_GL_SetSwapInterval(on ? 1 : 0); }
