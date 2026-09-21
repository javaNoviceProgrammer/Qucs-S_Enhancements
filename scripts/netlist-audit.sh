#!/usr/bin/env bash
# Survey every built-in component's netlist and run the ngspice decks
# through the real simulator.
#
#   scripts/netlist-audit.sh <build-dir> <out-dir>
#
# Writes into <out-dir>:
#   netlist_audit_{ngspice,xyce,qucsator}.txt  every component, its
#                          properties and every netlist flavour
#   marker_audit.txt       every property set to a value of its own, with
#                          the ones the SPICE netlist ignores or repeats
#   cir/*.cir              one ngspice deck per component
#   ngspice_complaints.txt what ngspice said about each deck (only the
#                          decks it complained about)
#
# Needs the test executable build/qucs/tests/test_netlist_audit and
# ngspice on PATH. An ngspice without a spinit (no code models) gets a
# .spiceinit from NGSPICE_CM_DIR, the directory of its *.cm files.
set -euo pipefail

build="${1:?build dir}"
out="${2:?output dir}"
test_bin="$build/qucs/tests/test_netlist_audit"
[ -x "$test_bin" ] || { echo "not built: $test_bin" >&2; exit 1; }

mkdir -p "$out"
QT_QPA_PLATFORM=offscreen QUCS_NETLIST_AUDIT="$out" "$test_bin" \
    everyComponentNetlists everyPropertyMarked everyComponentAsADeck \
    2>&1 | grep -E "^(PASS|FAIL|Totals)" || true

command -v ngspice >/dev/null || { echo "ngspice not on PATH; decks written, not run" >&2; exit 0; }

cd "$out/cir"
if [ -n "${NGSPICE_CM_DIR:-}" ]; then
    : > .spiceinit
    for cm in "$NGSPICE_CM_DIR"/*.cm; do echo "codemodel $cm" >> .spiceinit; done
fi
# File sources read these.
printf '0\t0\n1e-5\t1\n1e-4\t0\n' > vfile.dat
cp vfile.dat ifile.dat

: > ../ngspice_complaints.txt
for f in *.cir; do
    said="$(ngspice -b "$f" 2>&1 < /dev/null \
        | grep -i "error\|warning\|failed\|unable\|invalid\|unknown\|ignored\|not found\|singular\|aborted\|too small\|no such\|undefined\|missing\|cannot\|limited" \
        | grep -v "^Warning: Pd\|singular matrix:  check nodes\|Warning: input contains\|Warning: can't find the initialization" \
        | sort -u | head -8 || true)"
    if [ -n "$said" ]; then
        { echo "=== $f"; echo "$said"; } >> ../ngspice_complaints.txt
    fi
done
echo "decks with complaints: $(grep -c '^===' ../ngspice_complaints.txt) of $(ls *.cir | wc -l | tr -d ' ')"
