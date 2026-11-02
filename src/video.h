// Blood Bros. video hardware: 3 tilemap layers + SEI0210 sprites.
//
// The renderer works directly from the 68000 work RAM image, so there is no
// separate VRAM copy and no dirty tracking. All drawing is done into an
// indexed 256x256 buffer (the real screen size) and then converted to ARGB for
// the visible 256x224 area.
//
// This is deliberately a plain software renderer with a very small interface
// so that a GPU / upscaled / widescreen renderer can replace it later.
#pragma once

#include <cstdint>
#include <vector>

#include "romset.h"

class Video {
public:
    static constexpr int kWidth = 256;
    static constexpr int kHeight = 224;   // visible lines 16..239 of the 256-line bitmap

    void init(const RomSet& rom);

    // ram:      64 KiB work RAM (68000 byte order), mapped at 0x080000
    // scroll:   6 words written through the Seibu CRTC (0x20..0x2b); [0],[1] = BG x/y, [2],[3] = FG x/y
    // layer_en: CRTC register 0x1c, active low: bit0 BG, bit1 FG, bit3 text, bit4 sprites
    // argb:     kWidth*kHeight output pixels (0xAARRGGBB)
    void render(const uint8_t* ram, const uint16_t* scroll, uint16_t layer_en, uint32_t* argb);

private:
    void draw_tilemap16(const uint8_t* vram, int scrollx, int scrolly, int code_base,
                        int color_base, bool transparent, uint8_t prio_value);
    void draw_text(const uint8_t* vram);
    void draw_sprites(const uint8_t* spriteram);
    void draw_sprite_tile(int code, int color, bool flipx, bool flipy, int x, int y, uint32_t pmask);

    std::vector<uint8_t> tx_;    // 4096 8x8 tiles, 1 byte per pixel
    std::vector<uint8_t> bg_;    // 8192 16x16 tiles
    std::vector<uint8_t> spr_;   // 8192 16x16 sprites

    uint16_t pix_[256 * 256];    // palette index per pixel
    uint8_t prio_[256 * 256];    // priority buffer (see MAME prio_transpen semantics)
};
