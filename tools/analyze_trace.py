#!/usr/bin/env python3
"""Analyse an execution trace of the 68000 program.

  python3 tools/dump_prg.py roms/bloodbro.zip prg.bin
  python3 tools/analyze_trace.py prg.bin data/trace_bloodbro.txt [--json analysis.json] [--top 15]

The trace (data/trace_bloodbro.txt) was recorded by the former interpreter build of the game
(the tracer is no longer part of the project); data/analysis_bloodbro.json is the result.

It combines what really ran (dynamic trace) with a static recursive-descent
disassembly (Capstone) and reports what a recompiler needs to know:

  * how much of the ROM is code / data / unknown,
  * what the dynamic trace found that static analysis could not (indirect calls, jump tables),
  * returns that do not match a call (RTS used as a jump), interrupt entry points,
  * code that is statically reachable but was never executed (untested paths),
  * a JSON file with function entries, basic-block leaders and indirect-jump targets.

Requires: pip install capstone
"""
import argparse
import collections
import json
import re
import sys

from capstone import CS_ARCH_M68K, CS_MODE_BIG_ENDIAN, CS_MODE_M68K_000, Cs

ROM_SIZE = 0x80000
COND = {"cc", "cs", "eq", "ne", "ge", "gt", "hi", "le", "ls", "lt", "mi", "pl", "vc", "vs", "hs", "lo"}
ABS_RE = re.compile(r"^\$([0-9a-f]+)(?:\.[wl])?$")
DBCC_RE = re.compile(r"^d[0-7],\s*\$([0-9a-f]+)$")


class Insn:
    __slots__ = ("addr", "size", "kind", "target", "text")

    def __init__(self, addr, size, kind, target, text):
        self.addr, self.size, self.kind, self.target, self.text = addr, size, kind, target, text

    @property
    def end(self):
        return self.addr + self.size


def make_decoder(prg):
    md = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_000)
    md.detail = True
    cache = {}

    def decode(addr):
        if addr in cache:
            return cache[addr]
        res = None
        if 0 <= addr < ROM_SIZE - 1 and not addr & 1:
            for i in md.disasm(prg[addr:addr + 12], addr):
                res = classify(i)
                break
        cache[addr] = res
        return res

    return decode


def classify(i):
    """kind: seq | branch (conditional, falls through) | dbcc | jump | jump_ind | call | call_ind | ret | rte"""
    m = i.mnemonic.split(".")[0]
    op = i.op_str.strip()
    text = f"{i.mnemonic} {op}".strip()
    if m in ("rts", "rtr"):
        return Insn(i.address, i.size, "ret", None, text)
    if m == "rte":
        return Insn(i.address, i.size, "rte", None, text)
    if m in ("bra", "jmp"):
        mt = ABS_RE.match(op)
        if mt:
            return Insn(i.address, i.size, "jump", int(mt.group(1), 16), text)
        return Insn(i.address, i.size, "jump_ind", None, text)
    if m in ("bsr", "jsr"):
        mt = ABS_RE.match(op)
        if mt:
            return Insn(i.address, i.size, "call", int(mt.group(1), 16), text)
        return Insn(i.address, i.size, "call_ind", None, text)
    if m.startswith("db"):
        mt = DBCC_RE.match(op)
        return Insn(i.address, i.size, "dbcc", int(mt.group(1), 16) if mt else None, text)
    if m.startswith("b") and m[1:] in COND:
        mt = ABS_RE.match(op)
        return Insn(i.address, i.size, "branch", int(mt.group(1), 16) if mt else None, text)
    return Insn(i.address, i.size, "seq", None, text)


def load_trace(path):
    inst, edges, ram, data = {}, collections.Counter(), {}, []
    irqs = collections.Counter()
    with open(path) as f:
        if not f.readline().startswith("bbtrace 1"):
            sys.exit("not a bbtrace file")
        for line in f:
            p = line.split()
            if p[0] == "I":
                inst[int(p[1], 16)] = int(p[2])
            elif p[0] == "E":
                edges[(int(p[1], 16), int(p[2], 16))] += int(p[3])
            elif p[0] == "Q":
                irqs[(int(p[1], 16), int(p[2], 16))] += int(p[3])
            elif p[0] == "X":
                ram[int(p[1], 16)] = (int(p[2]), int(p[3]))
            elif p[0] == "D":
                data.append((int(p[1], 16), int(p[2], 16)))
    return inst, edges, irqs, ram, data


def recursive_descent(decode, seeds):
    """Follow control flow from the seeds. Returns ({addr: Insn}, set of bad decode addresses)."""
    code, bad = {}, set()
    work = list(seeds)
    while work:
        a = work.pop()
        while True:
            if a in code or not (0 <= a < ROM_SIZE):
                break
            ins = decode(a)
            if ins is None:
                bad.add(a)
                break
            code[a] = ins
            k = ins.kind
            if k in ("ret", "rte", "jump_ind"):
                break
            if k == "jump":
                if ins.target is not None:
                    work.append(ins.target)
                break
            if k == "call":
                if ins.target is not None:
                    work.append(ins.target)
            elif k in ("branch", "dbcc") and ins.target is not None:
                work.append(ins.target)
            a = ins.end
    return code, bad


def ranges(addrs_sizes):
    """Merge (addr, size) items into [start, end) ranges."""
    out = []
    for a, s in sorted(addrs_sizes):
        if out and a <= out[-1][1]:
            out[-1][1] = max(out[-1][1], a + s)
        else:
            out.append([a, a + s])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prg")
    ap.add_argument("trace")
    ap.add_argument("--json", help="write machine readable results (for the code generator)")
    ap.add_argument("--top", type=int, default=12)
    args = ap.parse_args()

    prg = open(args.prg, "rb").read()
    if len(prg) != ROM_SIZE:
        sys.exit("program image must be 512 KiB (use --dump-prg)")
    inst, edges, irqs, ram, data_ranges = load_trace(args.trace)
    decode = make_decoder(prg)

    # ---- validate Capstone against the interpreter's instruction lengths ----
    bad_len = [a for a, n in inst.items() if (decode(a) is None or decode(a).size != n)]
    executed = {a: decode(a) for a in inst if decode(a)}

    # ---- vectors ----
    vec = [int.from_bytes(prg[i:i + 4], "big") for i in range(0, 0x100, 4)]
    vec_targets = sorted({v for v in vec[1:] if 0x400 <= v < ROM_SIZE and not v & 1})
    reset_pc = vec[1]

    # ---- static analysis: vectors only vs. vectors + dynamic knowledge ----
    code_static, bad_static = recursive_descent(decode, vec_targets)
    dyn_targets = {t for (_, t) in edges if t < ROM_SIZE}
    seeds = set(vec_targets) | dyn_targets | set(inst)
    code_all, bad_all = recursive_descent(decode, seeds)

    exec_found_static = sum(1 for a in executed if a in code_static)

    # ---- indirect transfers ----
    ind_sites = collections.defaultdict(collections.Counter)
    for (f, t), c in edges.items():
        ins = executed.get(f) or decode(f)
        if ins and ins.kind in ("call_ind", "jump_ind"):
            ind_sites[f][t] += c
    # direct calls whose operand capstone could not resolve (e.g. pc-relative) also show up as 'call_ind'

    # ---- returns ----
    ret_sites = {i.end for i in list(code_all.values()) if i.kind in ("call", "call_ind")}
    odd_returns = collections.Counter()
    rte_targets = collections.Counter()
    for (f, t), c in edges.items():
        ins = executed.get(f)
        if not ins:
            continue
        if ins.kind == "ret" and t not in ret_sites:
            odd_returns[(f, t)] += c
        elif ins.kind == "rte":
            rte_targets[t] += c

    # ---- asynchronous interrupts (recorded by the tracer at the interrupt acknowledge) ----
    irq_targets = collections.Counter()
    irq_from = collections.defaultdict(set)
    for (ret, handler), c in irqs.items():
        irq_targets[handler] += c
        irq_from[handler].add(ret)

    # ---- functions, leaders ----
    call_targets = set()
    for i in code_all.values():
        if i.kind == "call" and i.target is not None:
            call_targets.add(i.target)
    for f, tg in ind_sites.items():
        if (executed.get(f) or decode(f)).kind == "call_ind":
            call_targets.update(tg)
    functions = sorted(call_targets | set(vec_targets) | {reset_pc})
    leaders = set(functions) | dyn_targets
    for i in code_all.values():
        if i.kind in ("branch", "dbcc", "jump", "call", "call_ind", "jump_ind", "ret", "rte"):
            leaders.add(i.end)
            if i.target is not None:
                leaders.add(i.target)
    leaders = sorted(a for a in leaders if a in code_all)

    # ---- byte classification of the ROM ----
    cls = bytearray(ROM_SIZE)  # 0 unknown, 1 executed code, 2 static-only code, 3 data read, 4 vectors
    for a, i in code_all.items():
        v = 1 if a in inst else 2
        for b in range(a, min(a + i.size, ROM_SIZE)):
            if cls[b] != 1:
                cls[b] = v
    for s, e in data_ranges:
        for b in range(s, min(e + 1, ROM_SIZE)):
            if cls[b] == 0:
                cls[b] = 3
    for b in range(0, 0x100):
        cls[b] = 4
    counts = collections.Counter(cls)
    pad = sum(1 for b in range(ROM_SIZE) if cls[b] == 0 and prg[b] in (0x00, 0xFF))
    last_used = max((b for b in range(ROM_SIZE) if prg[b] not in (0x00, 0xFF)), default=0)

    # ---- overlap check (decoded code that overlaps other decoded code = probably data) ----
    ov = 0
    prev_end = 0
    for a in sorted(code_all):
        if a < prev_end:
            ov += 1
        prev_end = max(prev_end, code_all[a].end)

    # ================= report =================
    p = print
    p(f"program image: {len(prg)} bytes, last non-padding byte at {last_used:#x}")
    p(f"reset PC {reset_pc:#x}, SSP {vec[0]:#x}, {len(vec_targets)} distinct vector targets in ROM")
    p()
    p("== dynamic trace ==")
    p(f"  executed instructions      : {len(inst)}")
    p(f"  control-transfer edges     : {len(edges)}")
    p(f"  code executed from RAM     : {len(ram)} addresses" + ("  <-- self-modifying / copied code!" if ram else ""))
    p(f"  Capstone vs interpreter    : {len(bad_len)} length mismatches of {len(inst)}")
    p()
    p("== static analysis ==")
    p(f"  from vectors only          : {len(code_static)} instructions; reaches {exec_found_static} of {len(executed)} executed "
      f"({100.0 * exec_found_static / max(1, len(executed)):.1f}%)")
    p(f"  + dynamic knowledge        : {len(code_all)} instructions "
      f"({len(code_all) - len(executed)} reachable but never executed)")
    p(f"  undecodable targets        : {len(bad_all)}  overlapping decodes: {ov}")
    p(f"  functions (call targets)   : {len(functions)}")
    p(f"  basic-block leaders        : {len(leaders)}")
    p()
    p("== ROM classification (bytes) ==")
    names = {1: "executed code", 2: "static-only code", 3: "read as data", 4: "vector table", 0: "unclassified"}
    for k in (1, 2, 3, 4, 0):
        p(f"  {names[k]:18s}: {counts[k]:7d}  ({100.0 * counts[k] / ROM_SIZE:5.1f}%)")
    p(f"  (of the unclassified bytes, {pad} are 00/FF padding)")
    p()
    p("== indirect jumps/calls (need dynamic information) ==")
    if not ind_sites:
        p("  none observed")
    for f in sorted(ind_sites):
        ins = executed.get(f) or decode(f)
        tg = ", ".join(f"{t:#x}" + (f"x{c}" if c > 1 else "") for t, c in sorted(ind_sites[f].items()))
        p(f"  {f:#08x}  {ins.text:20s} -> {tg}")
    p()
    p("== returns that do not match any call (RTS used as jump?) ==")
    if not odd_returns:
        p("  none: every observed RTS returns to the instruction after a call")
    for (f, t), c in odd_returns.most_common(args.top):
        p(f"  rts at {f:#08x} -> {t:#08x}  (x{c})")
    p()
    p("== interrupt entry points ==")
    for t, c in irq_targets.most_common(args.top):
        p(f"  handler {t:#08x}  taken {c} times, interrupting {len(irq_from[t])} different addresses")
    if not irq_targets:
        p("  none")
    p(f"  RTE returns observed to {len(rte_targets)} different addresses (all must be interrupted code)")
    p()

    # untested code: ranges of static-only instructions
    only = ranges([(a, i.size) for a, i in code_all.items() if a not in inst])
    only.sort(key=lambda r: r[0] - r[1])
    p(f"== largest never-executed code ranges (of {len(only)}) ==")
    fn_sorted = functions
    import bisect
    for s, e in only[: args.top]:
        k = bisect.bisect_right(fn_sorted, s) - 1
        fn = f"in/after function {fn_sorted[k]:#x}" if k >= 0 else ""
        p(f"  {s:#08x}-{e:#08x}  {e - s:5d} bytes  {fn}")

    if args.json:
        out = {
            "reset_pc": reset_pc,
            "ssp": vec[0],
            "vector_targets": vec_targets,
            "functions": functions,
            "leaders": leaders,
            "indirect": {f"{f:#x}": {f"{t:#x}": c for t, c in tg.items()} for f, tg in ind_sites.items()},
            "interrupt_entries": sorted(irq_targets),
            "executed": sorted(inst),
            "static_only": sorted(a for a in code_all if a not in inst),
            "code_ranges": [[s, e] for s, e in ranges([(a, i.size) for a, i in code_all.items()])],
            "data_ranges": [[s, e] for s, e in data_ranges],
        }
        with open(args.json, "w") as fh:
            json.dump(out, fh)
        p(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
