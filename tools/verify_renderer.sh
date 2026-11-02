#!/bin/bash
# Checks bloodbro's video rendering (tile layers, sprites, priorities) against MAME for the same game states.
# usage (project root, build done, MAME installed):   bash tools/verify_renderer.sh [rom-zip-or-dir] [states-per-stage]
#   1. plays several stages (stage-select cheats) and saves the video-relevant state of many frames
#   2. renders every state with bloodbro's renderer (--render-dump)
#   3. lets MAME draw the same states (its own game logic frozen) and compares pixel by pixel
set -e
ROM=${1:-roms/bloodbro.zip}
N=${2:-6}
B=build/bloodbro
W=work/render_check
MAME=${MAME:-mame}
rm -rf $W; mkdir -p $W/mine $W/mame
: > $W/list.txt
for stage in 4 8 12 1; do      # parameter numbers of "Select Starting Stage": 2-1, 3-1, 4-1, 1-2
  args=""
  for k in $(seq 0 $((N-1))); do
    f=$((1100 + k*200)); args="$args --ram-dump $f:$W/s${stage}_$f.bin"; echo "s${stage}_$f.bin" >> $W/list.txt
  done
  $B --headless --nothrottle --rom "$ROM" --no-ini --cheat "Select Starting Stage=$stage" --autoplay 11 \
     --press coin1@300+6 --press start1@360+6 --frames $((1100 + N*200)) $args > /dev/null
done
for d in $(cat $W/list.txt); do $B --headless --rom "$ROM" --no-ini --render-dump $W/$d:$W/mine/${d%.bin}.png > /dev/null; done
ROMDIR=$(dirname "$ROM"); [ -d "$ROM" ] && ROMDIR="$ROM"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy BB_STATE_DIR=$PWD/$W $MAME bloodbro -rompath "$ROMDIR" -video none -sound none \
    -nothrottle -skip_gameinfo -snapshot_directory $PWD/$W/mame -autoboot_script tools/mame_render_states.lua -autoboot_delay 0 > /dev/null 2>&1
python3 tools/compare_render_states.py $W/list.txt $W/mine $W/mame/bloodbro
