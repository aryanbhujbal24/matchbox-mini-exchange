#!/usr/bin/env bash
# Record a run for the visualization: exchange + market maker + simulated traders,
# with tracing on. Usage: scripts/record_demo.sh [seconds]   (from the repo root, after building)
set -euo pipefail
SECS=${1:-45}
BIN=${BIN:-build}
OUT=${OUT:-out/recording}
mkdir -p "$OUT"
rm -f "$OUT"/journal.bin "$OUT"/*.csv

"$BIN/mx_exchange" --journal "$OUT/journal.bin" --trace "$OUT/trace.csv" > "$OUT/exchange.log" 2>&1 &
EX=$!
trap 'kill -INT $EX 2>/dev/null || true' EXIT
sleep 0.5
"$BIN/mx_mm" --seconds $((SECS + 1)) --gamma 0.05 --events "$OUT/mm.csv" > "$OUT/mm.log" 2>&1 &
MM=$!
sleep 0.3
"$BIN/mx_flow" --seconds "$SECS" --noise-rate 5 --inf-rate 2 --seed 7 --events "$OUT/flow.csv" > "$OUT/flow.log" 2>&1 &
FLOW=$!
wait $MM $FLOW
sleep 0.5
kill -INT $EX; wait $EX 2>/dev/null || true
trap - EXIT

tail -n 1 "$OUT/mm.log"
python3 scripts/make_recording.py "$OUT/trace.csv" "$OUT/mm.csv" "$OUT/flow.csv" docs/visualization/recording.js
