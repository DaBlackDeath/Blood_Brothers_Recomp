// 68000 runtime for the statically recompiled Blood Bros. program.
//
// The generated code (generated/recomp_insns.inc, written by tools/gen_recomp.py)
// is a sequence of calls into this file. Every operation is written to behave
// exactly like Musashi (flags, bus accesses in the same order and size, cycle
// counts) because Musashi was the reference the whole port was verified against.
#pragma once

#include <cstdint>

namespace r68k {

constexpr uint32_t FC = 0x01, FV = 0x02, FZ = 0x04, FN = 0x08, FX = 0x10;
constexpr uint32_t FCCR = 0x1f, FS = 0x2000, FT = 0x8000;
constexpr uint32_t kSrMask = 0xa71f;  // implemented SR bits of the 68000

struct Cpu {
    uint32_t d[8] = {};
    uint32_t a[8] = {};       // a[7] is the active stack pointer
    uint32_t other_sp = 0;    // USP while in supervisor mode, SSP while in user mode
    uint32_t pc = 0;
    uint32_t sr = 0x2700;
    int32_t cycles = 0;       // cycles used (accumulated by the generated code)
    int32_t limit = 0;        // blocks stop between instructions once cycles >= limit
    uint32_t irq_level = 0;   // pending interrupt level (0 = none), set by the host
};

// ---- bus, implemented by the host (the Machine, or the test harness) ----
uint32_t bus_rd8(uint32_t addr);
uint32_t bus_rd16(uint32_t addr);
uint32_t bus_rd32(uint32_t addr);
void bus_wr8(uint32_t addr, uint32_t v);
void bus_wr16(uint32_t addr, uint32_t v);
void bus_wr32(uint32_t addr, uint32_t v);
// Called for instructions the generator cannot translate (exceptions, privileged ops ...).
void unsupported(Cpu& c, uint32_t pc, uint32_t opcode);
// Called when an interrupt is accepted; the host releases the interrupt line (c.irq_level = 0).
void irq_acknowledge(Cpu& c, int level);

// ---- sizes ----
template <int S> struct Sz;
template <> struct Sz<1> { static constexpr uint32_t mask = 0xff, msb = 0x80, bits = 8; };
template <> struct Sz<2> { static constexpr uint32_t mask = 0xffff, msb = 0x8000, bits = 16; };
template <> struct Sz<4> { static constexpr uint32_t mask = 0xffffffffu, msb = 0x80000000u, bits = 32; };

template <int S> inline uint32_t rd(uint32_t a);
template <> inline uint32_t rd<1>(uint32_t a) { return bus_rd8(a) & 0xff; }
template <> inline uint32_t rd<2>(uint32_t a) { return bus_rd16(a) & 0xffff; }
template <> inline uint32_t rd<4>(uint32_t a) { return bus_rd32(a); }
template <int S> inline void wr(uint32_t a, uint32_t v);
template <> inline void wr<1>(uint32_t a, uint32_t v) { bus_wr8(a, v & 0xff); }
template <> inline void wr<2>(uint32_t a, uint32_t v) { bus_wr16(a, v & 0xffff); }
template <> inline void wr<4>(uint32_t a, uint32_t v) { bus_wr32(a, v); }

inline uint32_t sx8(uint32_t v) { return (uint32_t)(int32_t)(int8_t)v; }
inline uint32_t sx16(uint32_t v) { return (uint32_t)(int32_t)(int16_t)v; }
template <int S> inline uint32_t sx(uint32_t v) {
    return S == 1 ? sx8(v) : S == 2 ? sx16(v) : v;
}

// data register access by size
template <int S> inline uint32_t getd(const Cpu& c, int n) { return c.d[n] & Sz<S>::mask; }
template <int S> inline void setd(Cpu& c, int n, uint32_t v) {
    if (S == 4) c.d[n] = v;
    else c.d[n] = (c.d[n] & ~Sz<S>::mask) | (v & Sz<S>::mask);
}

// ---- flags ----
inline void set_nz_clear_vc(Cpu& c, uint32_t n, uint32_t z) {
    c.sr = (c.sr & ~(FN | FZ | FV | FC)) | n | z;
}
template <int S> inline void flags_logic(Cpu& c, uint32_t r) {
    set_nz_clear_vc(c, (r & Sz<S>::msb) ? FN : 0, (r & Sz<S>::mask) == 0 ? FZ : 0);
}

inline bool cc_true(uint32_t sr, int cc) {
    const bool C = sr & FC, V = sr & FV, Z = sr & FZ, N = sr & FN;
    switch (cc & 15) {
    case 0: return true;
    case 1: return false;
    case 2: return !C && !Z;
    case 3: return C || Z;
    case 4: return !C;
    case 5: return C;
    case 6: return !Z;
    case 7: return Z;
    case 8: return !V;
    case 9: return V;
    case 10: return !N;
    case 11: return N;
    case 12: return N == V;
    case 13: return N != V;
    case 14: return !Z && (N == V);
    default: return Z || (N != V);
    }
}

// ---- SR / stack pointer switching ----
inline void set_sr(Cpu& c, uint32_t v) {
    v &= kSrMask;
    if ((v ^ c.sr) & FS) {
        const uint32_t t = c.a[7];
        c.a[7] = c.other_sp;
        c.other_sp = t;
    }
    c.sr = v;
}
// ---- exceptions (same order of bus accesses and cycle counts as Musashi, 68000 stack frame) ----
// Enter supervisor mode, clear trace; returns the SR that must be stacked.
inline uint32_t enter_supervisor(Cpu& c) {
    const uint32_t old = c.sr;
    c.sr &= ~FT;
    if (!(c.sr & FS)) {
        const uint32_t t = c.a[7];
        c.a[7] = c.other_sp;
        c.other_sp = t;
        c.sr |= FS;
    }
    return old;
}
inline void push16(Cpu& c, uint32_t v);
inline void push32(Cpu& c, uint32_t v);

// TRAP-like exception: stack PC and SR, then fetch the vector. `cycles` = total for the instruction.
inline void exception_trap(Cpu& c, int vector, uint32_t stacked_pc, int cycles) {
    const uint32_t old = enter_supervisor(c);
    push32(c, stacked_pc);
    push16(c, old);
    c.pc = bus_rd32((uint32_t)vector * 4);
    c.cycles += cycles;
}

// Autovectored interrupt: fetch the vector first, then stack PC and SR (44 cycles).
inline void take_interrupt(Cpu& c) {
    const int level = (int)c.irq_level;
    irq_acknowledge(c, level);
    const uint32_t old = enter_supervisor(c);
    c.sr = (c.sr & ~0x0700u) | ((uint32_t)level << 8);
    uint32_t np = bus_rd32((uint32_t)(24 + level) * 4);
    if (np == 0) np = bus_rd32(15 * 4);  // uninitialised interrupt vector
    push32(c, c.pc);
    push16(c, old);
    c.pc = np;
    c.cycles += 44;
}
inline void check_irq(Cpu& c) {
    if (c.irq_level > ((c.sr >> 8) & 7)) take_interrupt(c);
}

inline void set_ccr(Cpu& c, uint32_t v) { c.sr = (c.sr & ~FCCR) | (v & FCCR); }

inline void push16(Cpu& c, uint32_t v) { c.a[7] -= 2; bus_wr16(c.a[7], v); }
inline void push32(Cpu& c, uint32_t v) { c.a[7] -= 4; bus_wr32(c.a[7], v); }
inline uint32_t pop16(Cpu& c) { uint32_t v = bus_rd16(c.a[7]); c.a[7] += 2; return v; }
inline uint32_t pop32(Cpu& c) { uint32_t v = bus_rd32(c.a[7]); c.a[7] += 4; return v; }

// ---- arithmetic ----
template <int S> inline uint32_t op_add(Cpu& c, uint32_t s, uint32_t d) {
    constexpr uint32_t m = Sz<S>::mask, msb = Sz<S>::msb;
    const uint64_t r64 = (uint64_t)(s & m) + (d & m);
    const uint32_t r = (uint32_t)r64 & m;
    const uint32_t carry = r64 > m ? (FC | FX) : 0;
    const uint32_t ovf = (((s ^ r) & (d ^ r)) & msb) ? FV : 0;
    c.sr = (c.sr & ~FCCR) | carry | ovf | ((r & msb) ? FN : 0) | (r == 0 ? FZ : 0);
    return r;
}
template <int S> inline uint32_t op_addx(Cpu& c, uint32_t s, uint32_t d) {
    constexpr uint32_t m = Sz<S>::mask, msb = Sz<S>::msb;
    const uint64_t r64 = (uint64_t)(s & m) + (d & m) + ((c.sr & FX) ? 1 : 0);
    const uint32_t r = (uint32_t)r64 & m;
    const uint32_t carry = r64 > m ? (FC | FX) : 0;
    const uint32_t ovf = (((s ^ r) & (d ^ r)) & msb) ? FV : 0;
    c.sr = (c.sr & ~(FCCR & ~FZ)) | carry | ovf | ((r & msb) ? FN : 0);
    if (r) c.sr &= ~FZ;
    return r;
}
// flags for d - s; X only for sub/subx/neg
template <int S> inline uint32_t sub_flags(Cpu& c, uint32_t s, uint32_t d, bool set_x) {
    constexpr uint32_t m = Sz<S>::mask, msb = Sz<S>::msb;
    const uint32_t r = (d - s) & m;
    const bool borrow = (s & m) > (d & m);
    const uint32_t ovf = (((s ^ d) & (r ^ d)) & msb) ? FV : 0;
    uint32_t keep = FX;
    uint32_t add = 0;
    if (set_x) { keep = 0; if (borrow) add |= FX; }
    c.sr = (c.sr & ~(FCCR & ~keep)) | (borrow ? FC : 0) | add | ovf | ((r & msb) ? FN : 0) | (r == 0 ? FZ : 0);
    return r;
}
template <int S> inline uint32_t op_sub(Cpu& c, uint32_t s, uint32_t d) { return sub_flags<S>(c, s, d, true); }
template <int S> inline void op_cmp(Cpu& c, uint32_t s, uint32_t d) { sub_flags<S>(c, s, d, false); }
template <int S> inline uint32_t op_subx(Cpu& c, uint32_t s, uint32_t d) {
    constexpr uint32_t m = Sz<S>::mask, msb = Sz<S>::msb;
    const uint32_t x = (c.sr & FX) ? 1 : 0;
    const uint32_t r = (d - s - x) & m;
    const bool borrow = (uint64_t)(s & m) + x > (uint64_t)(d & m);
    const uint32_t ovf = (((s ^ d) & (r ^ d)) & msb) ? FV : 0;
    c.sr = (c.sr & ~(FCCR & ~FZ)) | (borrow ? (FC | FX) : 0) | ovf | ((r & msb) ? FN : 0);
    if (r) c.sr &= ~FZ;
    return r;
}
template <int S> inline uint32_t op_neg(Cpu& c, uint32_t d) { return op_sub<S>(c, d, 0); }
template <int S> inline uint32_t op_negx(Cpu& c, uint32_t d) { return op_subx<S>(c, d, 0); }

template <int S> inline uint32_t op_and(Cpu& c, uint32_t s, uint32_t d) { uint32_t r = (s & d) & Sz<S>::mask; flags_logic<S>(c, r); return r; }
template <int S> inline uint32_t op_or(Cpu& c, uint32_t s, uint32_t d) { uint32_t r = (s | d) & Sz<S>::mask; flags_logic<S>(c, r); return r; }
template <int S> inline uint32_t op_eor(Cpu& c, uint32_t s, uint32_t d) { uint32_t r = (s ^ d) & Sz<S>::mask; flags_logic<S>(c, r); return r; }
template <int S> inline uint32_t op_not(Cpu& c, uint32_t d) { uint32_t r = ~d & Sz<S>::mask; flags_logic<S>(c, r); return r; }
template <int S> inline void op_tst(Cpu& c, uint32_t v) { flags_logic<S>(c, v); }
template <int S> inline void op_move_flags(Cpu& c, uint32_t v) { flags_logic<S>(c, v); }
inline void op_clr_flags(Cpu& c) { c.sr = (c.sr & ~(FN | FV | FC)) | FZ; }

inline uint32_t op_ext_w(Cpu& c, uint32_t d) { uint32_t w = sx8(d) & 0xffff; flags_logic<2>(c, w); return (d & 0xffff0000u) | w; }
inline uint32_t op_ext_l(Cpu& c, uint32_t d) { uint32_t r = sx16(d); flags_logic<4>(c, r); return r; }
inline uint32_t op_swap(Cpu& c, uint32_t d) { uint32_t r = (d << 16) | (d >> 16); flags_logic<4>(c, r); return r; }

// ---- multiply / divide (flags as in Musashi) ----
inline uint32_t op_mulu(Cpu& c, uint32_t s, uint32_t d) {
    const uint32_t r = (s & 0xffff) * (d & 0xffff);
    set_nz_clear_vc(c, (r & 0x80000000u) ? FN : 0, r == 0 ? FZ : 0);
    return r;
}
inline uint32_t op_muls(Cpu& c, uint32_t s, uint32_t d) {
    const uint32_t r = (uint32_t)((int32_t)(int16_t)s * (int32_t)(int16_t)d);
    set_nz_clear_vc(c, (r & 0x80000000u) ? FN : 0, r == 0 ? FZ : 0);
    return r;
}
// 68000 multiply timing as modelled by Musashi: 2 cycles per set bit (mulu) / per bit transition (muls)
inline uint32_t mulu_cycles(uint32_t s) {
    uint32_t n = 0;
    for (uint32_t y = s & 0xffff; y; y >>= 1)
        if (y & 1) n += 2;
    return n;
}
inline uint32_t muls_cycles(uint32_t s) {
    uint32_t n = 0, f = 0;
    for (uint32_t y = sx16(s); y; y >>= 1)
        if ((y & 1) != f) { n += 2; f = 1 - f; }
    return n;
}

// Returns false for division by zero (the caller raises the exception through unsupported()).
inline bool op_divu(Cpu& c, uint32_t s, uint32_t& d) {
    s &= 0xffff;
    if (s == 0) return false;
    const uint32_t q = d / s, rem = d % s;
    if (q < 0x10000) {
        c.sr = (c.sr & ~(FN | FZ | FV | FC)) | ((q & 0x8000) ? FN : 0) | (q == 0 ? FZ : 0);
        d = (q & 0xffff) | (rem << 16);
    } else {
        c.sr |= FV;
    }
    return true;
}
inline bool op_divs(Cpu& c, uint32_t s, uint32_t& d) {
    const int32_t src = (int16_t)s;
    if (src == 0) return false;
    if (d == 0x80000000u && src == -1) {
        c.sr = (c.sr & ~(FN | FZ | FV | FC)) | FZ;
        d = 0;
        return true;
    }
    const int32_t q = (int32_t)d / src, rem = (int32_t)d % src;
    if (q == (int16_t)q) {
        c.sr = (c.sr & ~(FN | FZ | FV | FC)) | ((q & 0x8000) ? FN : 0) | ((q & 0xffff) == 0 ? FZ : 0);
        d = ((uint32_t)q & 0xffff) | ((uint32_t)rem << 16);
    } else {
        c.sr |= FV;
    }
    return true;
}

// ---- BCD (V/N follow Musashi's "undefined" behaviour) ----
inline uint32_t op_abcd(Cpu& c, uint32_t s, uint32_t d) {
    uint32_t res = (s & 0xf) + (d & 0xf) + ((c.sr & FX) ? 1 : 0);
    uint32_t v = ~res;
    if (res > 9) res += 6;
    res += (s & 0xf0) + (d & 0xf0);
    const bool carry = res > 0x99;
    if (carry) res -= 0xa0;
    v &= res;
    uint32_t sr = c.sr & ~(FC | FX | FV | FN);
    if (carry) sr |= FC | FX;
    if (v & 0x80) sr |= FV;
    if (res & 0x80) sr |= FN;
    res &= 0xff;
    if (res) sr &= ~FZ;
    c.sr = sr;
    return res;
}
inline uint32_t op_sbcd(Cpu& c, uint32_t s, uint32_t d) {
    uint32_t res = (d & 0xf) - (s & 0xf) - ((c.sr & FX) ? 1 : 0);
    uint32_t v = ~res;
    if (res > 9) res -= 6;
    res += (d & 0xf0) - (s & 0xf0);
    const bool carry = res > 0x99;
    if (carry) res += 0xa0;
    res &= 0xff;
    v &= res;
    uint32_t sr = c.sr & ~(FC | FX | FV | FN);
    if (carry) sr |= FC | FX;
    if (v & 0x80) sr |= FV;
    if (res & 0x80) sr |= FN;
    if (res) sr &= ~FZ;
    c.sr = sr;
    return res;
}

// ---- shifts and rotates ----
enum ShiftKind { SH_AS = 0, SH_LS = 1, SH_ROX = 2, SH_RO = 3 };

// `cnt` is the full shift count (0..63). Cycles are charged by the caller (2 per shift on a 68000).
template <int S> inline uint32_t op_shift(Cpu& c, int kind, bool left, uint32_t val, uint32_t cnt) {
    constexpr uint32_t m = Sz<S>::mask, msb = Sz<S>::msb, bits = Sz<S>::bits;
    const uint32_t d = val & m;
    uint32_t r = d;
    uint32_t carry = 0, ovf = 0;
    bool set_x = true;
    switch (kind) {
    case SH_LS:
    case SH_AS:
        if (cnt == 0) {
            set_x = false;
        } else if (left) {
            if (cnt < bits) {
                r = (d << cnt) & m;
                carry = (d >> (bits - cnt)) & 1;
            } else {
                r = 0;
                carry = (cnt == bits) ? (d & 1) : 0;
            }
            if (kind == SH_AS) {
                // V: the sign changed at any point during the shift
                uint32_t mask;
                if (cnt >= bits) mask = m;
                else mask = (m << (bits - cnt - 1)) & m;
                const uint32_t t = d & mask;
                ovf = (cnt >= bits) ? (t != 0) : !(t == 0 || t == mask);
            }
        } else {
            if (kind == SH_LS) {
                if (cnt < bits) {
                    r = d >> cnt;
                    carry = (d >> (cnt - 1)) & 1;
                } else {
                    r = 0;
                    carry = (cnt == bits) ? ((d & msb) ? 1 : 0) : 0;
                }
            } else {
                if (cnt < bits) {
                    r = (uint32_t)(((int32_t)sx<S>(d)) >> cnt) & m;
                    carry = (d >> (cnt - 1)) & 1;
                } else {
                    r = (d & msb) ? m : 0;
                    carry = (d & msb) ? 1 : 0;
                }
            }
        }
        break;
    case SH_RO:
        set_x = false;
        if (cnt == 0) break;
        {
            const uint32_t n = cnt % bits;
            if (left) {
                r = n ? (((d << n) | (d >> (bits - n))) & m) : d;
                carry = r & 1;
            } else {
                r = n ? (((d >> n) | (d << (bits - n))) & m) : d;
                carry = (r & msb) ? 1 : 0;
            }
        }
        break;
    case SH_ROX:
        if (cnt == 0) {
            set_x = false;
            carry = (c.sr & FX) ? 1 : 0;  // C = X
            break;
        }
        {
            uint32_t x = (c.sr & FX) ? 1 : 0;
            uint32_t n = cnt % (bits + 1);
            for (uint32_t i = 0; i < n; i++) {
                if (left) {
                    const uint32_t out = (r & msb) ? 1 : 0;
                    r = ((r << 1) | x) & m;
                    x = out;
                } else {
                    const uint32_t out = r & 1;
                    r = (r >> 1) | (x ? msb : 0);
                    x = out;
                }
            }
            carry = x;
        }
        break;
    }
    uint32_t sr = c.sr & ~(FN | FZ | FV | FC);
    if (carry) sr |= FC;
    if (set_x && cnt != 0) sr = (sr & ~FX) | (carry ? FX : 0);
    if (kind == SH_ROX && cnt == 0) sr &= ~FX, sr |= (c.sr & FX);
    if (ovf) sr |= FV;
    if (r & msb) sr |= FN;
    if (r == 0) sr |= FZ;
    c.sr = sr;
    return r;
}

// ---- bit operations: only Z is affected ----
inline void bit_z(Cpu& c, uint32_t val, uint32_t bit) {
    c.sr = (c.sr & ~FZ) | (((val >> bit) & 1) ? 0 : FZ);
}

}  // namespace r68k
