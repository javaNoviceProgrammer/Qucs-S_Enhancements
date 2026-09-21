# Qucs-S_Enhancements

Using Claude Code to enhance the Qucs-S circuit simulator interface.

- [ARCHITECTURE.md](ARCHITECTURE.md) — how the upstream code base is put together
- [ENHANCEMENT_PROPOSAL.md](ENHANCEMENT_PROPOSAL.md) — crash root causes and the plan
- `qucs-s-26.1.1/` — vendored upstream source (ra3xdh/qucs_s 26.1.1) with our changes

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
- **Drag and drop from the Content panel**: drag one or more files onto the
  document area (a schematic, a text document, the tab bar) to open them —
  schematics, data displays and symbols in their views, Verilog-A and other
  text files in the text editor, anything else the way a double-click would.
  Files dragged in from a file manager open the same way.

## Building locally

Dependencies: CMake, Ninja, Qt 6 (with QtCharts), flex, bison ≥ 3, gperf,
dos2unix. On macOS with Homebrew:

```bash
brew install cmake ninja qt bison flex gperf dos2unix
```

Configure and build (Homebrew's bison must shadow the Xcode one):

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

This is the same script the Release workflow uses. Any directory matching
`build*/` is git-ignored, as are the bundles under `bin/`. The bundle also
carries Qt's offscreen platform plugin, so its binary works headless too -
the smoke suites run against it unchanged:

```bash
scripts/ci/smoke-test.sh simulate bin/macos/apple-silicon/qucs-s-*.app/Contents/MacOS/qucs-s qucs-s-26.1.1/examples /tmp/smoke
```

### Sanitizer build and smoke tests

The same tests CI runs, against an ASan/UBSan build:

```bash
cmake -S qucs-s-26.1.1 -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan --parallel --target qucs-s
scripts/ci/smoke-test.sh hostile build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s qucs-s-26.1.1/examples /tmp/smoke
```

Unit tests live in `qucs-s-26.1.1/qucs/tests/` (QtTest, headless) and run with
`ctest --test-dir build-asan --output-on-failure`.

`scripts/ci/smoke-test.sh` has three suites — `load` (render every ngspice
example), `simulate` (netlist → ngspice → dataset → render; needs `ngspice` on
`PATH`) and `hostile` (render a fixture against damaged datasets). Everything
runs headless through the CLI modes of `qucs-s`; no window is opened.

`scripts/ci/fuzz-sch.py` mutates the example schematics (truncation, dropped
fields, unbalanced quotes, absurd numbers, missing terminators, random bytes)
and pushes every mutant through the netlister (`-n`) and the renderer (`-p`).
With `--extra <dir>` it also takes schematics that have a dataset next to
them - the `simulate` suite's work directory - and damages the dataset one
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
`~/.local/share/qucs-s/crash-reports/` on Linux) with the version, commit,
signal, a backtrace and the last log lines, and modified documents are
autosaved. The next start says so and offers to restore them. Modified
documents are autosaved every two minutes anyway (`AutosaveInterval` in the
settings file, seconds; 0 disables). Please attach the report when filing a
crash.

## Continuous integration

| Workflow | Trigger | What it does |
|---|---|---|
| [CI](.github/workflows/ci.yml) | every push / PR | Linux Debug build with ASan + UBSan, the unit tests, the `load`, `simulate` and `hostile` smoke suites, then 150 fuzzed schematics/datasets and 150 fuzzed simulator outputs. Logs, renders and any fuzz findings are uploaded as an artifact. |
| [Release](.github/workflows/release.yml) | manual (*Actions → Release → Run workflow*) or a `v*` tag | Release bundles per platform, published as a GitHub Release. |

The `hostile` suite is the regression guard for the dataset-loader crashes
fixed in WS1.1; it is blocking.

### Release bundles

| Platform | Runner | Artifact |
|---|---|---|
| macOS Apple Silicon | `macos-15` | `qucs-s-<ver>-<sha>-macos-apple-silicon.dmg` |
| macOS Intel | `macos-15-intel` | `qucs-s-<ver>-<sha>-macos-intel.dmg` |
| Linux x86_64 | `ubuntu-22.04` | `qucs-s-<ver>-<sha>-linux-intel.AppImage` |
| Linux arm64 | `ubuntu-22.04-arm` | `qucs-s-<ver>-<sha>-linux-arm.AppImage` |
| Windows x64 | `windows-2022` (MSYS2 ucrt64) | `...-windows-intel.zip` and `...-windows-intel-setup.exe` |
| Windows arm64 | `windows-11-arm` (MSYS2 clangarm64) | `...-windows-arm.zip` and `...-windows-arm-setup.exe` |

All bundles use Qt 6.10.3. A manual run publishes to the rolling `continuous`
pre-release (replaced each time); a tag push (`git tag v26.1.1-1 && git push
--tags`) publishes a permanent release named after the tag.

**Cost:** this repository is private, so Actions minutes are metered — macOS
counts 10×, Windows 2×. A full four-platform run is roughly 600–800 billed
minutes. The ARM runners are only free for public repositories, so the ARM
builds are off by default; enable them per run via the dispatch checkboxes or
for tag builds by setting the repository variable `BUILD_ARM=true`.

### Getting the bundles into `bin/`

```bash
scripts/fetch-binaries.sh              # latest "continuous" pre-release
scripts/fetch-binaries.sh v26.1.1-1    # a tagged release
scripts/fetch-binaries.sh --run 12345  # artifacts of one workflow run
```

Files land in `bin/<os>/<arch>/` and checksums are verified. The bundles are
git-ignored; only the directory skeleton is tracked.
