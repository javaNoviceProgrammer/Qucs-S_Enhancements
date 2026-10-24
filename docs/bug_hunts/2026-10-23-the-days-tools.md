# Bug hunt — the day's tools: the MCP wishlist, its three re-check rounds, and the reload of 23 October

Two hours in the foreground on everything committed since the morning's hunt (`a6921df`): the
wishlist's fourteen new items (`472a813`), the Extras menu (`825c4d6`), the three re-check rounds
(`a6eddae`, `abefdd2`, `fe1c00a`), "Open in Viewer" (`dc49cc0`) and the per-call reload (`2c0de7e`,
committed at the start of this hunt).

How: the 133 tools' schemas swept for what the new singular-for-plural rule rewrites; probes through the
real `--mcp-server` (one JSON file a probe, in this folder); `describe_tool` with `arguments` used to read
each guard's verdict for a call **before** making it, so a permission hole can be shown without running
anything (and the verdicts are the ones that applied: `get_state` reports
`"guards": {"commands": true, "files outside the workspace": true, "this conversation is asked": true}`
throughout); a new fuzzer of the day's tools (16,500 calls over four seeds, Release and ASan/UBSan);
`scripts/ci/fuzz-sch.py` (700 mutants, 1,400 headless runs under ASan); and the diffs read.

Findings are documented, not fixed.

| | What | Where |
|---|---|---|
| **A1** | A trailing "s" stripped from field names that are not plurals: a one-letter key becomes a real argument - `save_document {"a": "x.sch"}` writes the file | `asPlural`, `qucscontrol.cpp` |
| **A2** | The outside-the-workspace guard is read before the rename, so a write named by the singular is never put to the user | `filesOutside`, `qucscontrol_resources.cpp` |
| **A3** | `batch {"call": …}` hides every inner call from all three pre-call checks, and still runs it | `irreversible`/`askedEachTime`/`guarded` vs `asPlural` |
| **A4** | `run_script`'s check is substring matching on the script's text: `'trash'+'_file'` gets past it, and `console` is not in its list at all | `irreversible`, `qucscontrol_resources.cpp` |
| **A5** | A half-written schematic read from disk parses as an **empty** one: the tab loses its parts | `reloadOneIfChanged` → `Schematic::load` |
| **A6** | The damaged-file box is raised inside the per-call reload, outside the call's error capture: that call never returns | `callToolFor` → `readAnyFileChangedOnDisk` |
| **A7** | A model's file read again **while a solve of it runs**: the run ends against the new model | the reload's guard misses an async job |
| **A8** | `rename_file` moves a document anywhere outside the workspace and the guard names nothing | `filesOutside` has no branch for it |
| **A9** | A Touchstone file with one short row reads the **next row's frequency as an S-parameter**, and says nothing | the Touchstone reader, `dataimport.cpp` |
| **A10** | A Touchstone file whose frequencies do not ascend keeps only the rows up to the first break - 1 of 3 - and says nothing | the same reader |
| **A11** | A Touchstone header's frequency unit that is not one of the known ones is taken as **GHz**: every frequency 10⁹ times too large, said nowhere | the same reader |
| **A13** | A library is written outside the workspace with no question, in two unguarded steps: `set_settings` adds the folder to the library search paths, `create_library` writes there | `filesOutside` sees neither |
| **A12** | A missing file goes unreported for four of the six file-backed parts (`SPfile`, `SPICE`, `SPICE_dev`, `XSPICE_A`), where `Sub` and `SpLib` name it; for `SPfile` it empties the whole netlist | `check_schematic` |
| **F1** | **Crash** (SIGSEGV) reloading a truncated schematic with that box open | `Schematic::load` → `signalDocumentRebuilt` |
| **B1** | A `reference` named for something that does not exist is used anyway, and not reported unmatched | `valuesAgainst`, `qucscontrol_fem.cpp` |
| **B2** | A region that meets no text makes `read_pdf` call the PDF scanned | `readPdf`, `qucscontrol_design.cpp` |
| **B3** | `fem_edit` takes a geometry size of `1e308`, `nan` or `inf` without a word | `femEdit` |
| **B4** | A plot group shown again marks its display changed and rewrites the `.dpl`, though nothing changed | `showPythonDisplay`, `qucs_python.cpp` |
| **B5** | A count of zero taken and ignored: `fem_converge`'s `timeout: 0`, `tune`'s `max_runs: 0`, `find_shapes`'s `limit: 0` | three tools |
| **B6** | `line_calc` answers `nan` for a height, width or permittivity of zero or less | `lineCalc` |
| **B7** | `receiver_budget` takes a temperature below absolute zero, a negative bandwidth and a noise figure below 0 dB | `receiverBudget` |
| **B8** | `trigger_action {"action": ""}` fires the first menu action (File > New) instead of being refused | `actionNamed`, `qucscontrol.cpp:11758` |
| **B9** | A CSV whose rows are not all one width loses its header names in silence and gains a column | `import_data` / `dataimport.cpp` |
| **B10** | Two columns of one name: the `renamed` report names only the last of them | `import_data`, wishlist N2 |

## A1. A trailing "s" stripped from names that are not plurals

Wishlist N11 reads a singular where a tool takes a list of that name (`name` for `delete`'s `names`).
The rule is: for an argument the tool does not take, if the name plus `s` or `es` **is** a field, read it
as that field. Field names that merely end in "s" are not plurals, so the rule invents an argument:

| call | read as | what it does |
|---|---|---|
| `save_document {"a": "snuck.sch"}` | `as` | **writes the file** |
| `export_image / export_data / export_netlist / import_netlist / create_subcircuit / synthesize_* {"save_a": …}` | `save_as` | writes the file |
| `simulate / tune {"allow_command": true}` | `allow_commands` | **runs the schematic's commands besides the simulator** |
| `fem_describe {"v": "x"}` | `vs` | |
| `synthesize_filter {"f": 2e9}` | `fs` | **designs a different filter**: `f` becomes "where 'atten' is reached in the stop band", and the same order-5 Chebyshev comes back as for `fs`; without it the call is refused ("give 'order', or 'atten' at 'fs'") |
| `add_trace {"axi": …}`, `{"thicknes": …}`, `create_subcircuit {"nam": …}`, `set_dialog`/`set_ui {"pres": …}`, `add_diagram {"x_axi": …}`, `fem_plot {"sim"/"specie": …}`, `arrange {"supplie": …}`, `* {"propertie": …}`, `edit_component {"replace_equation": …}`, `make_symbol {"side": …}`, `git_remote {"tag": …}` | the field | |

146 such renames exist over the 133 tools (`renames.txt` in this folder lists them all). Shown
(`p1.json`, `p40.json`):

```
save_document {"a": "snuck.sch"}  -> Saved as …/ws/snuck.sch.  a was read as as.
simulate {"allow_command": true}  -> …  allow_command was read as allow_commands.
synthesize_filter {"fc":1e9, "f":2e9, "atten":40, …} -> order 5, "40 dB at 2e+09 Hz: 41.1 dB there"
synthesize_filter {"fc":1e9,          "atten":40, …} -> refused
```

It is said in the answer, but by then the file is written and the filter designed.

**Why a suffix cannot decide it.** Of the 146, **48** land on a field whose type is a list - where the
rule is doing what N11 asked - and **98** on a field of another type. Those 98 are a mixture: `undo`
`{"step"}` → `steps`, `console {"line"}` → `lines`, `read_pdf {"page"}` → `pages` and
`send_input {"key"}` → `keys` are true plurals that happen not to be arrays, while `as`, `axis`,
`press`, `vs`, `fs`, `analysis`, `properties`, `thickness`, `species` and `supplies` are not plurals of
anything a caller would send. In 26 of them the invented singular is four letters or fewer (`a`, `v`,
`f`, `axi`, `nam`, `pres`, `save_a`, `keep_a`, …), which is where a stray key or a typo turns into an
argument.

**And it is ambiguous.** The rule tries `key + "s"` and `key + "es"`, so two different keys fold into one
field and one of them is lost without a word (`p61.json`):

```
add_component {"x": 400, "y": 100, "properti": {"R":"444"}, "propertie": {"R":"555"}}
  -> R4 with R = 555          (the "properti" value is gone; nothing says so)
```

(Sent alone, `{"properti": …}` gives 333 and `{"propertie": …}` gives 222 - each is accepted as
`properties` in its own right.)

The rule wants a list of the fields it may fold into - the plurals N11 was about - rather than a rule on
the last letter.

## A2. The guard reads the arguments before the rename

`filesOutside()` takes the file a call writes from `a.value("as")` and `a.value("save_as")`.
`asPlural` runs later, in `call()`. So with "Ask Before Files Outside the Workspace" on, the two
spellings are treated differently (`p2.json`, `describe_tool` with `arguments`):

```
save_document {"as":   "/Users/…/Desktop/outside1.sch"} -> asked about each time.
                       "Outside the workspace …, it writes /Users/…/Desktop/outside1.sch."
save_document {"a":    "/Users/…/Desktop/outside1.sch"} -> let through once the tools are allowed.
                       "the guards name nothing in it"
export_image  {"save_as": …/outside2.png}               -> asked about each time
export_image  {"save_a":  …/outside2.png}               -> let through
export_netlist{"save_a":  …/outside3.cir}               -> let through
```

The write still happens (A1), so the question the user asked for is simply skipped.

It also makes a question **name the wrong files**. `export_image`'s slide export is recognised by
`isSlideExport(a)`/`slideExportFiles(a)`, which read `slides` - and `slide` is one of the 146 renames.
With a presentation open and `save_as` outside the workspace (`p36.json`):

```
export_image {"slides": "2-3", "save_as": "…/Desktop/outx.png"}
  -> asked about each time: "… it writes …/Desktop/outx-2.png, …/Desktop/outx-3.png."
export_image {"slide":  "2-3", "save_as": "…/Desktop/outx.png"}
  -> asked about each time: "… it writes …/Desktop/outx.png."
```

The user approves one file and two others are written. The overwrite check for those numbered files
(`irreversible`'s slide branch) is skipped the same way.

The **read** side goes the same way, through `filesOutside`'s `read()` list, which holds `symbols`
(`p48.json`):

```
import_netlist {"file": "bad.cir", "symbols": "…/Desktop/probe_outside.sym"}
  -> asked about each time: "… it reads …/Desktop/probe_outside.sym."
import_netlist {"file": "bad.cir", "symbol":  "…/Desktop/probe_outside.sym"}
  -> let through once the tools are allowed
```

The other keys it reads (`path`, `file`, `against`, `compare`, `dataset`, `folder`, `library`,
`measured`) do not end in "s" and are not rewritten, so `symbols` is the one.

## A3. `batch {"call": …}` hides what the batch does

Every pre-call check walks `a.value("calls")`: `irreversible()`, `askedEachTime()` and `guarded()` each
have a `batch` branch. `asPlural` rewrites `call` → `calls` only once the call is under way, so all three
see an empty list (`p3.json`):

```
batch {"calls": [{"tool":"trash_file", …}]} -> asked about, unless Claude acts on its own ("It cannot be undone")
batch {"call":   {"tool":"trash_file", …}}  -> let through once the tools are allowed
batch {"calls": [{"tool":"console","arguments":{"console":"terminal","input":"ls"}}]}
                                            -> asked about each time ("What is typed into a console runs with your rights")
batch {"call":   {"tool":"console", …}}      -> let through
batch {"call":   {"tool":"save_document","arguments":{"as":"…/Desktop/outside9.sch"}}}
                                            -> let through
```

And the call runs: `batch {"call": {"tool":"add_component", …}}` answers "1 of 1 calls done"
(`p4.json`). Nothing was executed here beyond `add_component`; the console line above was only put to
`describe_tool`, which is what reads the guard's verdict without making the call.

## A4. `run_script`'s check matches on the script's text

```cpp
if (tool == QLatin1String("run_script")) {
    const QString script = a.value(QLatin1String("script")).toString();
    return script.contains(QLatin1String("clean_scratch")) || script.contains(QLatin1String("trash_file"))
           || script.contains(QLatin1String("discard"))
           || script.contains(QLatin1String("replace")) || script.contains(QLatin1String("save_as"));
}
```

Two holes (`p9.json`, `p9b.json`):

- **The name built at run time gets past it.** `qucs.call('trash_file', …)` is "asked about, unless
  Claude acts on its own"; `qucs.call('trash'+'_file', …)` and `var t='trash_'+'file'` are "let through",
  and the script really trashed the file ("in trash": `~/.Trash/victim.sch`). Same for
  `'clean'+'_scratch'`.
- **The list leaves most of it out.** `irreversible()` itself names `console` with input, `git_remote`
  push, `git_commit` with push, `send_input`, `git_branch` force delete, `git_stash` drop and
  `git_resolve take_*`. None is in the five substrings, so
  `run_script {"script": "qucs.call('console', {console:'terminal', input:'ls'})"}` is "let through" -
  written plainly, no obfuscation needed. `batch` recurses into its calls; `run_script` could ask the
  same question of the names its script mentions, and treat a name it cannot read statically as the
  worst case.

## A5, A6, F1. A schematic half-written on disk

`mkpartial.py` writes a schematic and then truncates it to 75 %, as a crashed writer, an interrupted
`git checkout` or a half-flushed editor leaves one. Three things happen.

**A5, the tab is emptied, though the code means to prevent exactly that.** `get_schematic` after the
reload (`p13.json`): `"components": []`, where two were there, with the tab still on its file. A Save
from the window would write that over it.

`Schematic::load()` has a safeguard for this case - it keeps the document's text before clearing and puts
it back when the file does not read:

```cpp
const bool held = !a_DocComps.empty() || …;
const QString before = held ? documentText() : QString();
deleteAllElements();
…
const bool loaded = loadDocument();
if (!loaded && held) { deleteAllElements(); …; loadDocumentText(before); }
```

It did not hold here, and the reason is A6's message box. The box's `exec()` runs an event loop inside
`loadDocument()`, the watcher's timer fires in it and `load()` is entered a **second** time - by then the
elements are already gone, so the inner call sees `held == false` and has nothing to put back.
`reloadFromDisk` would stop a second attempt (`a_notReloaded`), but it writes that record only after
`reloadDocument()` returns, which is after the box. The window is then told `Reloaded::Failed`, whose
message reads "could not be read: the tab keeps what it had (a Save asks before writing over the file)" -
the opposite of what happened.

Two contrasts place it:

- **`open_document` on the same file refuses cleanly** - "cut.sch could not be opened: Form…" - with no
  box, no crash and nothing lost (`p32.json`). The opening path is careful where the reload is not.
- **A truncated `.qfem` is harmless**: the model keeps its 8 boundaries and 2 domains, no box, no crash
  (`p33.json`). `reloadDocument()`'s ZIP branch already carries `misc::ErrorCapture quiet` with the
  comment "(half written: no box - it is read when it is whole)"; the schematic branch has none.

The watcher waits 400 ms for a file written in pieces; the per-call reload added in `2c0de7e` has no such
wait, so a call can meet the file mid-write where the watcher would not.

**A6, the call that found it never returns.** The loader raises a modal warning. In `callToolFor` the
reload runs **before** `callTool` constructs `misc::ErrorCapture`, so the box is shown for real and its
`exec()` blocks inside the reload - before `call()` and before the `whileADialogWaits` list is consulted.
Reproduced twice out of two (`p16.json`): `get_schematic` sent straight after the truncation times out
(25 s, 60 s), where the same call with the box already open answers (`p15.json`, which reports
`"dialog open": "QMessageBox"`). Every later call that is not in `whileADialogWaits` queues behind it.

**F1, the crash.** 3 of 3 runs died in Release (`-11`, `-5`, `-11`); the report
(`~/Library/Logs/DiagnosticReports/qucs-s-2026-10-23-202647.ips`) is a null dereference at `0x430` under
the watcher's own timer:

```
QtCore  doActivate<false>(QObject*, int, void**)
qucs-s  Schematic::signalDocumentRebuilt(Schematic*)
qucs-s  Schematic::load()
qucs-s  QucsApp::reloadDocument(QucsDoc*)
qucs-s  QucsApp::reloadFromDisk(QucsDoc*)
qucs-s  QucsApp::reloadOneIfChanged(QucsDoc*, QFileInfo const&)
qucs-s  QucsApp::documentsChangedOnDisk()
QtCore  QTimer::timerEvent(QTimerEvent*)
```

The same probe on the ASan build (exit `-6`) names it exactly - a **heap-use-after-free**:

```
ERROR: AddressSanitizer: heap-use-after-free on address 0x61b0001238c0
    #0 Schematic::IndexedInsertion::~IndexedInsertion() schematic_element.cpp:417
    #2 std::__optional_destruct_base<Schematic::IndexedInsertion, false>::~...
freed by:
    #1 Schematic::~Schematic() schematic.cpp:145
    #2 QucsApp::closeAllFiles(int)
    #3 QucsApp::closeEvent(QCloseEvent*)
```

So the mechanism is the nested event loop, not the parse. `Schematic::load()` raises the modal warning;
its `exec()` runs an event loop; the client closes its end, the window takes a close event and
`closeAllFiles()` **deletes the Schematic** while the `load()` frame below is still live. That frame holds
an `ActualMutator` with a `std::optional<Schematic::IndexedInsertion>` (`schematic_element.cpp:3081`),
whose destructor writes `m_doc->a_insertionIndex` on the freed document:

```cpp
Schematic::IndexedInsertion::~IndexedInsertion()
{
    if (m_owner) {
        delete m_doc->a_insertionIndex;      // m_doc is gone
        m_doc->a_insertionIndex = nullptr;
    }
}
```

The chain is the watcher's own (the timer, not a tool call), so it is reachable without `2c0de7e`; that
commit adds a second way in. It is of a family with A6 of `2026-10-23-presentations-and-guards.md` (an
abort at quitting with a slide's Export dialog open): a document freed under a frame that is waiting in
a dialog's loop.

## A7. The model read again while a solve of it runs

The reload is skipped inside a batch, a script, a tune or a preview
(`a_callDepth == 0 && a_alone == 0 && a_spinning == 0`), but a FEM solve is none of those: `runsAlone()`
names only `batch`, `run_script`, `tune` and `preview`, and `fem_solve` answers through `whenJobEnds`
with the event loop running. So a call that arrives while the solve works reloads the model under it.

`racejob.py` opens `coax_rlgc.qfem` at `hmax` 0.008, sends `fem_solve` without waiting, writes
`coax.qfem` over the file two seconds in, and sends `fem_describe`. The describe answers at
`"at work": "30 % - Setting up Stationary"` - and the solve then **fails**:

```
"error": "f = 1 kHz: 2*pi*eps0*epsr/log(b/a)*1[m]: 'epsr' is not a parameter, a variable or a function here"
```

`epsr` is a parameter of the model being solved and not of the one written over it: the running solve
finished against the new model and blamed the user's own expression. A wrong result rather than a crash,
and nothing in the answer says the model changed under it.

The guard does work where it is written to work, which places the gap. `alonereload.py` sends a
`run_script` that reads the model, spins four seconds and reads it again, and writes the other model over
the file 1.5 s in:

```
script      -> "outer,dielectric,inner then outer,dielectric,inner"     (both reads: the model it began with)
queued call -> objects "shield","core"                                   (after the script: the new one)
```

So `a_alone` holds a script's ground steady, and a call behind it waits. A job started by `fem_solve`,
`fem_build` or `simulate` is not covered by any of the three counters, and that is the hole.

## A8. `rename_file` is not watched at all

`filesOutside()` names what each tool writes, tool by tool: `save_document`, `close_document`,
`export_image`, `export_netlist`, `export_data`, `get_netlist`, `import_netlist`, the synthesizers,
`create_subcircuit`, `copy_document`, `import_data`, `fem_model` and `fem_export`. `rename_file` is not
among them, and a rename takes a `to` that may be a path (`p50.json`):

```
rename_file   {"path": "a.sch", "to": "/Users/meisam/Desktop/moved.sch"}  -> let through once the tools are allowed
rename_file   {"path": "a.sch", "to": "../moved.sch"}                     -> let through
copy_document {"path": "a.sch", "to": "/Users/meisam/Desktop/copied.sch"} -> asked about each time ("it writes …/copied.sch")
save_document {"as": "/Users/meisam/Desktop/saved.sch"}                   -> asked about each time
trash_file    {"path": "a.sch"}                                           -> asked about ("It cannot be undone")
```

It is not idle: `rename_file {"path": "a.sch", "to": "../escaped.sch"}` put the file one level above the
workspace, out of it, and `list_documents` no longer shows it (`p49.json`). A rename is worse than a copy
here, because it also takes the file from where it was; the sibling tools on either side of it are both
watched.

**And it is the only one.** Every path-like field of all 133 tools was put to `describe_tool` with a
file that exists outside the workspace - 143 field/tool pairs (`p91.json`): **130 are named** by the
guard ("Outside the workspace and the open project, it reads/writes …"). Of the thirteen that are not,
twelve are not paths: `add_analysis`/`connect`/`rename_net`/`git_branch`/`zoom`/`fem_describe`/`fem_model`
`to` (a frequency, a pin, a net, a branch, a zoom target), `git_clone`/`git_remote` `url` (while
`git_clone.path` is named), `create_library`/`import_library` `destination` (a named location, not a free
path - but see **A13**, which is about reaching it anyway) and `fem_export.copy` (a boolean). `fem_model`'s `as` is named once `action` is `save`, which the
sweep left out. That leaves `rename_file`'s `to`.

The rest of the writing and reading tools were swept the same way and are **watched**:
`build_verilog_a {"file": …}` and `import_library {"path": …}` are named as reads, `export_data`,
`export_image`, `export_netlist`, `copy_document`, `save_document` and the synthesizers as writes,
`trash_file` and `clean_scratch` as what cannot be undone. Two more look unwatched and are not reachable:
`create_library`'s `destination` is `user_lib`, `project` or a configured library search path, not a free
path ("… is not a folder of the library search paths"), and `new_project` refuses a name with a slash in
it. `git_clone`'s `path` is a write target and is not in the list either - left unchecked here, since
nothing was cloned.

## A9. A short row in a Touchstone file makes up S-parameters

A 2-port Touchstone point is nine numbers: the frequency and four complex pairs. The reader takes the
numbers as one stream and cuts it into points without checking that each row carried a whole one, so one
short row shifts everything after it by however many numbers are missing. `odd.s2p`:

```
!a touchstone file
# Hz S RI R 50
1e9 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8      <- 9 numbers, a whole point
2e9 0.1 0.2 0.3 0.4                      <- 5: four are missing
3e9 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9  <- 10
```

imports as two points, with the second built from the ends of both rows (`o.dat`):

```
<dep S[1,2] frequency>
  +5.000000000000000e-01+j6.000000000000000e-01
  +3.000000000000000e+09+j1.000000000000000e-01      <- S12 = 3e9 + j0.1
```

`3e9` is the **third row's frequency**, read as a reflection coefficient; `0.4 … 0.9` are then left over,
too few for a point, and dropped - so the third frequency disappears too. The answer says only "S
parameters of 2 ports, their reference 50 Ohm" and reports 2 points: no note, no warning, nothing
`renamed` or `ignored`. A caller gets a dataset that looks ordinary and holds a reflection coefficient of
3×10⁹.

The same slicing misreads a file whose port count is not what its name says. Four rows of 1-port data
(`1e9 0.1 0.2`, …) in a file called `.s2p` come back as **one** 2-port point at 1 GHz, its four
S-parameters cut out of the other three rows' frequencies and values, with the same note and no warning
(`p80.json`).

**And it reaches the simulator.** An `SPfile` part whose `File` is `odd.s2p` netlists for ngspice with
the made-up value in its transfer table, and Check Schematic says nothing about the file (only that the
pins are unconnected and there is no ground):

```
.model xfer2 xfer R_I=true table=[
+ 1e+09Hz 0.5 0.6
+ 2e+09Hz 3e+09 0.1        <- S12 at 2 GHz, from the third row's frequency
+ ]
```

So the garbage is not confined to a dataset a reader might eye: it goes into the netlist a simulation
runs. (`p83.json`. The probe's schematic was deliberately unwired, so what the netlister does with a
**missing** `.s2p` is not shown by it - the netlist carries no instance lines at all for a circuit with
no ground.)

A row cut short is what a crashed instrument export, a truncated download or a hand-edited file looks
like, and the check is cheap: a point is 1 + 2n² numbers, and a row that does not carry them is worth
saying out loud. (`p77.json`; `good.s2p` beside it reads correctly, `S[1,1] = 0.1 + j0.2`,
`S[1,2] = 0.5 + j0.6`, at both points.)

## A10. Rows of a Touchstone file dropped without a word

A row is kept only when its frequency is above the last one kept, and what that drops is never said
(`p78.json`, the frequencies read out of each `.dat`):

| file | rows | points kept |
|---|---|---|
| `unsorted.s2p` | 3 GHz, 1 GHz, 2 GHz | **1** - only 3 GHz |
| `dupfreq.s2p` | 1 GHz, 1 GHz | **1** |
| `negfreq.s2p` | −1 GHz, 1 GHz | 2, the negative one kept |

Two of three rows of `unsorted.s2p` are gone and the answer is the same cheerful "S parameters of 2
ports, their reference 50 Ohm". A file written with its points in another order, or concatenated from two
sweeps, loses most of itself in silence; a caller who asks how many points came back is told nothing
either, since `points` is not in the answer for Touchstone. Dropping a duplicate or an out-of-order row
may well be right - saying so is the part that is missing. (And the negative frequency, which is not,
goes straight through.)

## A11. An unknown frequency unit is taken as GHz

The header's unit is read correctly where it is one of the known ones (`p79.json`, the frequencies and
`S[1,1]` read out of each `.dat`):

| file | header | row | frequency read | S[1,1] read |
|---|---|---|---|---|
| `hz.s2p` | `# Hz S RI R 50` | `1e9 0.1 0.2 …` | 1e9 | 0.1 + j0.2 |
| `ghz.s2p` | `# GHz S MA R 50` | `1 0.5 90 …` | 1e9 | 3e-17 + j0.5 (0.5∠90°) |
| `db.s2p` | `# MHz S DB R 50` | `1000 -6 90 …` | 1e9 | 3e-17 + j0.5012 (−6 dB∠90°) |
| `bad_unit.s2p` | `# BANANA S RI R 50` | `1e9 0.1 0.2 …` | **1e18** | 0.1 + j0.2 |

A unit the reader does not know falls back on a multiplier of 10⁹ - GHz - so `1e9` becomes 10¹⁸ Hz and
every frequency in the file is a billion times what it says. Nothing in the answer mentions the unit at
all: the only note is "S parameters of 2 ports, their reference 50 Ohm". The default itself is
defensible (GHz is the commonest), but an unreadable header field is exactly the thing to say out loud,
and Hz would at least be the identity.

**A file with no `#` line at all** goes the same way: `noheader.s2p`, nine numbers a row and no header,
imports as "S parameters of 2 ports, their reference 50 Ohm" with its frequencies at 10¹⁸ and 2×10¹⁸ Hz.
The port count, the format, the reference and the unit are all assumed, and the answer reports them as
though the file had said so.

**Nothing shipped is affected.** The repository holds one Touchstone file,
`qucsator_rf/tests/basic/components/spfile/R50C1pF.s1p`, and it is well formed: nine-number rows for its
port count, ascending frequencies, a `# Hz` header. A9 to A11 are about the files a user brings in - an
instrument export, a vendor model, a download - which is where S-parameter data comes from.

Taken together, A9, A10 and A11 are one reader that trusts its input: a short row makes up values from
the next row, rows out of order are dropped, an unknown unit is guessed, and none of the three is
mentioned in the answer. Beside it, the CSV reader *does* report a column renamed and a column left out
(B9, B10 are about where that reporting stops), so the habit exists in the same tool.

## A12. A missing S-parameter file empties the netlist in silence

One circuit, built twice and changed in one property - the `SPfile` part's `File` (`p84.json`,
`p85.json`). Two `Pac` ports, the part between them, everything grounded, an AC analysis:

| `File` | Check Schematic | `get_netlist` |
|---|---|---|
| `good.s2p` | 0 errors, 0 warnings, 0 notes | 38 lines: the subcircuit, its four `xfer` tables, the ports, the analysis |
| `nothing_here.s2p` (not there) | **0 errors, 0 warnings, 0 notes** | **nothing** - the header line and then an empty netlist |

`get_netlist` still answers "The netlist of spc.sch for Ngspice, as a simulation would write it now:" and
then writes no circuit at all. Nothing says the file is missing: not the netlist, not Check Schematic -
whose whole job is to find this before a run - and not the part, which keeps its property as given. A
simulation from here runs an empty netlist, and whatever the simulator then says will be about the empty
netlist rather than about the one file that is absent.

**Check Schematic already does this for two of the six file-backed parts.** The "file components"
category holds `Sub`, `SpLib`, `SPICE`, `SPfile`, `SPICE_dev` and `XSPICE_A`. The same circuit with each
pointing at a file that is not there (`p87.json`, `p89.json`, `p90.json`):

| part | its `File` | Check Schematic says | netlist |
|---|---|---|---|
| `Sub` (subcircuit) | `missing_sub.sch` | **error** - "SUB1: its subcircuit missing_sub.sch is not found (beside the schematic, in the project or …)" | empty |
| `SpLib` (SPICE library device) | `missing.lib` | **error** - "X1: its SPICE library missing.lib is not found (beside the schematic, in the project or …)" | – |
| `SPfile` (S parameter file) | `nothing_here.s2p` | nothing - 0 errors | **empty** |
| `SPICE` (SPICE netlist) | `missing.cir` | nothing - 0 errors | – |
| `SPICE_dev` (SPICE generic device) | `missing.cir` | nothing - 0 errors | written (16 lines) |
| `XSPICE_A` (XSPICE generic device) | `missing.mod` | nothing - 0 errors | written (9 lines) |

Two report it, four do not. The check has the habit, the wording and the search path already; the other
four are simply not among those it asks. A missing file is the commonest thing that goes wrong with such
a part - a project moved, a model not checked in, a path typed by hand - and for `SPfile` it costs the
whole netlist rather than one part.

## A13. A library written outside the workspace, in two steps the guard does not see

`create_library`'s `destination` is not a free path - it must be `user_lib`, `project` or **a folder on
`Locations/Library search paths`** - which is why A8's sweep set it aside. But that list is itself a
setting, and `set_settings` can add to it with the guard naming nothing (`p107.json`, `p108.json`):

```
set_settings {"scope":"app","values":{"Locations/Library search paths":"<a folder outside>"}}
  -> {"changed":[{"key":"Locations/Library search paths","now":["<outside>"],"was":[]}]}
  guard verdict: (not asked - 'values' is not among the keys filesOutside reads)

create_library {"name":"Escaped","subcircuits":["apart.sch"],"destination":"<that folder>"}
  guard verdict: let through once the tools are allowed - "the guards name nothing in it"
  -> {"file":"<outside>/Escaped.lib","library":"Escaped", …}
```

and `Escaped.lib` is there. Neither step is put to the user, so with "Ask Before Files Outside the
Workspace" on, a library still lands wherever the first step pointed. The pieces are each defensible on
their own - a settings change is not a file write, and a `destination` that must be a configured folder
looks safe - but together they are a write outside the workspace that the guard was turned on to catch.

Two ways to close it: have `filesOutside` read `set_settings`' `values` for the settings whose type is a
folder or a file (the tool already knows the type - `get_settings` answers `"type": "folders"`), and have
it name what `create_library` writes, resolving `destination` as the tool does.

## B1. A reference named for something that does not exist

`valuesAgainst()` has, for the bare-number form, "one number for one value, whatever it is named":

```cpp
QJsonValue want = reference.value(name);
if (want.isUndefined() && reference.size() == 1 && values.size() == 1) want = reference.constBegin().value();
...
met.insert(reference.contains(name) ? name : reference.constBegin().key());
```

The fallback also catches the **named** form, and `met` then marks the name as matched, so the "rows of
the reference that no value answers to" list added by the re-check of 23 October (item 4) stays empty:

```
fem_evaluate {"expression": "es.C11", "reference": {"nothing": 1}}
  -> "largest difference":"es.C11, 100 % of its reference" … "reference": 1
```

A misspelt or mistaken name is measured against silently. The bare number already arrives under an empty
key, so the fallback could be restricted to that.

## B2. A region with no text says the PDF is scanned

```
read_pdf {"path":"paper.pdf","pages":2}                              -> 2,600 characters of text
read_pdf {"path":"paper.pdf","pages":2,"region":[1,1,1.0001,1.0001]}
  -> "of the region": "nothing: no text of the page is in it"
     "note": "The pages have no text (scanned): 'image' draws them."
```

The note is drawn from the returned text being empty, without knowing a region narrowed it. On a paper
whose text the same call returns in full, a caller is told to draw the page as a picture (`p10.json`,
`p11.json`).

## B3. A geometry size of 1e308, nan or inf taken without a word

```
fem_edit add rectangle {"size": ["1e308","1e308"]} -> added r1
fem_edit add rectangle {"size": ["nan","inf"]}     -> added r2
fem_describe tree                                  -> "geometry error":"r2: nan: 'nan' is not a parameter…"
```

The edit answers as if it were fine; the model is broken from then on, and `1e308` is not reported at
all. (Of a family with G1/M1 of `docs/bug_hunts/2026-10-21-fuzz.md`, which bounded the casts further in.)

## B4. A plot group shown again rewrites its display

Wishlist N5 keeps the user's own axis words when a plot group is shown again. The rewrite moved the three
lines that follow out of the "a label differs" condition:

```cpp
if (own) {
  if (same->yAxis.Label != yLabel) { … }
  if (same->xAxis.Label != xLabel) { … }
  same->updateGraphData();
  shown->setChanged(true, true);     // now every time
  shown->viewport()->update();
}
```

and `showGraphs` saves any display that reports changes
(`if (… == QFileInfo(dpl) && d->getDocChanged()) d->save();`). So each `fem_plot` of the same group
writes the `.dpl` again although nothing changed - `bratu_pg1d1.dpl`, 588 bytes, over three re-shows
(`p24.json`): `20:35:20`, `20:35:22`, `20:35:24`. This process is not disturbed by its own write (the
save moves `lastSaved` with it, so no "changed on disk" note follows - checked, `p28.json`), but git sees
the file modified at every plot, and another Qucs-S or another conversation watching the folder sees it
change.

## B5. A count of zero is taken and ignored

`fem_converge`'s `'tolerance'` is bounded (above 0, below 1) and its `'refine'` is bounded (above 1, at
most 100, 5 meshes at most), but `timeout: 0` is accepted and the solve runs to the end regardless.
`tune {"max_runs": 0}` is the same: it still runs the range's two ends. `find_shapes {"limit": 0}` and
`{"limit": -5}` list **one** shape of the four there are (its note does say "1 listed of 4"). None is
harmful - each answers with what it did - but a count of zero is either a refusal or means none, and in
all three it means something else.

## B6. `line_calc` answers `nan` where it should refuse

An unphysical geometry or substrate is computed rather than turned down, and the answer carries the
result as text (`p41.json`):

| call | answer |
|---|---|
| microstrip `h: 0` | `"ErEff": "nan "`, `"Conductor Losses": "nan dB"`, `"Dielectric Losses": "nan dB"` |
| microstrip `w: 0` | the same |
| microstrip `er: 0` | the same |
| microstrip `er: -5` | the same |
| microstrip `w: 1e300, h: 1e-300` | the same |
| microstrip `f: 0` | `"Skin Depth": "inf m"`, losses 0 |
| coax synthesize `z0: 0` | `"Conductor Losses": "inf dB"` |

A caller reading `ErEff` gets the text "nan", not an error, so a wrong number flows on into whatever it
is doing. A height, a width and a permittivity of zero or less have no line to compute.

`line_calc` is the odd one out: every synthesizer beside it both checks its inputs and refuses a design
that comes out as nan (`p44.json`, `p45.json`) -

```
synthesize_attenuator {"attenuation": 0}          -> "No attenuator: 'attenuation' is in dB, above 0"
synthesize_attenuator {"z_in": 0}                 -> "'z_in' and 'z_out' are in ohms, above 0"
synthesize_attenuator {"attenuation": 1e300}      -> "The design has a value that is no number ("nan Ohm")"
synthesize_matching   {"z_load": [0, 0]}          -> "The design has a value that is no number ("nanF")"
synthesize_power_combiner {"z0": 0} / {"f": 0} / {"ways": 0}
                                                  -> each refused by name
```

so the message and the habit are already there to borrow.

## B7. `receiver_budget` takes impossible conditions

(`p41.json`, `p42.json`.) The stages themselves are checked - an empty list is refused with what a stage
takes - but the conditions are not:

| call | answer |
|---|---|
| `temperature: -500` (below absolute zero) | every system number `null`: sensitivity, mds, snr, sfdr, the noise floor and density, blocking dynamic range |
| `bandwidth: -1` | six system numbers `null` |
| `bandwidth: 1e300`, a stage of `gain/nf: 1e300` | the bandwidth and most of the stage's own numbers `null` |
| a stage with `nf: -5` (a noise figure below 0 dB) | **a complete budget, nothing null, nothing said** |

The `null`s are nan and infinity as JSON writes them, so the answer looks like a budget with holes in it
rather than a refusal; the last row is worse, since an impossible stage gives numbers that read as
sound.

## B8. An empty action name fires the first menu action

```
trigger_action {"action": "nothing at all"} -> "There is no action nothing at all (list_actions lists them)."
trigger_action {"action": ""}               -> "New: done."   (a document was made)
```

`actionNamed()` matches either the menu path or the action's object name:

```cpp
if (normalized(paths.at(i)) == normalized(wanted) || actions.at(i)->objectName() == wanted) return actions.at(i);
```

Most `QAction`s never have an object name set, so `""` equals the object name of the first one in the
menus and that action is triggered. Today that is File > New, which only costs an untitled document, but
nothing about the match makes it so - it is whichever action the menu bar lists first without an object
name, and `trigger_action` reaches every menu item there is. An empty `action` wants the same refusal an
unknown one gets. (The one comparison of its kind in the tools; `set_ui`, `context_menu` and
`send_input` each refuse an empty or unknown target by name.)

## B9, B10. What `import_data` says about its columns

Wishlist N2 is that an input read under another name is said. It is, for the ordinary case - a header
with units and a column of words (`p73.json`):

```
meas.csv  columns ['Frequency', 'V_out', 'V_out_2']
          renamed {"Frequency (Hz)": "Frequency", "V out (V)": "V_out_2"}
          notes   ["The column ignored text has no numbers: left out."]
```

Three files of three lines each separate what goes wrong (`p75.json`):

| file | header | rows | columns read | `renamed` | `notes` |
|---|---|---|---|---|---|
| `plain.csv` | `a,b,c` | 3, 3 | `a, b, c` | – | – |
| `dup.csv` | `a,a,a` | 3, 3 | `a, a_2, a_3` | `{"a": "a_3"}` | – |
| `ragged.csv` | `a,b,c` | 3, **2**, **4** | `A, B, C, D` | – | – |

**B9, the ragged file.** The widest row decides how many columns there are, and when the header does not
name that many it is dropped whole: `a,b,c` is gone and the data comes back under `A`, `B`, `C` and a
fourth column `D` that no header mentions. Neither `renamed` nor `notes` says anything, so a caller
plotting `r:A` has no way to know it is the column the file calls `a` - or that the file's own names were
thrown away. A short row is the ordinary shape of a CSV cut short by a crash or a trailing comma, so
this is not an exotic input. Compare `meas.csv`, where a column that holds no numbers *is* reported as
left out.

**B10, two columns of one name.** `dup.csv`'s three `a` columns become `a`, `a_2` and `a_3`, which is
right, but `renamed` is keyed by the **original** name, so the three collapse into one entry and only
the last is reported: `{"a": "a_3"}`. The caller is told `a` became `a_3` and never hears about `a_2`.
The report wants to be a list of pairs, or keyed by the new name.

## Minor notes

- **N1** "a rule for boundarys is all, numbers, of …": the plural made by adding "s" to "boundary".
- **N2** `fem_evaluate {"reference": "1e-10"}` is read as a file name - "…/ws/1e-10 could not be read".
  A number sent as text is not converted here, because `reference` has no single JSON type.
- **N3** `fem_evaluate` of `1e300*1e300` and `0/0` answers `"SI": null` beside `"value": "inf"` / `"NaN"`.
  A caller reading `SI` sees nothing rather than the value's kind.
- **N4** `read_pdf` refuses a region of `[0,0,0,0]` but takes `[1,1,1.0001,1.0001]`.
- **N5** "Open in Viewer" opens whatever an image in the conversation points at, with no question - a
  file outside the workspace included, where `open_document` would ask. It is the reader's own click on
  a picture Qucs-S drew, so this is a note, not a finding.
- **N6** A `timeout` given as text is not seen by `waitsFor()`, which reads it with
  `QJsonValue::toInt(default)` - a string gives the default back, not the number. `asNumbers` converts
  the value later, for the tool, so a call through `callNow` (a script's inner call) would be waited for
  by the default's time and not the one asked - `qucs.call('fem_solve', {timeout: '7200'})` cut at the
  default's 610 s while the solve runs on. Read from the code, not measured: it needs a solve longer than
  ten minutes to show. The `--mcp-server` and dock paths do not wait this way.
- **N7** The "Meant …?" hint can be wide of the mark: `add_analysis {"type": …}` answers "It takes
  analysis, from, kind, ... Meant y?".
- **N8** `read_pdf` takes `[3,4]`, `"2-5"` and `7` but refuses `"all"` ("paper.pdf has pages 1 to 7"),
  which `read_pptx`'s `slides` accepts (B4 of `2026-10-23-presentations-and-guards.md` is about `"all"`
  there being cut at 500 slides); neither description mentions it. A range past the end (`"1-99999"`) is clamped in silence
  while a single page past it (`"0"`, `-1`) is refused with "paper.pdf has pages 1 to 7".
- **N13** A Qucs `.dat` whose declared count does not match its data (`<indep frequency 5>` over two
  values, a `<dep>` of three) is read down to what is there - two points, `final` 2, the min and max
  right - without the mismatch being mentioned. Unlike A9 nothing is invented, so this is a note.
- **N12** A Touchstone reference impedance of 0 Ω is taken as given - "their reference 0 Ohm" - where
  every conversion from S parameters divides by it.
- **N10** `add_trace {"diagram": 1, "variable": "nothing at all"}` is **added**, with a `"no data"` line
  saying the dataset has no `ac.v(nothing at all)` - where `add_trace` with a diagram that does not
  exist and `add_marker` with a trace that does not exist are both refused by number. Defensible (a
  trace for a variable a later run will produce) and said in the answer, so a note.
- **N11** `add_marker {"at": [1000]}` - `at` as a list - answers "Say where: 'at', an x value or peak,
  min, -3dB, crossing:<y>", as though `at` had not been given. `fem_evaluate`'s `at` is a list, so the
  shapes differ between tools.
- **N9** A place far out is clamped without a word. `set_schematic` `{"x": 2147483647}` comes back as
  8388608 (its pin at 8388578), ±2²³; `add_diagram {"x": 1e300, "y": 1e300}` comes back at `"x": 0`. A
  zero-length wire is dropped the same way ("wire count": 0). The clamp can reach a message:
  `probe {"what": [1e300, 1e300]}` is refused with "There is nothing to probe at **0, 0**", naming a
  place the caller did not ask about. The rest of `set_schematic`'s checks are
  right: empty text, text with no section, and two parts of one name all answer "Not changed" and leave
  the document alone.

## Checked and found right

- **The coercions chain and are both said.** `undo {"step": "1"}` → "step was read as steps. steps was
  given as text ("1") and read as the number it holds". `get_settings {"key": …}` answers with
  `"renamed": {"key": "keys"}`.
- **Bounds.** `undo`/`redo` `steps` 1 to 1000; `fem_converge`'s `refine` and `tolerance`; a region of 3
  or 5 numbers, a region outside the page, a region corner given as `"NaN"`.
- **A reversed region is normalised**: `[495,376,117,343]` reads the same three lines as
  `[117,343,495,376]`.
- **Selection rules** (wishlist N2): a second rule in one selection is named and refused ("all was read,
  and of of the same selection left out"), and `objects`/`only` at boundary level are refused with the
  rules that level takes.
- **An over-nested request** (a batch inside itself 400 deep, past Qt's 1024 levels) answers
  `-32700 Parse error: too deeply nested document` with a null id, as JSON-RPC asks; a batch inside a
  batch is refused at any depth ("A batch cannot hold another batch").
- **The transcript's context menu.** `customContextMenuRequested` gives the point in the widget's
  coordinates and `cursorForPosition` wants the viewport's; they agree here because the view is
  `QFrame::NoFrame`, so the picture under the pointer is found and the menu opens in the right place.
- **A valid model written over an open one** is read again by the very next call, at a higher revision,
  with the note in the same answer - the fix of `2c0de7e` behaving as intended.
- **`set_ui` and `get_ui`.** An `area` that is not one of the window's answers with the list it takes
  (`dock:<title>`, `toolbar:<title>`, `statusbar`, `tabs`, `document`), and a `press` that names nothing
  the same way.
- **The outside-files guard's coverage.** 130 of the 143 path-like field/tool pairs over all 133 tools
  are named by it, including every `path`, every `save_as`, the git tools' `path`, `diff`'s `against`,
  `get_dataset`'s `compare`, `fem_plot`'s `measured` and `import_netlist`'s `symbols`. A8 is the one
  genuine gap the sweep found.
- **The reload's guard inside a call that runs alone.** A `run_script` keeps the model it began with for
  all of its own calls while the file is written over mid-script, and the call queued behind it gets the
  new one (`alonereload.py`). That is what A7 asks for an async job.
- **The `xml` tool.** A node path that climbs out (`/root/../../etc/passwd`) and an index past the end
  are refused with "outline gives the paths there are"; `<bad>` and `1bad` are refused as element and
  attribute names; `set` on an element says only an attribute's, a text's or a comment's value is
  edited; `depth`/`max` of 999999 answer at once. An entity-expansion bomb (552 bytes declaring 10⁹
  characters) comes back as "Entity expands to more characters than the entity expansion limit" in
  0.0 s with memory unmoved.
- **The diagram axes.** `add_diagram`/`edit_diagram` refuse `from` equal to or above `to`, a logarithmic
  axis from 0 or below, `decimals` outside -1 to 15 and a `notation` that is not one of the six, each by
  name and with what it takes.
- **The tools cannot switch off the guards that watch them.** `set_settings` with
  `{"ClaudeCode/askOutsideWorkspace": false}` answers "no such setting (get_settings lists them …)" and
  changes nothing, and `get_state` still reports both guards on. The two keys the Extras menu writes are
  not among the settings the tool can reach, which is the right side of that line to be on.
- **The git tools outside a repository.** `git_init` with no folder and no open project refuses with what
  it needs ("Which folder? ('path'; the open project's when one is open)"), and `git_status`,
  `git_commit`, `git_log`, `git_show`, `git_branch` and `git_tag` each say the workspace "is in no git
  repository: git_init makes one" rather than failing obscurely. `git_stage {"files": …}`,
  `git_blame {"line": …}` and `git_diff {"against": …}` are refused with the fields they do take
  (`paths`, `from`, `of`) - the argument check doing its work on a family of 20 tools.
- **`create_subcircuit` and `make_symbol`.** An empty `names` is refused ("'names' are the components
  that go into the subcircuit"), a name that is not there is refused, and once the parts have gone into
  one subcircuit a second call correctly says they are no longer in the schematic. `make_symbol` on a
  file that is not open says how to open it, and a wrongly shaped `sides.left` is refused by name.
  Nothing written outside the workspace by either.
- **The layout tools on a real GDS.** `thermo_optic_heater.gds` opens as a layout; `find_shapes` lists
  its four shapes with bounds, layer and points, notes when it lists fewer than it found, takes a
  `depth` and a `region` of ±1e300 without trouble, and refuses a layer that is not there with the list
  ("The layers: 1/0, 11/0, 12/0"), as does `show_layout`.
- **Net naming, and an old serious fix holding.** A second `set_label` on the same net **replaces** the
  first - the netlist goes `R1 0 mid | R2 mid 0` then `R1 0 second | R2 second 0`, one name to a net -
  which is what A1 of `2026-09-30-data-and-wishlist.md` (a net with two names netlisted as two nodes)
  was about. `rename_net` from a name that is gone is refused with "get_schematic lists the nets", and to
  `gnd` or `0` with "gnd is ground's name: a net so called is ground in the netlist, shorted to it".
- **`probe`.** A pin, a part, a net label, a place and a list are each taken or refused by name: "R1.99"
  → "R1 has no pin 99 (it has 2, by number)", a name that is nothing → the forms it accepts, `[]` → "A
  place is [x, y]", a diagram past the last → how many there are. Pin 2's current comes back as
  `r1_pin2 = -@r1[i]`, the negation of pin 1's, which is what A5 of
  `2026-10-08-new-diagrams.md` was about.
- **Non-finite values in imported data.** A CSV holding `nan`, `inf` and `-inf` imports with them kept
  verbatim in the `.dat`, and what reads them behaves: a trace takes all five points, a log x axis
  reports "1 point(s) at or below 0, which a log axis has no place for", and `add_marker` with `peak`
  and `min` finds 2 and 1 - the finite extremes, skipping the infinities - with no crash. (`import_data`
  also refuses a dataset name that a simulation of the open schematic would later write over: "n.sch is
  there, and a simulation of it writes n.dat over the import. Choose another name.")
- **`export_data` → `import_data` round trips.** A real 200-point AC run exported to CSV
  (`frequency, ac.gain_db`, full double precision - `696000,-13.511042613268163`) and imported back gives
  both columns under their own names, 200 points, nothing renamed and no note needed.
- **A truncated Qucs dataset is reported, not guessed.** With the `.dat.ngspice` of a real ngspice run
  cut to 60 %, `reload_data` answers `"traces with data": 0` and lists each trace under
  `"traces without data"` with why ("AC_Passive_analysis.dat.ngspice has ac.gain_db, but …"). No crash,
  nothing invented - the opposite of A9 to A11 on the same kind of input.
- **`tune` never applies what it could not verify.** A backwards `range` ([2, 1]) runs both ends and
  stops with "The target is not between what the range's ends give … widen the range"; a `measure`
  naming a variable the dataset has not, or a `what` that is not a measurement, stops with the list of
  what there is; in every case "R is 800 as it was" and the part is untouched.
- **`preview` leaves everything alone.** `set_schematic`, `arrange`, `add_component` and `delete` each
  with `"preview": true` answer `"changed": false` beside the answer the call would have given, and the
  schematic stays as it was (two parts, no unsaved changes, the same revision) - including `delete`,
  whose preview says "Deleted R1." inside `its answer` and changes nothing.
- **Bounds and refusals across the tools not otherwise touched.** `export_data` without `diagram` or
  `variables`, or naming a variable the dataset has not; `export_image` `scale` outside 0.1 to 20;
  `move_to_pane` past the last pane; `read_help` with words that match nothing; `reload_data` on a real
  dataset (4 traces) - each answers by name with what it takes or what there is.
- **The fuzzers.** 16,500 calls over 29 of the day's tools across four seeds (`fuzz_day.py`): 6,500 in
  Release and 10,000 under ASan and UBSan, the last pass putting the window back in a usable state every
  120 calls so the calls after it reach past "nothing is open". No crash, no sanitizer report, no call
  over 8 s, no answer that was neither an answer nor a refusal; memory flat (152 → 180 MB in Release,
  about 1 GB under ASan).
  `scripts/ci/fuzz-sch.py`, 700 mutants of 252 examples, 1,400 headless runs under ASan, seed 2310: no
  failure.
- **The per-call reload under load.** 280 calls pipelined (sent without waiting) while a thread wrote one
  of two models over the open file every 30 ms, in Release and again under ASan and UBSan
  (`stress_reload.py`): all answered, no crash, no sanitizer report, a clean exit. With a **valid** file
  the reload holds up; A5 to A7 are about a damaged one and about a job already running.
- **A batch's and a script's inner arguments get the same checks.** An unknown key inside
  `batch {"calls": [...]}` or inside `qucs.call(...)` is refused with the tool's own list, a number as
  text is converted, and a singular is read as its plural. Nested objects too: `add_diagram`
  `{"traces": [{"bogus": 1}]}` and `{"x_axis": {"bogus": 1}}` name the field and what it takes.
- **`edit_text` changes nothing when an edit cannot be made.** A `find` that matches three places is
  refused with the count ("give more of the text around it, or 'all': true"), an empty `find`, a
  `lines` range past the end, a negative one and a backwards one are each refused by name, and the text
  is untouched in every case; `all: true` changes all three and makes one undo step. `goto_line` past
  the end or below 1 says how many lines the file has.
- **The synthesizers' own checks.** `synthesize_attenuator`, `synthesize_matching` and
  `synthesize_power_combiner` each refuse an attenuation of 0 or less, an impedance of 0 or less, a
  frequency of 0, a way count their topology has not, and a design whose values come out nan - by name,
  before anything is drawn. (Which is what B6 asks of `line_calc`.)
- **Ground reuse** (wishlist N9). Three parts grounded in turn near one another share one ground node and
  one net: `R1 _net0 0`, `R2 _net0 0`, `V1 _net0 0`, and Check Schematic finds no error.
- **`import_netlist` drops a `.control` block.** A `.cir` carrying `.control` / `shell echo hello` is
  imported as a schematic whose netlist includes only the generated `imported_subcircuits.lib` (the
  `.subckt`); neither the schematic nor the include holds the shell line, so the commands guard naming
  nothing in `simulate` of it is correct. Nothing was simulated to check this - the netlist and the
  written files were read. (A route worth knowing beside A1/A7 of
  `2026-10-23-presentations-and-guards.md`.)

## Not covered

- **`get_dataset`'s `points`, `harmonics` and `periods`.** `points: 0`, `-5` and `2147483647` are all
  accepted without a word, but the dataset reached here (`examples/templates_ngspice/AC_Passive_analysis.sch`
  through the real ngspice) gave one point, so whether the count is clamped, ignored or honoured was not
  established. Data in and out was the subject of `2026-09-30-data-and-wishlist.md`.
- **`git_clone`'s `path`.** A write target absent from `filesOutside`'s list (A8's family); nothing was
  cloned, so it is unverified.
- **The Claude panel's own surfaces.** The Extras menu and "Open in Viewer" were read, not driven: the
  context-menu coordinates were checked by reading (`QFrame::NoFrame`, so widget and viewport agree) and
  the entry's own test already exists in `test_claude_code`. A GUI pass on the menu would need a window.
- **The file browser's folders** (`63c2d2c`, committed today) were not touched - not part of the tools.
- **`build_verilog_a`'s compile.** With an isolated settings directory OpenVAF is not configured
  ("OpenVAF is not set: Application Settings > Locations > OpenVAF Path"), so a broken `.va` never
  reached a compiler. The refusals around it are right - a missing file is named, a bad `timeout` does
  not change the refusal.
- **`scripts/ci/fuzz-simout.py`** was not run: its corpus is what the `simulate` smoke suite leaves
  behind (`<work>/<circuit>/simout/` of real ngspice output), and that suite was not run here. It is the
  fuzzer that would push hardest on the readers A9 to A11 are about.
- **The Touchstone reader's other formats.** `# Hz/MHz/GHz` with `RI`, `MA` and `DB` were checked and
  read correctly; `.s3p` and up, noise-figure rows, and the `[Version]`/`[Number of Ports]` keywords of
  Touchstone 2.0 were not.
- **Nothing was simulated with a schematic that carries commands**, and no console line was run: the
  commands findings (A3, A4) rest on `describe_tool`'s verdict for the call, which is what reads the
  guard without making it.

## Reproducing

Everything is in `2026-10-23-the-days-tools/`. `mcpcalls.py <app> <home> <probe.json> [deck…]` plays one
probe through the real server and prints each answer with its time and the server's RSS; a probe's
`__copy` and `__run` steps write or run something between calls.

```sh
cd docs/bug_hunts/2026-10-23-the-days-tools
APP=../../../build/qucs/qucs-s.app/Contents/MacOS/qucs-s
python3 -B -I list_tools.py $APP /tmp/w > tools.json          # the 133 schemas
python3 -B -I mcpcalls.py $APP /tmp/w1 p1.json                # A1: the one-letter write
python3 -B -I mcpcalls.py $APP /tmp/w2 p2.json                # A2: the guard's two verdicts
python3 -B -I mcpcalls.py $APP /tmp/w3 p3.json                # A3: batch {"call": …}
python3 -B -I mcpcalls.py $APP /tmp/w4 p9b.json               # A4: the script's name built up
python3 -B -I mcpcalls.py $APP /tmp/w5 p16.json               # A5/A6: the half-written schematic
python3 -B -I racejob.py  $APP /tmp/w6 /tmp/slow.qfem /tmp/coax.qfem   # A7: the solve's model swapped
python3 -B -I fuzz_day.py $APP /tmp/w7 2500 1 fuzz1.log       # the day's tools fuzzed
python3 -B -I nestprobe.py $APP /tmp/w8 50 300 400            # the nesting limit
```

`/tmp/slow.qfem` is `examples/multiphysics/coax_rlgc.qfem` with `hmax` 0.05 → 0.008 and 0.025 → 0.004,
so its solve takes long enough for a second call to arrive; `/tmp/coax.qfem` is
`examples/multiphysics/coax.qfem`, which has no `epsr`.
