# Feature gaps: Qucs-S's MCP tools, plotting a data file

*30 September 2026 — Qucs-S 26.1.4.*

Claude plotted a CSV of three variables in an empty schematic
(`~/QucsWorkspace/csv_plot.sch`, no circuit, no simulation) and wrote down
what the tools lacked (`qucs-s-csv-import-mcp-fix.md`). It got there only by
writing the dataset itself, in the format the Import tab writes, and then
the diagram's text with `set_schematic`. The three findings were reproduced
through `qucs-s --mcp-server` (the installed app, an isolated HOME and
settings), with the report's own `dummy_data.csv` and `dummy_data.dat`.

## The findings

| Feedback | Now |
|---|---|
| **1. There is no import tool.** Qucs-S takes a data file only through the Import tab of a diagram's dialog, behind a file picker the tools cannot drive. | A tool, **`import_data`**. `file` is read as the Import tab reads it: CSV or TSV, an Excel workbook (a sheet of it), text of numbers in columns, NumPy `.npy` and `.npz`, Touchstone, or a Qucs-S dataset from elsewhere. It is written beside the schematic as `name.dat`, with the file it came from. `file` is looked for beside the schematic, then in the project and the workspace. `name` is the file's by default, with a number after it when a dataset or schematic there has it, as the Import tab names it. `x` is a column's name (in another case, or as the column was labelled, `"Voltage (V)"`, works too) or `"#row"`. `sheet` is a workbook's. The answer gives the dataset's name and file, its variables (x first, each with its points and range), **the traces to use** (`dummy_data:v1`, ...), the columns x may be, a workbook's sheets and what was left out. The diagrams of that folder are read again, and the answer says how many traces of it each has and how many show data. `reload` reads an imported dataset again from its file, with the x and sheet it had unless given. `remove` deletes one, to the trash; the file it came from stays. `undo` with `files` puts a dataset written or removed back. Refused, with nothing written: a schematic with no file ("save it first - the dataset goes beside it"); a file not there; an `x` that is no column (the columns are listed); a `sheet` of a file that has none, or not one of its sheets; a `name` that is not letters, digits and `_`; a `name` of a schematic there (its simulation would write `name.dat` over the import), of a simulation's dataset (`name.dat.ngspice`), or of a `name.dat` that was not imported. The Import tab reads on after a wrong sheet or x with the first sheet and x chosen by itself, and says so; a tool's caller is told before anything is written. |
| **2. `add_diagram`, `add_trace` and `edit_trace` force a simulator's prefix onto `name:variable`.** Every trace of another dataset became `ngspice/name:variable`, read from a `name.dat.ngspice` that an imported dataset never has: "there is no dataset dummy_data.dat.ngspice (simulate to make it)". `edit_trace` put the prefix back, `changed: false`. | Reproduced: all three traces `ngspice/dummy_data:vN`, no data. The rules are now as the report proposed. When `name.dat` beside the schematic was imported, its trace is `name:variable`, with no prefix, even with a `name.dat.ngspice` beside it. When `name.dat` is there with no `name.dat.<simulator>` beside it (a Qucsator run's), no prefix either. Otherwise, and when nothing is there yet, the simulator's prefix as before: a run kept by `simulate`'s `keep_as` (`run1.dat.ngspice`) is still `ngspice/run1:tran.v(out)`. The report's call now plots at once, 101 points per trace, and `edit_trace` with `dummy_data:v1` takes a prefix off. **Found on the way:** `qucsator/` was kept as a prefix, and a diagram then read `name.dat.qucsator`, which no simulation writes. A Qucsator dataset has no suffix, so `qucsator/` now means none (`qucsator/name:v1` is `name:v1`). It names a plain `name.dat` when a `name.dat.ngspice` is beside it. A prefix given as the diagram's dialog writes it is kept. **Also:** a trace added before its dataset was imported still gets the prefix, since nothing was there to tell. `import_data` lists such traces in the answer, each with the name that reads it (`"to read it": "dummy_data:v1"`), and `edit_trace` fixes them. It does not rewrite a diagram by itself. |
| **3. The "no data" text does not know imported datasets.** It said "simulate to make it" when `name.dat` was beside the schematic, imported. `list_documents` called the imported dataset Qucsator's (`"simulator": "qucsator"`) and said the traces need `dummy_data.dat.ngspice`. | A trace with a prefix whose `name.dat` is imported: "there is no dataset dummy_data.dat.ngspice: dummy_data.dat is imported (from dummy_data.csv), and its traces are dummy_data:v1, without a simulator's prefix (edit_trace's 'variable' makes it so)". A plain `name.dat` is told apart ("a dataset of no simulator (a Qucsator run's)"). A variable named alone that the schematic's own dataset does not have, but an imported one does, is pointed to, in the note and in "no data": "dummy_data (imported from dummy_data.csv) has v3: the trace dummy_data:v3 shows it". `list_documents` gives an imported dataset `"imported from"` (the file) and `"traces": "dummy_data:variable"` in place of a simulator. A trace without its dataset gets `"instead"` when the `name.dat` it meant is there. `get_dataset` gives `"imported from"` (file, format, x, sheet, when) and each variable's `trace`. |

## Not taken

**D, a diagram reading `name.csv` itself**, is not taken, as the report
itself leans. The imported `.dat` is what the Import tab lists, reads again
and removes, and it records the file and the options it came from. A second
path through `Diagram::loadVarData` would be a trace no dialog can manage.
It would also have to read a workbook or a Touchstone file every time the
diagram loads.

The report suggested a `datasets` argument to `delete`, or a `remove` flag
on `import_data`. `remove` is taken, as the Import tab has its Remove
beside Add.

## Tests

- **`test_qucs_control`**, new: `aDataFileIsImportedAndPlotted`.
  - A 3-column CSV is imported beside a saved schematic. An untitled one is refused. The test checks the name, x, columns, variables, traces and the origin written.
  - `add_diagram` with the three traces: no prefix, 11 points each.
  - A trace added before the import: listed with its prefix, its "no data" text, and `edit_trace` taking the prefix off.
  - The rules on their own:
    - an explicit prefix is kept;
    - `qucsator/` is none;
    - a plain `name.dat` has no prefix, but gets one with a `name.dat.ngspice` beside it;
    - an imported dataset with one beside it has none;
    - a kept run (`run1.dat.ngspice`) keeps the prefix.
  - A variable named alone is pointed to the imported dataset.
  - `list_documents` and `get_dataset` say imported and from where.
  - Nine refusals, each with nothing written, and `x` by another case and by `#row`.
  - The file changed: read again, first with the x it had (the row), then with `time`. The diagram then shows 21 points.
  - Removed under another name, the CSV kept, and put back by `undo` with `files`.
- **`scripts/mcp-e2e-scenarios.py`**: **s10** is the report's own task. A CSV of 101 rows goes in beside a saved schematic and is plotted. A trace added before the import is told and fixed. `get_dataset` names the traces. The CSV grows to 201 rows and is read again, and the diagram shows it: 6 checks. s1 to s10: 77 checks, none failed, on the build tree's app. The scenario runner picks `s10` by its name (it took the first two letters, which made it `s1`).
- `scripts/ci/check-tool-arguments.py`: 70 tools, no disagreement between schemas and code.

Each change was broken on purpose, 20 breaks in all, and each is caught. The breaks:
- a trace always given the prefix;
- the import's origin not looked at;
- a `name.dat.ngspice` beside a plain `name.dat` not looked at;
- `qucsator/` kept;
- the "no data" text and the note back to the old words;
- `list_documents` without `imported from` and `instead`;
- `get_dataset` without `trace`;
- a wrong `x` or `sheet` read on;
- a `name` of a schematic, and of a `name.dat` not imported, taken;
- a removal not recorded for `undo`;
- the diagrams not read again;
- `remove` removing nothing;
- an untitled schematic taken;
- `reload` forgetting its options;
- the prefixed traces not listed;
- `reload` and `remove` taken together.

The full suite passes (79). `~/QucsWorkspace` and `~/Library/Caches` are
as they were before the runs (checked against a marker file). The one file
`remove` sends to the trash in the test goes to the system's trash, as
`clean_scratch`'s test already does.

The tools sent every turn are as they were. `import_data` is found by the
tool search (CSV, Excel, measurement, Touchstone, ...); the instructions
name it once.
