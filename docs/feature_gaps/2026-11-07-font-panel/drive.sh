#!/bin/bash
# One drive of the real Qucs-S and the real macOS font panel. Takes the screen for
# some 10 s: windows open and take the keyboard, so tell whoever is at the Mac.
#
#   cmake -S docs/feature_gaps/2026-11-07-font-panel -B build-paneldriver -G Ninja -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
#   cmake --build build-paneldriver
#   drive.sh <qucs-s binary> <libpaneldriver.dylib> <empty or new folder> DRIVE_BUTTON=2 DRIVE_SIZE=14 DRIVE_END=apply
#
# The binary must be a local build, on the Qt the library was built with. Its own
# settings folder, in One Dark, Text document font Menlo,10; drive.log, pictures and
# the settings are left in the folder. Started directly: a protected launcher
# (/usr/bin/env, /usr/bin/timeout) would strip DYLD_INSERT_LIBRARIES.
bin=$1; lib=$2; out=$3; shift 3
mkdir -p "$out/settings/qucs"
ini=$out/settings/qucs/qucs_s.ini
if [ ! -e "$ini" ]; then
    # (Without firstRun and a simulator a box stops the start, and the drive.)
    cat > "$ini" <<'INI'
[General]
firstRun=false
DefaultSimulator=1
NgspiceExecutable=/bin/sh
Theme=10
textFont="Menlo,10,-1,5,400,0,0,0,0,0,0,0,0,0,0,1,,0,0"
INI
fi
export QUCS_SETTINGS_DIR="$out/settings" QUCS_NO_SHELL_ENV=1 QUCS_TRASH_DIR="$out/trash" QUCS_CLAUDE=/nonexistent/claude QUCS_GH=/nonexistent/gh DRIVE_OUT="$out"
for kv in "$@"; do export "$kv"; done
DYLD_INSERT_LIBRARIES="$lib" "$bin" </dev/null >"$out/out.log" 2>&1 &
pid=$!
( sleep "${WATCHDOG:-40}"; kill -9 $pid 2>/dev/null ) >/dev/null 2>&1 &
dog=$!
wait $pid; rc=$?
kill $dog 2>/dev/null
echo "exit=$rc"; cat "$out/drive.log" 2>/dev/null
sed -nE '/^(font|appFont|textFont)=/p' "$ini"
