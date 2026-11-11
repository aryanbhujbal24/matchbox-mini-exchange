#!/usr/bin/env bash
# Live demo: exchange + simulated participants + market maker + feed listener.
# Usage: scripts/run_demo.sh [seconds]   (run from the repo root after building)
set -euo pipefail
SECS=${1:-20}
BIN=${BIN:-build}
OUT=${OUT:-out}
mkdir -p "$OUT"
rm -f "$OUT/journal.bin"

"$BIN/mx_exchange" --journal "$OUT/journal.bin" > "$OUT/exchange.log" 2>&1 &
EX=$!
trap 'kill -INT $EX 2>/dev/null || true' EXIT
sleep 0.5

"$BIN/mx_md_listener" --seconds "$SECS" > "$OUT/md.log" 2>&1 &
"$BIN/mx_mm" --seconds "$SECS" --csv "$OUT/mm_live.csv" > "$OUT/mm.log" 2>&1 &
MM=$!
sleep 0.2
"$BIN/mx_flow" --seconds "$SECS" > "$OUT/flow.log" 2>&1 &
FLOW=$!

wait $MM $FLOW
sleep 1.2
kill -INT $EX; wait $EX 2>/dev/null || true
trap - EXIT

echo "== market maker (last lines) ==";  tail -n 4 "$OUT/mm.log"
echo "== flow ==";                        tail -n 1 "$OUT/flow.log"
echo "== market data ==";                 tail -n 1 "$OUT/md.log"
echo "== exchange ==";                    tail -n 2 "$OUT/exchange.log"
echo "== crash recovery: restart from the journal =="
"$BIN/mx_exchange" --journal "$OUT/journal.bin" --port 9002 --no-md > "$OUT/recover.log" 2>&1 &
R=$!; sleep 0.5; kill -INT $R; wait $R 2>/dev/null || true
head -n 1 "$OUT/recover.log"
