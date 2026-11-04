#!/bin/bash
# Main's head (ac435e2) without and with the change, the apps BEFORE and
# AFTER (their qucs-s): fgc_S62 at its graded mesh, past the automatic dense
# limit, the limit set to 8000 (RF 2.5D > Settings...): 10 GHz, three
# solves a process; the sweep at the default memory and in 1.5 GB. Rows to
# $BENCH_RUNS/main.jsonl.
cd "$(dirname "$0")"
: "${BEFORE:?the before app's qucs-s}" "${AFTER:?the after app's qucs-s}"
export BENCH_RUNS="${BENCH_RUNS:-${TMPDIR:-/tmp}/rf25-dense-fill-bench}"
mkdir -p "$BENCH_RUNS"
OUT="$BENCH_RUNS/main.jsonl"
export BENCH_INI='[RF25]\ndenseLimit=8000'
python3 -B -I bench.py "$BEFORE" main-before one 3 "$OUT"
python3 -B -I bench.py "$AFTER" main-after one 3 "$OUT"
python3 -B -I bench.py "$BEFORE" main-before sweep 1 "$OUT"
python3 -B -I bench.py "$AFTER" main-after sweep 1 "$OUT"
export BENCH_INI='[RF25]\ndenseLimit=8000\nsweepMemory=1.5'
python3 -B -I bench.py "$BEFORE" main-before-1.5GB sweep 1 "$OUT"
python3 -B -I bench.py "$AFTER" main-after-1.5GB sweep 1 "$OUT"
