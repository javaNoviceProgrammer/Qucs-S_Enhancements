#!/usr/bin/env bash
#
# Download the bundles built by .github/workflows/release.yml into bin/,
# sorted by platform and architecture:
#
#   bin/macos/apple-silicon/   *.dmg
#   bin/macos/intel/           *.dmg
#   bin/linux/intel/           *.AppImage
#   bin/linux/arm/             *.AppImage
#   bin/windows/intel/         *.zip, *-setup.exe
#   bin/windows/arm/           *.zip, *-setup.exe
#
# Usage:
#   scripts/fetch-binaries.sh            # the rolling "continuous" pre-release
#   scripts/fetch-binaries.sh v26.1.1-1  # a tagged release
#   scripts/fetch-binaries.sh --run 123  # artifacts of a specific workflow run
#
# Needs the GitHub CLI (gh) authenticated for this repository. Binaries are
# git-ignored; only the directory skeleton is tracked.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bin="$repo_root/bin"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

command -v gh >/dev/null || { echo "gh (GitHub CLI) is required: https://cli.github.com" >&2; exit 1; }

if [ "${1:-}" = "--run" ]; then
  [ -n "${2:-}" ] || { echo "usage: $0 --run <run-id>" >&2; exit 2; }
  echo "Downloading artifacts of workflow run $2 ..."
  gh run download "$2" --dir "$tmp"
else
  tag="${1:-continuous}"
  echo "Downloading release '$tag' ..."
  gh release download "$tag" --dir "$tmp" --clobber
fi

# Place each file by the platform/arch suffix the release workflow gives it.
placed=0
while IFS= read -r -d '' f; do
  name="$(basename "$f")"
  case "$name" in
    *-macos-apple-silicon.dmg) dest="macos/apple-silicon" ;;
    *-macos-intel.dmg)         dest="macos/intel" ;;
    *-linux-intel.AppImage)    dest="linux/intel" ;;
    *-linux-arm.AppImage)      dest="linux/arm" ;;
    *-windows-intel.zip|*-windows-intel-setup.exe) dest="windows/intel" ;;
    *-windows-arm.zip|*-windows-arm-setup.exe)     dest="windows/arm" ;;
    SHA256SUMS.txt)            dest="." ;;
    *) echo "  skipping unrecognised file: $name"; continue ;;
  esac
  mkdir -p "$bin/$dest"
  # Keep only the newest bundle per platform to avoid accumulating old builds.
  if [ "$dest" != "." ]; then
    find "$bin/$dest" -maxdepth 1 -type f ! -name .gitkeep -delete
  fi
  mv -f "$f" "$bin/$dest/$name"
  chmod +x "$bin/$dest/$name" 2>/dev/null || true
  echo "  bin/$dest/$name"
  placed=$((placed+1))
done < <(find "$tmp" -type f -print0)

echo "Placed $placed file(s) under bin/."
if [ -f "$bin/SHA256SUMS.txt" ]; then
  echo "Verifying checksums ..."
  (cd "$bin" && while read -r sum name; do
     path="$(find . -type f -name "$name" | head -1)"
     [ -n "$path" ] || continue
     echo "$sum  $path"
   done < SHA256SUMS.txt | shasum -a 256 -c -) || echo "WARNING: checksum verification failed" >&2
fi
