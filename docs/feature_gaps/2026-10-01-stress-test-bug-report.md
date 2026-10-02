# Feature gaps: the stress test of large schematics

*1 October 2026 - Qucs-S 26.1.5.*

Claude drove Qucs-S through schematics of up to 25,000 transistors
(`qucs-s-stress-test-bug-report.md`). It reported:
- six bugs: three high, one medium, two performance;
- one suggestion: a hint when a transient's step is long for its sources' edges.

Each was reproduced first through `qucs-s --mcp-server`, with its own workspace,
settings, trash and HOME. The report's files (`stress_2500.sch`, `stress_10k.sch`,
`stress_25k.sch`, `inverter_chain_500.sch`, `stress_dataset.sch`) were copied into
scratch; the originals in `~/QucsWorkspace/opamp741_prj` were not touched. The "Before"
figures are from the build before this round, the "Now" ones from this one, on the same
machine and files.

## The bugs

| Report | Now |
|---|---|
| **1. More than about 999 nets: an empty dataset, reported as success.** ngspice reads at most 1000 words of a command (`LOTS`). The one `write` naming every net of 1,252 failed ("write: too many args."), and the run said it had worked with an empty dataset. | **The nets are saved before the analysis in commands ngspice takes** (`save`, 900 vectors each). The plot is written whole by a bare `write`, and the saves are cleared after it (`delete all`). A DC point is printed in parts into one file (`print ... >`, then `>>`). The same holds for a sweep's and the corners' waveforms. `stress_2500.sch` now simulates with all 1,252 nets in the dataset (`"variable count": 1252`). Equations (or a sweep's or the corners' records) read the plot after the analysis, and a save list would hide from them what it leaves out, such as a subcircuit's inner node (`let k = v(x1.m)` failed, tried). So with them, nothing is saved: the plot holds all of ngspice's vectors. Fewer vectors: one `write` naming them, as before. |
| (the same) | **No results is an error now.** When the outputs a run expected hold nothing, the dataset is not written over: the one before stays, and the run fails with "The simulator wrote no results: its output (...) holds none. Look for an error in its log - the dataset is the one from before." The status log shows it as the simulator's own errors are shown, with no message box besides. A simulator that also failed by its exit code has that error first, as the reason. "write: too many args." is an error in the log and in `simulate`'s errors. The dataset is written as it is made, into a file put in place when whole (`QSaveFile`), not held as one string first (900 MB for the report's 100,001-point run). |
| **2. `move {selection: true}` fails with grounds**: 252 times "There is no component ." in one answer. `select {names: ["MN1", "GND"]}` quietly moved MN1 alone. | A selection names each part by its ref, as `get_schematic` gives it: a ground is `GND#17`, one alone `GND`. Select All, then `move`, moves every part, grounds and all. Repeated messages are said once, with how often ("There is no component R999. (×25)"), twenty at most, and how many more. Lists of moved or deleted parts are capped the same way. `select` of a bare `GND` among many says which there are ("There are 252 of GND: say which, GND#1 to GND#252"), as before. |
| **3. The variables list is cut at 40, and miscounted**: `brief` said "... 37 more" of 252. | `simulate` gives the first 40 by name, then "... 212 more", and the count of all as `"variable count"`. `brief` gives three and "... 249 more". |
| **4. Checks and edits grow faster than the schematic.** At 25,000 parts `check_schematic` took 187 s, one `edit_component` 151 s, an undo 76 s. | Each was profiled (`sample`) and its hot spot fixed. Asking whether the circuit is digital (every part) for every net's name now comes after the cheap name test. A component's line is split once, not once per field (`QString::section`). The change list (`describeChanges`) reads only the lines that differ. An undo by the tools reads the one state it needs. The figures are below. |
| **5. Memory: undo copies the schematic each step; a dataset takes 8× its file.** | **Undo**: each step is kept packed (`qCompress`): a 15 MB state of 10,000 parts keeps under 1 MB. **Datasets**: read where they lie on disk (mapped), not read whole into memory, by the diagrams' graphs and by the tools' `Dataset`. Room for the values is made once, and no more than the rest of the file could hold, whatever a header claims. The figures are below. |
| **6. Components leak everything they own.** 4.7 GB stayed after every document was closed. | **A component frees its symbol, ports and properties when deleted.** `Component` had no destructor. Also freed now: a symbol made again (each edit of a MOSFET, `MultiViewComponent::recreate`), what a library component takes over from its model (`copyComponent`), the ports of a subcircuit or SPICE library part made again, the properties dropped by six components' dialogs and loaders, every diagram's grid when it is drawn again (nine types), the symbol editor's lists, and the icon export's scene. Opening and closing the 500-transistor chain five times: +44 MB before, nothing now. |

## The figures

The same commands on the report's files, before this round and now (wall time;
resident memory as `ps` gives it):

| | Before | Now |
|---|---|---|
| `stress_10k.sch` (10,000 transistors): open | 2.1 s | 0.7 s |
| … `check_schematic` | 6.8 s | 0.2 s |
| … `edit_component` | 4.9 s | 0.5 s |
| … `undo` | 2.7 s | 0.9 s |
| … `get_netlist` | 3.4 s | 0.9 s |
| `stress_25k.sch` (25,000): open | 5.2 s | 1.9 s |
| … `check_schematic` | 187 s | 0.6 s |
| … `edit_component` | 151 s | 1.2 s |
| … `undo` | 76 s | 2.4 s |
| … `get_netlist` | 88 s | 2.3 s |
| Memory after both closed | 2,190 MB | 854 MB |
| `inverter_chain_500.sch` opened and closed 5 times | 123 → 190 MB | 121 → 143 MB, flat after the first |
| … then 3 undo steps | +37 MB | +7 MB |
| `stress_2500.sch` simulated | "succeeded", 0 variables | succeeded, 1,252 nets |
| `stress_dataset.sch` (100,001 points × 252 vectors) simulated: peak | 3,767 MB | 1,011 MB |
| … after the run | 3,781 MB | 561 MB |
| … its 481 MB dataset, opened with its diagrams (in a fresh process) | +509 MB | +49 MB |
| … all its variables listed, two measured | 1,315 MB | 388 MB |

**What stays.** At 10,000 parts each edit still raises the process's footprint at first,
then it levels off: 305 → 791 → 933 → 927 → 980 MB over 80 edits, with its memory in
use flat at 267-277 MB (`heap`). It is macOS's allocator keeping freed large blocks for
the process's next allocations; `malloc_zone_pressure_relief` hands back nothing on
macOS 26 (tried, also in a C program of its own). `misc::releaseFreedMemory()` asks
after a closed document, a run's results, a dataset read and a large undo step: glibc's
`malloc_trim` does give it back.

## The suggestion

| Report | Now |
|---|---|
| A hint, from `check_schematic` or the `.TR` dialog, when the transient's step is longer than the sources' rise times. With 0.5 ns steps over 0.5 ns edges the chain's delay came out 12% too long. | **A note of `check_schematic`** for ngspice (the window's check shows errors and warnings, not notes). ngspice steps at most (Stop − Start)/(Points − 1), or (Stop − Start)/50 when that is less, or MaxStep when it is set. When an edge of the fastest source (a pulse's, a PWL's steepest, the PRBS source's) is crossed in fewer than five steps: "TR1's time step is long for Vin's edges of 500 ps: its step is 500 ps (0 s to 1 us in 2001 points), and an edge crossed in so few steps comes out coarse - delays and rise times off by 10% or more. A MaxStep of 100 ps or less (or more Points) resolves them". Edges of no rise time are the step itself, so there is nothing to compare. Edges that a million steps would not resolve over the run are a step to it (a relay switched in 1 ns, run for 20 ms): no note. The examples' survey gives this note nowhere; the PRBS eye example now runs in 3 ps steps (20,001 points), five per 15 ps edge. |

## Found on the way

- **A preview lost the selection.** A preview puts the schematic back by rebuilding it
  from its text, and the selection went with the old elements: `move {selection: true,
  preview: true}`, then the same move, found "Nothing is selected". The selection is
  kept now: parts by name (an unnamed one by its type and place), wires by their ends,
  diagrams and paintings by their places.
- **A moved ground read as deleted and added again** in the change lists (a preview's
  "would change", "since your last call"), 60 lines for 30 grounds. Unnamed parts gone
  and come of one type are paired in their order as moves: "GND: moved from (100, 90)
  to (110, 90)".
- **A run with no results opened a box** in the window: the sweep's and corners' "an
  ngspice without the command" tests hung on it for 300 s. It is the simulator's
  failure, in the log as its errors are.
- **A DC point printed nothing in every circuit without a named net.** ngspice said
  "print: too few args." each time, harmlessly: the devices' values (`show all`) are the
  results there. The suite caught it when "too few args" was first made an error too.
  No print is written then, and "too few args" is no error.
- **The timing diagram kept its arcs** when drawn again (it draws none, so nothing
  leaked); it clears them as the others do.
- **The `indep` header's room** was made as the header claimed, up to 10 million values
  (80 MB from a header alone); it is capped by the file now.
- The sweep's and corners' bare `write` ended in a space.

## Not done

- **Only the variables a diagram shows, values as doubles, binary output from
  ngspice** (the report's ideas for datasets). Mapping the file removed the copy of the
  whole file per graph, the larger part. The others change the dataset's format or what
  every reader expects.
- **Idle CPU of 15-30% with large documents** (the Content panel's 3 s rescan): not a
  bug, as the report says, and not looked into.

## Tests

- **`test_scale_and_memory`** (new):
  - `componentsFreeWhatTheyOwn`: delete, `copyComponent` and `recreate`, counted
    exactly by lines, arcs and texts whose destructors count;
  - `diagramsFreeTheirOldGrid`: the nine diagram types;
  - `deletedComponentsGiveTheirMemoryBack`: 12,000 components made and deleted, the
    heap where it was (it grew 35 MB before);
  - `aComponentLineIsReadFieldByField`: every field, fewer values than the type has,
    `''` and `\n`, equations, a simulation block, broken lines;
  - `vectorsComeInChunksNgspiceTakes`, `aNetlistOfManyNetsWritesThemAll`: 1,100
    labelled nets, with and without an equation, 20, and a DC point with no net named
    (no print);
  - `anEmptyOutputIsAnError`: no output, an empty one, a read-only dataset, a good
    run, and no file of `QSaveFile`'s left;
  - `tooManyArgsIsAnError` (and "too few args" is none);
  - `undoStepsArePackedAndComeBackWhole`: ten edits of 2,000 parts undone and redone,
    each state as it was;
  - `aDatasetIsReadAsItIs`: a header claiming 10⁹ values, "nan", the file gone after;
  - `aGraphReadsTheFileWithoutChangingIt`: a file of exactly 16 KiB, read to its last
    byte, and left as it was.
- **`test_qucs_control`**:
  - `aSelectionWithItsGroundsMovesWhole`: Select All, a preview, the move, the
    grounds moved, refs, once-each messages;
  - `theChangesOfALargeSchematicAreFoundQuickly`: 40,000 lines;
  - `aCircuitOfManyNetsIsSimulatedWhole`: 1,100 nets through ngspice, the values down
    the chain, the count and `brief`.
  - `aFailedRunSaysWhyFirst`: a stand-in that exits with 3 and writes nothing; its
    exit code first, then no results, and the dataset as it was.
  - `backgroundSimulationsAreFollowed`: the stand-in that writes nothing is no
    success now.
- **`test_ngsweep`** `manyNodesAreSavedBeforeTheSweep` and **`test_ngstats`**
  `manyNodesAreSavedBeforeTheCorners`: with and without records, and at the limit.
- **`test_erc`**: the step note.
- Loading every example (253) gives the same text as before the change to
  `Component::load`.

Breaks: 20, each caught. One was caught only after a test was added: the exit code
before "no results". Once the empty DC print was gone, the run of `/usr/bin/false`
expected no output, so that path was no longer tried. `aFailedRunSaysWhyFirst` is that
test.

The full suite passes 83/83, on the ASan/UBSan build too, with no report. The e2e
scenarios pass 77/77. Nothing was written to the user's workspace, cache or Trash
(a marker file, `find -newer`, the Trash's time).
