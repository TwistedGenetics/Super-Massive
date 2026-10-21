#!/usr/bin/env bash
# Build and run the desktop DSP tests. Renders impulse responses and demo clips
# for every mode into test/out/ and fails on NaN/inf, runaway feedback, DC
# build-up or clicks on mode changes.
#
#   ./test/run.sh                         run all checks
#   ./test/harness in.wav out.wav [mode time fb warp density mod tone mix]
#                                         process your own 48 kHz WAV
#     mode 0-9, time in ms, others 0-1 (tone and mix -1..1)
set -euo pipefail
cd "$(dirname "$0")"
make -s harness
./harness out
