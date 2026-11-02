#include "video.h"

#include <cstring>

namespace {

// Same description as MAME's gfx_layout: bit offsets counted from the start of
// a tile, bit 0 = MSB of the first byte, plane 0 = most significant pixel bit.
struct GfxLayout {
    int w, h;
    int planes;
    int planeoffs[4];
    int xoffs[16];
    int yoffs[16];
    int charincrement;  // bits per tile in the ROM
};

inline int readbit(const uint8_t* base, size_t bitnum) {
    return (base[bitnum >> 3] >> (7 - (bitnum & 7))) & 1;
}

std::vector<uint8_t> decode_gfx(const std::vector<uint8_t>& rom, const GfxLayout& L, size_t total) {
    std::vector<uint8_t> out(total * L.w * L.h);
    for (size_t c = 0; c < total; c++) {
        const uint8_t* base = rom.data() + c * (size_t)(L.charincrement / 8);
        uint8_t* d = &out[c * L.w * L.h];
        for (int y = 0; y < L.h; y++) {
            for (int x = 0; x < L.w; x++) {
                int v = 0;
                for (int p = 0; p < L.planes; p++)
                    v = (v << 1) | readbit(base, (size_t)(L.planeoffs[p] + L.xoffs[x] + L.yoffs[y]));
                d[y * L.w + x] = (uint8_t)v;
            }
        }
    }
    return out;
}

inline unsigned be16(const uint8_t* p) { return ((unsigned)p[0] << 8) | p[1]; }

}  // namespace

void Video::init(const RomSet& rom) {
    // textlayout: 8x8, planes {0, 4, half+0, half+4}, 16 bytes per tile, 4096 tiles
    {
        const int half_bits = (int)(rom.txtiles.size() / 2) * 8;
        GfxLayout L{};
        L.w = 8; L.h = 8; L.planes = 4;
        L.planeoffs[0] = 0; L.planeoffs[1] = 4; L.planeoffs[2] = half_bits + 0; L.planeoffs[3] = half_bits + 4;
        const int xo[8] = {3, 2, 1, 0, 8 + 3, 8 + 2, 8 + 1, 8 + 0};
        for (int i = 0; i < 8; i++) { L.xoffs[i] = xo[i]; L.yoffs[i] = i * 16; }
        L.charincrement = 16 * 8;
        tx_ = decode_gfx(rom.txtiles, L, (rom.txtiles.size() / 2) / 16);
    }
    // spritelayout: 16x16, planes {8, 12, 0, 4}, 128 bytes per tile
    GfxLayout S{};
    S.w = 16; S.h = 16; S.planes = 4;
    S.planeoffs[0] = 8; S.planeoffs[1] = 12; S.planeoffs[2] = 0; S.planeoffs[3] = 4;
    for (int i = 0; i < 8; i++) {
        const int sub = (i < 4) ? 0 : 16;
        const int k = 3 - (i & 3);
        S.xoffs[i] = sub + k;
        S.xoffs[8 + i] = sub + k + 32 * 16;
    }
    for (int i = 0; i < 16; i++) S.yoffs[i] = 2 * i * 16;
    S.charincrement = 128 * 8;
    bg_ = decode_gfx(rom.bgtiles, S, rom.bgtiles.size() / 128);
    spr_ = decode_gfx(rom.sprites, S, rom.sprites.size() / 128);

    std::memset(pix_, 0, sizeof pix_);
    std::memset(prio_, 0, sizeof prio_);
}

// 16x16 tilemap, 32x16 tiles (512x256 pixels), TILEMAP_SCAN_ROWS.
// A screen pixel (x,y) shows tilemap pixel (x+scrollx, y+scrolly).
void Video::draw_tilemap16(const uint8_t* vram, int scrollx, int scrolly, int code_base,
                           int color_base, bool transparent, uint8_t prio_value) {
    for (int y = 0; y < 256; y++) {
        const int my = (y + scrolly) & 255;
        const int row = my >> 4, ty = my & 15;
        for (int x = 0; x < 256; x++) {
            const int mx = (x + scrollx) & 511;
            const int col = mx >> 4, tx = mx & 15;
            const unsigned word = be16(vram + (row * 32 + col) * 2);
            const int code = (int)(word & 0xfff) + code_base;
            const int color = (int)(word >> 12);
            const uint8_t p = bg_[(size_t)code * 256 + ty * 16 + tx];
            if (transparent && p == 15) continue;
            pix_[y * 256 + x] = (uint16_t)(color_base + color * 16 + p);
            prio_[y * 256 + x] = prio_value;
        }
    }
}

// 8x8 text layer, 32x32 tiles, no scrolling, pen 15 transparent.
void Video::draw_text(const uint8_t* vram) {
    for (int y = 0; y < 256; y++) {
        const int row = y >> 3, ty = y & 7;
        for (int x = 0; x < 256; x++) {
            const int col = x >> 3, tx = x & 7;
            const unsigned word = be16(vram + (row * 32 + col) * 2);
            const int code = (int)(word & 0xfff);
            const int color = (int)(word >> 12);
            const uint8_t p = tx_[(size_t)code * 64 + ty * 8 + tx];
            if (p == 15) continue;
            pix_[y * 256 + x] = (uint16_t)(0x70 * 16 + color * 16 + p);
        }
    }
}

// A sprite pixel is drawn only if ((1 << prio[x]) & pmask) == 0; a drawn pixel then sets the priority buffer to 31.
void Video::draw_sprite_tile(int code, int color, bool flipx, bool flipy, int x, int y, uint32_t pmask) {
    const uint8_t* src = &spr_[(size_t)(code & 0x1fff) * 256];
    for (int ty = 0; ty < 16; ty++) {
        const int dy = y + ty;
        if (dy < 0 || dy > 255) continue;
        const int sy = flipy ? 15 - ty : ty;
        for (int tx = 0; tx < 16; tx++) {
            const int dx = x + tx;
            if (dx < 0 || dx > 255) continue;
            const int sx = flipx ? 15 - tx : tx;
            const uint8_t p = src[sy * 16 + sx];
            if (p == 15) continue;
            const int idx = dy * 256 + dx;
            // A pixel that is hidden behind a higher-priority layer must not claim the priority buffer: otherwise a
            // sprite drawn later at the same place would shine through the foreground (verified pixel for pixel
            // against MAME 0.264 on 75 game states).
            if (((1u << (prio_[idx] & 0x1f)) & pmask) == 0) {
                pix_[idx] = (uint16_t)(color * 16 + p);
                prio_[idx] = 31;
            }
        }
    }
}

// SEI0210 "alt format" (Blood Bros.), 8 bytes per sprite, 512 sprites
void Video::draw_sprites(const uint8_t* spriteram) {
    // Drawn from the last entry to the first, so entry 0 ends up on top. This matches the
    // behaviour of MAME 0.264 pixel for pixel (e.g. the balcony rail on the stage 1 saloon,
    // which sits in front of the building column that follows it in sprite RAM).
    for (int i = 511; i >= 0; i--) {
        const uint8_t* s = spriteram + i * 8;
        const unsigned w0 = be16(s + 0), w1 = be16(s + 2), w2 = be16(s + 4), w3 = be16(s + 6);
        if (w0 & 0x8000) continue;  // disabled
        const bool flipy = (w0 >> 14) & 1;
        const bool flipx = (w0 >> 13) & 1;
        const bool pri = (w0 >> 11) & 1;
        const int sizex = (int)((w0 >> 7) & 7) + 1;
        const int sizey = (int)((w0 >> 4) & 7) + 1;
        const int color = (int)(w0 & 15);
        int code = (int)(w1 & 0x1fff);
        int x = (int)(w2 & 0x1ff);
        int y = (int)(w3 & 0x1ff);
        if (x >= 0x180) x -= 0x200;
        if (y >= 0x180) y -= 0x200;
        const uint32_t pmask = pri ? 0x02 : 0;  // pri_cb: sprites with the priority bit go behind the foreground layer
        for (int ax = 0; ax < sizex; ax++) {
            for (int ay = 0; ay < sizey; ay++) {
                draw_sprite_tile(code++, color, flipx, flipy,
                                 flipx ? x + 16 * (sizex - ax - 1) : x + 16 * ax,
                                 flipy ? y + 16 * (sizey - ay - 1) : y + 16 * ay, pmask);
            }
        }
    }
}

void Video::render(const uint8_t* ram, const uint16_t* scroll, uint16_t layer_en, uint32_t* argb) {
    // RAM map (offsets from 0x080000): sprites 0xb000, BG 0xc000, FG 0xd000, text 0xd800,
    // palette 0xe800 (2048 words, xBGR_444)
    std::memset(pix_, 0, sizeof pix_);
    std::memset(prio_, 0, sizeof prio_);

    if (!(layer_en & 0x01)) draw_tilemap16(ram + 0xc000, scroll[0], scroll[1], 0x0000, 0x40 * 16, false, 0);
    if (!(layer_en & 0x02)) draw_tilemap16(ram + 0xd000, scroll[2], scroll[3], 0x1000, 0x50 * 16, true, 1);
    if (!(layer_en & 0x10)) draw_sprites(ram + 0xb000);
    if (!(layer_en & 0x08)) draw_text(ram + 0xd800);

    uint32_t pal[2048];
    for (int i = 0; i < 2048; i++) {
        const unsigned w = be16(ram + 0xe800 + i * 2);
        const unsigned r = (w & 0xf) * 17, g = ((w >> 4) & 0xf) * 17, b = ((w >> 8) & 0xf) * 17;
        pal[i] = 0xff000000u | (r << 16) | (g << 8) | b;
    }
    for (int y = 0; y < kHeight; y++)
        for (int x = 0; x < kWidth; x++) argb[y * kWidth + x] = pal[pix_[(y + 16) * 256 + x] & 2047];
}
