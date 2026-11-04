#include "renderer.h"

#include <algorithm>
#include <cmath>

#include "gl_renderer.h"
#ifdef BLOODBRO_HAVE_VULKAN
#include "vk_renderer.h"
#endif

// Each phosphor dot keeps glowing after the beam has passed: per channel the shown light is the new picture or the
// faded previous light, whichever is brighter. Done in linear light on the 256x224 picture, so it is cheap on the CPU.
const uint32_t* GpuRenderer::afterglow(const uint32_t* game, int w, int h, int persistence) {
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

std::unique_ptr<GpuRenderer> make_renderer(RendererApi api) {
    switch (api) {
    case RendererApi::OpenGL: return std::make_unique<GlRenderer>();
    case RendererApi::Vulkan:
#ifdef BLOODBRO_HAVE_VULKAN
        return std::make_unique<VkRenderer>();
#else
        return nullptr;
#endif
    }
    return nullptr;
}

SDL_WindowFlags renderer_window_flag(RendererApi api) {
    return api == RendererApi::Vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL;
}

void renderer_prepare_window(RendererApi api) {
    if (api != RendererApi::OpenGL) return;
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}
