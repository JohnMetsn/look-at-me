#!/bin/sh
# Frame-by-frame capture on the emulators (raw palette, no watch frame).
#   tools/capture.sh anim  -> capture/<platform>/fNNN.png   (one glance at 15 fps, 120 frames)
#   tools/capture.sh store -> capture/<platform>/pose_N.png (store poses)
# The CAPTURE build reads its frame index from the watch clock (10:00:00 + 4 s per frame).
set -eu
cd "$(dirname "$0")/.."
P=../../tools/pebble-local
MODE=${1:-anim}
if [ "$MODE" = anim ]; then CAP=1; N=120; else CAP=2; N=6; fi
CAPTURE=$CAP $P build >/dev/null
for PL in emery gabbro; do
  mkdir -p capture/$PL
  $P install --emulator $PL >/dev/null 2>&1
  sleep 6
  k=0
  while [ $k -lt $N ]; do
    s=$((k * 4 + 1))
    $P emu-set-time --emulator $PL "$(printf '10:%02d:%02d' $((s / 60)) $((s % 60)))" >/dev/null 2>&1
    sleep 1.0
    if [ "$MODE" = anim ]; then out=$(printf 'capture/%s/f%03d.png' $PL $k); else out="capture/$PL/pose_$k.png"; fi
    $P screenshot --no-open --no-correction --emulator $PL "$out" >/dev/null 2>&1
    k=$((k + 1))
  done
done
echo "capture $MODE done"
