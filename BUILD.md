# Building from source

Build dependencies on every platform: a C++20 compiler (GCC 12, Clang 15 or newer), CMake ≥ 3.16, Ninja,
Qt 6 with the Svg, Xml, PrintSupport, Charts and Tools (Linguist) modules,
flex, bison ≥ 3, gperf and dos2unix. ngspice is a run-time dependency only.
The source tree is `qucs-s-26.1.1/`; every recipe below configures it into a
sibling build directory (`build*/` is git-ignored). The commit a build is of
is read from git at every build (`cmake/commit.cmake`, into `qucs_commit.h`)
and shown in *Help → About* ("Version 26.1.7 (a4e23cf)"), `--version` and a
crash report; `-DGIT` names it where the sources are in no repository (a
tarball). Read once when the tree was configured, it named that commit long
after.

## macOS (Homebrew)

```bash
brew install cmake ninja qt bison flex gperf dos2unix ngspice
```

Homebrew's bison must shadow the one in Xcode:

```bash
export PATH="$(brew --prefix bison)/bin:$PATH"
cmake -S qucs-s-26.1.1 -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" -DGIT="$(git rev-parse --short HEAD)"
cmake --build build --parallel
```

The app is then at `build/qucs/qucs-s.app`, but it still points at the
Homebrew Qt and has no libraries, examples or tool apps inside. To get a
self-contained bundle you can run anywhere (and a `.dmg`):

```bash
scripts/package-macos.sh build bin/macos/apple-silicon
```

The release DMGs are made with this script too, locally: the Release
workflow builds no macOS bundle. The bundle also carries
Qt's offscreen platform plugin, so its binary works headless too — the smoke
suites run against it unchanged. If Homebrew upgrades Qt underneath an
existing build directory, rebuild from scratch: the new headers carry old
timestamps, and an incremental build mixes objects of two Qt versions.

## macOS, with Qt's own build

Homebrew builds Qt, and every library it brings, for the macOS it runs on,
so a bundle made with Homebrew's Qt needs that macOS or newer. A bundle for
older macOS takes Qt's own build instead — from the Qt installer, or
aqtinstall — and names the oldest macOS it runs on. The release DMGs are
built this way:

```bash
python3 -m venv ~/aqt && ~/aqt/bin/pip install aqtinstall
~/aqt/bin/aqt install-qt mac desktop 6.10.3 clang_64 -m qtcharts qtpdf -O ~/Qt
export PATH="$HOME/Qt/6.10.3/macos/bin:$(brew --prefix bison)/bin:$PATH"
cmake -S qucs-s-26.1.1 -B build-qt -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.3/macos" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DGIT="$(git rev-parse --short HEAD)"
cmake --build build-qt --parallel
scripts/package-macos.sh build-qt bin/macos/apple-silicon
```

Qt 6.10 runs on macOS 13 and newer, Qt 6.12 on 14.4 and newer. With that
Qt's `bin` first on `PATH`, the package script takes its `macdeployqt`, not
Homebrew's. The configure step sees to what Qt's own builds need on current
Xcode, and says so as it goes (`cmake/qt_macos.cmake`): Qt 6.10's
`qyieldcpu.h` with the macOS 27 SDK, Qt 6.12's forwarding headers, and Qt
6.12 in a folder whose path has a space.

## Linux

Debian / Ubuntu (22.04 or newer):

```bash
sudo apt-get install build-essential git cmake ninja-build flex bison gperf dos2unix ngspice \
    qt6-base-dev qt6-tools-dev qt6-tools-dev-tools qt6-l10n-tools linguist-qt6 \
    libqt6svg6-dev qt6-charts-dev libqt6opengl6-dev libglx-dev libgl1-mesa-dev libcups2-dev zlib1g-dev
```

Fedora:

```bash
sudo dnf install gcc-c++ git cmake ninja-build flex bison gperf dos2unix ngspice \
    qt6-qtbase-devel qt6-qtsvg-devel qt6-qttools-devel qt6-qtcharts-devel zlib-devel
```

Arch:

```bash
sudo pacman -S base-devel git cmake ninja flex bison gperf dos2unix ngspice qt6-base qt6-svg qt6-charts qt6-tools
```

Then:

```bash
cmake -S qucs-s-26.1.1 -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGIT="$(git rev-parse --short HEAD)"
cmake --build build --parallel
build/qucs/qucs-s
```

To install (`/usr/local` by default; the tool apps, examples and library go
with it):

```bash
sudo cmake --build build --target install
```

If your distribution's Qt is older than 6.5, or you want exactly what the
releases use, fetch Qt 6.10.3 with [aqtinstall](https://github.com/miurahr/aqtinstall)
and point CMake at it:

```bash
pip install aqtinstall
aqt install-qt linux desktop 6.10.3 linux_gcc_64 -m qtcharts -O "$HOME/Qt"
cmake -S qucs-s-26.1.1 -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.3/gcc_64"
```

The AppImage the Release workflow publishes is made from an install into
`AppDir/usr` with [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy)
and its Qt plugin; the exact steps are in the `linux` job of
[release.yml](.github/workflows/release.yml).

## Windows (MSYS2)

The releases are built with [MSYS2](https://www.msys2.org/) in the UCRT64
environment (CLANGARM64 on arm64), which is also the simplest way to build
locally. In a *MSYS2 UCRT64* shell:

```bash
pacman -S --needed git bison flex dos2unix zip \
    mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-gperf \
    mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-tools mingw-w64-ucrt-x86_64-qt6-svg mingw-w64-ucrt-x86_64-qt6-charts \
    mingw-w64-ucrt-x86_64-zlib
```

Clone with LF line endings (`git config --global core.autocrlf false` before
cloning — the flex/bison sources and the smoke scripts need it), then:

```bash
cmake -S qucs-s-26.1.1 -B qucs-s-26.1.1/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PWD/qucs-s-26.1.1/build/qucs-suite" -DGIT="$(git rev-parse --short HEAD)"
cmake --build qucs-s-26.1.1/build --parallel
cmake --build qucs-s-26.1.1/build --target install
```

`qucs-suite/bin/qucs-s.exe` runs from the MSYS2 shell as is. To make the
`bin/` directory self-contained (runnable from Explorer, or to zip it), copy
the Qt and runtime DLLs next to the executables:

```bash
cd qucs-s-26.1.1/build/qucs-suite/bin
for exe in qucs-s qucs-sactivefilter qucs-sattenuator qucs-sfilter qucs-spowercombining qucs-strans qucs-sspar-viewer rxcalc; do
  windeployqt.exe "$exe.exe" --svg --no-translations --no-system-d3d-compiler --no-network
done
for f in $(ldd qucs-s.exe | awk '($3 ~ /\/ucrt64\/bin\//) {print $3}'); do case "$(basename "$f")" in Qt6*) ;; *) cp -f "$f" .;; esac; done
```

Install [ngspice for Windows](https://ngspice.sourceforge.io/download.html)
and set its path under *Application Settings → Locations*. The installer
(`-setup.exe`) is produced with [Inno Setup](https://jrsoftware.org/isinfo.php)
from `qucs-s-26.1.1/contrib/InnoSetup/qucs.iss`; the `windows` job of
[release.yml](.github/workflows/release.yml) has the exact commands,
including where it puts a bundled ngspice.
