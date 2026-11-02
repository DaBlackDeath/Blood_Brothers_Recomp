// OpenGL 2.1 renderer with the CRT shader. It draws the 256x224 game picture into a rectangle of the
// window (aspect ratio and integer scaling are decided by the caller) and can draw the menu overlay on top.
// If OpenGL is not available, init() fails and the program falls back to the plain SDL renderer.
#pragma once

#include <SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "video_settings.h"

class GlRenderer {
public:
    ~GlRenderer() { shutdown(); }

    // The window must have been created with SDL_WINDOW_OPENGL.
    bool init(SDL_Window* window, std::string& error);
    void shutdown();
    const std::string& info() const { return info_; }

    // The bezel picture (straight-alpha 0xAARRGGBB); call once after init().
    void set_bezel(const uint32_t* argb, int w, int h);
    // Draws a complete frame into the back buffer (does not swap). `bezel_dst` (optional) is where the bezel picture
    // goes; it is drawn over the game picture, or behind it when `bezel_on_top` is false.
    void draw(const uint32_t* game_argb, int game_w, int game_h, const VideoSettings& vs, int win_w, int win_h,
              const SDL_Rect& dst, const uint32_t* overlay_argb, int overlay_w, int overlay_h,
              const SDL_Rect* bezel_dst = nullptr, bool bezel_on_top = true);
    // Reads the back buffer (call after draw(), before swap()) as 0xAARRGGBB, top row first.
    bool read_pixels(std::vector<uint32_t>& out, int& w, int& h);
    void swap();
    void set_vsync(bool on);

private:
    bool compile(std::string& error);
    // Phosphor afterglow: mixes the fading previous picture into `game` and returns the result.
    const uint32_t* afterglow(const uint32_t* game, int w, int h, int persistence);

    SDL_Window* window_ = nullptr;
    void* context_ = nullptr;
    std::string info_;
    unsigned prog_ = 0;
    unsigned tex_game_ = 0, tex_overlay_ = 0, tex_bezel_ = 0;
    int game_w_ = 0, game_h_ = 0, overlay_w_ = 0, overlay_h_ = 0;
    std::vector<float> glow_lin_;      // afterglow per channel, linear light
    std::vector<uint32_t> glow_px_;    // picture with afterglow, sent to the shader
    struct Uniforms { int tex, src_size, out_size, scan, mask, curv, glow, vig, bright, sat, filter; } u_{};
};
