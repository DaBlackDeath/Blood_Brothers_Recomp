#!/bin/bash
# Generates the recompiled program from YOUR ROM. The generated code is a translation of the game's
# program, so it is not shipped with the project.
#
# usage (from the project root):
#     bash tools/recomp_pipeline.sh roms/bloodbro.zip
# needs: python3 (no extra packages). Compiling the generated code takes a few minutes (los.sh).
set -e
ROM=${1:-roms/bloodbro.zip}
mkdir -p work generated
echo "== 1. extract the 68000 program image"
python3 tools/dump_prg.py "$ROM" work/prg.bin
echo "== 2. translate instructions and basic blocks"
python3 tools/gen_recomp.py work/prg.bin data/analysis_bloodbro.json data/m68k_cycles.bin \
        work/recomp_insns.inc --all --blocks work/recomp_blocks.inc \
        --symbols data/symbols_bloodbro.txt
# replace the files only when they changed (compiling them takes minutes)
for f in recomp_insns.inc recomp_blocks.inc; do
    cmp -s work/$f generated/$f || cp work/$f generated/$f
done
rm -rf work
echo "done: generated/recomp_insns.inc, generated/recomp_blocks.inc"
