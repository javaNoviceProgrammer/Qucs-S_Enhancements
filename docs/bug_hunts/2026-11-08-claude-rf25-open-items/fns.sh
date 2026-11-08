#!/bin/sh
# usage: fns.sh <tree> <binary> <function...>: runs them through onetest.sh and prints each function's verdict.
S="${SCRATCH:-${TMPDIR:-/tmp}}"
"$(dirname "$0")/onetest.sh" "$@" > "$S/t.log" 2>&1
f=$(ls -td "$S"/ctest.* | head -1)
grep -E "^(PASS|FAIL|SKIP|XFAIL)|Loc:|Actual|Expected|AddressSanitizer|runtime error|Received signal" "$f/Testing/Temporary/LastTest.log" | grep -v "initTestCase\|cleanupTestCase\|::init()"
grep -E "tests passed" "$S/t.log"
