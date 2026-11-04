// OpenGL 2.1 renderer with the CRT shader (GLSL 1.20). See renderer.h for the interface.
#pragma once

#include "renderer.h"

class GlRenderer final : public GpuRenderer {
public:
    ~GlRenderer() override { shutdown(); }

    bool init(SDL_Window* window, bool vsync, std::string& error) override;
    void shutdown() override;
    const char* api_name() const override { return "OpenGL"; }

    void set_bezel(const uint32_t* argb, int w, int h) override;
    void draw(const uint32_t* game_argb, int game_w, int game_h, const VideoSettings& vs, int win_w, int win_h,
              const SDL_Rect& dst, const uint32_t* overlay_argb, int overlay_w, int overlay_h,
              const SDL_Rect* bezel_dst = nullptr, bool bezel_on_top = true) override;
    bool read_pixels(std::vector<uint32_t>& out, int& w, int& h) override;
    void present() override;
    void set_vsync(bool on) override;

private:
    bool compile(std::string& error);

    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    unsigned prog_ = 0;
    unsigned tex_game_ = 0, tex_overlay_ = 0, tex_bezel_ = 0;
    int game_w_ = 0, game_h_ = 0, overlay_w_ = 0, overlay_h_ = 0;
    struct Uniforms { int tex, src_size, out_size, scan, mask, curv, glow, vig, bright, sat, filter; } u_{};
};
