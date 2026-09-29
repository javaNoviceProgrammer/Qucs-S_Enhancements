# Feature gaps: Qucs-S's MCP tools, end-to-end scenarios

*29 September 2026 — Qucs-S 26.1.4.*

After the fourth round (`2026-09-28-claude-mcp-api-feedback-round4.md`), a
script of end-to-end scenarios (`qucs-mcp-e2e-scenarios.py`) drove
`qucs-s --mcp-server --workspace /tmp/e2e/ws`. It chains tools as an agent
does and checks the final netlist and measured numbers, not the answers
along the way. There are seven scenarios, with 49 checks:

1. Import a netlist, arrange it, simulate, measure the bandwidth, tune it, export.
2. Build a divider, make a subcircuit of it, draw its symbol, simulate again, replace a part.
3. A parameter sweep.
4. An RLC step: its measurements, a marker, a data display, a picture.
5. A copy's life: renamed net, a new run, closed and reopened.
6. Recovery from failures.
7. The user's `project1` (13 schematics), each opened, checked, netlisted, arranged in preview and simulated.

The first run, at `474a0f5` from the build tree, had 3 checks fail and 2
schematics fail to simulate.

## What the scenarios found

| Scenario | Now |
|---|---|
| **s2: after `create_subcircuit` of the divider's R1 and R2, v(mid) read 5 V, not 2.5 V.** The subcircuit netlisted `R2 0 _net0`. | A bug. R1.2 and R2.2 share `mid`, which goes on to C1 outside. The wire between them therefore touched a part outside and stayed in the parent. Inside, only the first pin found on the net got the port's label, so R2.2 was on nothing, and `check_schematic` on the subcircuit said so. Every pin of the group on a port's net now gets the port's label inside, one label to a place. The subcircuit netlists `R2 0 mid`. `aSubcircuitKeepsPinsWhoseWireGoesOn`. |
| **s1: `tune` took 9 runs to set C1 for a 1 kHz bandwidth.** | A bandwidth goes as 1/C. Neither the value's scale nor its logarithm makes that a straight line, but the logarithms of both do. The secant is now drawn on one of four pairs of scales: the value or its logarithm, and the measurement or its logarithm. At first the logarithms are used when the range spans two decades or more, and, for the measurement, when both ends measured more than 0 and a decade apart. After each run, the pair that predicted it best is used. C1 is found in 3 runs. Worked out on thirteen responses beforehand: bandwidth vs C 9 → 3, an RC's corner vs R 9 → 4, √x 9 → 3, x² 8 → 3. A divider's output over three decades (7 → 8) and tanh (6 → 8) lose a run or two. `theFourthRoundsTuneAndSimulate` adds the supply voltage for 1 mW in a resistor (a square): at most 4 runs. |
| **s7: `lm386_amp.sch` could not be netlisted** ("Cannot load library component LM386"), and `check_schematic` gave only ten warnings about wire ends connected to nothing. | Where it was run: the build tree's app has no component libraries, which the packaging copies in. The packaged app netlists and simulates it. `check_schematic` now reports why, as an error: a library part that could not be loaded ("U1: the library part LM386 of AudioIC could not be loaded ... it has no pins, and what was wired to them is on nothing"), and a subcircuit whose file is not found. Across the 252 examples it flags six, each of which really fails to netlist: three tests of a MESFETs library that is not shipped, a subcircuit file missing (`lspice2g6.sch`), and `example_probe_and_subcircuit.sch`, whose subcircuits are named without `.sch`. `aPartThatDidNotLoadIsSaid`. |
| **s7: `feedback_loop.sch` and `hs_driver_test.sch` failed to simulate** (`ctl_sum`, `hs_driver`: no definition). | The project keeps its Verilog-A modules in `va/`, which Qucs-S loads with `pre_osdi`. The script copies only the project's top-level files. With `va/` copied, both simulate. |

## Run again

Run again with the packaged app, the project copied with `va/`, and the
ngspice the user's settings name (on `PATH`): 49 checks, none failed. 12 of
the 13 project schematics simulate; `diffpair.sch` has no analysis. `tune`
finds C1 in 3 runs.

About the script: it opens `/tmp/e2e/server.err` before anything creates
`/tmp/e2e`, so a first run on a clean machine stops there. Its server
inherits the user's `HOME`, so a project it opens keeps its scratch files
in `~/Library/Caches/qucs-s`. With fresh settings, the server runs the
first `ngspice` on `PATH`, which may not be the user's.

## Tests

- **`test_qucs_control`:** `aSubcircuitKeepsPinsWhoseWireGoesOn` and `aPartThatDidNotLoadIsSaid` are new; `theFourthRoundsTuneAndSimulate` is extended.
- Each change was broken on purpose and fails its test. The log-measurement scale is broken at both its start and its switch, which cover for each other.
- The full suite passes (75), under ASan and UBSan too.
