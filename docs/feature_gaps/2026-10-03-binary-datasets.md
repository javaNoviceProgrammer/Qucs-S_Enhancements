# Feature gaps: binary datasets

*3 October 2026 - Qucs-S 26.1.5.*

Claude proposed keeping a run's results binary instead of converting them to text
(`qucs-s-binary-datasets-proposal.md`). For ngspice and Xyce, the simulator writes a
binary raw file; Qucs-S wrote every value of it again as text (`name.dat.ngspice`), then
read that text back for the diagrams and the tools. The conversion was slow, held the
whole run in memory, lost digits, and left two copies on the disk.

It was measured here first, on a run as large as the proposal's `stress_25k`: 12,503
vectors of 11,715 points, a 1.17 GB raw file, converted by `convertToQucsData()` in
`test_binary_dataset`'s benchmark (`QUCS_BENCH_DIR`):

| | Text, 12 digits, as it was (the proposal's figures) | Text, every digit (now) | Binary (now) |
|---|---|---|---|
| Time after the simulator | 40 to 80 s | 45.3 s | **0.72 s** |
| Peak memory (macOS footprint) | several GB | 2.46 GB | **114 MB** (46 MB before it) |
| Dataset | 2.65 GB | 3.31 GB | **1.17 GB** |
| On the disk with the raw file | 3.8 GB | 4.5 GB | **1.17 GB** (the raw file goes) |
| Read whole (`Dataset::read()`, Claude's tools) | | 14.3 s | **0.20 s** |
| The last vector's graph (a diagram) | | 1.24 s | **11 ms** |
| Precision | 13 digits | every digit | every digit |

## What was done, against the proposal

| Proposal | Now |
|---|---|
| 1. The raw file as the dataset: no conversion; the raw file moved or linked beside the schematic, or a small index file naming it. | **Done differently:** one file, `name.dat.ngspice` as before, binary (`datasetfile.h`): a text first line, the same blocks as text with their values as little-endian doubles (a complex one's parts in turn), each block's values together, an index of the blocks at the end. A plain raw file goes straight into it, block by block, in 0.72 s for 1.17 GB. One file kept everything that copies, renames, trashes or keeps a dataset working as it was (`keep_as`, `copy_document`, `rename_file`, `trash_file`, `clean_scratch`), and each variable's values are together, so it is read in one piece, not gathered from every point of a raw file. |
| The reader: parse the raw header, memory-map, one vector on request. | `BinaryReader`: maps the file, reads the index, gives one block's values (or the first n, as pairs for a graph). A diagram's graph reads the blocks it plots and the ones they are over; nothing else of the file. |
| Point-major raw: a strided read, or a transpose cached on first open. | The transpose is the conversion: as many whole variables as 64 MB holds at a time, read from every point 64 columns at a time (a few pages), then each written whole, in the file's order. A variable larger than the buffer, or a file that cannot be mapped: a few points at a time instead. |
| Complex data: pairs of doubles. | So: a block marked `c` in the index. |
| Several analyses, sweeps: names exactly as now. | The same names: a plain raw file's variables named by `normalizeVarsNames()`, as the rows' way names them; everything else (sweeps, XSPICE digital nodes, noise, pole-zero, Xyce's prints, Monte Carlo, NgSweep, the operating point's devices) through the same blocks, written binary instead of text. |
| 2. Qucsator: keep text. | Untouched: Qucsator writes its own `.dat`. |
| Export: "Save Dataset as Text…". | **Save as Text…** in the Content panel's menu of a binary dataset; opening one there offers it too (it has no text to edit). Claude's `export_data` to a Qucs dataset writes text already. **Convert Data File** gives `qucsconv` a text copy of a binary dataset, and lists its variables from its index. |
| Small runs: below a threshold (say 10 MB), text as now; a setting, to switch it off later. | **Simulators Settings > Results:** *Keep large results binary* (on) and *Binary above* (10 MB, of the simulator's output; 0 makes every run's binary). Below it, text. Claude's `set_settings` sets both (`Results/...`). |
| 3. Later: compression; only saving what is plotted. | Not done, as the proposal says. |
| Compatibility: everything that read the text. | Diagrams and data displays (`Graph::loadDatFile()`), Claude's tools (`Dataset::read()`: `get_dataset`, `tune`, `compare`, `export_data`, `import_data`'s reading), the optimizer's `readDataset()`, Convert Data File. A schematic that runs an Octave script after the simulation, or whose data display is one (`.m`, `.oct`), keeps text, which the script reads. Old projects' text datasets read as before. |

## Tests, as the proposal asked

1. **The same numbers.** `eachKindOfOutputGivesTheSameNumbers`: a transient (binary raw,
   real), an AC (complex, a subcircuit's node), an ASCII raw, a parameter sweep (two
   plots), XSPICE digital nodes (`dims=`), each with an operating point's devices beside
   it, converted as text and as binary: every variable the same, to the bit.
   `textAndBinaryReadTheSame` does it for the writers: real, complex, a block real then
   complex, values that are no number, NaN, infinities, -0, a name twice, Monte Carlo's
   blocks. And the end-to-end scenarios with every run binary (below).
2. **No conversion.** `aLargeRunIsBinaryAndQuick` (34.6 MB, the settings deciding):
   binary, no text, the raw file gone, 22 ms. The benchmark above: 0.72 s for 1.17 GB.
3. **Precision.** `numbersAreReadBackExactly`: 15.000001 and 15, the smallest and largest
   doubles, 1 + one ulp: the same doubles back, binary and text; their difference is the
   doubles' own. `aBinaryDatasetHasTheSimulatorsNumbers`: a raw file's 3,000 values, each
   the bits the simulator wrote.
4. **Compatibility.** `aGraphReadsBinaryAsItReadsText`: a diagram's graph of a real curve,
   a complex one as its phase, a family over two variables, an independent variable over
   its index, one over another (`@`), a name alone found as a voltage, a block shorter
   than its variables, a name twice (a graph takes the first), one not there: axes, curves
   and values the same as from text. `saveAsTextGivesTheSameNumbers` and
   `theWindowSavesABinaryDatasetAsText`: Save as Text has the same numbers, and a CSV
   export of each is the same, byte for byte. `theConverterIsGivenText`,
   `theOptimizerReadsBinary`. In `test_qucs_control`, `get_dataset` with `compare`: the
   same answer with the other run binary, then both. The settings test sets the Results
   tab's two settings.
5. **Under ASan.** `aDamagedBinaryDatasetIsRefused`, 17 damaged files: cut in its values,
   its index or its last line; its last line elsewhere or garbled; more values than the
   file holds; a count of 2^62; complex with a real's room; values before the data,
   misaligned, a negative count; neither real nor complex; a line of no block, of no kind,
   of one word; not a dataset; something between the index and the last line. Each is
   refused by the reader, `Dataset::read()`, a graph, and the index's names.
   `aRawFileIsTakenInPieces`: real and complex, from a point at a time to all at once,
   both ways of taking the raw file; and the raw files that are not plain (two plots, cut
   short, `dims=`, ASCII, no header).

## Found while doing it

- **-0 lost in text.** A complex value's imaginary part -0 was written `+j0`, as it had
  always been. Text and binary now give the same bits: text keeps the sign (as the CSV
  export already did), so a binary dataset and its text give the same CSV.
- **Two settings in one call.** *Binary above* was greyed out while *Keep large results
  binary* was off, and `set_settings` applies a call's values in the order of their keys:
  setting both at once was refused for the first. The size is no longer greyed out.
- **Writing in pieces was slow.** The first way took the raw file a few points at a time
  and wrote each variable's few values where they belong: 225,000 writes all over the
  file, 6.4 s of the 7.7 s. Taking whole variables, mapped, and writing the file in order
  made it 0.72 s.
- **A wait in a test.** The window, made in `test_binary_dataset` with the settings'
  ngspice, waited 60 s for it at start; the test now names `sh`, as the panes' test does.

## Checked

- Each part was broken on purpose, 41 breaks, and 38 were caught: the text's digits and
  its -0, the complex sign, the writer's blocks, the reader's checks one by one, the
  first and last of a name, the graph's way, the conversion's names, the raw file
  removed, the settings and their dialog, the Octave case, the two ways of taking a raw
  file, the plain test, the optimizer, the dialogs, the tools. Three were not caught at
  first - the reader's alignment and index-length checks (the damaged files tested broke
  another check too) and a writer of Monte Carlo's blocks alone counting as empty - and
  are now, with tests of their own. Not caught, by design: the Convert dialog's own
  listing of a binary dataset's variables, which its old text scan finds too, only
  slowly (the index's lines begin with `<dep`); and the graph's check of a block shorter
  than its variables, which `pairs()` refuses as well. A third break was of the axis's
  `min()` and `max()` in the binary graph, as the text's way calls them: they never
  change anything (they move only outwards of the infinities they begin at), and the
  binary graph no longer calls them.
- The suite passes 86/86; `test_binary_dataset`, `test_qucs_control` and the other
  dataset suites under ASan and UBSan.
- The end-to-end scenarios pass 77/77 as they are, and 77/77 with every run binary
  (`QUCS_E2E_DATASET_LIMIT_MB=0`, new in `scripts/mcp-e2e-scenarios.py`): all 21 datasets
  of that run binary, every measurement, comparison, import and export as with text.
- `fuzz-simout.py` with `QUCS_DATASET_FORMAT=binary` (new in the harness), under ASan:
  200 mutants of real ngspice output, no failure. CI runs it too, 100 mutants.

## Not done

- **Compression, only saving what is plotted:** the proposal's "later".
- **Claude's tools reading only the variables asked for.** `get_dataset` reads the whole
  dataset (a binary one in 0.2 s for 1.17 GB, but all of it in memory while it answers):
  its listings, expressions and hints go through every variable. A diagram reads only
  what it plots.
