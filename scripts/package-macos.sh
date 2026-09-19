#!/usr/bin/env bash
#
# Turn a finished macOS build into a self-contained, redistributable
# qucs-s.app and a .dmg. Used by the Release workflow and for local builds.
#
# Usage:
#   scripts/package-macos.sh <build-dir> <output-dir> [name]
#
#   build-dir   a configured and built tree (cmake --build <dir>)
#   output-dir  where <name>.dmg (and the assembled <name>.app) are written
#   name        base name of the outputs, default "qucs-s-<VERSION>-<sha>-macos-<arch>"
#
# What it does, mirroring upstream's deploy.yml:
#   * copies the tool apps (filter, transcalc, ...) and qucsator into
#     qucs-s.app/Contents/MacOS/bin, and examples/library/symbols/lang into
#     Contents/MacOS/share/qucs-s (main.cpp resolves resources relative to
#     the executable, so this layout is what the app expects);
#   * runs macdeployqt on the main app and every nested app so the Qt
#     frameworks and plugins travel with the bundle;
#   * ad-hoc code-signs the bundle;
#   * packs a compressed .dmg with an /Applications shortcut (hdiutil, no
#     extra dependencies).
#
# ngspice is not bundled (as upstream); install it separately.
set -euo pipefail

build="${1:?build dir}"
out="${2:?output dir}"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="$repo/qucs-s-26.1.1"

[ -d "$build/qucs/qucs-s.app" ] || { echo "no qucs-s.app in $build/qucs - build first" >&2; exit 1; }
for tool in macdeployqt hdiutil codesign lipo; do
  command -v "$tool" >/dev/null || { echo "$tool not found in PATH" >&2; exit 1; }
done

arch="$(lipo -archs "$build/qucs/qucs-s.app/Contents/MacOS/qucs-s")"
case "$arch" in
  arm64)         label="apple-silicon" ;;
  x86_64)        label="intel" ;;
  *arm64*x86_64*|*x86_64*arm64*) label="universal" ;;
  *)             label="$arch" ;;
esac
version="$(tr -d '[:space:]' < "$src/VERSION")"
sha="$(git -C "$repo" rev-parse --short HEAD 2>/dev/null || echo local)"
name="${3:-qucs-s-${version}-${sha}-macos-${label}}"

mkdir -p "$out"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

echo "==> Assembling $name.app ($arch)"
app="$stage/qucs-s.app"
cp -pR "$build/qucs/qucs-s.app" "$app"
bin="$app/Contents/MacOS/bin"
res="$app/Contents/MacOS/share/qucs-s"
mkdir -p "$bin" "$res/examples" "$res/library" "$res/symbols" "$res/lang"

for tool in qucs-activefilter/qucs-sactivefilter qucs-attenuator/qucs-sattenuator \
            qucs-filter/qucs-sfilter qucs-powercombining/qucs-spowercombining \
            qucs-s-spar-viewer/qucs-sspar-viewer qucs-transcalc/qucs-strans rxcalc/rxcalc; do
  if [ -d "$build/$tool.app" ]; then
    cp -pR "$build/$tool.app" "$bin/"
  else
    echo "    warning: $tool.app not built, skipping" >&2
  fi
done
cp -p "$build/qucsator_rf/src/qucsator_rf" "$build/qucsator_rf/src/converter/qucsconv_rf" "$bin/"
cp -pR "$src/examples/." "$res/examples/"
cp -p  "$src"/library/*.lib "$src"/library/*.blacklist "$res/library/"
cp -pR "$src/library/symbols/." "$res/symbols/"
cp -p  "$build"/translations/*.qm "$res/lang/" 2>/dev/null || echo "    warning: no translations found" >&2

echo "==> Bundling Qt (macdeployqt)"
macdeployqt "$app" -verbose=1 >/dev/null
for nested in "$bin"/*.app; do
  macdeployqt "$nested" -verbose=1 >/dev/null
done
strip "$bin/qucsator_rf" "$bin/qucsconv_rf" 2>/dev/null || true

# macdeployqt ships only the cocoa platform plugin. Add the offscreen one
# so the bundled binary can also run headless (qucs-s -n / -p with
# QT_QPA_PLATFORM=offscreen), which is what the smoke tests and scripted
# use need. Its Qt dependencies are already in the bundle.
plugins_src="$(dirname "$(command -v macdeployqt)")/../share/qt/plugins/platforms"
[ -d "$plugins_src" ] || plugins_src="$(qmake -query QT_INSTALL_PLUGINS 2>/dev/null)/platforms"
if [ -f "$plugins_src/libqoffscreen.dylib" ]; then
  cp -p "$plugins_src/libqoffscreen.dylib" "$app/Contents/PlugIns/platforms/"
  # Point its Qt dependencies into the bundle, as macdeployqt did for cocoa.
  for fw in $(otool -L "$app/Contents/PlugIns/platforms/libqoffscreen.dylib" | grep -o '@rpath/Qt[A-Za-z]*\.framework[^ ]*'); do
    install_name_tool -change "$fw" "@executable_path/../Frameworks/${fw#@rpath/}" \
      "$app/Contents/PlugIns/platforms/libqoffscreen.dylib"
  done
else
  echo "    warning: offscreen platform plugin not found next to macdeployqt; headless use of the bundle will not work" >&2
fi

# macdeployqt rewrites what depends on what, but leaves the install name
# (LC_ID_DYLIB) of some third-party libraries (brotli, webp, ...) pointing
# at the Homebrew path they were copied from. Harmless for loading, but
# misleading; make them relocatable too.
find "$app" -path '*/Frameworks/*.dylib' -type f | while IFS= read -r lib; do
  id="$(otool -D "$lib" | sed -n '2p')"
  case "$id" in
    /opt/homebrew/*|/usr/local/Cellar/*|/usr/local/opt/*|*/Qt/*)
      install_name_tool -id "@rpath/$(basename "$lib")" "$lib" ;;
  esac
done

echo "==> Signing (ad hoc)"
codesign --force --deep --sign - "$app"

# Anything still pointing at a Homebrew or Qt install path would break on
# another machine.
if otool -L "$app/Contents/MacOS/qucs-s" | grep -qE '/opt/homebrew|/usr/local/(Cellar|opt)|/Qt/'; then
  echo "error: qucs-s still links against an install path:" >&2
  otool -L "$app/Contents/MacOS/qucs-s" | grep -E '/opt/homebrew|/usr/local/(Cellar|opt)|/Qt/' >&2
  exit 1
fi

echo "==> Creating $name.dmg"
dmgroot="$stage/dmg"
mkdir -p "$dmgroot"
mv "$app" "$dmgroot/"
ln -s /Applications "$dmgroot/Applications"
rm -f "$out/$name.dmg"
hdiutil create -volname "Qucs-S $version" -srcfolder "$dmgroot" -ov -format UDZO -quiet "$out/$name.dmg"

# Keep the assembled app next to the image for direct use.
rm -rf "$out/$name.app"
mv "$dmgroot/qucs-s.app" "$out/$name.app"

echo "==> Done:"
ls -la "$out/$name.dmg" "$out/$name.app/Contents/MacOS/qucs-s"
du -sh "$out/$name.app" | sed 's/^/    /'
