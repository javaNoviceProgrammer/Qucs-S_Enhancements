#!/bin/bash
# usage: run.sh <tests directory>: each past find put back, on its test, with
# the same test untouched first. A row: the find, the test, how the control
# ended, how the broken one ended, and the sanitizer's first word for it.
cd "$1" || exit 1
export QT_QPA_PLATFORM=offscreen ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
export QUCS_CLAUDE=/nonexistent/claude QUCS_GH=/nonexistent/gh QTEST_FUNCTION_TIMEOUT=900000
if command -v timeout > /dev/null; then limit="timeout 900"; else limit=""; fi
printf '%-4s %-44s %-8s %-8s %s\n' find test control broken report
while read -r n test function; do
  [ -z "$n" ] && continue
  $limit ./"$test" $function > /tmp/break-c-$$.log 2>&1; c=$?
  env QUCS_BREAK_$n=1 $limit ./"$test" $function > /tmp/break-b-$$.log 2>&1; b=$?
  word=$(grep -m1 -oE 'AddressSanitizer: (attempting )?[a-zA-Z-]+|runtime error: .*|ASSERT[^ ]* .*|ASSERT: .*|Assertion .* failed' /tmp/break-b-$$.log | cut -c1-110)
  printf '%-4s %-44s %-8s %-8s %s\n' "$n" "$test $function" "$c" "$b" "$word"
done <<'LIST'
1 test_qucs_control aNewDocumentsFirstEditIsCounted
2 test_file_browser
3 test_markdown_doc
4 test_python_doc
5 test_multiphysics_doc aModelIsBuiltInThePanel
6 test_fem_engine hostileExpressions
7 test_erc aWindingsCoreThatIsNoCore
8 test_fem_lumerical damagedInputsAreRefused
9 test_spreadsheet oddNumbersInAWorkbookAreRead
10 test_multiphysics_doc aWaitOutlivesItsModel
11 test_fem_sparse aPatternThatChanges
13 test_fem_engine hostilePositions
15 test_python_extras quickFixesOfALine
LIST
