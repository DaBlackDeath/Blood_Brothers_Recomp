#include "machine.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// Z80 bus callbacks
// ---------------------------------------------------------------------------
static uint8_t z80_read_cb(void* ud, uint16_t a) { return static_cast<Machine*>(ud)->z80_read(a); }
static void z80_write_cb(void* ud, uint16_t a, uint8_t d) { static_cast<Machine*>(ud)->z80_write(a, d); }
static uint8_t z80_port_in_cb(z80*, uint8_t) { return 0xff; }
static void z80_port_out_cb(z80*, uint8_t, uint8_t) {}
static uint8_t z80_int_ack_cb(z80* z) { return static_cast<Machine*>(z->userdata)->z80_irq_ack(); }

// ---------------------------------------------------------------------------
// YM3812 host: timers and IRQ line
// ---------------------------------------------------------------------------
void Machine::OplHost::ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) {
    if (tnum >= 2) return;
    timer[tnum] = duration_in_clocks >= 0 ? duration_in_clocks : -1;
}

void Machine::OplHost::ymfm_update_irq(bool asserted) { machine->opl_irq(asserted); }

void Machine::OplHost::advance(uint32_t clocks) {
    for (uint32_t t = 0; t < 2; t++) {
        if (timer[t] < 0) continue;
        timer[t] -= clocks;
        if (timer[t] <= 0) {
            const int64_t late = -timer[t];
            timer[t] = -1;
            m_engine->engine_timer_expired(t);
            if (timer[t] >= 0) timer[t] -= late;  // re-armed inside the callback: keep the overshoot
        }
    }
}

// ---------------------------------------------------------------------------
Machine::Machine() : opl_(opl_host_) {
    opl_host_.machine = this;
    std::memset(ram_, 0, sizeof ram_);
    std::memset(zram_, 0, sizeof zram_);
    std::memset(crtc_regs_, 0, sizeof crtc_regs_);
    std::memset(scroll_, 0, sizeof scroll_);
    std::memset(fb_, 0, sizeof fb_);
    std::memset(&z80_, 0, sizeof z80_);
}

bool Machine::init(const RomSet& rom) {
    rom_ = rom;
    rom_orig_ = rom.maincpu;
    video_.init(rom_);
    oki_.init(rom_.oki);

    cpu_ = make_recomp_backend(this, rom_.maincpu, init_error_);
    if (!cpu_) {
        std::fprintf(stderr, "recompiled CPU unavailable: %s\n", init_error_.c_str());
        return false;
    }

    z80_init(&z80_);
    z80_.read_byte = z80_read_cb;
    z80_.write_byte = z80_write_cb;
    z80_.port_in = z80_port_in_cb;
    z80_.port_out = z80_port_out_cb;
    z80_.int_ack = z80_int_ack_cb;
    z80_.userdata = this;

    reset();
    return true;
}

void Machine::reset() {
    std::memset(ram_, 0, sizeof ram_);
    std::memset(zram_, 0, sizeof zram_);
    std::memset(crtc_regs_, 0, sizeof crtc_regs_);
    std::memset(scroll_, 0, sizeof scroll_);
    layer_en_ = 0;
    main2sub_[0] = main2sub_[1] = 0;
    sub2main_[0] = sub2main_[1] = 0;
    main2sub_pending_ = sub2main_pending_ = false;
    rst10_irq_ = rst18_irq_ = rst10_service_ = rst18_service_ = false;
    z80_bank_ = 0;
    coin_timer_[0] = coin_timer_[1] = 0;
    prev_pressed_ = 0;

    z80_init(&z80_);
    z80_.read_byte = z80_read_cb;
    z80_.write_byte = z80_write_cb;
    z80_.port_in = z80_port_in_cb;
    z80_.port_out = z80_port_out_cb;
    z80_.int_ack = z80_int_ack_cb;
    z80_.userdata = this;

    opl_host_.timer[0] = opl_host_.timer[1] = -1;
    opl_.reset();
    oki_.reset();

    budget_68k_ = budget_z80_ = 0;
    opl_acc_ = 0;
    oki_phase_ = 0;
    oki_prev_ = oki_cur_ = 0;
    audio_.clear();
    frame_ = 0;

    cpu_->reset();
}

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------
uint16_t Machine::in0() const {
    uint16_t v = 0xffff;
    struct { uint32_t in; uint16_t bit; } map[] = {
        {IN_P1_UP, 0x0001},   {IN_P1_DOWN, 0x0002},  {IN_P1_LEFT, 0x0004},  {IN_P1_RIGHT, 0x0008},
        {IN_P1_B1, 0x0010},   {IN_P1_B2, 0x0020},    {IN_P1_B3, 0x0040},
        {IN_P2_UP, 0x0100},   {IN_P2_DOWN, 0x0200},  {IN_P2_LEFT, 0x0400}, {IN_P2_RIGHT, 0x0800},
        {IN_P2_B1, 0x1000},   {IN_P2_B2, 0x2000},    {IN_P2_B3, 0x4000},
    };
    for (auto& m : map)
        if (pressed_ & m.in) v &= (uint16_t)~m.bit;
    return v;
}

uint16_t Machine::in1() const {
    uint16_t v = 0xffff;
    if (pressed_ & IN_START1) v &= (uint16_t)~0x0001;
    if (pressed_ & IN_START2) v &= (uint16_t)~0x0010;
    if (pressed_ & IN_SERVICE) v &= (uint16_t)~0x1000;
    return v;
}

// coin inputs are read by the sound CPU (SEIBU_COIN_INPUTS, active high, impulse 4 frames)
uint8_t Machine::coin_port() const {
    uint8_t v = 0;
    if (coin_timer_[0] > 0) v |= 0x01;
    if (coin_timer_[1] > 0) v |= 0x02;
    return v;
}

// ---------------------------------------------------------------------------
// Seibu CRTC (registers 0x00-0x4f)
// ---------------------------------------------------------------------------
uint16_t Machine::crtc_read(uint32_t off) {
    if (off >= 0x28) return 0;
    const uint32_t byte = off * 2;
    // registers with write-only handlers read back from the (never written) backing RAM
    if (byte == 0x14 || byte == 0x1c || (byte >= 0x20 && byte <= 0x3b)) return 0;
    return crtc_regs_[off];
}

void Machine::crtc_write(uint32_t off, uint16_t data, uint16_t mask) {
    if (off >= 0x28) return;
    const uint32_t byte = off * 2;
    if (byte == 0x1c) {
        layer_en_ = data;               // layer enable, active low
    } else if (byte >= 0x20 && byte <= 0x2a) {
        uint16_t& s = scroll_[(byte - 0x20) / 2];
        s = (uint16_t)((s & ~mask) | (data & mask));
    } else if (byte == 0x1a) {
        crtc_regs_[off] = (uint16_t)((crtc_regs_[off] & ~mask) | (data & mask));  // flip screen etc. (unused)
    } else if (byte == 0x14 || (byte >= 0x2c && byte <= 0x3b)) {
        // decrypt key / base scroll: not connected on this board
    } else {
        crtc_regs_[off] = (uint16_t)((crtc_regs_[off] & ~mask) | (data & mask));
    }
}

// ---------------------------------------------------------------------------
// Seibu sound interface (the "YM3931")
// ---------------------------------------------------------------------------
void Machine::seibu_irq_event(SeibuIrq e) {
    switch (e) {
    case RST10_ASSERT: rst10_irq_ = true; break;
    case RST10_CLEAR: rst10_irq_ = false; break;
    case RST10_ACK: rst10_service_ = true; break;
    case RST10_EOI: rst10_service_ = false; break;
    case RST18_ASSERT: rst18_irq_ = true; break;
    case RST18_ACK: rst18_service_ = true; rst18_irq_ = false; break;
    case RST18_EOI: rst18_service_ = false; break;
    }
}

bool Machine::seibu_irq_line() const {
    return (rst10_irq_ && !rst10_service_) || (rst18_irq_ && !rst18_service_);
}

// IM0 vector placed on the bus: RST 18 (main CPU request) wins over RST 10 (YM3812)
uint8_t Machine::z80_irq_ack() {
    if (rst18_irq_ && !rst18_service_) {
        seibu_irq_event(RST18_ACK);
        return 0xdf;
    }
    if (rst10_irq_ && !rst10_service_) {
        seibu_irq_event(RST10_ACK);
        return 0xd7;
    }
    return 0x00;
}

void Machine::opl_irq(bool asserted) { seibu_irq_event(asserted ? RST10_ASSERT : RST10_CLEAR); }

uint8_t Machine::seibu_main_r(uint32_t offset) {
    switch (offset) {
    case 2:
    case 3: return sub2main_[offset - 2];
    case 5: return main2sub_pending_ ? 1 : 0;
    default: return 0xff;
    }
}

void Machine::seibu_main_w(uint32_t offset, uint8_t data) {
    switch (offset) {
    case 0:
    case 1: main2sub_[offset] = data; break;
    case 4: seibu_irq_event(RST18_ASSERT); break;
    case 2:
    case 6:
        sub2main_pending_ = false;
        main2sub_pending_ = true;
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// 68000 bus
// ---------------------------------------------------------------------------
uint32_t Machine::read16(uint32_t a) {
    a &= 0xffffff;
    if (a < 0x80000) return ((uint32_t)rom_.maincpu[a] << 8) | rom_.maincpu[a + 1];
    if (a < 0x90000) {
        a -= 0x80000;
        return ((uint32_t)ram_[a] << 8) | ram_[a + 1];
    }
    if (a >= 0xa0000 && a <= 0xa000d) return seibu_main_r((a - 0xa0000) >> 1);  // low byte lane only
    if (a >= 0xc0000 && a <= 0xc004f) return crtc_read((a - 0xc0000) >> 1);
    if (a == 0xe0000) return dsw_;
    if (a == 0xe0002) return in0();
    if (a == 0xe0004) return in1();
    return 0;
}

uint32_t Machine::read8(uint32_t a) {
    a &= 0xffffff;
    if (a < 0x80000) return rom_.maincpu[a];
    if (a < 0x90000) return ram_[a - 0x80000];
    if (a >= 0xa0000 && a <= 0xa000d) return (a & 1) ? seibu_main_r((a - 0xa0000) >> 1) : 0;
    const uint32_t w = read16(a & ~1u);
    return (a & 1) ? (w & 0xff) : (w >> 8);
}

void Machine::write16(uint32_t a, uint32_t d) {
    a &= 0xffffff;
    if (watch_addr_ && (a == (watch_addr_ & ~1u))) {
        CpuState s;
        cpu_->get_state(s);
        std::printf("watch: frame %llu pc=%06x write16 %06x <- %04x\n", (unsigned long long)frame_, s.pc, a, d & 0xffff);
    }
    if (a >= 0x80000 && a < 0x90000) {
        a -= 0x80000;
        ram_[a] = (uint8_t)(d >> 8);
        ram_[a + 1] = (uint8_t)d;
        return;
    }
    if (a >= 0xa0000 && a <= 0xa000d) {
        seibu_main_w((a - 0xa0000) >> 1, (uint8_t)d);
        return;
    }
    if (a >= 0xc0000 && a <= 0xc004f) crtc_write((a - 0xc0000) >> 1, (uint16_t)d, 0xffff);
    // 0c0080 / 0c00c0 / 0c0100: IRQ ack / watchdog (ignored)
}

void Machine::write8(uint32_t a, uint32_t d) {
    a &= 0xffffff;
    if (watch_addr_ && a == watch_addr_) {
        CpuState s;
        cpu_->get_state(s);
        const uint32_t sp = s.a[7];
        std::printf("watch: frame %llu pc=%06x write8 %06x <- %02x   stack: %06x %06x %06x\n", (unsigned long long)frame_, s.pc, a, d & 0xff,
                    (read16(sp) << 16 | read16(sp + 2)) & 0xffffff, (read16(sp + 4) << 16 | read16(sp + 6)) & 0xffffff,
                    (read16(sp + 8) << 16 | read16(sp + 10)) & 0xffffff);
    }
    if (a >= 0x80000 && a < 0x90000) {
        ram_[a - 0x80000] = (uint8_t)d;
        return;
    }
    if (a >= 0xa0000 && a <= 0xa000d) {
        if (a & 1) seibu_main_w((a - 0xa0000) >> 1, (uint8_t)d);
        return;
    }
    if (a >= 0xc0000 && a <= 0xc004f) {
        const uint16_t d16 = (a & 1) ? (uint16_t)(d & 0xff) : (uint16_t)((d & 0xff) << 8);
        crtc_write((a - 0xc0000) >> 1, d16, (a & 1) ? 0x00ff : 0xff00);
    }
}

// ---------------------------------------------------------------------------
// Z80 bus (Seibu sound map)
// ---------------------------------------------------------------------------
uint8_t Machine::z80_read(uint16_t a) {
    if (a < 0x2000) return rom_.audiocpu[a];
    if (a >= 0x2000 && a < 0x2800) return zram_[a - 0x2000];
    if (a >= 0x8000) return rom_.audiocpu[0x10000 + z80_bank_ * 0x8000 + (a - 0x8000)];
    switch (a) {
    case 0x4008:
    case 0x4009: return opl_.read(a - 0x4008);
    case 0x4010:
    case 0x4011: return main2sub_[a - 0x4010];
    case 0x4012: return sub2main_pending_ ? 1 : 0;
    case 0x4013: return coin_port();
    case 0x6000: return oki_.read();
    default: return 0;
    }
}

void Machine::z80_write(uint16_t a, uint8_t d) {
    if (a >= 0x2000 && a < 0x2800) {
        zram_[a - 0x2000] = d;
        return;
    }
    switch (a) {
    case 0x4000:  // pending_w (just a guess in MAME)
        main2sub_pending_ = false;
        sub2main_pending_ = true;
        break;
    case 0x4001: seibu_irq_event(RST18_EOI); break;  // irq_clear_w
    case 0x4002: seibu_irq_event(RST10_EOI); break;  // rst10_ack_w
    case 0x4003: seibu_irq_event(RST18_EOI); break;  // rst18_ack_w
    case 0x4007: z80_bank_ = d & 1; break;           // bank_w
    case 0x4008:
    case 0x4009: opl_.write(a - 0x4008, d); break;
    case 0x4018:
    case 0x4019: sub2main_[a - 0x4018] = d; break;
    case 0x401b: break;                              // coin counters
    case 0x6000: oki_.write(d); break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Sound generation: one YM3812 sample every 72 Z80 clocks; OKI is interpolated in
// ---------------------------------------------------------------------------
void Machine::generate_sample() {
    ymfm::ym3812::output_data out;
    opl_.generate(&out, 1);
    int s = out.data[0];

    oki_phase_ += Oki6295::kSampleRate / audio_rate();
    while (oki_phase_ >= 1.0) {
        oki_phase_ -= 1.0;
        oki_prev_ = oki_cur_;
        oki_cur_ = oki_.generate();
    }
    s += oki_prev_ + (int)((oki_cur_ - oki_prev_) * oki_phase_);

    audio_.push_back((int16_t)std::clamp(s, -32768, 32767));
}

void Machine::advance_sound(uint32_t clocks) {
    opl_host_.advance(clocks);
    opl_acc_ += clocks;
    while (opl_acc_ >= (uint32_t)kOplDivider) {
        opl_acc_ -= kOplDivider;
        generate_sample();
    }
}

void Machine::take_audio(std::vector<int16_t>& out) {
    out.insert(out.end(), audio_.begin(), audio_.end());
    audio_.clear();
}

// ---------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------
void Machine::begin_frame() {
    // coin impulse: 4 frames high after a press
    const uint32_t rising = pressed_ & ~prev_pressed_;
    if (rising & IN_COIN1) coin_timer_[0] = 4;
    if (rising & IN_COIN2) coin_timer_[1] = 4;
    prev_pressed_ = pressed_;
}

int Machine::line_begin(int line) {
    const double cycles68_per_line = k68kClock / kRefreshHz / kLinesPerFrame;
    if (line == kVblankLine) {
        static const char* dbg = std::getenv("BB_LAYER_MASK");  // debug: extra active-low layer-disable bits
        const uint16_t extra = dbg ? (uint16_t)std::strtoul(dbg, nullptr, 0) : 0;
        video_.render(ram_, scroll_, (uint16_t)(layer_en_ | extra), fb_);  // screen update at the start of VBLANK
        cpu_->set_irq(4);                                                   // irq4_line_hold
    }
    budget_68k_ += cycles68_per_line;
    return (int)budget_68k_;
}

void Machine::line_end(int used) {
    const double cyclesz80_per_line = kZ80Clock / kRefreshHz / kLinesPerFrame;
    last_used_ = used;
    budget_68k_ -= used;

    budget_z80_ += cyclesz80_per_line;
    while (budget_z80_ > 0) {
        z80_.int_pending = seibu_irq_line();
        const unsigned long before = z80_.cyc;
        z80_step(&z80_);
        const uint32_t d = (uint32_t)(z80_.cyc - before);
        budget_z80_ -= d;
        advance_sound(d);
    }
}

void Machine::run_line(int line) {
    const int n = line_begin(line);
    line_end(n > 0 ? cpu_->execute(n) : 0);
}

uint32_t Machine::cheat_read(int size, uint32_t a) const {
    a &= 0xffffff;
    uint32_t v = 0;
    for (int i = 0; i < size; i++) {
        const uint32_t b = a + i;
        uint8_t byte = 0;
        if (b < 0x80000) byte = rom_.maincpu[b];
        else if (b < 0x90000) byte = ram_[b - 0x80000];
        v = (v << 8) | byte;
    }
    return v;
}

void Machine::cheat_write(int size, uint32_t a, uint32_t value) {
    a &= 0xffffff;
    if (a < 0x80000) {
        patch_rom(a, size, value);
        return;
    }
    for (int i = size - 1; i >= 0; i--) {
        const uint32_t b = a + i;
        if (b >= 0x80000 && b < 0x90000) ram_[b - 0x80000] = (uint8_t)value;
        value >>= 8;
    }
}

bool Machine::patch_rom(uint32_t a, int size, uint32_t value) {
    a &= 0xffffff;
    bool changed = false;
    for (int i = size - 1; i >= 0; i--) {
        const uint32_t b = a + i;
        if (b < rom_.maincpu.size() && rom_.maincpu[b] != (uint8_t)value) {
            rom_.maincpu[b] = (uint8_t)value;
            changed = true;
        }
        value >>= 8;
    }
    if (changed) {
        // ranges that differ from the original program
        std::vector<std::pair<uint32_t, uint32_t>> diff;
        for (uint32_t i = 0; i < rom_.maincpu.size(); i++) {
            if (rom_.maincpu[i] == rom_orig_[i]) continue;
            uint32_t j = i;
            while (j < rom_.maincpu.size() && rom_.maincpu[j] != rom_orig_[j]) j++;
            diff.push_back({i, j});
            i = j;
        }
        cpu_->code_changed(diff);
    }
    return changed;
}

void Machine::end_frame() {
    if (frame_hook_) frame_hook_();
    for (int& t : coin_timer_)
        if (t > 0) t--;
    frame_++;
}

void Machine::run_frame() {
    begin_frame();
    for (int line = 0; line < kLinesPerFrame; line++) run_line(line);
    end_frame();
}
