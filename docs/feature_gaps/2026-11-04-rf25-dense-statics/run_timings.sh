#!/bin/bash
# The apps BEFORE and AFTER (their qucs-s) in turn through the dense fill's
# bench.py: fgc_S62 at its graded mesh (4660 runs over 2336 cells, past the
# automatic dense limit, the limit set to 8000 - RF 2.5D > Settings...) at
# 10 GHz, four solves a process (the first makes the stack's tables, the
# rest keep them), two processes of each; then the phase-timed apps
# PHASES_BEFORE and PHASES_AFTER (phases_*.py, their stderr in each run's
# server.err), three solves each; then the model's 40-frequency sweep, one
# of each in turn, three rounds. Rows to $BENCH_RUNS/timings.jsonl.
cd "$(dirname "$0")"
: "${BEFORE:?the before app's qucs-s}" "${AFTER:?the after app's qucs-s}"
BENCH=../2026-11-04-rf25-dense-fill/bench.py
export BENCH_RUNS="${BENCH_RUNS:-${TMPDIR:-/tmp}/rf25-dense-statics-bench}"
mkdir -p "$BENCH_RUNS"
OUT="$BENCH_RUNS/timings.jsonl"
export BENCH_INI='[RF25]\ndenseLimit=8000'
for round in 1 2; do
  python3 -B -I "$BENCH" "$BEFORE" before one 4 "$OUT"
  python3 -B -I "$BENCH" "$AFTER" after one 4 "$OUT"
done
if [ -n "$PHASES_BEFORE" ] && [ -n "$PHASES_AFTER" ]; then
  python3 -B -I "$BENCH" "$PHASES_BEFORE" phases-before one 3 "$OUT"
  python3 -B -I "$BENCH" "$PHASES_AFTER" phases-after one 3 "$OUT"
fi
for round in 1 2 3; do
  python3 -B -I "$BENCH" "$BEFORE" before sweep 1 "$OUT"
  python3 -B -I "$BENCH" "$AFTER" after sweep 1 "$OUT"
done
