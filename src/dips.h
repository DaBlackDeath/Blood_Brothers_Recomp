// DIP switches of Blood Bros. (from the MAME driver): SW1 = low byte, SW2 = high byte of the DSW word.
// A setting is shown only while its condition (cond_mask/cond_value on the DSW word) holds.
#pragma once

#include <cstdint>

struct DipOption {
    uint16_t value;
    const char* label;
};

struct DipDef {
    const char* name;
    const char* loc;       // switch positions, e.g. "SW1:2-5"
    uint16_t mask;
    uint16_t def;
    uint16_t cond_mask;    // 0 = always shown
    uint16_t cond_value;
    int count;
    DipOption opts[16];
};

constexpr uint16_t kDipDefault = 0xffff;

static const DipDef kDips[] = {
    {"Coin Mode", "SW1:1", 0x0001, 0x0001, 0, 0, 2, {{0x0001, "Mode 1"}, {0x0000, "Mode 2"}}},
    {"Coinage", "SW1:2-5", 0x001e, 0x001e, 0x0001, 0x0001, 16,
     {{0x0014, "6 Coins/1 Credit"}, {0x0016, "5 Coins/1 Credit"}, {0x0018, "4 Coins/1 Credit"}, {0x001a, "3 Coins/1 Credit"},
      {0x0002, "8 Coins/3 Credits"}, {0x001c, "2 Coins/1 Credit"}, {0x0004, "5 Coins/3 Credits"}, {0x0006, "3 Coins/2 Credits"},
      {0x001e, "1 Coin/1 Credit"}, {0x0008, "2 Coins/3 Credits"}, {0x0012, "1 Coin/2 Credits"}, {0x0010, "1 Coin/3 Credits"},
      {0x000e, "1 Coin/4 Credits"}, {0x000c, "1 Coin/5 Credits"}, {0x000a, "1 Coin/6 Credits"}, {0x0000, "Free Play"}}},
    {"Coin A", "SW1:2-3", 0x0006, 0x0006, 0x0001, 0x0000, 4,
     {{0x0000, "5 Coins/1 Credit"}, {0x0002, "3 Coins/1 Credit"}, {0x0004, "2 Coins/1 Credit"}, {0x0006, "1 Coin/1 Credit"}}},
    {"Coin B", "SW1:4-5", 0x0018, 0x0018, 0x0001, 0x0000, 4,
     {{0x0018, "1 Coin/2 Credits"}, {0x0010, "1 Coin/3 Credits"}, {0x0008, "1 Coin/5 Credits"}, {0x0000, "1 Coin/6 Credits"}}},
    {"Starting Coin", "SW1:6", 0x0020, 0x0020, 0, 0, 2, {{0x0020, "Normal"}, {0x0000, "x2"}}},
    {"Unused", "SW1:7", 0x0040, 0x0040, 0, 0, 2, {{0x0040, "Off"}, {0x0000, "On"}}},
    {"Unused", "SW1:8", 0x0080, 0x0080, 0, 0, 2, {{0x0080, "Off"}, {0x0000, "On"}}},
    {"Lives", "SW2:1-2", 0x0300, 0x0300, 0, 0, 4, {{0x0000, "1"}, {0x0200, "2"}, {0x0300, "3"}, {0x0100, "5"}}},
    {"Bonus Life", "SW2:3-4", 0x0c00, 0x0c00, 0, 0, 4,
     {{0x0c00, "300K 500K+"}, {0x0800, "500K+"}, {0x0400, "500K"}, {0x0000, "None"}}},
    {"Difficulty", "SW2:5-6", 0x3000, 0x3000, 0, 0, 4,
     {{0x2000, "Easy"}, {0x3000, "Normal"}, {0x1000, "Hard"}, {0x0000, "Very Hard"}}},
    {"Allow Continue", "SW2:7", 0x4000, 0x4000, 0, 0, 2, {{0x0000, "No"}, {0x4000, "Yes"}}},
    {"Demo Sounds", "SW2:8", 0x8000, 0x8000, 0, 0, 2, {{0x0000, "Off"}, {0x8000, "On"}}},
};
constexpr int kDipCount = (int)(sizeof(kDips) / sizeof(kDips[0]));
