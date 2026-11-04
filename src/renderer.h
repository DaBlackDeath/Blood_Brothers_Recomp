// Common interface of the GPU renderers with the CRT shader (Vulkan: vk_renderer.*, OpenGL: gl_renderer.*).
// A renderer draws the 256x224 game picture into a rectangle of the window (aspect ratio and integer scaling
// are decided by the caller), optionally with a bezel picture and the menu overlay on top.
// If no GPU renderer can be started, the program falls back to the plain SDL renderer (no effects).
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "video_settings.h"

class GpuRenderer {
public:
    virtual ~GpuRenderer() = default;

    // The window must have been created with the flag returned by window_flag().
    virtual bool init(SDL_Window* window, bool vsync, std::string& error) = 0;
    virtual void shutdown() = 0;
    virtual const char* api_name() const = 0;     // "Vulkan" / "OpenGL"
    const std::string& info() const { return info_; }

    // The bezel picture (straight-alpha 0xAARRGGBB); call once after init().
    virtual void set_bezel(const uint32_t* argb, int w, int h) = 0;
    // Draws a complete frame (not shown yet: see present()). `bezel_dst` (optional) is where the bezel picture
    // goes; it is drawn over the game picture, or behind it when `bezel_on_top` is false.
    virtual void draw(const uint32_t* game_argb, int game_w, int game_h, const VideoSettings& vs, int win_w, int win_h,
                      const SDL_Rect& dst, const uint32_t* overlay_argb, int overlay_w, int overlay_h,
                      const SDL_Rect* bezel_dst = nullptr, bool bezel_on_top = true) = 0;
    // Reads the frame drawn by draw() (call before present()) as 0xAARRGGBB, top row first.
    virtual bool read_pixels(std::vector<uint32_t>& out, int& w, int& h) = 0;
    virtual void present() = 0;
    virtual void set_vsync(bool on) = 0;

protected:
    // Phosphor afterglow: mixes the fading previous picture into `game` and returns the result.
    const uint32_t* afterglow(const uint32_t* game, int w, int h, int persistence);

    std::string info_;

private:
    std::vector<float> glow_lin_;      // afterglow per channel, linear light
    std::vector<uint32_t> glow_px_;    // picture with afterglow, sent to the shader
};

enum class RendererApi { Vulkan, OpenGL };

// Creates a renderer object (not initialised); nullptr if the API was not built in.
std::unique_ptr<GpuRenderer> make_renderer(RendererApi api);
// The SDL window flag the API needs (SDL_WINDOW_VULKAN / SDL_WINDOW_OPENGL).
SDL_WindowFlags renderer_window_flag(RendererApi api);
// Called before the window is created (OpenGL: context attributes).
void renderer_prepare_window(RendererApi api);
