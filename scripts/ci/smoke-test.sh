#!/usr/bin/env bash
#
# Headless smoke test for qucs-s. Drives the CLI modes that exist in
# qucs/main.cpp (no window is ever shown) over the shipped examples so that
# the file parsers, netlist generators, simulator output parsers and the
# dataset -> diagram path all execute. Meant to run under ASan/UBSan in CI
# but works with any build.
#
# Usage:
#   smoke-test.sh <suite> <qucs-s-executable> <examples-dir> <out-dir>
#
# Suites:
#   load      Load + render every ngspice example schematic to PNG (-p).
#   simulate  For a curated set of circuits: generate a netlist, run ngspice
#             headless, convert to a Qucs dataset and render the schematic so
#             the diagrams load the data (Graph::loadDatFile / calcData).
#   hostile   Render a fixture schematic against generated, deliberately
#             damaged datasets. Needs no ngspice. Regression guard for the
#             crash class behind upstream #1711 / #1539 (fixed in WS1.1 of
#             ENHANCEMENT_PROPOSAL.md).
#
# Exit code is non-zero if any test in the suite failed. Per-test logs are
# written to <out-dir>/<suite>/.
set -uo pipefail

suite="${1:?suite}"
QUCS="${2:?qucs-s executable}"
EXAMPLES="${3:?examples dir}"
OUT="${4:?output dir}"

export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
# Memory errors are what we are after; Qt itself leaks at exit and would
# drown the signal. Enable leak detection deliberately once WS1.3 lands.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:abort_on_error=1:symbolize=1:allocator_may_return_null=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1}"

OUT="$OUT/$suite"
mkdir -p "$OUT"
pass=0; fail=0; failed_names=()

# GNU timeout is standard on Linux; macOS has it only via coreutils (gtimeout).
if command -v timeout >/dev/null; then TIMEOUT=(timeout --signal=KILL)
elif command -v gtimeout >/dev/null; then TIMEOUT=(gtimeout --signal=KILL)
else TIMEOUT=(); echo "note: no 'timeout' command, running without per-test time limit"; fi

# run <name> <timeout-sec> <cmd...> : runs cmd, logs to $OUT/<name>.log,
# treats non-zero exit or a sanitizer report as failure.
run() {
  local name="$1" tmo="$2"; shift 2
  local log="$OUT/$name.log"
  if [ ${#TIMEOUT[@]} -gt 0 ]; then "${TIMEOUT[@]}" "$tmo" "$@" >"$log" 2>&1; else "$@" >"$log" 2>&1; fi
  local rc=$?
  if [ $rc -ne 0 ] || grep -q "ERROR: AddressSanitizer\|ERROR: LeakSanitizer" "$log"; then
    fail=$((fail+1)); failed_names+=("$name")
    printf '  FAIL  %-60s rc=%s\n' "$name" "$rc"
    # Surface the sanitizer report / last lines inline for the CI log.
    grep -A25 "ERROR: AddressSanitizer\|SUMMARY:" "$log" | head -40 | sed 's/^/        /'
    [ $rc -ge 124 ] && printf '        (killed: timeout or signal %s)\n' $((rc-128))
    return 1
  fi
  pass=$((pass+1))
  printf '  ok    %s\n' "$name"
  return 0
}

# The examples reference dataset variables as "ngspice/ac.v(out)"; that
# prefix is only emitted when this setting is on, so make it deterministic.
prepare_settings() {
  local cfg="${XDG_CONFIG_HOME:-$HOME/.config}/qucs"
  mkdir -p "$cfg"
  if [ ! -f "$cfg/qucs_s.conf" ]; then
    cat >"$cfg/qucs_s.conf" <<EOF
[General]
DefaultSimulator=1
NgspiceExecutable=ngspice
alwaysPrefixDataset=true
EOF
  fi
}

sanitize_name() { echo "$1" | sed 's#[^A-Za-z0-9._-]#_#g'; }

# --------------------------------------------------------------------------
suite_load() {
  echo "== load: render every ngspice example schematic"
  while IFS= read -r -d '' sch; do
    local rel="${sch#"$EXAMPLES"/}"
    local name; name="$(sanitize_name "$rel")"
    run "$name" 120 "$QUCS" -p -i "$sch" -o "$OUT/$name.png"
  done < <(find "$EXAMPLES/ngspice" -name '*.sch' -not -path '*/OpenVAF/*' -print0 | sort -z)
  # OpenVAF/ examples reference user-compiled Verilog-A devices that only
  # exist after the module is loaded in the GUI; they cannot load headless.
}

# --------------------------------------------------------------------------
# Circuits that plain ngspice (>= 42, Ubuntu 24.04) simulates without
# external models, PDKs or OpenVAF. Paths relative to <examples>/ngspice.
SIM_SET=(
  "RF/Miscellaneous/RCL_resonance.sch"
  "RF/Miscellaneous/stab.sch"
  "General Electronics/RC_filter_FFT.sch"
  "General Electronics/chargepump.sch"
  "General Electronics/Active Filters/active_bp.sch"
  "Devices/gyrator.sch"
  # One per simulator-output format the parsers know (see fuzz-simout.py):
  "RF/Amplifiers/Q2N2222A/BJT-swp.sch"              # parameter sweep: _swp.plot + .cir.res
  "Devices/diode_dblswp_qucs.sch"                   # DC sweep
  "RF/Amplifiers/Q2N2222A/BJT-noise.sch"            # .cir.noise + swept .raw
  "NGspice features/sensitivityACandDC.sch"         # .sens.prn, .sens.dc.prn
  "RF/Amplifiers/Distortion simulations/Distortion.sch"  # disto*.plot
  "Devices/CV_curve.sch"                            # custom nutmeg script: custom1.plot
  "RF/Miscellaneous/giacoletto.sch"                 # S-parameters: sp1.plot
  "NGspice features/par_sweep_test.sch"             # AC sweep + plain TR
)

# simulate_one <sch-in-examples> -> copies the whole example directory
# (subcircuits are referenced relatively), simulates, then renders.
simulate_one() {
  local rel="$1"
  local name; name="$(sanitize_name "$rel")"
  local src="$EXAMPLES/ngspice/$rel"
  local work="$OUT/work/$name"
  rm -rf "$work"; mkdir -p "$work"
  cp -R "$(dirname "$src")/." "$work/"
  local sch="$work/$(basename "$src")"
  local ds; ds="$(grep -o '<DataSet=[^>]*' "$sch" | head -1 | cut -d= -f2)"
  [ -n "$ds" ] || ds="$(basename "$src" .sch).dat"

  # The GUI writes the converted ngspice output as <DataSet>.ngspice, and
  # that is the file a diagram trace named "ngspice/..." is loaded from.
  ds="$ds.ngspice"
  run "$name.netlist"  120 "$QUCS" -n --ngspice -i "$sch" -o "$work/out.net"   || return
  run "$name.simulate" 300 "$QUCS" -n --ngspice --run -i "$sch" -o "$work/$ds" || return
  stash_simulator_outputs "$name" "$work" "$(basename "$sch")"
  if ! grep -q '^<dep ' "$work/$ds" 2>/dev/null; then
    fail=$((fail+1)); failed_names+=("$name.dataset")
    printf '  FAIL  %-60s dataset has no dependent variables\n' "$name.dataset"
    return 1
  fi
  run "$name.render"   120 "$QUCS" -p -i "$sch" -o "$work/render.png"        || return
  # (Only verifiable with a Debug build: Release compiles qDebug() out.)
  local rlog="$OUT/$name.render.log"
  if grep -q '"ngspice/' "$sch" && grep -q '^Debug:' "$rlog" && ! grep -q "Loading data from .*$ds" "$rlog"; then
    fail=$((fail+1)); failed_names+=("$name.render.data")
    printf '  FAIL  %-60s render did not load the dataset\n' "$name.render.data"
    return 1
  fi
  # Not a failure, but worth knowing: do the diagram traces resolve?
  local missing=0 var
  while IFS= read -r var; do
    grep -q "^<dep $var " "$work/$ds" || grep -q "^<indep $var " "$work/$ds" || missing=$((missing+1))
  done < <(grep -oE '"ngspice/[^"]+"' "$sch" | tr -d '"' | sed 's#^ngspice/##' | sort -u)
  [ $missing -gt 0 ] && echo "::warning::$rel: $missing diagram trace(s) not found in dataset (ngspice version?)"
  return 0
}

# The raw simulator outputs are the corpus for scripts/ci/fuzz-simout.py.
# A Debug build leaves them in the kernel's work directory (Release removes
# them); that directory is printed by Ngspice::slotSimulate() in Debug.
stash_simulator_outputs() {
  local name="$1" work="$2" schname="$3"
  local simdir; simdir="$(grep -o 'Debug: "[^"]*" ([^)]*slotSimulate' "$OUT/$name.simulate.log" | head -1 | sed 's/^Debug: "//; s/".*$//')"
  [ -n "$simdir" ] && [ -d "$simdir" ] || return 0
  mkdir -p "$work/simout"
  cp "$simdir"/spice4qucs.* "$work/simout/" 2>/dev/null || return 0
  echo "$schname" > "$work/simout/schematic.txt"
}

suite_simulate() {
  echo "== simulate: netlist -> ngspice -> dataset -> render"
  command -v ngspice >/dev/null || { echo "ngspice not found"; return 1; }
  ngspice --version 2>/dev/null | sed -n '2p'
  for rel in "${SIM_SET[@]}"; do simulate_one "$rel"; done
}

# --------------------------------------------------------------------------
# Self-contained: uses scripts/ci/fixtures/hostile.sch (two rectangular
# diagrams plotting v_out / i_out against time in line and symbol styles) and
# generates the datasets here, so it needs neither ngspice nor any settings.
FIXTURES="$(cd "$(dirname "${BASH_SOURCE[0]}")/fixtures" && pwd)"

# gen_dataset <file> <n_indep> <n_v_out> <n_i_out> [indep_header_count]
# Writes a Qucs dataset with a complete 'time' block of n_indep samples, then
# v_out (real) with n_v_out samples and i_out (complex) with n_i_out samples.
# The header count of the independent block can be overridden to lie.
gen_dataset() {
  local f="$1" n="$2" nv="$3" ni="$4" hdr="${5:-$2}"
  awk -v n="$n" -v nv="$nv" -v ni="$ni" -v hdr="$hdr" 'BEGIN {
    print "<Qucs Dataset 26.1.1>"
    print "<indep time " hdr ">"
    for (i = 0; i < n; i++) printf "%.6e\n", i * 1e-6
    print "</indep>"
    print "<dep v_out time>"
    for (i = 0; i < nv; i++) printf "%.6e\n", sin(i / 10.0)
    print "</dep>"
    print "<dep i_out time>"
    # Qucs writes complex samples as "re+jim" / "re-jim".
    for (i = 0; i < ni; i++) { im = sin(i / 10.0); printf "%.6e%sj%.6e\n", cos(i / 10.0), (im < 0 ? "-" : "+"), (im < 0 ? -im : im) }
    print "</dep>"
  }' >"$f"
}

# hostile_case <name> <generator-command...> : renders the fixture against
# the dataset the generator writes to $ds.
hostile_case() {
  local name="$1"; shift
  local work="$OUT/work/$name"
  rm -rf "$work"; mkdir -p "$work"
  cp "$FIXTURES/hostile.sch" "$work/"
  ds="$work/hostile.dat"
  "$@"
  run "$name" 120 "$QUCS" -p -i "$work/hostile.sch" -o "$work/render.png"
}

suite_hostile() {
  echo "== hostile: render fixture against damaged datasets (exercises Graph::loadDatFile / Diagram::calcData)"
  local ds
  # Baseline: a well-formed dataset must render.
  hostile_case valid_1000        gen_dataset_wrap 1000 1000 1000
  hostile_case valid_1_sample    gen_dataset_wrap 1 1 1
  hostile_case valid_2_samples   gen_dataset_wrap 2 2 2
  hostile_case valid_3_samples   gen_dataset_wrap 3 3 3
  # Dependent block far shorter than the independent block it references.
  hostile_case dep_short_small   gen_dataset_wrap 1000 10 10
  hostile_case dep_short_large   gen_dataset_wrap 300000 10 10
  hostile_case dep_empty         gen_dataset_wrap 1000 0 0
  # Dependent block longer than declared (extra data must be ignored).
  hostile_case dep_long          gen_dataset_wrap 100 1000 1000
  # Independent header lies about its length.
  hostile_case indep_hdr_bigger  gen_dataset_wrap 100 100 100 1000
  hostile_case indep_hdr_smaller gen_dataset_wrap 100 100 100 10
  hostile_case indep_hdr_zero    gen_dataset_wrap 100 100 100 0
  hostile_case indep_hdr_negative gen_dataset_wrap 100 100 100 -5
  # Absurd sample count: new double[n] must not take the process down (#1539).
  hostile_case indep_hdr_huge    gen_dataset_wrap 10 10 10 2000000000
  # Structural damage.
  hostile_case truncated_mid_dep  gen_truncated
  hostile_case no_closing_angle   gen_no_closing_angle
  hostile_case garbage_tokens     gen_garbage_tokens
  hostile_case malformed_complex  gen_malformed_complex
  hostile_case empty_file         gen_empty
  hostile_case header_only        gen_header_only
  hostile_case missing_file       gen_missing
  hostile_case binary_junk        gen_binary_junk
}
# Generators used by hostile_case; each writes the dataset to $ds.
gen_dataset_wrap()     { gen_dataset "$ds" "$@"; }
gen_truncated()        { gen_dataset "$ds" 1000 1000 1000; head -c 12000 "$ds" >"$ds.t" && mv "$ds.t" "$ds"; }
gen_no_closing_angle() { gen_dataset "$ds" 100 100 100; sed 's/^<dep v_out time>/<dep v_out time/' "$ds" >"$ds.t" && mv "$ds.t" "$ds"; }
gen_garbage_tokens()   { gen_dataset "$ds" 100 100 100; awk '{print} /^<dep v_out time>/{print "NaN nan inf -inf 1e999 +j 1.0+j 1.0+2.0k"}' "$ds" >"$ds.t" && mv "$ds.t" "$ds"; }
gen_malformed_complex(){ gen_dataset "$ds" 100 100 100; sed 's/j$/x/' "$ds" >"$ds.t" && mv "$ds.t" "$ds"; }
gen_empty()            { : >"$ds"; }
gen_header_only()      { echo "<Qucs Dataset 26.1.1>" >"$ds"; }
gen_missing()          { rm -f "$ds"; }
gen_binary_junk()      { head -c 65536 /dev/urandom >"$ds"; }

# --------------------------------------------------------------------------
prepare_settings
case "$suite" in
  load)     suite_load ;;
  simulate) suite_simulate ;;
  hostile)  suite_hostile ;;
  *) echo "unknown suite: $suite"; exit 2 ;;
esac

# UBSan reports do not change the exit code; surface them as annotations.
ub=$(grep -l "runtime error:" "$OUT"/*.log 2>/dev/null | wc -l | tr -d ' ')
[ "$ub" != "0" ] && echo "::warning::$suite: UBSan reported runtime errors in $ub log file(s) under $OUT"

echo "== $suite: $pass passed, $fail failed"
[ $fail -eq 0 ] || { printf '   - %s\n' "${failed_names[@]}"; exit 1; }
exit 0
