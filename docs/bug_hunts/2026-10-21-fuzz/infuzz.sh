#!/bin/bash
# infuzz.sh NAME TEST FUNCTION VAR=VALUE... : one of the tests' fuzzers on the
# ASan/UBSan build, with its own HOME, settings, cache, trash and output under
# $HUNT_RUNS/NAME, its log $HUNT_RUNS/logs/NAME.log. From the repository:
#   HUNT_RUNS=/tmp/qucs-hunt docs/bug_hunts/2026-10-21-fuzz/infuzz.sh expr25 test_fem_engine fuzz QUCS_EXPR_FUZZ=1200 QUCS_EXPR_FUZZ_SEED=25
B="${BRK_BUILD:-build-asan}/qucs/tests"
name=$1; test=$2; fn=$3; shift 3
R=$HUNT_RUNS/$name; mkdir -p "$R/home" "$R/settings" "$R/cache" "$R/trash" "$R/out" "$HUNT_RUNS/logs"
start=$(date +%s)
env "$@" QT_QPA_PLATFORM=offscreen QTEST_FUNCTION_TIMEOUT=7200000 QUCS_FUZZ_OUT="$R/out" HOME="$R/home" QUCS_SETTINGS_DIR="$R/settings" \
    QUCS_CACHE_DIR="$R/cache" QUCS_TRASH_DIR="$R/trash" ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$B/$test" "$fn" -maxwarnings 0 > "$HUNT_RUNS/logs/$name.log" 2>&1
echo "exit $? after $(( $(date +%s) - start )) s" >> "$HUNT_RUNS/logs/$name.log"
