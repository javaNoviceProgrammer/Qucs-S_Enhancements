# Qucs-S_Enhancements

Using Claude Code to enhance the Qucs-S circuit simulator interface.

- [ARCHITECTURE.md](ARCHITECTURE.md) — how the upstream code base is put together
- [ENHANCEMENT_PROPOSAL.md](ENHANCEMENT_PROPOSAL.md) — crash root causes and the plan
- `qucs-s-26.1.1/` — vendored upstream source (ra3xdh/qucs_s 26.1.1) with our changes

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
`build*/` is git-ignored, as are the bundles under `bin/`.

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

## Continuous integration

| Workflow | Trigger | What it does |
|---|---|---|
| [CI](.github/workflows/ci.yml) | every push / PR | Linux Debug build with ASan + UBSan, the unit tests, then the `load`, `simulate` and `hostile` smoke suites. Logs and renders are uploaded as an artifact. |
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
