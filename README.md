# Qucs-S Enhancements
**Owner: Dr. Meisam Bahadori**

[![CI](https://github.com/javaNoviceProgrammer/Qucs-S_Enhancements/actions/workflows/ci.yml/badge.svg)](https://github.com/javaNoviceProgrammer/Qucs-S_Enhancements/actions/workflows/ci.yml)
[![Release](https://github.com/javaNoviceProgrammer/Qucs-S_Enhancements/actions/workflows/release.yml/badge.svg)](https://github.com/javaNoviceProgrammer/Qucs-S_Enhancements/actions/workflows/release.yml)

A hardened and extended build of [Qucs-S](https://github.com/ra3xdh/qucs_s),
the Qt circuit simulator front-end for ngspice, Xyce and SPICE OPUS. It
starts from the upstream 26.1.1 source, fixes the crash classes that source
had, and adds workflow features of its own — with a headless test
suite, sanitizer builds and fuzzers behind every change. The work is done
with Claude Code, reviewed and driven by a human.

Ready-made bundles for macOS, Linux and Windows are on the
[Releases page](https://github.com/javaNoviceProgrammer/Qucs-S_Enhancements/releases).
This build calls itself **27.0.1** (`qucs-s-26.1.1/VERSION`; the source
directory keeps the name of the upstream version it started from). Documents
it saves say `<Qucs Schematic 27.0.1>`; upstream 26.1.1 asks before opening a
file from a newer version unless *Load documents from future versions* is on.

- [ARCHITECTURE.md](ARCHITECTURE.md) — how the upstream code base is put together
- [ENHANCEMENT_PROPOSAL.md](ENHANCEMENT_PROPOSAL.md) — crash root causes, the plan, and what has been done
- `qucs-s-26.1.1/` — the vendored upstream source (ra3xdh/qucs_s 26.1.1) with the changes applied

## Downloads

Every release carries one bundle per platform, built by the
[Release workflow](.github/workflows/release.yml) from a single Qt version:

| Platform | Bundle | Notes |
|---|---|---|
| macOS, Apple Silicon | `qucs-s-<ver>-macos-apple-silicon.dmg` | ad-hoc signed, not notarised: on first launch right-click the app and choose *Open*, or `xattr -d com.apple.quarantine <app>` |
| macOS, Intel | `qucs-s-<ver>-macos-intel.dmg` | same |

On macOS the application is called **Physics Room** in its menu, the
Dock and the Finder (its bundle's name); its program, `qucs-s.app`, and
its settings, caches and data keep the name qucs-s.
| Linux x86_64 | `qucs-s-<ver>-linux-intel.AppImage` | `chmod +x` and run; needs FUSE 2 (`libfuse2`) or `--appimage-extract` |
| Linux arm64 | `qucs-s-<ver>-linux-arm.AppImage` | same; needs glibc 2.38 or newer (Ubuntu 24.04, Debian 13) |
| Windows x64 | `...-windows-intel.zip` and `...-windows-intel-setup.exe` | |
| Windows arm64 | `...-windows-arm.zip` and `...-windows-arm-setup.exe` | |

No bundle includes ngspice: install it separately (`brew install ngspice`,
`apt install ngspice`, the Windows archive from
[ngspice.sourceforge.io](https://ngspice.sourceforge.io/download.html), …)
and point *Application Settings → Locations → Ngspice* at it if it is not
on `PATH` — on Windows at `ngspice_con.exe`. Without it the bundled
qucsator is the default simulator.

The rolling **`continuous`** pre-release is replaced on every manual run of
the workflow and follows `main` (its bundles carry the commit too:
`qucs-s-<ver>-<sha>-...`); tagged releases (`v*`) are permanent. A
`SHA256SUMS.txt` accompanies each release. Bundles live only on the Releases
page — nothing built is committed to this repository (`bin/` is git-ignored;
`scripts/fetch-binaries.sh` downloads a release into it).

## Building from source

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

### macOS (Homebrew)

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

This is the same script the Release workflow uses. The bundle also carries
Qt's offscreen platform plugin, so its binary works headless too — the smoke
suites run against it unchanged. If Homebrew upgrades Qt underneath an
existing build directory, rebuild from scratch: the new headers carry old
timestamps, and an incremental build mixes objects of two Qt versions.

### Linux

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

### Windows (MSYS2)

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

### Tests, smoke suites and fuzzers

The same checks CI runs, against an ASan/UBSan build (macOS shown; on Linux
drop `-DCMAKE_PREFIX_PATH` and use `build-asan/qucs/qucs-s` as the binary):

```bash
cmake -S qucs-s-26.1.1 -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
scripts/ci/smoke-test.sh hostile build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s qucs-s-26.1.1/examples /tmp/smoke
```

Unit tests live in `qucs-s-26.1.1/qucs/tests/` (QtTest, headless). They keep
their settings in a file under their temporary directory
(`tests/isolated_settings.h`), so a test run never touches your own Qucs-S
preferences; the application offers the same for trying a build out:
`QUCS_SETTINGS_DIR=<dir> qucs-s` keeps that run's settings in
`<dir>/qucs/qucs_s.ini`, and its crash reports and autosave copies under
`<dir>` as well — a trial run that is killed leaves nothing for your own
next start to report or offer. `QUCS_TRASH_DIR=<dir>` does the same for
the trash: what Qucs-S moves to the trash (the File Browser, Delete
Project, Claude's `trash_file`, `clean_scratch` and `import_data`'s
`remove`) goes into `<dir>` instead of the system's. The tests set it, so a
test run puts nothing in your trash. `QUCS_MANUAL_URL=<url>` is where
`read_help` fetches the manual from (a `file://` copy, as the tests give it),
instead of qucs-s-help.readthedocs.io.

`scripts/ci/smoke-test.sh` has three suites — `load` (render every ngspice
example, and one in every export format), `simulate` (netlist → ngspice → dataset → render; needs `ngspice` on
`PATH`) and `hostile` (render a fixture against damaged datasets). Everything
runs headless through the CLI modes of `qucs-s`; no window is opened.

`scripts/ci/fuzz-sch.py` mutates the example schematics (truncation, dropped
fields, unbalanced quotes, absurd numbers, missing terminators, random bytes)
and pushes every mutant through the netlister (`-n`) and the renderer (`-p`).
With `--extra <dir>` it also takes schematics that have a dataset next to
them — the `simulate` suite's work directory — and damages the dataset one
time in six. A mutant may be rejected, but must never crash, hang or trip a
sanitizer. Runs are seeded and reproducible; each finding is kept as a
directory with the mutant, its dataset and the log:

```bash
python3 scripts/ci/fuzz-sch.py build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s qucs-s-26.1.1/examples /tmp/fuzz --count 500 --seed 7 --extra /tmp/smoke/simulate/work
```

`scripts/ci/fuzz-simout.py` does the same to the simulator's output: the
`simulate` suite (with a Debug build) stashes the real ngspice files of every
circuit it ran under `<work>/<circuit>/simout/`, and the fuzzer damages one
of them per mutant and runs `qucs/tests/simout_harness`, which converts them
to a Qucs dataset exactly as the GUI does after a simulation:

```bash
python3 scripts/ci/fuzz-simout.py build-asan/qucs/tests/simout_harness /tmp/smoke/simulate/work /tmp/fuzz-simout --count 500 --seed 7
```

`qucs/tests/test_gui_monkey` does it to the main window: a seeded random
walk over copies of the ngspice examples — clicks, drags (some interrupted
by an undo or a delete), double clicks, the wheel and keys in every mouse
mode, the menu and toolbar commands, tabs, symbol view and hierarchy,
placing components, closing and reopening — where every dialog that comes
up is cancelled or filled with odd values and accepted. The wheel is a
mouse's or a touchpad's, sometimes tilted sideways, under either choice of
*Mouse wheel*. It asserts nothing but that the application survives and
that the wheel's turns reach the canvas: some of them must move the view.
Its turns were once sent where they reached nothing. A watchdog turns a
step that never ends into an abort with the stack, and the last steps are
printed with any failure. CTest runs a short walk; a stress run takes more steps and seeds,
and a seed replays the same walk on every platform:

```bash
QT_QPA_PLATFORM=offscreen QUCS_MONKEY_SEED=42 QUCS_MONKEY_STEPS=2000 build-asan/qucs/tests/test_gui_monkey
seq 1 100 | xargs -P 10 -I{} sh -c 'QT_QPA_PLATFORM=offscreen QUCS_MONKEY_SEED={} QUCS_MONKEY_STEPS=2000 build-asan/qucs/tests/test_gui_monkey > /tmp/monkey-{}.log 2>&1 || echo "seed {} failed"'
```

What a walk finds gets a test of its own in `qucs/tests/test_stress_findings`,
since the walk goes elsewhere after any change.

`qucs/tests/test_large_schematics` times loading, rotating, undo, deleting
and dragging on chains of 2,000 and 8,000 components and fails when
four times the elements take ten times as long: these were quadratic until
the node and wire lookups by place (`qucs/conductor_index.h`), and a
16,000-component schematic stopped for nine seconds after each edit.
`qucs/tests/test_canvas_drawing` paints the canvas of the examples (every
symbol of the symbol galleries) in parts and checks each looks as in a
paint of the whole, checks that a step of a gesture shows what a whole
paint would, also after an edit or a selection made from elsewhere, and
times a corner and a gesture's step against a whole paint of 16,000
components.

`qucs/tests/test_claude_code` drives the Claude Code dock's session and
the dock itself with a shell script that answers as `claude` does (stream
events, a permission request, a result), and the GUI monkey has one
(`qucs/tests/monkey_claude.sh`) that also dies now and then. No test
reaches a `claude` installed on the machine: `QUCS_CLAUDE`, which names
the program over the settings, points nowhere for them. The same script
answers the dock's question for the models on offer, and the test checks
the menu it builds and which models have auto mode. It also checks the
tools folding into a line and opening, the conversations in tabs (what
their tabs say, which one the status bar leads to, closing them), and
the math: found in Markdown but not in code or money, typeset, and
1,500 random scraps of TeX set without harm (the test runs under the
address sanitizer too), and the host's tool server over the stream: the
handshake, tools/list and tools/call answered, a tool that looks used
without asking, one that changes asked about, and none after *Allow
Qucs-S Control*. `qucs/tests/test_qucs_control` uses Claude's tools on
the application itself: a schematic built part by part (placed, wired
pin to pin, labelled, changed, turned, moved, deleted, each one step to
undo), read back as a summary and as text and replaced from text (and
left alone, without a message box, when the text does not read), a
picture of it, documents saved, closed and opened, menu actions used,
Document Settings opened by its action and filled in and answered from
within its own event loop, and a simulation waited for. The GUI
monkey's fake claude calls the tools too, some with arguments no one
would give (coordinates of two billion, junk .sch text, pins that do not
exist).

`qucs/tests/test_netlist_audit` places every built-in component on a
schematic and checks that it netlists in every flavour without a crash and
survives a save/load and a properties-dialog Apply unchanged. With
`QUCS_NETLIST_AUDIT=<dir>` it also writes the surveys (every component's
netlists, and every property marked to show which ones the SPICE netlist
ignores), and `scripts/netlist-audit.sh <build> <out>` runs the resulting
decks through ngspice. What that turned up is written up in
[docs/bug_hunts/](docs/bug_hunts/README.md).

## Crash reports and recovery

If the app dies, a report is written to the application-data directory
(`~/Library/Application Support/qucs-s/crash-reports/` on macOS,
`~/.local/share/qucs-s/crash-reports/` on Linux, `%LOCALAPPDATA%\qucs-s\crash-reports\`
on Windows) with the version, commit, signal, a backtrace and the last log
lines, and modified documents are autosaved. The next start says so and
offers to restore them. Modified documents are autosaved every two minutes
anyway (`AutosaveInterval` in the settings file, seconds; 0 disables).
Please attach the report when filing a crash.

## Continuous integration and releases

| Workflow | Trigger | What it does |
|---|---|---|
| [CI](.github/workflows/ci.yml) | every push / PR | Linux Debug build with ASan + UBSan, the unit tests, the `load`, `simulate` and `hostile` smoke suites, then 150 fuzzed schematics/datasets and 150 fuzzed simulator outputs; a macOS Release build with the unit tests; a Windows (MSYS2 ucrt64) Release build. Logs, renders and any fuzz findings are uploaded as a workflow artifact (kept 14 days). |
| [Release](.github/workflows/release.yml) | manual (*Actions → Release → Run workflow*) or a `v*` tag | Builds the bundles of the table above on GitHub's runners and publishes them, with checksums, as a GitHub Release. |

The `hostile` suite is the regression guard for the dataset-loader crashes
fixed in WS1.1; it is blocking.

Nothing either workflow builds goes into git: CI keeps logs as short-lived
artifacts, and the Release workflow uploads the bundles as release assets
only. A manual run publishes to the rolling `continuous` pre-release
(replaced each time); a tag push (`git tag -a v27.0.1 && git push origin v27.0.1`)
publishes a permanent release named after the tag, with the annotated tag's
message as its notes and the generated change list below it. All platforms, the ARM
ones included, are built by default; deselect any in the dispatch form, or
set the repository variable `BUILD_ARM=false` to leave ARM out of tag builds.

| Platform | Runner |
|---|---|
| macOS Apple Silicon | `macos-15` |
| macOS Intel | `macos-15-intel`, newest Xcode on the image (its default Apple clang 17 crashes on one source file for x86_64) |
| Linux x86_64 | `ubuntu-22.04` (AppImage runs on anything as new as that) |
| Linux arm64 | `ubuntu-24.04-arm` (Qt's arm64 binaries need glibc 2.38, so the AppImage does too) |
| Windows x64 | `windows-2022`, MSYS2 ucrt64 |
| Windows arm64 | `windows-11-arm`, MSYS2 clangarm64 |

To pull a release's bundles into the git-ignored `bin/<os>/<arch>/`
directories (checksums verified; needs the GitHub CLI):

```bash
scripts/fetch-binaries.sh              # latest "continuous" pre-release
scripts/fetch-binaries.sh v27.0.1      # a tagged release
scripts/fetch-binaries.sh --run 12345  # artifacts of one workflow run
```

## Reporting problems

Open an issue with the platform, the bundle name (or commit), what you did,
and — for a crash — the report from the directory above. Problems that exist
in upstream Qucs-S as well are best reported [there](https://github.com/ra3xdh/qucs_s/issues)
too; this repository tracks upstream and sends fixes back where they apply.

## License

Qucs-S is © its authors and released under the GNU General Public License,
version 2 or (at your option) any later version — see
[`qucs-s-26.1.1/COPYING`](qucs-s-26.1.1/COPYING). The changes in this
repository are released under the GNU General Public License, version 3
([`LICENSE`](LICENSE)), as the upstream "or any later version" terms allow;
the combined work is therefore distributed under GPL-3.0.

## Trademarks

Ansys, Lumerical, Zemax and OpticStudio, and any and all ANSYS, Inc. brand,
product, service and feature names, are registered trademarks or trademarks
of ANSYS, Inc. or its subsidiaries in the United States or other countries.
This product is not affiliated with, sponsored or endorsed by ANSYS, Inc. or
Synopsys, Inc.: their names appear here only to say which files it reads and
writes and which published results it is compared with. All other brand,
product, service and feature names or trademarks are the property of their
respective owners.
