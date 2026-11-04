#!/bin/bash
# The apps BEFORE and AFTER (their qucs-s) in turn: fgc_S62's 10 GHz, four
# solves a process; its 40-frequency sweep, two a process (the stack's
# tables made, then kept); fgc_S51's sweep, two a process. bench.py's rows
# to $BENCH_RUNS/timings.jsonl.
cd "$(dirname "$0")"
: "${BEFORE:?the before app's qucs-s}" "${AFTER:?the after app's qucs-s}"
export BENCH_RUNS="${BENCH_RUNS:-${TMPDIR:-/tmp}/rf25-dense-fill-bench}"
mkdir -p "$BENCH_RUNS"
OUT="$BENCH_RUNS/timings.jsonl"
for round in 1 2; do
  python3 -B -I bench.py "$BEFORE" before one 4 "$OUT"
  python3 -B -I bench.py "$AFTER" after one 4 "$OUT"
done
for round in 1 2; do
  python3 -B -I bench.py "$BEFORE" before sweep 2 "$OUT"
  python3 -B -I bench.py "$AFTER" after sweep 2 "$OUT"
done
export BENCH_MODEL=fgc_S51_L2000_r1p8.qrf25 BENCH_GDS=fgc_S51_L2000.gds
python3 -B -I bench.py "$BEFORE" before sweep 2 "$OUT"
python3 -B -I bench.py "$AFTER" after sweep 2 "$OUT"
