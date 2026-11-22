# Tests, smoke suites and fuzzers

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

`qucs/tests/test_claude_jobs` does the same for the Background Jobs window
(⋯ > Show Background Jobs): each status from the program's events, a real
percentage against an unknown one, Stop and Clear Finished. Only its opt-in
`QUCS_JOBS_REAL=1` reaches the `claude` on the machine; a test run leaves it
alone.

`qucs/tests/test_plot_digitizer` holds the PDF viewer's plot digitizer to the
truth: PDFs written from known functions (vector art, pictures, a turned scan,
log and polar axes, dashes, markers) and the numbers read back, with a fuzz of
the PDF reader and of the file kept beside a PDF. `test_plot_digitizer_app`
drives its panel and Claude's `find_plots`, `digitize_plot` and `get_digitized`,
over stdio too.

`qucs/tests/test_netlist_audit` places every built-in component on a
schematic and checks that it netlists in every flavour without a crash and
survives a save/load and a properties-dialog Apply unchanged. With
`QUCS_NETLIST_AUDIT=<dir>` it also writes the surveys (every component's
netlists, and every property marked to show which ones the SPICE netlist
ignores), and `scripts/netlist-audit.sh <build> <out>` runs the resulting
decks through ngspice.
