#!/bin/sh
# usage: onetest.sh <tree> <test binary name> <function> [function...] - one test's functions through ctest,
# with qucs_add_test's environment and a throwaway HOME.
tree="$1"; bin="$2"; shift 2
S="${SCRATCH:-${TMPDIR:-/tmp}}"
d=$(mktemp -d "$S/ctest.XXXXXX"); mkdir -p "$d/home"
{
  printf 'add_test(%s "%s/qucs/tests/%s"' "$bin" "$tree" "$bin"
  for f in "$@"; do printf ' "%s"' "$f"; done
  printf ')\n'
  printf 'set_tests_properties(%s PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;ASAN_OPTIONS=detect_leaks=0;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;QUCS_CLAUDE=/nonexistent/claude;QUCS_GH=/nonexistent/gh;HOME=%s/home;QTEST_FUNCTION_TIMEOUT=3000000" TIMEOUT 3600)\n' "$bin" "$d"
} > "$d/CTestTestfile.cmake"
ctest --test-dir "$d" --output-on-failure
