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
#     qucs-s.app/Contents/MacOS/bin, and examples/library/symbols/spicelibrary/
#     lang into Contents/MacOS/share/qucs-s (main.cpp resolves resources relative to
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
# The build-tree app may carry a share tree of its own (the smoke suite
# links the source tree's spicelibrary into it): start from nothing.
rm -rf "$app/Contents/MacOS/share"
mkdir -p "$bin" "$res/examples" "$res/library" "$res/symbols" "$res/lang" "$res/spicelibrary"

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
# The library as library/CMakeLists.txt installs it: the .lib files, the
# blacklist, the model directories some of them include, the symbols, and
# the SPICE subcircuits of the transformer, relay, switch, coax and
# magnetic core (spicelibrary - without it those components' netlists
# point at a file that is not there).
cp -p  "$src"/library/*.lib "$src"/library/*.blacklist "$res/library/"
for models in TubesExtended BJT_Darlington Optocoupler DualGateMOSFET; do
  cp -pR "$src/library/$models" "$res/library/"
done
cp -pR "$src/library/symbols/." "$res/symbols/"
cp -pR "$src/library/spicelibrary/." "$res/spicelibrary/"
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
# The same for the frameworks it leaves alone (QtDBus, the QtQml ones).
find "$app" -path '*/Frameworks/*.framework/Versions/*' -type f | while IFS= read -r lib; do
  case "$(file -b "$lib" 2>/dev/null)" in Mach-O*) ;; *) continue ;; esac
  id="$(otool -D "$lib" | sed -n '2p')"
  case "$id" in
    /opt/homebrew/*|/usr/local/Cellar/*|/usr/local/opt/*|*/Qt/*)
      install_name_tool -id "@rpath/${lib#*/Frameworks/}" "$lib" ;;
  esac
done

# Every Mach-O in the bundle: the executables, the nested apps, the
# frameworks and the plugins.
machos() {
  find "$app" -type f \( -perm -u+x -o -name '*.dylib' -o -path '*.framework/Versions/*' \) \
    | while IFS= read -r f; do
        case "$(file -b "$f" 2>/dev/null)" in Mach-O*) printf '%s\n' "$f" ;; esac
      done
}

# The build's rpath into the Homebrew Qt (CMake's, so the tree runs from
# the build directory) stays on the binaries after macdeployqt. On a
# machine without that Qt it is dead; on one with it, anything the bundle
# does not resolve is looked up there - a plugin then loads that Qt next
# to the bundled one ("Class ... is implemented in both", spurious casting
# failures, crashes). Drop every such rpath.
machos | while IFS= read -r bin_; do
  otool -l "$bin_" | awk '/LC_RPATH/ {f=1} f && /path / {print $2; f=0}' | while IFS= read -r rp; do
    case "$rp" in
      /opt/homebrew/*|/usr/local/Cellar/*|/usr/local/opt/*|*/Qt/*)
        install_name_tool -delete_rpath "$rp" "$bin_" ;;
    esac
  done
done

# Third-party dylibs macdeployqt copied still name each other through
# @rpath (libwebp wants @rpath/libsharpyuv, brotlidec wants brotlicommon)
# with an rpath of @loader_path/../lib, which is nowhere in a bundle. They
# sit side by side in Contents/Frameworks: let @rpath mean that.
find "$app" -path '*/Contents/Frameworks/*.dylib' -type f | while IFS= read -r lib; do
  if otool -L "$lib" | tail -n +2 | awk '{print $1}' | grep -q '^@rpath/'; then
    install_name_tool -add_rpath '@loader_path' "$lib" 2>/dev/null || true
  fi
done

# A plugin that wants a Qt framework macdeployqt did not bring (the PDF
# image format wants QtPdf, from qtwebengine) cannot load; leave it out
# rather than have it find a Qt outside the bundle.
find "$app" -path '*/Contents/PlugIns/*' -name '*.dylib' -type f | while IFS= read -r plugin; do
  owner="${plugin%%/Contents/PlugIns/*}"   # the app (main or nested) the plugin belongs to
  for fw in $(otool -L "$plugin" | tail -n +2 | awk '{print $1}' | grep '^@' | grep -o '[A-Za-z0-9_]*\.framework' | sort -u); do
    if [ ! -d "$owner/Contents/Frameworks/$fw" ]; then
      echo "    dropping plugin ${plugin#$app/Contents/} (needs $fw, not bundled)"
      rm -f "$plugin"
      break
    fi
  done
done

echo "==> Signing (ad hoc)"
codesign --force --deep --sign - "$app"

# Anything still pointing at a Homebrew or Qt install path - as a
# dependency or as an rpath, in any binary of the bundle - would break
# on another machine, or load a second Qt on this one.
bad="$(machos | while IFS= read -r bin_; do
  if otool -L "$bin_" | tail -n +2 | grep -qE '/opt/homebrew|/usr/local/(Cellar|opt)|/Qt/'; then
    echo "error: ${bin_#$app/Contents/} still links against an install path:" >&2
    otool -L "$bin_" | tail -n +2 | grep -E '/opt/homebrew|/usr/local/(Cellar|opt)|/Qt/' >&2
    echo bad
  fi
  if otool -l "$bin_" | awk '/LC_RPATH/ {f=1} f && /path / {print $2; f=0}' | grep -qE '/opt/homebrew|/usr/local/(Cellar|opt)|/Qt/'; then
    echo "error: ${bin_#$app/Contents/} keeps an rpath into an install path" >&2
    echo bad
  fi
done)"
[ -z "$bad" ] || exit 1

# The resources main.cpp looks for next to the executable; a component
# whose netlist includes one of the spicelibrary files fails in the
# simulator when it is missing.
for want in examples/ngspice library/Ideal.lib library/BJT_Darlington symbols \
            spicelibrary/xfmr.cir spicelibrary/spdt.cir spicelibrary/spdt_xyce.cir \
            spicelibrary/coax.cir spicelibrary/core.cir spicelibrary/winding.cir; do
  [ -e "$res/$want" ] || { echo "error: share/qucs-s/$want is missing from the bundle" >&2; exit 1; }
done
# codesign refuses a symbolic link that points out of the bundle.
if find "$res" -type l | grep -q .; then
  echo "error: symbolic links under share/qucs-s:" >&2; find "$res" -type l >&2; exit 1
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
