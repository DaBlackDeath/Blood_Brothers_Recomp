// Blood Bros. (TAD Corporation, 1990) - ROM set loading
//
// ROM names, sizes, CRCs and the layout of the memory regions follow the
// MAME driver (src/mame/seibu/bloodbro.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct RomSet {
    std::vector<uint8_t> maincpu;   // 0x80000  68000 program, already in 68k byte order
    std::vector<uint8_t> audiocpu;  // 0x20000  Z80 program + bank area
    std::vector<uint8_t> txtiles;   // 0x20000  8x8 text tiles
    std::vector<uint8_t> bgtiles;   // 0x100000 16x16 background/foreground tiles
    std::vector<uint8_t> sprites;   // 0x100000 16x16 sprites
    std::vector<uint8_t> oki;       // 0x40000  OKI M6295 samples
    std::vector<uint8_t> prom;      // 0x100    (unused by the emulation)
    std::string set_name;
    std::string description;
};

// Only the main set "bloodbro" is supported: the recompiled program is a translation of its 68000 code.
// `paths` is a list of .zip files or directories searched in order (a merged zip with clones works too).
// Within a zip/dir a file is looked up as "<name>" and as "bloodbro/<name>".
// CRC mismatches only produce warnings (appended to `warnings`).
bool load_romset(const std::vector<std::string>& paths, RomSet& out, std::string& error, std::string& warnings);
