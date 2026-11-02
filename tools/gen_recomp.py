#!/usr/bin/env python3
"""Translate the 68000 program of Blood Bros. into C++ (one function per instruction).

  python3 tools/dump_prg.py roms/bloodbro.zip prg.bin
  python3 tools/gen_recomp.py prg.bin data/analysis_bloodbro.json data/m68k_cycles.bin generated/recomp_insns.inc

Every instruction that the trace/static analysis found gets a function

    static inline void i_00410(Cpu& c) { ... }

which performs exactly what the interpreter would do: operands, flags, bus
accesses (same order and size), cycles and the new program counter. The
functions use recomp/cpu68k.h. Operand addressing modes are decoded here, at
generation time, so the generated code contains constants, not decoding.

Instructions the generator does not handle (exceptions, privileged ops, ...)
become calls to r68k::unsupported(), which ends the game with an error message
(there is no interpreter; the current program has no such instructions).

The generator was verified against the Musashi interpreter (all 17,012 instructions
of the program and a sweep over the whole 68000 instruction set); data/m68k_cycles.bin
is Musashi's cycle table.
"""
import argparse
import json
import sys


class Unsupported(Exception):
    pass


def u16(prg, a):
    return (prg[a] << 8) | prg[a + 1]


def s8(v):
    return v - 0x100 if v & 0x80 else v


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def hexu(v):
    return f"{v & 0xffffffff:#x}u"


SZ = {0: 1, 1: 2, 2: 4}  # size field (bits 7-6) -> bytes
CCNAME = ["t", "f", "hi", "ls", "cc", "cs", "ne", "eq", "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"]


class EA:
    """A decoded effective address. kind: d, a, mem, imm."""

    def __init__(self, kind, reg=0, var=None, value=None, mode=None):
        self.kind, self.reg, self.var, self.value, self.mode = kind, reg, var, value, mode


class Gen:
    def __init__(self, prg, addr, cycles_table):
        self.prg = prg
        self.addr = addr
        self.op = u16(prg, addr)
        self.pos = addr + 2          # address of the next extension word
        self.lines = []
        self.tmp = 0
        self.extra_cycles = []       # C++ expressions added to the base cycle count
        self.base_cycles = cycles_table[self.op]
        self.pc_set = False
        self.writes_sr = False      # may change the interrupt mask: check for pending interrupts afterwards
        self.may_trap = False       # may raise an exception (division by zero)

    # ---- helpers ----
    def emit(self, s):
        self.lines.append(s)

    def new(self, prefix="t"):
        self.tmp += 1
        return f"{prefix}{self.tmp}"

    def word(self):
        w = u16(self.prg, self.pos)
        self.pos += 2
        return w

    @property
    def next_pc(self):
        return self.pos

    # ---- effective addresses ----
    def ea(self, mode, reg, size):
        """Decode (and emit address computation for) an effective address. size in bytes."""
        if mode == 0:
            return EA("d", reg)
        if mode == 1:
            return EA("a", reg)
        v = self.new("ea")
        step = 2 if (size == 1 and reg == 7) else size
        if mode == 2:
            self.emit(f"uint32_t {v} = c.a[{reg}];")
        elif mode == 3:
            self.emit(f"uint32_t {v} = c.a[{reg}]; c.a[{reg}] += {step};")
        elif mode == 4:
            self.emit(f"c.a[{reg}] -= {step}; uint32_t {v} = c.a[{reg}];")
        elif mode == 5:
            d = s16(self.word())
            self.emit(f"uint32_t {v} = c.a[{reg}] + {hexu(d)};")
        elif mode == 6:
            ext = self.word()
            self.emit(f"uint32_t {v} = c.a[{reg}] + {hexu(s8(ext & 0xff))} + {self.index(ext)};")
        elif mode == 7:
            if reg == 0:
                a = s16(self.word())
                self.emit(f"uint32_t {v} = {hexu(a)};")
            elif reg == 1:
                hi = self.word()
                lo = self.word()
                self.emit(f"uint32_t {v} = {hexu((hi << 16) | lo)};")
            elif reg == 2:
                base = self.pos
                d = s16(self.word())
                self.emit(f"uint32_t {v} = {hexu(base + d)};")
            elif reg == 3:
                base = self.pos
                ext = self.word()
                self.emit(f"uint32_t {v} = {hexu(base + s8(ext & 0xff))} + {self.index(ext)};")
            elif reg == 4:
                if size == 4:
                    hi = self.word()
                    lo = self.word()
                    val = (hi << 16) | lo
                elif size == 2:
                    val = self.word()
                else:
                    val = self.word() & 0xff
                return EA("imm", value=val)
            else:
                raise Unsupported("ea mode 7/%d" % reg)
        else:
            raise Unsupported("ea mode")
        return EA("mem", reg, var=v, mode=mode)

    def index(self, ext):
        if ext & 0x100:
            raise Unsupported("68020 index format")
        r = (ext >> 12) & 7
        base = f"c.a[{r}]" if ext & 0x8000 else f"c.d[{r}]"
        return base if ext & 0x800 else f"sx16({base})"

    def read(self, e, size):
        if e.kind == "d":
            return f"getd<{size}>(c, {e.reg})"
        if e.kind == "a":
            return f"c.a[{e.reg}]" if size == 4 else f"(c.a[{e.reg}] & {hexu((1 << (8 * size)) - 1)})"
        if e.kind == "imm":
            return hexu(e.value)
        return f"rd<{size}>({e.var})"

    def write(self, e, size, val):
        if e.kind == "d":
            self.emit(f"setd<{size}>(c, {e.reg}, {val});")
        elif e.kind == "mem" and e.var:
            self.emit(f"wr<{size}>({e.var}, {val});")
        else:
            raise Unsupported("write to " + e.kind)

    # ---- finishing ----
    def finish(self, set_pc=None):
        cyc = str(self.base_cycles)
        for x in self.extra_cycles:
            cyc += f" + ({x})"
        self.emit(f"c.cycles += {cyc};")
        if set_pc is not None:
            self.emit(f"c.pc = {set_pc};")
        elif not self.pc_set:
            self.emit(f"c.pc = {hexu(self.next_pc)};")
        if self.writes_sr:
            self.emit("check_irq(c);")


def pc_rel_or_imm(mode, reg):
    """Modes that are not valid for data-alterable operands on the 68000 (PC-relative, immediate)."""
    return mode == 7 and reg in (2, 3, 4)


def mnemonic_of(g):
    return g.op


# =============================================================================
# Instruction decoding / code generation
# =============================================================================
def gen_instruction(prg, addr, cycles):
    g = Gen(prg, addr, cycles)
    op = g.op
    line = op >> 12
    try:
        if line == 0:
            gen_line0(g)
        elif line in (1, 2, 3):
            gen_move(g)
        elif line == 4:
            gen_line4(g)
        elif line == 5:
            gen_line5(g)
        elif line == 6:
            gen_branch(g)
        elif line == 7:
            gen_moveq(g)
        elif line == 8:
            gen_line8_c(g, is_or=True)
        elif line == 9:
            gen_addsub(g, sub=True)
        elif line == 0xB:
            gen_lineB(g)
        elif line == 0xC:
            gen_line8_c(g, is_or=False)
        elif line == 0xD:
            gen_addsub(g, sub=False)
        elif line == 0xE:
            gen_shift(g)
        else:
            raise Unsupported("line A/F")
    except Unsupported as e:
        return None, g, str(e)
    return g.lines, g, None


def modereg(op):
    return (op >> 3) & 7, op & 7


# ---- line 0: immediate ops, bit ops ----
def gen_line0(g):
    op = g.op
    mode, reg = modereg(op)
    if op & 0x0100:
        # dynamic bit op (btst/bchg/bclr/bset Dn,<ea>) or movep
        if mode == 1:
            raise Unsupported("movep")
        bitreg = (op >> 9) & 7
        gen_bitop(g, (op >> 6) & 3, f"c.d[{bitreg}]", mode, reg)
        return
    kind = (op >> 9) & 7
    if kind == 4:
        # static bit op: immediate bit number comes first
        bitnum = g.word() & 0xff
        gen_bitop(g, (op >> 6) & 3, hexu(bitnum), mode, reg)
        return
    size = SZ.get((op >> 6) & 3)
    if size is None or kind == 7:
        raise Unsupported("line0")
    names = {0: "or", 1: "and", 2: "sub", 3: "add", 5: "eor", 6: "cmp"}
    if kind not in names:
        raise Unsupported("line0 kind")
    name = names[kind]
    if mode == 7 and reg == 4:
        # ori/andi/eori to CCR / SR
        if name not in ("or", "and", "eor"):
            raise Unsupported("imm to ccr")
        imm = g.word()
        if size == 1:
            imm &= 0xff
            fn = {"or": "|", "and": "&", "eor": "^"}[name]
            g.emit(f"set_ccr(c, (c.sr {fn} {hexu(imm)}) & FCCR);")
        elif size == 2:
            fn = {"or": "|", "and": "&", "eor": "^"}[name]
            g.emit(f"set_sr(c, c.sr {fn} {hexu(imm)});")
            g.writes_sr = True
        else:
            raise Unsupported("imm to sr size")
        g.finish()
        return
    imm = g.word() if size != 4 else ((g.word() << 16) | g.word())
    if size == 1:
        imm &= 0xff
    if mode == 7 and reg in (2, 3):
        raise Unsupported("immediate op on PC-relative (68010+)")
    e = g.ea(mode, reg, size)
    if e.kind == "a":
        raise Unsupported("imm op on An")
    src = hexu(imm)
    dst = g.read(e, size)
    fn = {"or": "op_or", "and": "op_and", "eor": "op_eor", "sub": "op_sub", "add": "op_add"}.get(name)
    if name == "cmp":
        g.emit(f"op_cmp<{size}>(c, {src}, {dst});")
    else:
        r = g.new("r")
        g.emit(f"uint32_t {r} = {fn}<{size}>(c, {src}, {dst});")
        g.write(e, size, r)
    g.finish()


def gen_bitop(g, which, bitexpr, mode, reg):
    # which: 0 btst, 1 bchg, 2 bclr, 3 bset
    if mode == 0:
        b = g.new("b")
        g.emit(f"uint32_t {b} = {bitexpr} & 31;")
        g.emit(f"uint32_t v = c.d[{reg}];")
        g.emit(f"bit_z(c, v, {b});")
        if which == 1:
            g.emit(f"c.d[{reg}] = v ^ (1u << {b});")
        elif which == 2:
            g.emit(f"c.d[{reg}] = v & ~(1u << {b});")
        elif which == 3:
            g.emit(f"c.d[{reg}] = v | (1u << {b});")
    else:
        if mode == 1:
            raise Unsupported("bit op on An")
        if which != 0 and pc_rel_or_imm(mode, reg):
            raise Unsupported("modifying bit op on PC-relative/immediate")
        e = g.ea(mode, reg, 1)
        if e.kind == "imm" and which != 0:
            raise Unsupported("bit op imm")
        b = g.new("b")
        g.emit(f"uint32_t {b} = {bitexpr} & 7;")
        g.emit(f"uint32_t v = {g.read(e, 1)};")
        g.emit(f"bit_z(c, v, {b});")
        if which == 1:
            g.emit(f"wr<1>({e.var}, v ^ (1u << {b}));")
        elif which == 2:
            g.emit(f"wr<1>({e.var}, v & ~(1u << {b}));")
        elif which == 3:
            g.emit(f"wr<1>({e.var}, v | (1u << {b}));")
    g.finish()


# ---- lines 1-3: move ----
def gen_move(g):
    op = g.op
    size = {1: 1, 3: 2, 2: 4}[op >> 12]
    smode, sreg = modereg(op)
    dmode, dreg = (op >> 6) & 7, (op >> 9) & 7
    src = g.ea(smode, sreg, size)
    if dmode == 1:
        if size == 1:
            raise Unsupported("movea.b")
        v = g.new("v")
        g.emit(f"uint32_t {v} = {g.read(src, size)};")
        g.emit(f"c.a[{dreg}] = sx<{size}>({v});")
        g.finish()
        return
    v = g.new("v")
    g.emit(f"uint32_t {v} = {g.read(src, size)};")
    if pc_rel_or_imm(dmode, dreg):
        raise Unsupported("move to PC-relative/immediate")
    dst = g.ea(dmode, dreg, size)
    if dst.kind in ("a", "imm"):
        raise Unsupported("move dst")
    g.emit(f"op_move_flags<{size}>(c, {v});")
    if size == 4 and dst.kind == "mem" and dst.mode == 4:
        g.emit(f"wr<2>({dst.var} + 2, {v} & 0xffff); wr<2>({dst.var}, {v} >> 16);")
    else:
        g.write(dst, size, v)
    g.finish()


# ---- line 4: miscellaneous ----
def gen_line4(g):
    op = g.op
    mode, reg = modereg(op)
    if op == 0x4E71:
        g.finish()
        return
    if op == 0x4E75:
        g.emit("c.pc = pop32(c);")
        g.pc_set = True
        g.finish()
        return
    if op == 0x4E73:
        g.emit("uint32_t nsr = pop16(c); uint32_t npc = pop32(c); set_sr(c, nsr); c.pc = npc;")
        g.pc_set = True
        g.writes_sr = True
        g.finish()
        return
    if op == 0x4E77:
        g.emit("uint32_t nccr = pop16(c); uint32_t npc = pop32(c); set_ccr(c, nccr); c.pc = npc;")
        g.pc_set = True
        g.finish()
        return
    if (op & 0xFFF8) == 0x4E50:  # link
        d = s16(g.word())
        if reg == 7:  # the pushed value is the already decremented stack pointer
            g.emit(f"c.a[7] -= 4; bus_wr32(c.a[7], c.a[7]); c.a[7] += {hexu(d)};")
        else:
            g.emit(f"push32(c, c.a[{reg}]); c.a[{reg}] = c.a[7]; c.a[7] += {hexu(d)};")
        g.finish()
        return
    if (op & 0xFFF8) == 0x4E58:  # unlk
        g.emit(f"c.a[7] = c.a[{reg}]; c.a[{reg}] = pop32(c);")
        g.finish()
        return
    if (op & 0xFFC0) == 0x4E80:  # jsr
        e = g.ea(mode, reg, 4)
        if e.kind != "mem":
            raise Unsupported("jsr ea")
        g.emit(f"push32(c, {hexu(g.next_pc)});")
        g.emit(f"c.pc = {e.var};")
        g.pc_set = True
        g.finish()
        return
    if (op & 0xFFC0) == 0x4EC0:  # jmp
        e = g.ea(mode, reg, 4)
        if e.kind != "mem":
            raise Unsupported("jmp ea")
        g.emit(f"c.pc = {e.var};")
        g.pc_set = True
        g.finish()
        return
    if (op & 0xF1C0) == 0x41C0:  # lea
        e = g.ea(mode, reg, 4)
        if e.kind != "mem":
            raise Unsupported("lea ea")
        g.emit(f"c.a[{(op >> 9) & 7}] = {e.var};")
        g.finish()
        return
    if (op & 0xFFC0) == 0x4840 and mode != 0:  # pea
        e = g.ea(mode, reg, 4)
        if e.kind != "mem":
            raise Unsupported("pea ea")
        g.emit(f"push32(c, {e.var});")
        g.finish()
        return
    if (op & 0xFFF8) == 0x4840:  # swap
        g.emit(f"c.d[{reg}] = op_swap(c, c.d[{reg}]);")
        g.finish()
        return
    if (op & 0xFFB8) == 0x4880:  # ext.w / ext.l
        if op & 0x40:
            g.emit(f"c.d[{reg}] = op_ext_l(c, c.d[{reg}]);")
        else:
            g.emit(f"c.d[{reg}] = op_ext_w(c, c.d[{reg}]);")
        g.finish()
        return
    if (op & 0xFB80) == 0x4880:  # movem
        gen_movem(g)
        return
    sz = SZ.get((op >> 6) & 3)
    base = op & 0xFF00
    if base in (0x4200, 0x4400, 0x4000, 0x4600, 0x4A00) and sz is not None:
        if pc_rel_or_imm(mode, reg):
            raise Unsupported("unary op on PC-relative/immediate (68020+)")
        e = g.ea(mode, reg, sz)
        if e.kind in ("a", "imm"):
            raise Unsupported("unary ea")
        if base == 0x4A00:
            g.emit(f"op_tst<{sz}>(c, {g.read(e, sz)});")
        elif base == 0x4200:
            g.emit("op_clr_flags(c);")
            g.write(e, sz, "0")
        else:
            fn = {0x4400: "op_neg", 0x4000: "op_negx", 0x4600: "op_not"}[base]
            r = g.new("r")
            g.emit(f"uint32_t {r} = {fn}<{sz}>(c, {g.read(e, sz)});")
            g.write(e, sz, r)
        g.finish()
        return
    if (op & 0xFFC0) == 0x40C0:  # move from sr
        e = g.ea(mode, reg, 2)
        g.write(e, 2, "c.sr")
        g.finish()
        return
    if (op & 0xFFC0) == 0x44C0:  # move to ccr
        e = g.ea(mode, reg, 2)
        g.emit(f"set_ccr(c, {g.read(e, 2)});")
        g.finish()
        return
    if (op & 0xFFC0) == 0x46C0:  # move to sr
        e = g.ea(mode, reg, 2)
        g.emit(f"set_sr(c, {g.read(e, 2)});")
        g.writes_sr = True
        g.finish()
        return
    raise Unsupported("line4 0x%04x" % op)


def gen_movem(g):
    op = g.op
    mode, reg = modereg(op)
    size = 4 if op & 0x40 else 2
    to_reg = bool(op & 0x400)
    mask = g.word()
    cyc_per = 8 if size == 4 else 4  # count << CYC_MOVEM_L (3) / CYC_MOVEM_W (2)
    if not to_reg:
        if mode == 4:  # predecrement: bit 0 = A7 ... bit 15 = D0
            g.emit(f"uint32_t ea = c.a[{reg}]; int n = 0;")
            for i in range(16):
                if mask & (1 << i):
                    r = 15 - i
                    regexpr = f"c.d[{r}]" if r < 8 else f"c.a[{r - 8}]"
                    if size == 4:
                        g.emit(f"ea -= 4; wr<2>(ea + 2, {regexpr} & 0xffff); wr<2>(ea, {regexpr} >> 16); n++;")
                    else:
                        g.emit(f"ea -= 2; wr<2>(ea, {regexpr}); n++;")
            g.emit(f"c.a[{reg}] = ea;")
        else:
            if mode in (0, 1, 3) or (mode == 7 and reg > 1):
                raise Unsupported("movem ea")
            e = g.ea(mode, reg, size)
            g.emit(f"uint32_t ea = {e.var}; int n = 0;")
            for i in range(16):
                if mask & (1 << i):
                    regexpr = f"c.d[{i}]" if i < 8 else f"c.a[{i - 8}]"
                    g.emit(f"wr<{size}>(ea, {regexpr}); ea += {size}; n++;")
    else:
        if mode in (0, 1, 4) or (mode == 7 and reg == 4):
            raise Unsupported("movem ea")
        e = g.ea(mode, reg, size)
        if mode == 3:
            g.emit(f"uint32_t ea = {e.var}; int n = 0;")
        else:
            g.emit(f"uint32_t ea = {e.var}; int n = 0;")
        for i in range(16):
            if mask & (1 << i):
                regexpr = f"c.d[{i}]" if i < 8 else f"c.a[{i - 8}]"
                if size == 2:
                    g.emit(f"{regexpr} = sx16(rd<2>(ea)); ea += 2; n++;")
                else:
                    g.emit(f"{regexpr} = rd<4>(ea); ea += 4; n++;")
        if mode == 3:
            g.emit(f"c.a[{reg}] = ea;")
    g.extra_cycles.append(f"n * {cyc_per}")
    g.finish()


# ---- line 5: addq / subq / scc / dbcc ----
def gen_line5(g):
    op = g.op
    mode, reg = modereg(op)
    if ((op >> 6) & 3) == 3:
        cc = (op >> 8) & 15
        if mode == 1:  # dbcc
            disp = s16(g.word())
            target = g.pos - 2 + disp
            g.emit(f"if (cc_true(c.sr, {cc})) {{")
            g.emit(f"  c.cycles += {g.base_cycles};")
            g.emit(f"  c.pc = {hexu(g.next_pc)};")
            g.emit("} else {")
            g.emit(f"  uint32_t w = (c.d[{reg}] - 1) & 0xffff;")
            g.emit(f"  c.d[{reg}] = (c.d[{reg}] & 0xffff0000u) | w;")
            g.emit("  if (w != 0xffff) {")
            g.emit(f"    c.cycles += {g.base_cycles - 2}; c.pc = {hexu(target)};")
            g.emit("  } else {")
            g.emit(f"    c.cycles += {g.base_cycles + 2}; c.pc = {hexu(g.next_pc)};")
            g.emit("  }")
            g.emit("}")
            g.pc_set = True
            g.base_cycles = None
            return
        if pc_rel_or_imm(mode, reg):
            raise Unsupported("scc on PC-relative/immediate")
        e = g.ea(mode, reg, 1)
        if e.kind in ("a", "imm"):
            raise Unsupported("scc ea")
        if e.kind == "d" and cc == 0:  # st Dn: its own handler, no extra cycles
            g.emit(f"setd<1>(c, {reg}, 0xff);")
        elif e.kind == "d":
            g.emit(f"if (cc_true(c.sr, {cc})) {{ setd<1>(c, {reg}, 0xff); c.cycles += 2; }} else {{ setd<1>(c, {reg}, 0); }}")
        else:
            g.emit(f"wr<1>({e.var}, cc_true(c.sr, {cc}) ? 0xff : 0);")
        g.finish()
        return
    size = SZ[(op >> 6) & 3]
    q = (op >> 9) & 7 or 8
    sub = bool(op & 0x100)
    e = g.ea(mode, reg, size)
    if e.kind == "a":
        g.emit(f"c.a[{reg}] {'-=' if sub else '+='} {q};")
    else:
        fn = "op_sub" if sub else "op_add"
        r = g.new("r")
        g.emit(f"uint32_t {r} = {fn}<{size}>(c, {q}, {g.read(e, size)});")
        g.write(e, size, r)
    g.finish()


# ---- line 6: bra / bsr / bcc ----
def gen_branch(g):
    op = g.op
    cc = (op >> 8) & 15
    disp = op & 0xff
    base = g.pos  # address of the (optional) extension word = pc used for the displacement
    if disp == 0:
        disp = s16(g.word())
        wide = True
    elif disp == 0xff:
        raise Unsupported("68020 long branch")
    else:
        disp = s8(disp)
        wide = False
    target = base + disp
    if cc == 0:  # bra
        g.emit(f"c.pc = {hexu(target)};")
        g.pc_set = True
        g.finish()
    elif cc == 1:  # bsr
        g.emit(f"push32(c, {hexu(g.next_pc)});")
        g.emit(f"c.pc = {hexu(target)};")
        g.pc_set = True
        g.finish()
    else:
        nt = 2 if wide else -2  # CYC_BCC_NOTAKE_W / _B
        g.emit(f"if (cc_true(c.sr, {cc})) {{ c.cycles += {g.base_cycles}; c.pc = {hexu(target)}; }}")
        g.emit(f"else {{ c.cycles += {g.base_cycles + nt}; c.pc = {hexu(g.next_pc)}; }}")
        g.pc_set = True
        g.base_cycles = None


def gen_moveq(g):
    op = g.op
    if op & 0x100:
        raise Unsupported("moveq")
    g.emit(f"c.d[{(op >> 9) & 7}] = {hexu(s8(op & 0xff))};")
    g.emit(f"flags_logic<4>(c, c.d[{(op >> 9) & 7}]);")
    g.finish()


# ---- lines 8 / C: or, and, div, mul, bcd, exg ----
def gen_line8_c(g, is_or):
    op = g.op
    mode, reg = modereg(op)
    dn = (op >> 9) & 7
    opm = (op >> 6) & 7
    if opm == 3:  # divu / mulu
        e = g.ea(mode, reg, 2)
        if e.kind == "a":
            raise Unsupported("mul/div An")
        if is_or:
            g.emit(f"uint32_t dv = c.d[{dn}];")
            g.emit(f"if (!op_divu(c, {g.read(e, 2)}, dv)) {{ exception_trap(c, 5, {hexu(g.next_pc)}, 38); return; }}")
            g.may_trap = True
            g.emit(f"c.d[{dn}] = dv;")
        else:
            g.emit(f"uint32_t ms = {g.read(e, 2)};")
            g.emit(f"c.d[{dn}] = op_mulu(c, ms, c.d[{dn}]);")
            g.extra_cycles.append("mulu_cycles(ms)")
        g.finish()
        return
    if opm == 7:  # divs / muls
        e = g.ea(mode, reg, 2)
        if e.kind == "a":
            raise Unsupported("mul/div An")
        if is_or:
            g.emit(f"uint32_t dv = c.d[{dn}];")
            g.emit(f"if (!op_divs(c, {g.read(e, 2)}, dv)) {{ exception_trap(c, 5, {hexu(g.next_pc)}, 38); return; }}")
            g.may_trap = True
            g.emit(f"c.d[{dn}] = dv;")
        else:
            g.emit(f"uint32_t ms = {g.read(e, 2)};")
            g.emit(f"c.d[{dn}] = op_muls(c, ms, c.d[{dn}]);")
            g.extra_cycles.append("muls_cycles(ms)")
        g.finish()
        return
    if opm == 4 and mode in (0, 1):  # sbcd / abcd
        fn = "op_sbcd" if is_or else "op_abcd"
        if mode == 0:
            g.emit(f"setd<1>(c, {dn}, {fn}(c, getd<1>(c, {reg}), getd<1>(c, {dn})));")
        else:
            sreg = 7
            sstep = 2 if reg == 7 else 1
            dstep = 2 if dn == 7 else 1
            g.emit(f"c.a[{reg}] -= {sstep}; uint32_t s = rd<1>(c.a[{reg}]);")
            g.emit(f"c.a[{dn}] -= {dstep}; uint32_t d = rd<1>(c.a[{dn}]);")
            g.emit(f"wr<1>(c.a[{dn}], {fn}(c, s, d));")
        g.finish()
        return
    if not is_or and opm in (5, 6) and mode in (0, 1):  # exg
        if opm == 5 and mode == 0:
            g.emit(f"{{ uint32_t t = c.d[{dn}]; c.d[{dn}] = c.d[{reg}]; c.d[{reg}] = t; }}")
        elif opm == 5 and mode == 1:
            g.emit(f"{{ uint32_t t = c.a[{dn}]; c.a[{dn}] = c.a[{reg}]; c.a[{reg}] = t; }}")
        elif opm == 6 and mode == 1:
            g.emit(f"{{ uint32_t t = c.d[{dn}]; c.d[{dn}] = c.a[{reg}]; c.a[{reg}] = t; }}")
        else:
            raise Unsupported("exg")
        g.finish()
        return
    size = SZ.get(opm & 3)
    if size is None:
        raise Unsupported("line8/C")
    fn = "op_or" if is_or else "op_and"
    if opm < 4:  # <ea>,Dn
        e = g.ea(mode, reg, size)
        if e.kind == "a":
            raise Unsupported("logic An")
        g.emit(f"setd<{size}>(c, {dn}, {fn}<{size}>(c, {g.read(e, size)}, getd<{size}>(c, {dn})));")
    else:  # Dn,<ea>
        e = g.ea(mode, reg, size)
        if e.kind in ("a", "d", "imm"):
            raise Unsupported("logic Dn,<ea> ea")
        r = g.new("r")
        g.emit(f"uint32_t {r} = {fn}<{size}>(c, getd<{size}>(c, {dn}), {g.read(e, size)});")
        g.write(e, size, r)
    g.finish()


# ---- lines 9 / D: sub, add, suba, adda, subx, addx ----
def gen_addsub(g, sub):
    op = g.op
    mode, reg = modereg(op)
    dn = (op >> 9) & 7
    opm = (op >> 6) & 7
    fn = "op_sub" if sub else "op_add"
    if opm in (3, 7):  # suba / adda
        size = 2 if opm == 3 else 4
        e = g.ea(mode, reg, size)
        g.emit(f"c.a[{dn}] {'-=' if sub else '+='} sx<{size}>({g.read(e, size)});")
        g.finish()
        return
    size = SZ[opm & 3]
    if opm < 4:
        e = g.ea(mode, reg, size)
        if e.kind == "a" and size == 1:
            raise Unsupported("byte op on An")
        g.emit(f"setd<{size}>(c, {dn}, {fn}<{size}>(c, {g.read(e, size)}, getd<{size}>(c, {dn})));")
        g.finish()
        return
    # opm 4..6: addx/subx or Dn,<ea>
    xfn = "op_subx" if sub else "op_addx"
    if mode == 0:
        g.emit(f"setd<{size}>(c, {dn}, {xfn}<{size}>(c, getd<{size}>(c, {reg}), getd<{size}>(c, {dn})));")
        g.finish()
        return
    if mode == 1:
        sstep = 2 if (size == 1 and reg == 7) else size
        dstep = 2 if (size == 1 and dn == 7) else size
        g.emit(f"c.a[{reg}] -= {sstep}; uint32_t s = rd<{size}>(c.a[{reg}]);")
        g.emit(f"c.a[{dn}] -= {dstep}; uint32_t d = rd<{size}>(c.a[{dn}]);")
        g.emit(f"wr<{size}>(c.a[{dn}], {xfn}<{size}>(c, s, d));")
        g.finish()
        return
    e = g.ea(mode, reg, size)
    if e.kind != "mem":
        raise Unsupported("add Dn,<ea>")
    r = g.new("r")
    g.emit(f"uint32_t {r} = {fn}<{size}>(c, getd<{size}>(c, {dn}), {g.read(e, size)});")
    g.write(e, size, r)
    g.finish()


# ---- line B: cmp, cmpa, cmpm, eor ----
def gen_lineB(g):
    op = g.op
    mode, reg = modereg(op)
    dn = (op >> 9) & 7
    opm = (op >> 6) & 7
    if opm in (3, 7):
        size = 2 if opm == 3 else 4
        e = g.ea(mode, reg, size)
        g.emit(f"op_cmp<4>(c, sx<{size}>({g.read(e, size)}), c.a[{dn}]);")
        g.finish()
        return
    size = SZ[opm & 3]
    if opm < 4:
        e = g.ea(mode, reg, size)
        if e.kind == "a" and size == 1:
            raise Unsupported("cmp.b An")
        g.emit(f"op_cmp<{size}>(c, {g.read(e, size)}, getd<{size}>(c, {dn}));")
        g.finish()
        return
    if mode == 1:  # cmpm (Ay)+,(Ax)+
        step_s = 2 if (size == 1 and reg == 7) else size
        step_d = 2 if (size == 1 and dn == 7) else size
        g.emit(f"uint32_t s = rd<{size}>(c.a[{reg}]); c.a[{reg}] += {step_s};")
        g.emit(f"uint32_t d = rd<{size}>(c.a[{dn}]); c.a[{dn}] += {step_d};")
        g.emit(f"op_cmp<{size}>(c, s, d);")
        g.finish()
        return
    e = g.ea(mode, reg, size)  # eor Dn,<ea>
    if e.kind in ("a", "imm"):
        raise Unsupported("eor ea")
    r = g.new("r")
    g.emit(f"uint32_t {r} = op_eor<{size}>(c, getd<{size}>(c, {dn}), {g.read(e, size)});")
    g.write(e, size, r)
    g.finish()


# ---- line E: shifts and rotates ----
def gen_shift(g):
    op = g.op
    mode, reg = modereg(op)
    left = bool(op & 0x100)
    if ((op >> 6) & 3) == 3:  # memory shift, word, count 1
        if op & 0x0800:
            raise Unsupported("68020 bit-field instruction")
        kind = (op >> 9) & 3
        if pc_rel_or_imm(mode, reg):
            raise Unsupported("memory shift on PC-relative/immediate")
        e = g.ea(mode, reg, 2)
        if e.kind != "mem":
            raise Unsupported("memory shift ea")
        r = g.new("r")
        g.emit(f"uint32_t {r} = op_shift<2>(c, {kind}, {str(left).lower()}, {g.read(e, 2)}, 1);")
        g.write(e, 2, r)
        g.finish()
        return
    size = SZ[(op >> 6) & 3]
    kind = (op >> 3) & 3
    cnt_field = (op >> 9) & 7
    if op & 0x20:
        g.emit(f"uint32_t cnt = c.d[{cnt_field}] & 63;")
    else:
        g.emit(f"uint32_t cnt = {cnt_field or 8};")
    g.emit(f"setd<{size}>(c, {reg}, op_shift<{size}>(c, {kind}, {str(left).lower()}, getd<{size}>(c, {reg}), cnt));")
    g.extra_cycles.append("cnt * 2")
    g.finish()


# =============================================================================
SYMBOLS = []      # (start, size, name)
SYM_STARTS = []


def load_symbols(path):
    import bisect  # noqa: F401
    for line in open(path):
        if line[:1].isspace():  # continuation of the previous description
            continue
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        p = line.split(None, 3)
        SYMBOLS.append((int(p[0], 16), int(p[1], 0), p[2]))
    SYMBOLS.sort()
    SYM_STARTS.extend(s[0] for s in SYMBOLS)


def symbol_names(lines, own_addr):
    """Names of known symbols whose addresses appear as constants in the generated lines."""
    import bisect
    import re
    names = []
    for l in lines:
        for m in re.finditer(r"0x([0-9a-f]+)u", l):
            v = int(m.group(1), 16)
            if v == own_addr:
                continue
            k = bisect.bisect_right(SYM_STARTS, v) - 1
            if k >= 0:
                a, size, name = SYMBOLS[k]
                if v == a or (size and v < a + size):
                    label = name if v == a else f"{name}+{v - a:#x}"
                    if label not in names:
                        names.append(label)
    return names


def emit_function(addr, prg, cycles):
    lines, g, err = gen_instruction(prg, addr, cycles)
    name = f"i_{addr:05x}"
    out = []
    if lines is None:
        out.append(f"// {addr:#07x}: unsupported ({err})")
        out.append(f"static inline void {name}(Cpu& c) {{ unsupported(c, {addr:#x}, {g.op:#x}); }}")
        return out, False, err, g
    note = ""
    if SYMBOLS:
        own = symbol_names([f"0x{addr:x}u"], -1)  # does this instruction start a named function?
        refs = symbol_names(lines, addr)
        if own:
            out.append(f"// ---- {own[0]} ----")
        if refs:
            note = "  [" + ", ".join(refs) + "]"
    out.append(f"static inline void {name}(Cpu& c) {{  // {addr:#07x}  opcode {g.op:04x}{note}")
    for l in lines:
        out.append("    " + l)
    out.append("}")
    return out, True, None, g


def build_blocks(info, leaders):
    """info: addr -> (translated, length, terminates). Returns a list of blocks (lists of addresses)."""
    blocks = []
    for lead in sorted(leaders):
        if lead not in info or not info[lead][0]:
            continue
        seq = []
        a = lead
        while True:
            translated, length, term = info[a]
            if not translated:
                break
            seq.append(a)
            if term:
                break
            nxt = a + length
            if nxt in leaders or nxt not in info:
                break
            a = nxt
        if seq:
            blocks.append(seq)
    return blocks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prg")
    ap.add_argument("analysis")
    ap.add_argument("cycles")
    ap.add_argument("out")
    ap.add_argument("--blocks", help="also write basic-block functions to this file")
    ap.add_argument("--symbols", help="symbol table (data/symbols_bloodbro.txt): names appear as comments in the output")
    ap.add_argument("--all", action="store_true", help="also translate statically reachable, never executed code")
    args = ap.parse_args()

    if args.symbols:
        load_symbols(args.symbols)
    prg = open(args.prg, "rb").read()
    cyc = open(args.cycles, "rb").read()
    if len(cyc) != 65536:
        sys.exit("cycle table must be 65536 bytes (data/m68k_cycles.bin)")
    a = json.load(open(args.analysis))
    addrs = sorted(set(a["executed"]) | (set(a["static_only"]) if args.all else set()))

    out = ["// Generated by tools/gen_recomp.py - do not edit.",
           "// One function per translated 68000 instruction (see recomp/cpu68k.h).",
           "// Include after cpu68k.h / insn_table.h with `using namespace r68k;` in scope."]
    table = []
    info = {}
    ok = bad = 0
    reasons = {}
    for ad in addrs:
        lines, good, err, g = emit_function(ad, prg, cyc)
        out.extend(lines)
        term = (not good) or g.pc_set or g.writes_sr or g.may_trap
        length = g.next_pc - ad
        table.append((ad, g.op, length, good, 1 if term else 0))
        info[ad] = (good, length, term)
        if good:
            ok += 1
        else:
            bad += 1
            reasons[err] = reasons.get(err, 0) + 1
    out.append("static const InsnEntry kInsns[] = {")
    for ad, op, ln, good, fl in table:
        out.append(f"    {{0x{ad:05x}, 0x{op:04x}, {ln}, {'true' if good else 'false'}, {fl}, i_{ad:05x}}},")
    out.append("};")
    with open(args.out, "w") as f:
        f.write("\n".join(out) + "\n")
    print(f"{len(addrs)} instructions: {ok} translated, {bad} unsupported -> {args.out}")
    for r, n in sorted(reasons.items(), key=lambda x: -x[1]):
        print(f"  unsupported: {r}: {n}")

    if args.blocks:
        leaders = set(a["leaders"]) | set(a["vector_targets"]) | {a["reset_pc"]}
        blocks = build_blocks(info, leaders)
        bl = ["// Generated by tools/gen_recomp.py - do not edit.",
              "// Basic blocks: sequences of instruction functions that always run together.",
              "// A block stops early (before the next instruction) once c.cycles >= c.limit."]
        for seq in blocks:
            bl.append(f"static void b_{seq[0]:05x}(Cpu& c) {{")
            for k, ad in enumerate(seq):
                bl.append(f"    i_{ad:05x}(c);")
                if k + 1 < len(seq):
                    bl.append("    if (c.cycles >= c.limit) return;")
            bl.append("}")
        bl.append("static const BlockEntry kBlocks[] = {")
        for seq in blocks:
            bl.append(f"    {{0x{seq[0]:05x}, {len(seq)}, b_{seq[0]:05x}}},")
        bl.append("};")
        with open(args.blocks, "w") as f:
            f.write("\n".join(bl) + "\n")
        total = sum(len(s) for s in blocks)
        print(f"{len(blocks)} blocks ({total} instructions, avg {total / max(1, len(blocks)):.1f}) -> {args.blocks}")


if __name__ == "__main__":
    main()
