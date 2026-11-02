// OKI MSM6295 4-voice ADPCM sample player.
// Behaviour follows the MAME okim6295 device (command protocol, phrase table,
// OKI ADPCM decoder, volume table).
#pragma once

#include <cstdint>
#include <vector>

class Oki6295 {
public:
    // rom: sample ROM (phrase table at the start). Addresses wrap at 0x40000.
    void init(const std::vector<uint8_t>& rom);
    void reset();

    uint8_t read();
    void write(uint8_t data);

    // Produce one output sample at clock/divider (1 MHz / 132 = 7575.76 Hz on this board).
    // Range roughly +-32768 per voice, up to four voices summed.
    int generate();

    static constexpr double kSampleRate = 12000000.0 / 12.0 / 132.0;  // PIN7_HIGH

private:
    struct Voice {
        bool playing = false;
        uint32_t base = 0;
        uint32_t sample = 0;
        uint32_t count = 0;
        int signal = 0;
        int step = 0;
        int volume = 0;
    };
    uint8_t rom_byte(uint32_t addr) const;
    int adpcm_clock(Voice& v, int nibble);

    std::vector<uint8_t> rom_;
    Voice voice_[4];
    int command_ = -1;
};
