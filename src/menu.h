// In-game menus: F1 = DIP switches (SW1/SW2), F2 = cheats. Drawn into an ARGB overlay buffer with a
// built-in 8x8 font, so there is no dependency on SDL_ttf. The caller feeds navigation keys and shows
// the overlay on top of the game picture.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <vector>

#include "cheats.h"
#include "video_settings.h"

class Menu {
public:
    enum class Page { None, Dip, Cheats, Video };
    enum class Key { Up, Down, Left, Right, Enter, Back };

    struct Hooks {
        std::function<uint16_t()> get_dsw;
        std::function<void(uint16_t)> set_dsw;
        std::function<void()> reset_game;
        std::function<void()> settings_changed;  // persist settings (ini)
        cheat::Engine* cheats = nullptr;
        std::string cheat_file_note;             // shown on the cheat page when no cheats are loaded
        VideoSettings* video = nullptr;          // F4 page
        std::function<void()> video_changed;
        std::function<bool()> shader_available;  // CRT effects need the Vulkan or OpenGL renderer
        bool bezel_available = false;            // a bezel image was found
        std::function<std::string()> active_renderer;   // the renderer in use, for the F4 page
    };

    static constexpr int kWidth = 512;   // overlay size; it is stretched over the game picture
    static constexpr int kHeight = 448;

    void set_hooks(Hooks h) { hooks_ = std::move(h); }
    bool active() const { return page_ != Page::None; }
    Page page() const { return page_; }
    void open(Page p);
    void close() { page_ = Page::None; }
    void toggle(Page p) { if (page_ == p) close(); else open(p); }

    void key(Key k);
    // Fills `argb` (kWidth*kHeight pixels) with the menu (transparent where nothing is drawn).
    void render(uint32_t* argb);

private:
    struct Row {
        std::string left, right;
        int kind = 0;      // 0 = separator/empty, 1 = selectable item, 2 = action, 3 = info line
        int ref = -1;      // dip / cheat index
        bool dim = false;
    };
    void build_rows();
    int move(int from, int dir) const;
    void change(int dir);
    void activate();

    Page page_ = Page::None;
    Hooks hooks_;
    std::vector<Row> rows_;
    int sel_ = 0;
    int top_ = 0;
};
