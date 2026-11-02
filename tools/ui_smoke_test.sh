#!/bin/bash
# Starts the real program in a virtual X display and presses F4, F2, F1 and every menu row like a user, then checks
# that it is still running and quits cleanly with Esc. Needs: xvfb, xdotool (and a build; best with the sanitizer flags, see README).
#   usage: bash tools/ui_smoke_test.sh [path/to/bloodbro] [rom.zip]
set -u
EXE=${1:-build/bloodbro}
ROM=${2:-roms/bloodbro.zip}
D=${DISPLAY_NUM:-77}
export ASAN_OPTIONS=detect_leaks=0
Xvfb :$D -screen 0 1400x1000x24 >/dev/null 2>&1 &
XPID=$!
sleep 1
export DISPLAY=:$D SDL_AUDIODRIVER=dummy
LOG=$(mktemp)
INI=$(mktemp)
"$EXE" --ini "$INI" --rom "$ROM" --nosound --scale 3 --autoplay 3 > "$LOG" 2>&1 &
GPID=$!
sleep 6
WID=$(xdotool search --name "Blood Bros" | head -1)
key() { xdotool key --window "$WID" "$@" 2>/dev/null; sleep 0.25; }
fail=0
alive() { if kill -0 $GPID 2>/dev/null; then echo "ok   $1"; else echo "FAIL $1: the program is gone"; fail=1; fi; }
key F4; alive "F4 opens the video menu"
for i in $(seq 1 14); do key Down; key Right; key Left; key Return; done; alive "every video row, both directions"
key F4
key F2; for i in $(seq 1 18); do key Down; key Return; key Right; done; alive "cheat page"; key F2
key F1; for i in $(seq 1 14); do key Down; key Right; key Left; done; key Down; key Return; alive "DIP page"
key F4; key F2; key F1; key F4; key F4; key F4; key Escape; alive "switching between pages"
key Escape
sleep 3
wait $GPID; code=$?
[ $code -eq 0 ] || { echo "FAIL exit code $code"; fail=1; }
grep -qiE "sanitizer|runtime error|segmentation" "$LOG" && { echo "FAIL the program log contains errors:"; grep -iE "sanitizer|runtime error" "$LOG" | head -3; fail=1; }
kill $XPID 2>/dev/null
rm -f "$LOG" "$INI"
[ $fail -eq 0 ] && echo "ui smoke test passed" || echo "ui smoke test FAILED"
exit $fail
