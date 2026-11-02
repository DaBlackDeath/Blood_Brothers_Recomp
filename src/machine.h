// Blood Bros. machine: 68000 main CPU, Z80 + Seibu sound interface, YM3812,
// OKI M6295, Seibu CRTC and the video renderer, wired as in the MAME driver.
//
//   68000  10 MHz   ROM 000000-07ffff, RAM 080000-08ffff, Seibu sound latch 0a0000,
//                   CRTC 0c0000, DIP/inputs 0e0000
//   Z80    3.58 MHz ROM 0000-1fff, RAM 2000-27ff, Seibu I/O 4000-401f, OKI 6000, bank 8000-ffff
//   YM3812 3.58 MHz (timers drive the Z80 RST 10 interrupt), OKI M6295 1 MHz
//   VBLANK IRQ4 once per frame at 59.39 Hz
//
// The 68000 runs the statically recompiled program (recomp_cpu.cpp, behind CpuBackend).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "cpu_backend.h"
#include "oki6295.h"
#include "romset.h"
#include "video.h"
#include "ymfm_opl.h"

extern "C" {
#include "z80.h"
}

enum InputBit : uint32_t {
    IN_P1_UP = 1u << 0,   IN_P1_DOWN = 1u << 1,  IN_P1_LEFT = 1u << 2,  IN_P1_RIGHT = 1u << 3,
    IN_P1_B1 = 1u << 4,   IN_P1_B2 = 1u << 5,    IN_P1_B3 = 1u << 6,
    IN_P2_UP = 1u << 7,   IN_P2_DOWN = 1u << 8,  IN_P2_LEFT = 1u << 9,  IN_P2_RIGHT = 1u << 10,
    IN_P2_B1 = 1u << 11,  IN_P2_B2 = 1u << 12,   IN_P2_B3 = 1u << 13,
    IN_START1 = 1u << 14, IN_START2 = 1u << 15,
    IN_COIN1 = 1u << 16,  IN_COIN2 = 1u << 17,
    IN_SERVICE = 1u << 18,
};

class Machine {
public:
    static constexpr double kRefreshHz = 59.39;      // verified on PCB (MAME)
    static constexpr int kLinesPerFrame = 256;
    static constexpr int kVblankLine = 240;          // first line after the visible area
    static constexpr double k68kClock = 10000000.0;  // 20 MHz / 2
    static constexpr double kZ80Clock = 3579545.0;   // 7.15909 MHz / 2
    static constexpr int kOplDivider = 72;           // YM3812 sample = 72 clocks

    Machine();
    bool init(const RomSet& rom);
    const std::string& init_error() const { return init_error_; }   // why init() failed
    void reset();

    // Runs exactly one video frame. The screen is rendered when VBLANK starts.
    void run_frame();
    // The same, in pieces: begin_frame, 256 x run_line, end_frame.
    void begin_frame();
    void run_line(int line);
    // run_line split into pieces
    int line_begin(int line);                 // VBLANK handling + 68000 budget for this line
    void line_end(int cpu_cycles_used);       // budget bookkeeping + Z80/sound for this line
    void end_frame();

    // CPU introspection
    void cpu_state(CpuState& s) const { cpu_->get_state(s); }
    int last_cpu_cycles() const { return last_used_; }
    double cpu_budget() const { return budget_68k_; }
    const char* cpu_name() const { return cpu_->name(); }
    std::string cpu_report() const { return cpu_->report(); }
    const uint16_t* crtc_scroll() const { return scroll_; }

    const uint32_t* framebuffer() const { return fb_; }
    uint64_t frame_count() const { return frame_; }

    // Mono audio at audio_rate(); appends to `out`, then clears the internal buffer.
    void take_audio(std::vector<int16_t>& out);
    static double audio_rate() { return kZ80Clock / kOplDivider; }

    // pressed inputs (bitmask of InputBit); coin inputs produce a 4 frame impulse
    void set_inputs(uint32_t pressed) { pressed_ = pressed; }
    void set_dsw(uint16_t dsw) { dsw_ = dsw; }

    // ---- bus accessors used by the CPU glue (public for the C callbacks) ----
    uint32_t read8(uint32_t a);
    uint32_t read16(uint32_t a);
    void write8(uint32_t a, uint32_t d);
    void write16(uint32_t a, uint32_t d);

    uint8_t z80_read(uint16_t a);
    void z80_write(uint16_t a, uint8_t d);
    uint8_t z80_irq_ack();

    // OPL host callbacks
    void opl_irq(bool asserted);

    const uint8_t* work_ram() const { return ram_; }

    // ---- program ROM patching (cheats that change code) ----
    // Writes `size` (1, 2 or 4) big-endian bytes into the 68000 program. Returns true if anything changed.
    bool patch_rom(uint32_t addr, int size, uint32_t value);
    // Memory access for the cheat engine: ROM and work RAM only (no I/O side effects).
    uint32_t cheat_read(int size, uint32_t addr) const;
    void cheat_write(int size, uint32_t addr, uint32_t value);
    // Registers a function the cheat engine runs after every frame / when the machine is reset
    void set_frame_hook(std::function<void()> f) { frame_hook_ = std::move(f); }
    // debugging: write a byte of work RAM (address 0x080000-0x08ffff)
    // debugging: print frame and program counter whenever the 68000 writes this RAM address
    void set_watch_write(uint32_t addr) { watch_addr_ = addr; }
    void poke8(uint32_t addr, uint8_t v) { if (addr >= 0x80000 && addr < 0x90000) ram_[addr - 0x80000] = v; }
    const uint16_t* scroll_regs() const { return scroll_; }
    uint16_t layer_enable() const { return layer_en_; }

private:
    // ---- Seibu sound interface (YM3931 equivalent) ----
    enum SeibuIrq { RST10_ASSERT, RST10_CLEAR, RST10_ACK, RST10_EOI, RST18_ASSERT, RST18_ACK, RST18_EOI };
    void seibu_irq_event(SeibuIrq e);
    bool seibu_irq_line() const;
    uint8_t seibu_main_r(uint32_t offset);
    void seibu_main_w(uint32_t offset, uint8_t data);
    uint8_t coin_port() const;

    // ---- CRTC ----
    uint16_t crtc_read(uint32_t word_offset);
    void crtc_write(uint32_t word_offset, uint16_t data, uint16_t mask);

    uint16_t in0() const;
    uint16_t in1() const;

    // ---- timing ----
    void advance_sound(uint32_t z80_clocks);
    void generate_sample();

    struct OplHost : public ymfm::ymfm_interface {
        Machine* machine = nullptr;
        int64_t timer[2] = {-1, -1};
        void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override;
        void ymfm_update_irq(bool asserted) override;
        void advance(uint32_t clocks);
    };

    RomSet rom_;
    Video video_;
    uint8_t ram_[0x10000];
    uint8_t zram_[0x800];
    uint16_t crtc_regs_[0x28];
    uint16_t scroll_[6];
    uint16_t layer_en_ = 0;
    uint32_t fb_[Video::kWidth * Video::kHeight];

    uint32_t pressed_ = 0;
    uint32_t prev_pressed_ = 0;
    int coin_timer_[2] = {0, 0};
    uint16_t dsw_ = 0xffff;
    std::vector<uint8_t> rom_orig_;
    std::string init_error_;
    std::function<void()> frame_hook_;
    uint32_t watch_addr_ = 0;

    // Seibu sound interface state
    uint8_t main2sub_[2] = {0, 0};
    uint8_t sub2main_[2] = {0, 0};
    bool main2sub_pending_ = false, sub2main_pending_ = false;
    bool rst10_irq_ = false, rst18_irq_ = false, rst10_service_ = false, rst18_service_ = false;
    int z80_bank_ = 0;

    z80 z80_;
    OplHost opl_host_;
    ymfm::ym3812 opl_;
    Oki6295 oki_;

    std::unique_ptr<CpuBackend> cpu_;
    int last_used_ = 0;
    double budget_68k_ = 0, budget_z80_ = 0;
    uint32_t opl_acc_ = 0;
    double oki_phase_ = 0;
    int oki_prev_ = 0, oki_cur_ = 0;
    std::vector<int16_t> audio_;
    uint64_t frame_ = 0;
};
