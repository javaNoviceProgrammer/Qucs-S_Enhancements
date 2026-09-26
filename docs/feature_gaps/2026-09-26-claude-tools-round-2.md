# Feature gaps: Claude's Qucs-S tools, round 2

*26 September 2026 — Qucs-S 26.1.3.*

Claude reviewed its tools a second time, after a session on a
complementary JFET follower (ngspice, `op` + `tran` + `ac` in one run).
Round 1 ([2026-09-25](2026-09-25-claude-tools-results.md)) had given it the
capabilities. This round was about trusting what they report.

Each of its nine items was checked against the source and reproduced on
the same schematic with ngspice 46. The table below says what was done.

| # | Item | Was | Now |
|---|---|---|---|
| 1 | `get_dataset` reports magnitudes of real-valued complex vectors | A Nutmeg equation `y = db(norm(v(out)))` is written as complex numbers with a zero imaginary part. Read as a magnitude, the curve falling to −44 dB came back rising to +44 dB. Its "bandwidth" was 224 MHz, measured as the peak over √2, against a true 6.1 MHz. Nothing said anything was wrong. | A vector whose imaginary parts are all zero is read as real, its sign kept, and says so (`"real": true`). Each variable gives its `units` (dB, V, A, degrees, s, Hz), from its name or from the equation that defines it, which is quoted (`"defined as"`). `bandwidth` works per kind of curve: in dB, 3 dB below the peak; for a magnitude, the peak over √2; a signed curve that is neither is refused. A `decibels` argument overrides the detection. On the schematic, `ac.y` now gives 6.09 MHz and `v(out)` gives 6.10 MHz. (`qucs/dataset.h`) |
| 2 | The operating point is unreachable and goes stale | The op analysis's node values were in the dataset, but written as one-point *independent* variables that looked like sweep axes. Device quantities (id, gm, …) came only from the separate DC bias run. Its `.dc_op*` files outlived every later run: the review read a day-old file describing a circuit with a different Beta. | The op analysis runs ngspice's `show all` as well. Each device's operating quantities are written into the dataset beside the node values (`@jt1[id]`), so they travel with the dataset, `keep_as` copies included. `get_dataset` presents an `operating point`: nodes, and devices under their components (T1: id 10 mA, gm 20 mS). `operating_point: true` returns it alone. `v(out)` gives the op value along with the analyses' `v(out)`. The files of a DC bias run are removed before every run. (`extsimkernels/`) |
| 3 | A change by the user cannot be told from Claude's own | `get_state` said "unsaved changes" and nothing more. | Every document counts its revisions: each edit, undo, redo and reload, whoever made it. `get_state` gives the revision, who made the last edit (you, the user, another conversation) and when the dataset was last written. Every tool's result for a conversation then says what changed since its last call that it did not change itself: the user's edits, another conversation's, a simulation run again, documents opened or closed. It says so once. Each conversation is told this in its own results, where Claude reliably sees it. An MCP notification is not surfaced to the model. (`QucsDoc::revision()`, `ToolHost::callToolFor()`) |
| 4 | Markers have no tools | Moving one marker meant rewriting the whole `<Diagrams>` section by hand, and `get_schematic` did not list markers at all. | `add_marker` takes `at`: an x value, `peak`, `min`, `-3dB`, or `crossing:<y>`. It finds the exact point, places the marker on the nearest sample (a marker shows a sample) and reports both. It also sets the label (where it goes, or its offset from the point), precision, format, transparency, indicator and colours. `edit_marker` and `delete_marker` complete the set, each one step to undo. `get_schematic` lists every diagram's markers with the sample each shows. |
| 5 | The summary is lossy and verbose at once | A JFET listed all 27 properties, 24 at their defaults, yet the marker was missing. A 24,000-part schematic would have made an unusable summary. | `properties` is `non_default` (the new default: shown ones, and those differing from the type's defaults), `shown` or `all`, with a count of what was left out. `components` (names) or `region` narrow the list, along with their nets and wires. Lists stop at 200, and a net at 60 pins, with a note on what was left out: a region of the 24,000-part ladder is 3.8 KB. The summary now covers the whole file: markers, each painting as its `.sch` line, and the document's settings. |
| 6 | No netlist export | *Save netlist* and *Save CDL netlist* open file dialogs that no tool can answer. | `export_netlist` writes a SPICE or CDL netlist to a named file, the current one or the last run's, and asks permission like any change. `get_netlist` gains `format: cdl`. |
| 7 | Panes are invisible | The layout had to be inferred from which pane actions were enabled. | `get_state` lists the panes: each one's number, row and column, rectangle, documents, the one in front, and which pane is active. Each document says which pane it is in. `move_to_pane` puts a document in pane *n*, or in a new pane to the right or below. It refuses to move a pane's only document to a new pane, since the emptied pane would close again (the trap the review fell into). |
| 8 | The two screenshot modes differ in a way nothing documents | `all` drew the document on white paper; `visible` captured the themed canvas. A marker's dark box only appeared in the second, so the review wrongly concluded it could not see the problem. | The modes are named for what they are: `paper` (alias `all`) and `screen` (alias `visible`). The description and each result say what the picture shows. `window` captures the whole application window, plus each dialog open over it as its own picture. The marker's dark box itself was fixed in 26.1.3: it is filled with the paper's colour and written in its ink. |
| 9 | `set_schematic` gives almost no feedback | "20 components and 19 wires" came back for a call that replaced only the diagrams. | It returns what it read of each section it replaced: components by name and type, wire count, the diagrams as `get_schematic` lists them (each trace's points or why it has none, each marker and its sample), and the paintings' lines. `describe_format` documents every field of a component, wire, diagram (all of its ~30), trace, marker and painting line. The trap it warns about: field 9 of a diagram line is two digits written together. |

Found on the way:

- **Change notices:** loading a text document counted as several edits:
  inserting the text, the load's own call, and highlighting. Only a
  change to the document's text revision counts now.
- **Painting lines:** the summary would have given them without their
  `<…>`.
- **`unitOf`:** it took Qucsator's `out.Vt` to be an analysis `out` and a
  name `Vt`.

## Tests

- **`test_dataset`:** a complex vector with no imaginary part is read as
  real. Units come from names and equations, and bandwidth is measured
  in dB or refused. An op analysis's values are told apart, and
  `v(out)` resolves to the op value plus the analyses'.
- **`test_qucs_control`:**
  - A low-pass with `y = db(norm(v(out)))`: its units and definition,
    the bandwidth in dB and as a magnitude, the refusal on a signed
    curve, and the operating point under its components.
  - Markers at `-3dB`, the peak, an x and a crossing: listed, restyled,
    deleted and undone.
  - `set_schematic` reporting diagrams with their markers, and
    `describe_format`.
  - Revision notices: the conversation's own edits, the user's, another
    conversation's, a dataset written again, documents opened and
    closed.
  - The summary's property modes, filters and 200-part cap; SPICE and
    CDL export; panes arranged and restored; `window` screenshots,
    including one over a dialog.
  - With ngspice installed, a real run with an op analysis: the stale
    `.dc_op*` files are gone and R1's operating point is read.
  - The new tools are added to the 600 calls of odd arguments.
- **GUI monkey:** its fake Claude also calls the new tools with hostile
  arguments.

Four of the fixes were broken on purpose to check that their tests
fail, and they did: the real reading of item 1, the stale-file cleanup of
item 2, the notices of item 3, and the non-default property mode of
item 5.
