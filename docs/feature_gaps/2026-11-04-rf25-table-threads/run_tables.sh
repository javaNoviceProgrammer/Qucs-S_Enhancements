#!/bin/bash
# The apps BEFORE (ac3f5fc) and AFTER (this change), their qucs-s, in turn,
# through rf25_solve with the dense fill round's bench.py: fgc_S62 at its
# graded mesh (4660 runs over 2336 cells), past the automatic dense limit,
# the limit set to 8000 (RF 2.5D > Settings...). 10 GHz, three solves a
# process - the first makes the stack's tables, the others keep them - two
# processes of each; a fresh 40-frequency sweep at the default memory (24
# at a time here, a thread each) and in 1.5 GB (three at a time, eight
# threads each), two of each. Rows to $BENCH_RUNS/tables.jsonl.
cd "$(dirname "$0")"
: "${BEFORE:?the before app's qucs-s}" "${AFTER:?the after app's qucs-s}"
BENCH=../2026-11-04-rf25-dense-fill/bench.py
export BENCH_RUNS="${BENCH_RUNS:-${TMPDIR:-/tmp}/rf25-table-threads-bench}"
mkdir -p "$BENCH_RUNS"
OUT="$BENCH_RUNS/tables.jsonl"
export BENCH_INI='[RF25]\ndenseLimit=8000'
for round in 1 2; do
  python3 -B -I "$BENCH" "$BEFORE" before one 3 "$OUT"
  python3 -B -I "$BENCH" "$AFTER" after one 3 "$OUT"
done
for round in 1 2; do
  python3 -B -I "$BENCH" "$BEFORE" before sweep 1 "$OUT"
  python3 -B -I "$BENCH" "$AFTER" after sweep 1 "$OUT"
done
export BENCH_INI='[RF25]\ndenseLimit=8000\nsweepMemory=1.5'
for round in 1 2; do
  python3 -B -I "$BENCH" "$BEFORE" before-1.5GB sweep 1 "$OUT"
  python3 -B -I "$BENCH" "$AFTER" after-1.5GB sweep 1 "$OUT"
done
