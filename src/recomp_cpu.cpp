// The statically recompiled 68000 program: block dispatcher.
//
// generated/recomp_insns.inc  one function per instruction (tools/gen_recomp.py)
// generated/recomp_blocks.inc basic blocks built from those functions
//
// The dispatcher looks the program counter up in a block table; at an address that is
// not the start of a block (for instance after an interrupt returned into the middle of
// a block) it executes single instructions until the next block start. There is no
// interpreter: reaching an address without recompiled code ends the program with an error.
#include <cstdio>
#include <set>
#include <vector>

#include "cpu68k.h"
#include "cpu_backend.h"
#include "insn_table.h"
#include "machine.h"

namespace r68k {
static Machine* g_bus = nullptr;

uint32_t bus_rd8(uint32_t a) { return g_bus->read8(a); }
uint32_t bus_rd16(uint32_t a) { return g_bus->read16(a); }
uint32_t bus_rd32(uint32_t a) { return (g_bus->read16(a) << 16) | g_bus->read16(a + 2); }
void bus_wr8(uint32_t a, uint32_t v) { g_bus->write8(a, v); }
void bus_wr16(uint32_t a, uint32_t v) { g_bus->write16(a, v); }
void bus_wr32(uint32_t a, uint32_t v) {
    g_bus->write16(a, v >> 16);
    g_bus->write16(a + 2, v & 0xffff);
}
void irq_acknowledge(Cpu& c, int) { c.irq_level = 0; }

[[noreturn]] void missing_code(const Cpu& c) {
    char msg[200];
    std::snprintf(msg, sizeof msg,
                  "The game reached code at %06X that is not part of the recompiled program (SR %04X).",
                  (unsigned)(c.pc & 0xffffff), (unsigned)c.sr);
    recomp_fatal(msg);
}
// Instruction the generator could not translate (the current program has none).
void unsupported(Cpu& c, uint32_t pc, uint32_t) {
    c.pc = pc;
    missing_code(c);
}
}  // namespace r68k

using namespace r68k;

#include "recomp_insns.inc"
#include "recomp_blocks.inc"

namespace {

constexpr uint32_t kRomWords = 0x80000 / 2;

class RecompCpu : public CpuBackend {
public:
    explicit RecompCpu(Machine* m)
        : machine_(m), blocks_(kRomWords, nullptr), insns_(kRomWords, nullptr), ilen_(kRomWords, 0) {
        for (const InsnEntry& i : kInsns)
            if (i.translated && i.fn) {
                insns_[i.addr >> 1] = i.fn;
                ilen_[i.addr >> 1] = i.length;
            }
        for (const BlockEntry& b : kBlocks)
            if (b.fn) blocks_[b.addr >> 1] = b.fn;
    }

    // Cheats can patch the program ROM. The recompiled code is fixed, so a patch of program code has no
    // effect (patches of data in the ROM, e.g. palettes, still work); it is reported once.
    void code_changed(const std::vector<std::pair<uint32_t, uint32_t>>& patched) override {
        for (const auto& r : patched)
            for (uint32_t s = (r.first >= 10 ? r.first - 10 : 0) & ~1u; s < r.second && s < 0x80000; s += 2)
                if (insns_[s >> 1] && s + ilen_[s >> 1] > r.first) {
                    if (warned_.insert(s).second)
                        std::fprintf(stderr, "cheat patches program code at %06x: ignored (recompiled program)\n", s);
                    break;
                }
    }

    static bool available() {
        for (const InsnEntry& i : kInsns)
            if (i.translated && i.fn) return true;
        return false;
    }

    void reset() override {
        g_bus = machine_;
        c_ = Cpu{};
        c_.sr = 0x2700;
        c_.a[7] = bus_rd32(0);
        c_.pc = bus_rd32(4);
        reset_cycles_ = 40;  // the reset exception is charged on the next execute call
    }

    int execute(int cycles) override {
        g_bus = machine_;
        if (reset_cycles_) {
            const int rc = reset_cycles_;
            reset_cycles_ = 0;
            cycles -= rc;
            if (cycles <= 0) return rc;
        }
        c_.limit = cycles;
        c_.cycles = 0;
        check_irq(c_);  // "see if interrupts came in"
        do {
            const uint32_t pc = c_.pc & 0xffffff;
            if (pc < 0x80000) {
                if (auto b = blocks_[pc >> 1]) { b(c_); n_blocks_++; continue; }
                if (auto f = insns_[pc >> 1]) { f(c_); n_single_++; continue; }
            }
            missing_code(c_);
        } while (c_.cycles < c_.limit);
        return c_.cycles;
    }

    // Only raises the line. The interrupt is taken at the next interrupt check (start of execute(),
    // or after an instruction that writes SR).
    void set_irq(int level) override { c_.irq_level = (uint32_t)level; }

    void get_state(CpuState& s) const override {
        for (int i = 0; i < 8; i++) { s.d[i] = c_.d[i]; s.a[i] = c_.a[i]; }
        s.pc = c_.pc;
        s.sr = c_.sr & kSrMask;
        const bool sup = c_.sr & FS;
        s.usp = sup ? c_.other_sp : c_.a[7];
        s.isp = sup ? c_.a[7] : c_.other_sp;
    }

    const char* name() const override { return "recompiled code"; }

    std::string report() const override {
        char buf[160];
        std::snprintf(buf, sizeof buf, "recompiled CPU: %llu blocks, %llu single-stepped instructions",
                      (unsigned long long)n_blocks_, (unsigned long long)n_single_);
        return buf;
    }

private:
    Machine* machine_;
    Cpu c_;
    int reset_cycles_ = 0;
    std::vector<void (*)(Cpu&)> blocks_, insns_;
    std::vector<uint8_t> ilen_;
    uint64_t n_blocks_ = 0, n_single_ = 0;
    std::set<uint32_t> warned_;
};

}  // namespace

std::unique_ptr<CpuBackend> make_recomp_backend(Machine* m, const std::vector<uint8_t>& prg, std::string& why) {
    if (!RecompCpu::available()) {
        why = "no recompiled code in this build: run tools/recomp_pipeline.sh, then rebuild";
        return nullptr;
    }
    // The code is a translation of one particular program ROM: every translated opcode must still be there.
    size_t bad = 0;
    uint32_t first_bad = 0;
    for (const InsnEntry& i : kInsns) {
        if (!i.translated || !i.fn) continue;
        const uint16_t w = i.addr + 1 < prg.size() ? (uint16_t)((prg[i.addr] << 8) | prg[i.addr + 1]) : 0;
        if (w != i.opcode && bad++ == 0) first_bad = i.addr;
    }
    if (bad) {
        char buf[200];
        std::snprintf(buf, sizeof buf,
                      "the program ROM does not match the recompiled code (%zu instructions differ, first at %06x): "
                      "the code was generated from a different program", bad, (unsigned)first_bad);
        why = buf;
        return nullptr;
    }
    return std::make_unique<RecompCpu>(m);
}
