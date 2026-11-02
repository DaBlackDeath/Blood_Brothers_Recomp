// Bezel (monitor frame) image: a PNG with a transparent "screen" window. The game picture is drawn into
// that window, the frame on top of it. If the PNG has no transparent window it is used as a background.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Bezel {
    int w = 0, h = 0;
    std::vector<uint32_t> argb;   // straight (not premultiplied) 0xAARRGGBB, top row first
    bool has_window = false;      // false: the picture is a background, the game goes in front of it
    int win_x = 0, win_y = 0, win_w = 0, win_h = 0;   // bounding box of the transparent window (image pixels)
    std::string path;

    bool load(const std::string& file, std::string& error);
    bool valid() const { return w > 0 && h > 0; }
};
