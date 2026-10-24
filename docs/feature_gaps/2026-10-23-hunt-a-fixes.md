# The A-class findings of the evening hunt, fixed

The hunt of 23 October's evening (`docs/bug_hunts/2026-10-23-the-days-tools.md`) left 24 findings, none
fixed. The user: "Start fixing the A-class findings (A1-A13 and F1), in the order you think best." All
fourteen are done, in risk order: the crash and the data loss first, then the permission holes, then the
readers.

| | What it was | What it is now |
|---|---|---|
| **F1** | Heap-use-after-free reloading a truncated schematic | No modal box is raised from a reload, so no nested event loop and nothing is freed under the loading frame; `load()` also turns down a load entered from inside one |
| **A5** | The tab came out empty although `load()` keeps what it held | It keeps it. The re-entrant load that defeated the safeguard is gone, and a failed load puts the revision and the edits back |
| **A6** | The call that met the file never returned | It answers. The loader's complaint is taken, said in the status bar and told to the conversations |
| **A1** | A trailing "s" stripped from any field, so `{"a": "x.sch"}` wrote a file | A singular is folded only into a field that is a plural of it: English rules, no flags, no names that are not plurals, and two keys for one field refused |
| **A2** | The guards read the arguments before the rename | The three of them read what the tool will read |
| **A3** | `batch {"call": …}` hid every inner call from all three | It does not |
| **A4** | `run_script`'s check was five substrings | Every tool that cannot be undone is looked for, and a call whose name is not written out is taken as the worst case |
| **A7** | A model was read again under its own solve | A document with a job running is left until the job is over |
| **A8** | `rename_file` moved a file anywhere, unwatched | Both ends of a rename are named |
| **A13** | A library reached a folder outside the workspace in two unwatched steps | Both steps are named |
| **A9** | A short Touchstone row made up values from the next row | A file that does not hold whole points is refused, with the count |
| **A10** | Rows that do not rise were dropped in silence | Said, where they are not a two-port's noise data |
| **A11** | A frequency unit it could not read was taken as GHz | Refused; a file with no option line says the defaults it took |
| **A12** | A missing file went unreported for `SPfile` and `SPICE` | Reported by name, as `Sub` and `SpLib` already were |

## F1, A5, A6: one cause, three symptoms

All three came of a **modal box raised from inside a reload**. The loader complains about a damaged or
half-written file through `misc::reportError`, which shows a box; its `exec()` runs an event loop inside
`reloadDocument()`, and from there: the window could take a close event and `closeAllFiles()` freed the
Schematic under the frame that was loading it (F1, the use-after-free in
`Schematic::IndexedInsertion::~IndexedInsertion`); the watcher could fire in that loop and enter
`load()` a second time, by which point the elements were gone, so the inner load saw nothing to keep and
the outer one had nothing to put back (A5); and a tool call whose own reload raised the box never got
past it (A6).

`QucsApp::reloadOneIfChanged()` now takes what the loader says instead of showing it:

```cpp
QString why;
Reloaded how = Reloaded::AsBefore;
{
  misc::ErrorCapture quiet;
  how = reloadFromDisk(doc);
  why = quiet.errors().join(QStringLiteral("; "));
}
```

`reloadDocument()`'s ZIP branch already did this for a half-written archive - "no box, it is read when it
is whole" - and the schematic branch simply did not. What the loader said is now in the status bar and,
new, told to every conversation: a tool call used to see nothing of a failed reload at all.

**The user still gets the box.** It is the right thing to show someone at the keyboard - and
`test_git_integration`'s `aFailedReloadPutsTheDocumentBack` holds Qucs-S to it, one box a change with
the loader's words in it. So the message is not thrown away but **queued**: it opens once the reload has
finished unwinding, not from inside the load.

```cpp
if (!why.isEmpty() && (control == nullptr || !control->isHeadless()))
  QTimer::singleShot(0, this, [why] { misc::reportError(why); });
```

Where there is no user - `--mcp-server`, which sets `setHeadless(true)` - no box is opened at all: one
there is answered by nobody, and every later call that is not in `whileADialogWaits` would queue behind
it. Those conversations get the note instead.

Two more guards behind it. `Schematic::load()` turns down a load entered while one is running
(`a_loadingDocument`), so no nested load can clear what the outer one is keeping. And where the file does
not read, the document ends up holding exactly what it held, so its revision and its recent edits are put
back with `rewind()` - a conversation was being told "changed by its file … and loaded again" for a load
that failed.

```
get_schematic after the truncation:  R1, C1          (was: none)
                                     no dialog open   (was: a QMessageBox nobody could answer)
                                     answers at once  (was: the call's whole timeout)
exit code over three runs:           0, 0, 0          (was: -11, -5, -11)
Since your last call: half.sch was changed on disk but could not be read (Format Error: Wrong
'component' line format!), so the tab keeps what it had - which is not what the file holds.
```

## A1: a singular folded only into a plural of itself

The rule was "strip a trailing s or es" - which invents an argument out of any field whose name merely
ends in "s". It is the other way round now: the plural of the **key** is worked out and looked for among
the fields.

```cpp
QString QucsControl::pluralOf(const QString& word)
{
    if (word.size() < 3) return {};   // 'a' for 'as', 'v' for 'vs', 'f' for 'fs'
    if (word.endsWith(QLatin1Char('y')) && word.size() > 3) return word.chopped(1) + QLatin1String("ies");
    for (const QLatin1String end : {"s", "x", "z", "ch", "sh"})
        if (word.endsWith(end)) return word + QLatin1String("es");
    return word + QLatin1Char('s');
}
```

with three more rules in `asPlural()`: a field in a small list of names that are not plurals of anything
(`as`, `save_as`, `keep_as`, `axis`, `x_axis`, `analysis`, `press`, `thickness`, `species`, `nichols`, …)
is never folded into; a **flag** is never a list of one, so a boolean field is left alone; and two keys
that would fold into one field are refused rather than one of them being thrown away.

| call | before | now |
|---|---|---|
| `save_document {"a": "snuck.sch"}` | **wrote snuck.sch** | "takes no a", nothing written |
| `simulate {"allow_command": true}` | turned on running the schematic's commands | "takes no allow_command" |
| `export_image {"save_a": …}` | wrote the file | refused |
| `synthesize_filter {"f": 2e9}` | designed to a stop band never given | refused |
| `fem_describe {"v": …}`, `add_trace {"axi": …}` | folded | refused |
| `{"properti": …, "propertie": …}` | one silently dropped | refused |
| `delete {"name": "R1"}`, `undo {"step": "1"}`, `get_settings {"key": …}`, `batch {"call": …}`, `read_pdf {"page": 1}`, `receiver_budget {"stage": …}` | folded | **folded, as N11 asks** |

## A2, A3: the guards read what the tool reads

`guarded()`, `irreversible()` and `askedEachTime()` each normalise their own arguments now
(`asTheToolReadsIt()`, which applies the same folding and number reading without saying anything), so
every caller is covered - the server, the dock, `describe_tool`, and each step down a batch. Before, they
read `as`, `save_as` and `calls` as they came, so the rename that happened later in `call()` was invisible
to them.

```
batch {"calls": [{"tool":"console", …}]}  -> asked about each time   (unchanged)
batch {"call":   {"tool":"console", …}}  -> asked about each time   (was: let through)
batch {"call":   {"tool":"trash_file", …}} -> cannot be undone      (was: let through)
```

With A1 in place the file-writing half of A2 cannot arise any more - `a` and `save_a` are not read at all
- but the normalisation is what keeps the folds that remain (a list named in the singular, a number as
text) from hiding anything, and it is what fixes A3.

## A4: what a script may do

`irreversible("run_script", …)` searched the script's text for five words. It now looks for every tool
that cannot be undone or that the user is asked about each time - `console`, a git push, `send_input`,
`git_discard`, `git_stash` drop and the rest, which were never looked for at all - and, since a script
makes its calls at run time, a `qucs.call(` whose tool is **not a plain literal** is taken as the worst
case:

```cpp
static const QRegularExpression aCall("\\bcall\\s*\\(");
static const QRegularExpression aLiteral("^\\s*(['\"])[A-Za-z_][A-Za-z_0-9]*\\1\\s*[,)]");
for (auto it = aCall.globalMatch(script); it.hasNext();)
    if (!aLiteral.match(script.mid(it.next().capturedEnd())).hasMatch()) return true;
```

`'trash'+'_file'` and a name in a variable are asked about now; a script that only looks
(`qucs.call('get_state', {})`, arithmetic) is still let through. Erring this way costs one question before
a script runs, which is where the user is asked about it anyway.

## A7: nothing read again under its own job

The reload's guard counted a batch, a script, a tune and a preview. A job is none of those: `fem_solve`,
`fem_build` and a background `simulate` answer through `whenJobEnds` with the event loop running, so a
call arriving meanwhile read the file and swapped the model under the solve - which then failed blaming
`epsr`, a parameter of the model it had begun with. `QucsControl::busyWithAJob()` answers for both kinds
(a `MultiphysicsDoc`'s `isBusy()`, a `SimRun` of that schematic still going) and
`reloadOneIfChanged()` leaves such a document alone, saying so in the status bar; the next call reads it
once the job is over.

```
fem_solve of coax_rlgc at hmax 0.008, coax.qfem written over it 2 s in:
   the describe answers at "30 % - Setting up Stationary"
   the solve: derived values, no error        (was: "'epsr' is not a parameter …")
```

## A8, A13: the ways out of the workspace

`filesOutside()` names what each tool writes, tool by tool, and three were missing. `rename_file` now
names both ends - the file leaves where it was and lands where `to` says, which may be a path.
`create_library` names the `NAME.lib` it writes in the folder `destination` gives. And `set_settings`
names any value that looks like a path, because `Locations/Library search paths` is what
`create_library`'s `destination` must be a member of - the two steps of A13, each unwatched, that put a
library outside the workspace with no question.

## A9, A10, A11: a Touchstone file is not guessed at

The reader took every number of the file as one stream and cut it into points, so one short row shifted
everything after it. It now checks that what it read comes out whole:

```cpp
if (!stopped && read != numbers.size())
    return fail(tr("The Touchstone file does not hold whole points: %1 numbers for a %2-port file, where each "
                   "point is %3 …, so %5 are left over. A row of it is short or long …"));
```

`stopped` is the two-port noise-data case, where numbers are left on purpose - a legitimate file with
noise rows still reads, which the suite's own `touchstoneIsRead` holds to. Where the frequency stops
rising and what is left is **not** a multiple of five (so it cannot be noise data), the rows dropped are
said. A frequency unit the option line names and the reader does not know is refused rather than taken
for GHz; a file with no option line at all says which defaults it took; and a reference impedance of
nought is pointed out.

| file | before | now |
|---|---|---|
| a row of 5 where 9 are wanted | S12 at 2 GHz = 3e9 (the next row's frequency) | refused, "24 numbers … 6 are left over" |
| 1-port rows under an `.s2p` name | one made-up point | refused |
| 3 GHz, 1 GHz, 2 GHz | 1 point, nothing said | 1 point, and what was left out said |
| `# BANANA S RI R 50` | 1e9 read as 1e18 Hz | refused, "does not say a frequency unit we read" |
| no `#` line | 1e18 Hz, nothing said | the standard's defaults, said |
| `R 0` | nothing said | "no conversion from S parameters can divide by" |

## A12: a missing file, for every part that has one

Check Schematic reported a missing file for `Sub` and `SpLib` and for nothing else. The same check is now
made for `SPfile`, `SPICE`, `Ifile`, `Vfile` and `Verilog`, with what it costs said: an `SPfile` whose
file is gone empties the whole netlist, so that is what the message says.

```
E X1: its S parameter file nowhere.s2p is not found (beside the schematic, in the project or its
      user_lib): the netlist comes out empty and the simulation has nothing to run
E X4: its SPICE netlist nowhere.cir is not found (…): the netlist names a file the simulator cannot read
```

**A correction to the hunt report.** Its A12 table listed `SPICE_dev` and `XSPICE_A` as unreported too.
They have no `File` property at all (they take `NPins`, `Letter`, `Model`, `Params`), so the probe fed
them a field they do not take - a false positive of the same kind the report already notes for
`fem_export.copy`. A12 is about four parts that do have one, two of which were already checked.

## Tests

| test | suite | holds |
|---|---|---|
| `aHalfWrittenSchematicIsNotTakenForTheDocument` | `test_mcp_wishlist` | F1, A5, A6: both parts kept, no dialog, the revision unmoved, the call answers, the conversation told why |
| `onlyARealPluralIsReadFromItsSingular` | " | A1: six folds refused and the file not written, the collision refused, and six real plurals still read; `pluralOf` itself |
| `theGuardsSeeWhatTheToolWillRead` | " | A2, A3: a batch's inner console line and `trash_file` named through `call` as through `calls` |
| `aScriptIsAskedAboutWhateverWayItNamesTheTool` | " | A4: a name built up, a name in a variable, a console line, a push - and a script that only looks still let through |
| `everyWayAFileLeavesTheWorkspaceIsNamed` | " | A8, A13: a rename, a library's `.lib`, the setting that points at its folder |
| `aModelIsNotReadAgainUnderItsOwnJob` | " | A7: busy while the mesh builds, the model unchanged by a reload then, and read once the job is over |
| `aDamagedTouchstoneFileIsNotGuessedAt` | `test_data_import` | A9, A10, A11: a short row, a mislabelled port count, rows that do not rise, an unknown unit, no option line, a zero reference, and a whole file still read |
| `everyPartBackedByAFileSaysWhenItIsMissing` | `test_erc` | A12: `SPfile` and `SPICE` missing and empty, and the one whose file is there silent |

## Broken on purpose

`docs/feature_gaps/2026-10-23-hunt-a-fixes/breaks.py` holds 20, one or two for each fix, **all caught**:
the error capture scoped away so the box is raised from inside the load again, the revision not put back,
a one-letter key folded, a flag folded, the not-plural list emptied, each of the three guards given the
raw arguments, a script's unreadable name let through, the old five words alone, the job not looked for,
a model never busy, a rename / a library / a setting not named, the points not counted, the dropped rows
not said, an unknown unit guessed, the missing option line not said, and the file parts not checked.

**Eight went uncaught on the first pass**, one would not compile and one ran the wrong test - and chasing
them was worth more than the breaks. Every one was a test too weak to see its own fix, except where
noted:

- **Three A1 assertions only checked that the call failed**, which it does for other reasons too (no
  document, no analysis). They check *why* now - "takes no allow_command", "takes no save_a" - through
  one `refusedAs()` helper.
- **A2 had no assertion that reached `guarded()`.** The batch cases went through `askedEachTime()`'s own
  branch; a batch whose inner call writes outside the workspace is the one that goes through `guarded()`,
  and that is asserted now.
- **A7's observable was wrong twice over.** It first watched the model's objects during a mesh build,
  where nothing is reloaded anyway; then the solve's own answer, which is only corrupted if the write
  lands before the derived values are evaluated - timing, not a test. It now asserts the model during the
  solve, with the file's modification time set a few seconds ahead: without that, a solve sets the
  document's `lastSaved` to the file's own date (the wrinkle below), `changedOnDisk` is false and the
  reload is skipped whether the fix is there or not - the test could not tell.
- **F1's break would not compile** (the capture's declaration removed while its `errors()` was still
  read), and once it did, the test could not see it: the box is closed for a headless control anyway. The
  observable is the conversation's note, which carries the loader's own words ("Wrong 'component' line
  format!") only because they were captured.
- **The A12 break ran the wrong test**, because the harness copied from the previous round still defined
  `E` as `test_fem_engine:selectionRules` further down and overwrote the new one.

**Three were dropped rather than faked.** `load()`'s re-entrancy guard cannot be reached once the box is
captured - which is the fix that removes the nesting - so nothing in the suite can enter `load()` twice.
The headless guard stops a box being *created* where the control would close it anyway, so suppressing it
changes nothing a test can see. And the collision guard in `asPlural()` has no reachable case in today's
schema: `properti` only folded into `properties` through the "+es" path that is gone, and no tool has a
pair like `box`/`boxe` that the English rules would send to one field. The test holds `pluralOf()` to
that pair instead, so the guard's reason is on the record.

## Noticed while fixing, not fixed

- **A job takes the file's date for the document's.** During and after a solve or a mesh build, the
  model's `lastSaved` ends up equal to the file's modification time - so a write made *while* the job ran
  stops looking newer and is never read. The A7 test has to set the file's time a few seconds ahead to
  get past it, and it masked the fix entirely until that was found. This is worth its own round: A7 now
  defers a reload until the job is over, and this can turn that deferral into losing sight of the write
  altogether.
- `QFile::copy` keeps the source's modification time, which is why two of these tests write bytes rather
  than copying - a copy of a file dated in the past looks older than the tab that holds it.
