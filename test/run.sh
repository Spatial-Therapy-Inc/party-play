#!/usr/bin/env bash
# Host tests for Party Play's game logic and sounds (no device, no ESP-IDF needed).
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
M=../main
YM=../devkit/firmware/components/yaykit_motion  # yaykit_motion's core (pure C) and its host stub for esp_err.h
cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O1 -I"$M" -I"$YM" -I"$YM/include" -I"$YM/test/stub" \
  -o "$OUT/test_logic" test_logic.c \
  "$M/pp_seats.c" "$M/pp_dice.c" "$M/pp_wheel.c" "$M/pp_react.c" "$M/pp_bomb.c" "$M/pp_ten.c" \
  "$M/pp_record.c" "$M/pp_launch.c" "$M/pp_gesture.c" "$M/pp_motion_map.c" "$M/pp_play.c" "$M/pp_synth.c" \
  "$YM/yk_motion_core.c" -lm
# `run.sh wav` also writes every sound to test/out/ to listen to (not committed)
if [ "${1:-}" = wav ]; then mkdir -p out; PP_WAV_DIR=$PWD/out "$OUT/test_logic"; else "$OUT/test_logic"; fi
echo "party-play host tests: ok"
