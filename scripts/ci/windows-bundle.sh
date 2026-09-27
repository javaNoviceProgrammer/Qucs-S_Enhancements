#!/usr/bin/env bash
#
# The Windows bundle: the bin folder of an installed Qucs-S suite made to
# run on a PC without MSYS2, and checked the way such a PC runs it - so a
# DLL left out fails the build, not the user's first start. (26.1.3 went
# out without Qt6Network.dll: Qt PDF links Qt Network, and windeployqt was
# told --no-network. "The code execution cannot proceed because
# Qt6Network.dll was not found.")
#
# Usage, in the MSYS2 shell:
#   windows-bundle.sh deploy <bin folder> <msystem>
#       windeployqt for each program, then every DLL that a program, a DLL
#       or a plugin of the bundle needs from the MSYS2 environment copied
#       beside the programs (the folder Windows looks in first - for a
#       plugin's own needs too).
#   windows-bundle.sh check <bin folder> <msystem> <schematic>
#       1. Nothing in the bundle needs a DLL of the environment that the
#          bundle lacks.
#       2. qucs-s.exe started with PATH cut down to the bundle and
#          Windows's own folders, turning <schematic> into a netlist and
#          exiting: every DLL it links is loaded, as on a PC with nothing
#          else installed.
#
# <msystem> is the MSYS2 environment's folder: ucrt64, clangarm64, ...
set -uo pipefail

command="${1:?deploy or check}"
bin="${2:?bundle bin folder}"
msys="${3:?msystem (ucrt64, clangarm64, ...)}"
bin="$(cd "$bin" && pwd)" || exit 2   # (absolute: it goes into PATH)

programs=(qucs-s qucs-sactivefilter qucs-sattenuator qucs-sfilter
          qucs-spowercombining qucs-strans qucs-sspar-viewer rxcalc)

# What loads DLLs of its own accord, one to a line: the programs, and the
# plugins (in the folders below). ldd gives a file's whole closure, so the
# DLLs beside the programs - what these load - need no look of their own.
bundle_files() {
  find "$bin" -maxdepth 1 -type f -iname '*.exe'
  find "$bin" -mindepth 2 -type f -iname '*.dll'
}

# The DLLs of the environment that $1 needs, as "name path" lines; and
# those nothing supplies, as "name not-found". ldd starts the file under a
# debugger to see what it loads, which can hang on Windows on Arm (the
# 26.1.3 release waited 2 hours on one): each try has a time limit, and a
# file ldd could not finish in two is one line "? timed-out".
needs() {
  local out status try
  for try in 1 2; do
    out="$(timeout -k 10 60 ldd "$1" 2>/dev/null)"
    status=$?
    [ "$status" -ne 124 ] && [ "$status" -ne 137 ] && break
  done
  if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
    echo "? timed-out"
    return 0
  fi
  # api-ms-win-* and ext-ms-win-* are Windows's API sets, which its loader
  # maps to its own DLLs - never shipped; ldd on Windows on Arm finds them
  # nowhere.
  awk -v env="/$msys/" '
    tolower($1) ~ /^(api|ext)-ms-win-/ { next }
    index($3, env) == 1 { print $1, $3; next }
    /not found/         { print $1, "not-found" }' <<< "$out"
}

timed_out() {
  echo "::warning::ldd did not finish for $(basename "$1") in two tries of a minute; the start of qucs-s.exe below still tells what the bundle lacks"
}

has() {
  [ -f "$bin/$1" ]
}

deploy() {
  for exe in "${programs[@]}"; do
    if [ -f "$bin/$exe.exe" ]; then
      windeployqt.exe "$bin/$exe.exe" --svg --no-translations --no-system-d3d-compiler \
        || echo "::warning::windeployqt failed for $exe.exe (the check below tells what that left out)"
    else
      echo "::warning::$exe.exe not found in $bin"
    fi
  done
  # Qucs-S uses no network: Qt Network is there only because Qt PDF links
  # it, and its plugins (TLS backends, network information) are not.
  rm -rf "$bin/tls" "$bin/networkinformation"
  # What windeployqt does not know of (libstdc++, ICU, a plugin's image
  # library, ...). ldd gives each file's whole closure; a second pass
  # takes the needs of what the first one copied.
  for pass in 1 2; do
    copied=0
    while IFS= read -r f; do
      while read -r dll path; do
        [ "$path" = timed-out ] && { timed_out "$f"; continue; }
        [ "$path" = not-found ] && continue
        if ! has "$dll"; then
          cp -f "$path" "$bin/" && copied=$((copied + 1)) && echo "copied $dll (for $(basename "$f"))"
        fi
      done < <(needs "$f")
    done < <(bundle_files)
    [ "$copied" -eq 0 ] && break
  done
  return 0
}

check() {
  local schematic="${1:?schematic}"
  local failed=0 files=0
  # 1. Needs the bundle does not meet: "dll needer" lines, then grouped.
  local report
  report="$(mktemp)"
  while IFS= read -r f; do
    files=$((files + 1))
    while read -r dll path; do
      [ "$path" = timed-out ] && { timed_out "$f"; continue; }
      has "$dll" || echo "$dll $(basename "$f")" >> "$report"
    done < <(needs "$f")
  done < <(bundle_files)
  echo "Looked at $files programs and plugins in $bin."
  if [ -s "$report" ]; then
    failed=1
    awk '{ need[$1] = need[$1] " " $2 }
         END { for (d in need) print "::error::" d " is not in the bundle; needed by:" need[d] }' "$report"
  fi
  rm -f "$report"

  # 2. Started as on a PC without MSYS2 (timeout and env found first:
  # the program's PATH has nothing of MSYS2 in it).
  local out timeout_cmd env_cmd status
  out="$(mktemp -d)"
  timeout_cmd="$(command -v timeout)"
  env_cmd="$(command -v env)"
  local windows="/c/Windows/System32:/c/Windows:/c/Windows/System32/Wbem"
  "$timeout_cmd" -k 10 300 "$env_cmd" PATH="$bin:$windows" \
    "$bin/qucs-s.exe" -n -i "$schematic" -o "$out/check.net" --ngspice > "$out/log" 2>&1
  status=$?
  if [ "$status" -ne 0 ] || ! head -1 "$out/check.net" 2>/dev/null | grep -q '^\* Qucs'; then
    echo "::error::qucs-s.exe did not start on its own bundle (exit $status):"
    cat "$out/log"
    failed=1
  else
    echo "qucs-s.exe started on its own bundle and wrote a netlist of $(wc -l < "$out/check.net" | tr -d ' ') lines."
  fi
  rm -rf "$out"
  return "$failed"
}

case "$command" in
  deploy) deploy ;;
  check)  check "${4:-}" ;;
  *) echo "usage: $0 deploy|check <bin folder> <msystem> [schematic]" >&2; exit 2 ;;
esac
