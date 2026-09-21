# Qucs-S Enhancements

[![CI](https://github.com/javaNoviceProgrammer/Qucs-S_Enhancements/actions/workflows/ci.yml/badge.svg)](https://github.com/javaNoviceProgrammer/Qucs-S_Enhancements/actions/workflows/ci.yml)
[![Release](https://github.com/javaNoviceProgrammer/Qucs-S_Enhancements/actions/workflows/release.yml/badge.svg)](https://github.com/javaNoviceProgrammer/Qucs-S_Enhancements/actions/workflows/release.yml)

A hardened and extended build of [Qucs-S](https://github.com/ra3xdh/qucs_s),
the Qt circuit simulator front-end for ngspice, Xyce and SPICE OPUS. It
starts from the upstream 26.1.1 source, fixes the crash classes that source
had, and adds the workflow features listed below — with a headless test
suite, sanitizer builds and fuzzers behind every change. The work is done
with Claude Code, reviewed and driven by a human.

Ready-made bundles for macOS, Linux and Windows are on the
[Releases page](https://github.com/javaNoviceProgrammer/Qucs-S_Enhancements/releases).

- [ARCHITECTURE.md](ARCHITECTURE.md) — how the upstream code base is put together
- [ENHANCEMENT_PROPOSAL.md](ENHANCEMENT_PROPOSAL.md) — crash root causes, the plan, and what has been done
- `qucs-s-26.1.1/` — the vendored upstream source (ra3xdh/qucs_s 26.1.1) with the changes applied

## Downloads

Every release carries one bundle per platform, built by the
[Release workflow](.github/workflows/release.yml) from a single Qt version:

| Platform | Bundle | Notes |
|---|---|---|
| macOS, Apple Silicon | `qucs-s-<ver>-<sha>-macos-apple-silicon.dmg` | ad-hoc signed, not notarised: on first launch right-click the app and choose *Open*, or `xattr -d com.apple.quarantine <app>` |
| macOS, Intel | `qucs-s-<ver>-<sha>-macos-intel.dmg` | same |
| Linux x86_64 | `qucs-s-<ver>-<sha>-linux-intel.AppImage` | `chmod +x` and run; needs FUSE 2 (`libfuse2`) or `--appimage-extract` |
| Linux arm64 | `qucs-s-<ver>-<sha>-linux-arm.AppImage` | same |
| Windows x64 | `...-windows-intel.zip` and `...-windows-intel-setup.exe` | ngspice is included |
| Windows arm64 | `...-windows-arm.zip` and `...-windows-arm-setup.exe` | ngspice is included |

The macOS and Linux bundles do not include ngspice, as upstream's do not:
install it separately (`brew install ngspice`, `apt install ngspice`, …) and
point *Application Settings → Locations* at it if it is not on `PATH`.

The rolling **`continuous`** pre-release is replaced on every manual run of
the workflow and follows `main`; tagged releases (`v*`) are permanent. A
`SHA256SUMS.txt` accompanies each release. Bundles live only on the Releases
page — nothing built is committed to this repository (`bin/` is git-ignored;
`scripts/fetch-binaries.sh` downloads a release into it).

## What is different from upstream

- **Crash hardening** (WS1 of the proposal): the dataset and schematic
  loaders and the simulator-output parsers survive damaged input, the app
  keeps autosave copies and writes a crash report with a backtrace, and the
  next start offers to restore what was open.
- **Verilog-A "Build All"**: right-click the *Verilog-A* row in the
  Content panel to compile every `.va` file of the project with OpenVAF (the
  executable set under *Application Settings → Locations → OpenVAF Path*),
  one after the other, with the compiler output in the message dock and a
  pass/fail tally at the end. Files with unsaved changes are compiled as last
  saved, after a confirmation.
- **Content panel sees the whole project**: files in subdirectories of the
  project (at any depth, hidden directories excluded) are listed under
  their category, either as `sub/dir/name.ext` rows or as sub-trees with
  one folder row per directory — right-click the empty area of the panel,
  *Toggle hierarchy search view*, to switch; the choice is remembered.
  Open, copy, rename, delete, drag and subcircuit insertion work on them in
  both listings. A new *Osdi* category lists the compiled `.osdi` models;
  ngspice loads all of them, wherever they are in the project, and Build
  All compiles the `.va` files wherever they are.
- **A Scratch folder per project**: the temporary files of a simulation
  (netlist, the raw simulator output such as `spice4qucs.ac1.plot`, log) go
  to `Scratch/` inside the open project instead of the user's cache
  directory, and stay there until the next run (upstream's release builds
  deleted the raw output right after converting it). The folder is created
  with the project (or when an older project is opened) and the Content
  panel lists its contents under a *Scratch* category below *Others*.
  Headless runs (`-n`, `--run`) keep using the simulator work directory
  from the settings.
- **Text editor defaults and file types**: `.cir`, `.ckt` and `.sp` files
  open in the built-in text editor like `.va` and the other Qucs text
  documents; plain-text formats (`.txt`, `.py`, `.md`, `.json`, `.csv`,
  `.sh`, C/C++ sources, SPICE `.lib/.inc/.mod`, ngspice `.plot` files, the
  per-simulator datasets, ... — see `QucsApp::textFileSuffixes()`) open in
  the text editor from the settings, the built-in one by default. Under
  *Application Settings → File Types* a suffix can be registered with the
  program `qucs-editor` (there is a button for it) to open it in the
  built-in editor; a suffix registered with any program takes precedence
  over the defaults, and a program path may contain slashes (upstream cut
  it at the first one).
- **Embedded simulation console**: a simulation runs in a *Simulation*
  dock at the bottom of the window (tabbed with the build messages) instead
  of the modal "Simulate with external simulator" dialog, so the schematic
  stays usable while ngspice/Xyce work. The dock has the simulator's
  output, a status list, a progress bar, *Stop*, *Save netlist* and
  *Clear*; it comes up with the first simulation and can be shown or hidden
  from *View → Simulation Console*. A second Simulate while one is running
  is refused (the console says so); closing the schematic being simulated
  stops its run (upstream #235). *Simulation → Simulators Settings →
  Simulation console* offers two alternatives: the same console in a
  separate window that does not block the application either, or the
  legacy window — the dialog of earlier versions, which opens with every
  simulation, blocks until closed and stops a simulation still running
  when it is closed. The simulation toolbar (simulator choice, Simulate,
  Tune, …) starts a second row of toolbars.
- **Terminal and Python Shell docks** (*View → Terminal*, *View → Python
  Shell*): a shell — `$SHELL` as a login shell on macOS/Linux, PowerShell
  on Windows — and a Python interpreter, each in a dock next to the
  simulation console, started when the dock is first shown. On Unix the
  program runs on a pseudo-terminal, so prompts, echo and Ctrl-C work as
  in a terminal window; the output is shown as plain text (escape
  sequences dropped, carriage returns and backspaces applied), so
  full-screen programs are not for it. A command line below the output
  with a history (Up/Down), *Interrupt*, *Restart*, *Clear* and *Project
  dir* (a `cd` / `os.chdir` to the open project). The interpreter is the
  one under *Application Settings → Locations → Python Path*, or `python3`
  on `PATH` when that is empty.
- **The environment of your shell, even when started from the Finder or
  the Dock**: a desktop start gets a bare environment (`PATH` without
  Homebrew or `~/bin`, none of your exports), so simulators were not
  found and variables like `SPICE_LIB_DIR` were missing. At start Qucs-S
  now asks your login shell (`$SHELL -l -i`, so both the profile and the
  rc file count; fish and zsh-that-starts-fish included) for its
  environment and brings in what the process lacks — `PATH` is merged in
  the shell's order, with anything given on the command line kept in
  front; what the process already has is never replaced. It reports what
  it added on stderr. `QUCS_NO_SHELL_ENV=1` turns it off. Windows starts
  already carry the user's environment.
- **Content panel keeps itself current**: every few seconds it looks at
  the project's files and, only when one came, went or changed — saved by
  Qucs, written by a script in the Terminal dock, copied in by hand —
  lists them again (never while a simulation is writing its scratch
  files, under an open menu, or during a drag). *Application Settings →
  Settings* has the interval (default 3 s) and a checkbox to turn it off;
  *Refresh* on the right-click menu of the panel's empty area does it on
  the spot.
- **Editor panes** (*View → Panes*): documents side by side, up to a 2×2
  grid — a schematic next to its netlist, two schematics to compare.
  *Split Right* (Ctrl+\) and *Split Down* (Ctrl+Shift+\) open a new,
  empty pane; a pane's thin coloured top edge marks the active one, which
  is where files open (from the Content panel, the menus, or a drop) and
  which follows clicks and keyboard focus. *Move Document to Next Pane*
  (also on a tab's context menu; with a single pane it splits first),
  *Next Pane* (Ctrl+`) and *Close Pane* (its documents go to a
  neighbour); a pane whose last document is closed goes by itself. Save
  All, Close All and Find span every pane.
- **Diagram legend**: every graph diagram (Cartesian, polar, Smith, 3D, …)
  can show a legend — a sample of each graph's line (colour, thickness,
  style or symbol) with its variable — in a corner of its choice:
  *Edit Diagram Properties → Properties → Legend*. Off by default; the
  position is saved with the diagram, and files without it load as before
  (upstream #1719).
- **Cursor-key moves are undoable and cancellable**: moving the selection
  with the arrow keys marks the document modified, is one undo step for the
  whole sequence, and Escape takes it back while it is the latest change
  (upstream #1525; upstream recorded neither).
- **Drag and drop from the Content panel**: drag one or more files onto the
  document area (a schematic, a text document, the tab bar) to open them —
  schematics, data displays and symbols in their views, Verilog-A and other
  text files in the text editor, anything else the way a double-click would.
  Files dragged in from a file manager open the same way.

The detailed record — root causes, what each change does and how it is
tested — is in [ENHANCEMENT_PROPOSAL.md](ENHANCEMENT_PROPOSAL.md).

## Building from source

Build dependencies on every platform: a C++20 compiler (GCC 12, Clang 15 or newer), CMake ≥ 3.16, Ninja,
Qt 6 with the Svg, Xml, PrintSupport, Charts and Tools (Linguist) modules,
flex, bison ≥ 3, gperf and dos2unix. ngspice is a run-time dependency only.
The source tree is `qucs-s-26.1.1/`; every recipe below configures it into a
sibling build directory (`build*/` is git-ignored) and stamps the commit into
the About dialog with `-DGIT`.

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
    libqt6svg6-dev qt6-charts-dev libqt6opengl6-dev libglx-dev libgl1-mesa-dev libcups2-dev
```

Fedora:

```bash
sudo dnf install gcc-c++ git cmake ninja-build flex bison gperf dos2unix ngspice \
    qt6-qtbase-devel qt6-qtsvg-devel qt6-qttools-devel qt6-qtcharts-devel
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
    mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-tools mingw-w64-ucrt-x86_64-qt6-svg mingw-w64-ucrt-x86_64-qt6-charts
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
`<dir>/qucs/qucs_s.ini`.

`scripts/ci/smoke-test.sh` has three suites — `load` (render every ngspice
example), `simulate` (netlist → ngspice → dataset → render; needs `ngspice` on
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
| [CI](.github/workflows/ci.yml) | every push / PR | Linux Debug build with ASan + UBSan, the unit tests, the `load`, `simulate` and `hostile` smoke suites, then 150 fuzzed schematics/datasets and 150 fuzzed simulator outputs. Logs, renders and any fuzz findings are uploaded as a workflow artifact (kept 14 days). |
| [Release](.github/workflows/release.yml) | manual (*Actions → Release → Run workflow*) or a `v*` tag | Builds the bundles of the table above on GitHub's runners and publishes them, with checksums, as a GitHub Release. |

The `hostile` suite is the regression guard for the dataset-loader crashes
fixed in WS1.1; it is blocking.

Nothing either workflow builds goes into git: CI keeps logs as short-lived
artifacts, and the Release workflow uploads the bundles as release assets
only. A manual run publishes to the rolling `continuous` pre-release
(replaced each time); a tag push (`git tag v26.1.1-1 && git push --tags`)
publishes a permanent release named after the tag. All platforms, the ARM
ones included, are built by default; deselect any in the dispatch form, or
set the repository variable `BUILD_ARM=false` to leave ARM out of tag builds.

| Platform | Runner |
|---|---|
| macOS Apple Silicon | `macos-15` |
| macOS Intel | `macos-15-intel` |
| Linux x86_64 | `ubuntu-22.04` (AppImage runs on anything as new as that) |
| Linux arm64 | `ubuntu-22.04-arm` |
| Windows x64 | `windows-2022`, MSYS2 ucrt64 |
| Windows arm64 | `windows-11-arm`, MSYS2 clangarm64 |

To pull a release's bundles into the git-ignored `bin/<os>/<arch>/`
directories (checksums verified; needs the GitHub CLI):

```bash
scripts/fetch-binaries.sh              # latest "continuous" pre-release
scripts/fetch-binaries.sh v26.1.1-1    # a tagged release
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
