#!/usr/bin/env python3
"""Extend data/analysis_bloodbro.json with code the execution trace never reached.

  python3 tools/dump_prg.py roms/bloodbro.zip prg.bin
  python3 tools/extend_analysis.py prg.bin data/analysis_bloodbro.json [--out FILE] [--dry-run]

The trace that data/analysis_bloodbro.json was made from (tools/analyze_trace.py) knows the targets of the
indirect calls only as far as the trace went. The game dispatches stages and objects through jump tables:

    lea     TABLE(pc), a3            (or lea $TABLE.l, a0)
    movea.l (a3, d0.w), a3
    jsr     (a3)                     (or jmp)

so every stage or enemy type the trace never saw (e.g. stage 4-3 onwards) was missing from the translation,
and the game stopped with "code that is not part of the recompiled program".

This tool finds those tables statically, takes every entry that is a plausible code pointer, disassembles
recursively from there (Capstone, like analyze_trace.py) and repeats until nothing new is found. It adds the
new instructions to "static_only" and updates "leaders", "functions" and "code_ranges"; the jump tables found
are recorded under "jump_tables". Indirect transfers it cannot resolve are reported.

Requires: pip install capstone  (MSYS2: pacman -S mingw-w64-x86_64-python-capstone)
"""
import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze_trace import ROM_SIZE, make_decoder, ranges  # noqa: E402

LEA_RE = re.compile(r"^\$([0-9a-f]+)(?:\(pc\)|\.l|\.w)?,\s*(a[0-7])$")
MOVEA_IDX_RE = re.compile(r"^(?:\$0)?\((a[0-7]),\s*d[0-7]\.[wl]\),\s*(a[0-7])$")
IND_RE = re.compile(r"^\((a[0-7])\)$")
MAX_TABLE = 256


def find_table(decode, code, site):
    """Base address of the jump table used by the indirect jsr/jmp at `site`, or None."""
    ins = code[site]
    m = IND_RE.match(ins.text.split(None, 1)[1] if " " in ins.text else "")
    if not m:
        return None
    reg = m.group(1)
    ends = {i.end: a for a, i in code.items()}
    a = site
    loaded_from = None
    for _ in range(6):   # look back a few instructions
        prev = ends.get(a)
        if prev is None:
            return None
        p = code[prev]
        mn, _, op = p.text.partition(" ")
        op = op.strip()
        if loaded_from is None and mn == "movea.l":
            mm = MOVEA_IDX_RE.match(op)
            if mm and mm.group(2) == reg:
                loaded_from = mm.group(1)
        elif loaded_from is not None and mn == "lea.l":
            ml = LEA_RE.match(op)
            if ml and ml.group(2) == loaded_from:
                return int(ml.group(1), 16)
        a = prev
    return None


def covered_map(code):
    cov = {}
    for a, i in code.items():
        for b in range(a, a + i.size):
            cov[b] = a
    return cov


def table_entries(prg, decode, code, base):
    """Consecutive longwords at `base` that look like pointers to code. A zero is an unused slot
    (object type 0 = no object, gaps in state tables) and does not end the table."""
    cov = covered_map(code)
    out = []
    for k in range(MAX_TABLE):
        at = base + 4 * k
        if at + 4 > ROM_SIZE or (at in code and k > 0):   # ran into code
            break
        v = int.from_bytes(prg[at:at + 4], "big")
        if v == 0:
            continue
        if v & 1 or not (0x400 <= v < ROM_SIZE):
            break
        if v in cov and cov[v] != v:   # points into the middle of a known instruction: not code
            break
        if decode(v) is None:
            break
        out.append(v)
    return out


def descend(decode, code, seeds):
    """Recursive descent from `seeds`, adding to `code`. Returns the set of new addresses."""
    new = set()
    work = list(seeds)
    while work:
        a = work.pop()
        while 0 <= a < ROM_SIZE and a not in code:
            ins = decode(a)
            if ins is None:
                break
            code[a] = ins
            new.add(a)
            k = ins.kind
            if k in ("ret", "rte", "jump_ind"):
                break
            if k == "jump":
                if ins.target is not None:
                    work.append(ins.target)
                break
            if k in ("call", "branch", "dbcc") and ins.target is not None:
                work.append(ins.target)
            a = ins.end
    return new


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prg")
    ap.add_argument("analysis")
    ap.add_argument("--out", help="write here instead of overwriting the analysis file")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    prg = open(args.prg, "rb").read()
    if len(prg) != ROM_SIZE:
        sys.exit("program image must be 512 KiB")
    an = json.load(open(args.analysis))
    decode = make_decoder(prg)

    old = set(an["executed"]) | set(an["static_only"])
    code = {}
    for a in old:
        ins = decode(a)
        if ins is None:
            sys.exit(f"known instruction at {a:#x} does not decode: wrong program image?")
        code[a] = ins

    tables = {}      # site -> (base, entries)
    unresolved = set()
    seeds_from_tables = set()
    while True:
        found = set()
        for site, ins in list(code.items()):
            if ins.kind not in ("call_ind", "jump_ind") or site in tables:
                continue
            if ins.text.startswith(("jsr $", "jmp $")):   # pc-relative direct calls show up as indirect
                continue
            base = find_table(decode, code, site)
            if base is None:
                unresolved.add(site)
                continue
            ent = table_entries(prg, decode, code, base)
            tables[site] = (base, ent)
            unresolved.discard(site)
            found.update(ent)
        new_seeds = {e for e in found if e not in code}
        seeds_from_tables |= found
        if not new_seeds:
            break
        descend(decode, code, new_seeds)

    added = sorted(a for a in code if a not in old)

    # leaders and functions, as analyze_trace.py computes them
    leaders = set(an["leaders"]) | seeds_from_tables
    functions = set(an["functions"])
    for site, (base, ent) in tables.items():
        if code[site].kind == "call_ind":
            functions.update(ent)
    for a in added:
        i = code[a]
        if i.kind == "call" and i.target is not None:
            functions.add(i.target)
        if i.kind in ("branch", "dbcc", "jump", "call", "call_ind", "jump_ind", "ret", "rte"):
            leaders.add(i.end)
            if i.target is not None:
                leaders.add(i.target)
    leaders = sorted(a for a in leaders if a in code)

    print(f"jump tables: {len(tables)}")
    for site in sorted(tables):
        base, ent = tables[site]
        missing = [e for e in ent if e not in old]
        print(f"  {site:#08x} {code[site].text:10s} table {base:#08x}: {len(ent):3d} entries, {len(missing)} new"
              + (": " + " ".join(f"{e:#x}" for e in missing[:12]) + (" ..." if len(missing) > 12 else "") if missing else ""))
    if unresolved:
        print("indirect transfers without a recognised table (only the traced targets are known):")
        for s in sorted(unresolved):
            print(f"  {s:#08x} {code[s].text}")
    print(f"new instructions: {len(added)} (known before: {len(old)})")
    print(f"leaders: {len(an['leaders'])} -> {len(leaders)}, functions: {len(an['functions'])} -> {len(functions)}")

    if args.dry_run:
        return
    an["static_only"] = sorted(set(an["static_only"]) | set(added))
    an["leaders"] = leaders
    an["functions"] = sorted(functions)
    an["code_ranges"] = [[s, e] for s, e in ranges([(a, i.size) for a, i in code.items()])]
    an["jump_tables"] = {f"{s:#x}": {"table": f"{b:#x}", "entries": [f"{e:#x}" for e in ent]} for s, (b, ent) in sorted(tables.items())}
    out = args.out or args.analysis
    with open(out, "w") as fh:
        json.dump(an, fh)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
