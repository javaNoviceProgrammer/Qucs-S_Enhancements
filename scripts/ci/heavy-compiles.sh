#!/usr/bin/env bash
#
# A compiler launcher (CMAKE_CXX_COMPILER_LAUNCHER) that lets only so many of
# the build's heaviest units compile at once, the others as they come. CI's
# sanitized Debug build lost its runner - 16 GB, four processors - seven
# times in eleven runs, its memory gone: ninja ran six compilers there, and
# under GCC's ASan and UBSan at -O0 the multiphysics engine's units, RF
# 2.5D's and the tests of both each take 1.3 to 4.8 GB, side by side in the
# build's order.
#
# Usage: heavy-compiles.sh <compiler> <arguments...>
#
# A unit is heavy when its source is under multiphysics/ or rf25/, is
# qucscontrol.cpp, or is a test of the engine, of RF 2.5D or of the control
# (tests/test_fem*, test_rf25*, test_qucs_control*): where the units measured
# past 1.3 GB are - a third of the build's compiling, so two of four
# processors carry it. HEAVY_SLOTS of them compile at once (2 by default),
# each holding a lock on one of the slot files in HEAVY_SLOT_DIR (by default
# heavy-compiles/ under RUNNER_TEMP or TMPDIR) until its compiler exits.
set -uo pipefail

src=""
for a in "$@"; do
    case "$a" in
    *.cpp | *.cc | *.cxx) src=$a ;;
    esac
done

case "$src" in
*/multiphysics/* | */rf25/* | */qucscontrol.cpp | */tests/test_fem* | */tests/test_rf25* | */tests/test_qucs_control*) ;;
*) exec "$@" ;;
esac

# (Without flock - util-linux's - every unit as it comes.)
command -v flock > /dev/null || exec "$@"

slots=${HEAVY_SLOTS:-2}
dir=${HEAVY_SLOT_DIR:-${RUNNER_TEMP:-${TMPDIR:-/tmp}}/heavy-compiles}
mkdir -p "$dir"
while :; do
    for ((i = 1; i <= slots; i++)); do
        exec 9>>"$dir/slot$i"
        # The lock stays with the compiler: the descriptor is inherited.
        if flock -n 9; then
            exec "$@"
        fi
        exec 9>&-
    done
    sleep 1
done
