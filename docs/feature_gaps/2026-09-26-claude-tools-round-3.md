# Feature gaps: Claude's Qucs-S tools, round 3

*26 September 2026 — Qucs-S 26.1.3.*

Claude checked round 2 ([2026-09-26](2026-09-26-claude-tools-round-2.md))
against the running tools, on the same JFET follower. Eight of its nine
items were done. The ninth, change notification, was done except for
MCP push notifications. It then listed seven gaps it hit while checking,
ranked by how often they bite, and one question it left open: whether an
8000-component summary is workable.

Each wish was checked against the source. The table says what was done.

| # | Wish | Was | Now |
|---|---|---|---|
| 1 | Paintings have no tools | A `Text` or an `Arrow` marking up a result meant writing the whole `<Paintings>` section by hand through `set_schematic`. `delete` could not remove a painting. The summary gave each painting as its raw `.sch` line, including an image's whole picture in base64. | `add_painting` and `edit_painting` draw and change every kind by named fields: text, line, arrow, rectangle, ellipse, arc, polyline, image (from a file), rounded rectangle, polygon or star, brace, waveform, text box (a block, a note, or a callout with a `tip` it points at), table (its cells), dimension, formula (TeX). A text is `x`, `y`, `text`, `size`, `color`, `angle`. An arrow is `from`, `to`, `head`. A box is `x`, `y`, `width`, `height`. All take `color`, `thickness`, `style`, `fill_color`, `fill_style`, `filled`. What is not given stays. A field the type does not take is refused, and the error lists those it does. Each change is one step to undo. The painting is rebuilt from its changed line by the loader a file uses, so it always reads back. `delete` and `select` take paintings by number. `get_schematic` lists each painting by its fields, with the picture's format and size in place of its bytes. `describe_format` lists every type's fields, taken from the fields of the painting's own dialog. (`qucscontrol_paintings.cpp`) |
| 2 | No distortion or loop-stability measurements | `measure` had rise and fall time, overshoot, settling time, period, frequency, duty cycle, crossings, bandwidth. Crossover distortion, the question a push-pull follower is about, could not be asked. | `thd` works as ngspice's `.four` does. It takes the last whole periods of the fundamental before `to` (1 unless `periods` says more), resampled evenly at 32 points per period for each harmonic counted (at least 1024 points). It gives each harmonic's amplitude and dBc, harmonics 2 to `harmonics` (9 unless given), the fundamental's amplitude, the DC, and the THD in percent and dB. `fundamental` states the frequency. When it is not given, the frequency is the median period between rising crossings of the middle of the curve's later half, so a start-up does not skew it. A simulation with fewer than 10 samples a period of the highest harmonic is warned of. `gain` gives the gain at the lowest frequency and at the peak, as a ratio and in dB, and the unity-gain frequency. `phase_margin` is 180° plus the phase where \|T\| falls through 1. `gain_margin` is minus the gain in dB where the phase falls through −180°. Each gives the frequency too. The phase is taken from the complex variable and unwrapped. A loop broken at an inverting point (phase starting near ±180°) gets a note with the margin taken the other way. A curve without a phase is refused. (`dataset.h`) |
| 3 | No way to see what is in the workspace | `get_state` gave the workspace folder, never its contents. Without a shell, a project could not be discovered. | `list_documents` lists the files of the workspace, a project (by name, `amp` or `amp_prj`) or a folder. It looks 4 folders deep and follows a linked project at the top. It lists schematics, symbols, data displays, datasets, netlists, texts, PDFs, spreadsheets, Markdown, pictures and Verilog-A. Each has its path, kind, size, time changed, and whether it is open. A dataset says which simulator wrote it and which schematic it belongs to. Files come newest first, or by name, and can be narrowed by `kind` and `search`. Listing the workspace also lists its projects. At most 300 files, with a count of what is left out. Hidden files are left out. |
| 4 | `batch` is not transactional | A call failing at step 15 left 14 changes in place, each its own step to undo. | With `atomic: true`, the batch snapshots every open schematic and its symbol when it begins. When a call fails, they are restored as one step to undo, and undoing it brings the changes back. It does not undo 14 times, because *Edit > Undo* keeps 20 steps by default, fewer than a large batch makes. Files written, simulations run and documents opened stay, and the result says so. Without `atomic`, a batch that stops says how many changes stay on which document. `undo` and `redo` take `steps`. (`Schematic::snapshotAll()` and `restoreAll()`) |
| 5 | Image export is dialog-only and cannot be scoped to a diagram | *File > Export as image* was reachable through `trigger_action` and `set_dialog`, three calls. `select` took only component names, so *Selected elements only* stayed off, and a frequency response meant exporting the page and cropping it elsewhere. | `export_image` writes PNG, JPEG, BMP, TIFF, WebP, SVG, PDF, EPS or PDF+LaTeX (with its PDF), in one call. The format comes from the suffix, or is given. It takes `scale`, `colours` (colour, grayscale, monochrome) and `transparent` (a format with paper says it cannot be). `diagram` exports that diagram alone. `selection` exports what is selected. What was selected before is selected again after. `select` now takes diagrams and paintings by number. (`graphicsexport.h`) |
| 6 | No `set_simulator` | Only *Simulation > Simulators Settings...*, a dialog around every run. | `set_simulator` picks ngspice, Xyce, SpiceOpus or Qucsator from the toolbar's list of installed simulators, as a click on it does, and the setting is saved. It refuses a simulator that is not installed, naming those that are. It refuses to switch while a simulation runs. It returns the simulator chosen and the one before. Two engines now compare in four calls: `set_simulator`, `simulate` with `keep_as`, `set_simulator`, `simulate`. |
| 7 | Symbols have no tools | `.sym` files and a subcircuit's symbol opened, but nothing edited them. | The painting tools draw on a symbol. With `symbol: true`, the document is switched to show its symbol, as *Edit Circuit Symbol* does, and `symbol: false` switches back. A `.sym` file is all symbol. Each change is one step in the symbol's own undo. `get_schematic` with `symbol` lists the symbol's paintings, its ports and name text among them. Ports and the name text can be moved, but not made or deleted: the schematic's Port components make the ports. |

## The remaining gap: push notifications

Round 2's item 3 asked for an MCP notification on a document change.
Claude's check agreed that this is not a change Qucs-S can make alone:
the CLI would have to surface it to the model, and interrupting a turn
to report a nudged resistor is worse. It named the cheapest worthwhile
increment. That was done:

- **`simulate` says when the document changed while it ran.** It notes
  the schematic's revision when the run begins and compares it at the
  end. Edits after that are counted by who made them (the user, another
  conversation) with the time of the last one. The result says the
  numbers are of the schematic as it was when the run began. This
  closes the widest window, the user editing during a 120 s run.

## The open question: an 8000-component summary

Measured on a copy of `rc_ladder_8000.sch`: 24,004 components, 1.4 MB.

| Call | Time | Size |
|---|---|---|
| `get_schematic` (default summary: 200 parts, 200 nets) | 87 ms | 76 KB, about 19,000 tokens |
| `region` 1000 × 400 (95 parts) | 85 ms | 31 KB |
| `components` R1, C1, R4000 | 83 ms | 2.3 KB |
| `format: text` | 47 ms | 1.4 MB |
| **`format: overview`** (new) | 24 ms | **721 bytes** |

The filters make it workable, but a first look cost 19,000 tokens and
still showed less than 1% of the parts. The new `overview` format tells
a schematic at a glance: parts counted by type (8001 GND, 8000 C,
8000 R, ...), its analyses in full, the number of nets and the named
ones, its extent, and its diagrams. A summary that leaves parts out now
points to it.

## Found on the way

- **`slotChangeSimulator`** read the current tab's widget without
  checking there was one. With no document open, choosing a simulator
  on the toolbar dereferenced null.
- **A picture in the summary:** `get_schematic` gave every painting's
  line, so an image painting put its whole picture in base64 into the
  summary.

## Tests

- **`test_dataset`:**
  - THD of a sine with a 10 % second and a 5 % third harmonic, with a
    DC offset and a start-up: with the fundamental given, found from
    the curve, over three periods with 20 harmonics.
  - Refused on a range too short; a coarse simulation warned of.
  - A three-pole loop gain: gain, phase margin and gain margin against
    the crossings solved by bisection.
  - −T noted; two poles never reach −180°; no phase, no margin; the
    gain in dB.
- **`test_qucs_control`:**
  - Ten kinds of painting added, listed, changed field by field,
    undone, selected and deleted, and the file's lines checked.
  - Fifteen bad calls refused with nothing changed; a picture from a
    file.
  - A subcircuit's symbol drawn: switched to and back, its ports moved
    but not deleted, its own undo.
  - `list_documents` on a project with a dataset, a hidden file and an
    unknown kind: newest first, by name, by kind and name, the open
    one, the projects.
  - `export_image`: whole, one diagram (the selection restored), SVG by
    suffix, PDF+LaTeX, a JPEG asked to be transparent, seven refusals.
  - `set_simulator` over the simulators installed and those not.
  - An atomic batch rolled back and brought back by undo; a
    non-atomic one's count taken back by `undo` with `steps`.
  - THD and loop margins read through `get_dataset` from a dataset
    file.
  - A slow stand-in simulator during which the user edits the
    schematic, and a run without edits.
  - The overview's counts.
  - The new tools added to the 600 calls of odd arguments.
- **GUI monkey:** its fake Claude calls the new tools with hostile
  arguments: huge coordinates, quotes and percent signs in text, a
  broken formula, a 50 × 20 table, symbol switching, an atomic batch
  that fails, `undo` of 1000 steps, odd folders, exports at scale 20,
  THD at 10⁻³⁰⁰ Hz. It was run under ASan/UBSan over 20 seeds.

Four of the fixes were broken on purpose to check that their tests
fail, and they did:

- the atomic rollback;
- the note of edits during a run;
- edits keeping the fields not given;
- THD counting only harmonics 2 and up.
