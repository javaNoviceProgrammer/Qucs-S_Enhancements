# Continuous integration and releases

| Workflow | Trigger | What it does |
|---|---|---|
| [CI](.github/workflows/ci.yml) | every push / PR | Linux Debug build with ASan + UBSan, the unit tests, the `load`, `simulate` and `hostile` smoke suites, then 150 fuzzed schematics/datasets and 150 fuzzed simulator outputs; a macOS Release build with the unit tests; a Windows (MSYS2 ucrt64) Release build. Logs, renders and any fuzz findings are uploaded as a workflow artifact (kept 14 days). |
| [Release](.github/workflows/release.yml) | manual (*Actions → Release → Run workflow*) or a `v*` tag | Builds the bundles of the six platforms - Linux, Windows and macOS, each on Intel and on ARM - on GitHub's runners and publishes them, with checksums, as a GitHub Release. |

The `hostile` suite is the regression guard for the dataset-loader crashes
fixed in WS1.1; it is blocking.

Nothing either workflow builds goes into git: CI keeps logs as short-lived
artifacts, and the Release workflow uploads the bundles as release assets
only. A manual run publishes to the rolling `continuous` pre-release
(replaced each time); a tag push (`git tag -a v27.1.6 && git push origin v27.1.6`)
publishes a permanent release named after the tag. Releases carry no text.
Building a tag that already has a release swaps that release's files in
place and never deletes it; only the highest version tag takes *Latest*
([publish-release.sh](.github/scripts/publish-release.sh) has the details).
Since the run uses the workflow of the commit it builds, this holds only for
tags on commits made after this change. All platforms, the ARM
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
scripts/fetch-binaries.sh v27.1.6      # a tagged release
scripts/fetch-binaries.sh --run 12345  # artifacts of one workflow run
```
