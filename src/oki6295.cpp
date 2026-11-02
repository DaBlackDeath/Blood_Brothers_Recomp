#include "oki6295.h"

#include <cmath>

namespace {

// OKI ADPCM tables (same construction as MAME's oki_adpcm_state)
const int kIndexShift[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

int g_diff_lookup[49 * 16];
bool g_tables_ready = false;

void build_tables() {
    if (g_tables_ready) return;
    static const int nbl2bit[16][4] = {
        {1, 0, 0, 0},  {1, 0, 0, 1},  {1, 0, 1, 0},  {1, 0, 1, 1},
        {1, 1, 0, 0},  {1, 1, 0, 1},  {1, 1, 1, 0},  {1, 1, 1, 1},
        {-1, 0, 0, 0}, {-1, 0, 0, 1}, {-1, 0, 1, 0}, {-1, 0, 1, 1},
        {-1, 1, 0, 0}, {-1, 1, 0, 1}, {-1, 1, 1, 0}, {-1, 1, 1, 1}};
    for (int step = 0; step <= 48; step++) {
        const int stepval = (int)std::floor(16.0 * std::pow(11.0 / 10.0, (double)step));
        for (int nib = 0; nib < 16; nib++) {
            g_diff_lookup[step * 16 + nib] =
                nbl2bit[nib][0] * (stepval * nbl2bit[nib][1] + stepval / 2 * nbl2bit[nib][2] +
                                   stepval / 4 * nbl2bit[nib][3] + stepval / 8);
        }
    }
    g_tables_ready = true;
}

// volume attenuation per command nibble (0 dB, -3.2 dB, ... as in MAME)
const int kVolumeTable[16] = {0x20, 0x16, 0x10, 0x0b, 0x08, 0x06, 0x04, 0x03,
                              0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

}  // namespace

void Oki6295::init(const std::vector<uint8_t>& rom) {
    build_tables();
    rom_ = rom;
    reset();
}

void Oki6295::reset() {
    for (Voice& v : voice_) v = Voice{};
    command_ = -1;
}

uint8_t Oki6295::rom_byte(uint32_t addr) const {
    addr &= 0x3ffff;
    return addr < rom_.size() ? rom_[addr] : 0;
}

uint8_t Oki6295::read() {
    uint8_t result = 0xf0;  // bits 4-7 read as 1
    for (int i = 0; i < 4; i++)
        if (voice_[i].playing) result |= (uint8_t)(1 << i);
    return result;
}

void Oki6295::write(uint8_t data) {
    if (command_ != -1) {
        // second byte: voice mask (bits 4-7) and attenuation (bits 0-3)
        int voicemask = data >> 4;
        for (int n = 0; n < 4; n++, voicemask >>= 1) {
            if (!(voicemask & 1)) continue;
            Voice& v = voice_[n];
            if (v.playing) continue;
            const uint32_t base = (uint32_t)command_ * 8;
            const uint32_t start = (((uint32_t)rom_byte(base + 0) << 16) | ((uint32_t)rom_byte(base + 1) << 8) |
                                    rom_byte(base + 2)) & 0x3ffff;
            const uint32_t stop = (((uint32_t)rom_byte(base + 3) << 16) | ((uint32_t)rom_byte(base + 4) << 8) |
                                   rom_byte(base + 5)) & 0x3ffff;
            if (start < stop) {
                v.playing = true;
                v.base = start;
                v.sample = 0;
                v.count = 2 * (stop - start + 1);
                v.signal = -2;  // ADPCM reset
                v.step = 0;
                v.volume = kVolumeTable[data & 0x0f];
            }
        }
        command_ = -1;
    } else if (data & 0x80) {
        command_ = data & 0x7f;  // start of a play command: phrase number
    } else {
        // stop command: voices selected by bits 3-6
        int voicemask = data >> 3;
        for (int n = 0; n < 4; n++, voicemask >>= 1)
            if (voicemask & 1) voice_[n].playing = false;
    }
}

int Oki6295::adpcm_clock(Voice& v, int nibble) {
    v.signal += g_diff_lookup[v.step * 16 + (nibble & 15)];
    if (v.signal > 2047) v.signal = 2047;
    else if (v.signal < -2048) v.signal = -2048;
    v.step += kIndexShift[nibble & 7];
    if (v.step > 48) v.step = 48;
    else if (v.step < 0) v.step = 0;
    return v.signal;
}

int Oki6295::generate() {
    int sum = 0;
    for (Voice& v : voice_) {
        if (!v.playing) continue;
        const int nibble = (rom_byte(v.base + v.sample / 2) >> (((v.sample & 1) << 2) ^ 4)) & 0x0f;
        sum += adpcm_clock(v, nibble) * v.volume / 2;
        if (++v.sample >= v.count) v.playing = false;
    }
    return sum;
}
