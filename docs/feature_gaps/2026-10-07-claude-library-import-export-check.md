# Feature gaps: the check of the library import and export fixes

*7 October 2026 - Qucs-S 26.1.6.*

Claude checked commit `80f3f53`, the fixes of the library bug hunt (`638903e`), in the running
window, under ngspice 46 and the bundled Qucsator (`qucs-s-library-import-export-check-2026-10-07.md`).
It found that seven fixes hold: A3 + B1, A2, B3 / B4, B7, B8, B9 and `name`. Each was run in a
circuit and its output voltage compared with the subcircuit's. It found:

- one serious bug, with a wrong hint (1, 1b);
- an error reported as "Unknown error" (2);
- four minor notes (3).

Each was reproduced first, through `qucs-s --mcp-server` with its own HOME, settings, trash
and cache, and each is now done. Three were worse than written:

- The `name` retry the hint offered was refused too.
- `OR` passed the reserved-name check and failed in ngspice.
- The T-junction depended on the order of the lines, in an opened file too.

Reproducing the minor notes found two more:

- A Claude run whose netlist could not be made waited on a message box past its timeout.
- A Qucsator run stopped at "Cannot write netlist file!" for good once its netlist's folder was
  gone.

Every change has a test, and each was broken on purpose to see its test fail.

## 1. A shared include

| Check | Now |
|---|---|
| **1. A shared include is overwritten, and another library's results change.** `import_library` copied a SPICE library's includes to `<destination>/<their path>`, a folder every library there shares. `LHSP` includes `models/rmod.inc` (`RA=4k`, 0.2 V). Bringing in `LHSP2` from another folder, whose `models/rmod.inc` has `RA=9k`, with `replace`, changed `LHSP`'s `spdiv` to 0.1 V with no word. | Reproduced as written: 0.2 V, then 0.1 V. **Now** the reviewer's preferred fix: a SPICE library's files go into a folder of its own, `NAME/` beside `NAME.lib`, each at its path from the library's folder: `user_lib/LHSP/models/rmod.inc`. The copied library's include lines are rewritten to name them there: `.include LHSP/models/rmod.inc`. The rewrite changes only an include line whose file moved: a quoted path keeps its quotes, and `.lib FILE SECTION` keeps its section. Every other byte stays as it was (read as Latin-1: a `µ` in a vendor's comment is kept). Those included files keep their layout, so their own relative includes still lead where they did. An include that names the library itself follows it under its new name (`.lib "../back.lib"` becomes `"../../Back2.lib"` brought in as `Back2`). A SPICE library's folder of models comes into `NAME/` at its own path too. **No two libraries share a file**, and `replace` puts only `NAME.lib` and `NAME/` in the trash. The special case for an include already there with the same bytes is gone, as the reviewer expected. A `replace` that would trash a folder holding a file the library brings (one it includes from there) is refused, and nothing is touched. The re-run gives `LHSP` 0.2 V and `LHSP2` 0.1 V, each its own. A vendor library with a quoted include in a subfolder, a `.lib` section of a file beside it, a nested include and an include in its folder of models gives the same four voltages after import as where it was, under its own name and another. |
| **1b. The refusal's hint about `name` is wrong.** It offered `name` when the only conflict was an include, and the retry was refused again. | Reproduced, and the retry with `name` was refused ("`user_lib/models/rmod.inc` is there already"). **Now** nothing but `NAME.lib` and `NAME/` can be in the way, and `name` changes both: the hint is right as it stands, and the retry is brought in. |

## 2. A reserved net name

| Check | Now |
|---|---|
| **2. "Failed to start simulator … Unknown error".** With a net labelled `or`, `simulate` under ngspice answered `Failed to start simulator ".../ngspice": Unknown error`. The kernel's checks wrote why into the console and reported `FailedToStart`. The message was then made from the `errorString()` of a process never started. The same held for "No Ground found", "No simulation found" and "Only DC simulation found". | Reproduced, and wider: the check is case-sensitive, but ngspice reads its input in lower case. **`OR` and `Or` passed the check** and then failed in ngspice ("syntax error in line segment v(or)", tried with ngspice 46 for each of the nine words in three cases). **Now**: (a) the check ignores case (`spicecompat::check_nodename`, and the label dialog's refusal through it). (b) A kernel that does not start the simulator keeps its reason (`AbstractSpiceKernel::refusal()`), and the run says it: "Ngspice was not started: There were Nutmeg-incompatible node names … Incompatible node names are: or (Nutmeg reads and, or, not, eq, ne, gt, lt, ge, le as operators, in any case: rename the net.)". This goes in the status log and Claude's answer, for no ground and the rest too, and for Xyce's checks. (c) `check_schematic` gives it as an **error** under ngspice and SPICE OPUS, in the schematic that is run only: inside a subcircuit the node is `x1.or`, which ngspice reads fine (tried). (d) `set_label` and `rename_net` set the name with a warning under those simulators. On the way: the node-name check reused the list of incompatible parts, so a schematic with both named the parts among the nets. Each list now holds its own. |

## 3. Minor

| Check | Now |
|---|---|
| **A library without its folder: the warning is too strong.** `describe_part` said "a simulation of it stops at it", but a missing `inner.sch.lst` is only the Qucsator model's include: under ngspice the part runs from its `<Spice>` text. | Reproduced (0.5 V under ngspice). **Now** `describe_part` names each model's missing files and what stops at them: "a run of it under Qucsator stops at inner.sch.lst, which its Qucsator model needs (under ngspice or Xyce it is simulated from its SPICE model, which needs none of them)". A SPICE attachment's is "a run of it under ngspice or Xyce stops at dev.lib". The SPICE-model sentence is said only when the part has one. `import_library`'s warning says the same of the library, and *Check Schematic*'s error, already given only for the simulator in the settings, now names it ("a run under Qucsator stops at it"). |
| **Qucsator's second error is wrong.** After "Cannot load library component", it printed "ERROR: Cannot simulate a text file!" whenever the netlist could not be made. | Reproduced. That line is inside the branch for a document that is *not* a text file, so it was wrong every time it was printed. **Now** nothing follows the netlister's own reason. When the netlister said nothing, the line is "ERROR: The netlist could not be made: nothing was simulated." **Found on the way**, with a subcircuit file not there instead of a library: under Qucsator *and* ngspice, Claude's run hung. The netlist is made after `simulate`'s call has returned, where no error is caught, so the loader's "Cannot load document" box opened modal. It waited for an OK, and the run with it: the answer came at the timeout with "finished": false, "creating netlist...", and the next `simulate` was refused while the box was open. **Now** Claude's runs catch what the netlist would say in a box: Qucsator's in its error panel ("ERROR: Cannot load subcircuit … ERROR: Cannot load document …"), ngspice's in the answer's `errors`, each once (`"when": "its netlist was made"`; the netlist is made more than once per run). Both answer at once. The window's own runs keep their box. And a Qucsator run makes its netlist's folder when it is not there: emptied while Qucs-S runs, as by a cache cleaner, every run had stopped at "Cannot write netlist file!" until a restart (found when this test ran first in its file). |
| **`batch` with `brief` hides `get_dataset`'s values.** It reported "get_dataset: done (3 fields)". | **Now** under `brief` a call that only looks (`get_dataset`, `get_netlist`, `get_schematic`, … the read-only tools) is answered in full; the calls that change something are one line each, as before. |
| **`set_schematic` leaves a T-junction open.** A wire whose end lands mid-way on another wire was not joined to it. | Reproduced, and worse: **it depended on the order of the lines.** A new node splits the wires it lies on (`provideNode()`), so a stem listed after the bar was joined and one listed before it was not. The same two lines swapped were two circuits, in `set_schematic` and in a file opened from disk alike. The GUI joins such a wire when it is drawn. A scan of the 267 schematics of the source tree and the 84 of `~/QucsWorkspace` found no wire end lying mid-way on another wire, as the GUI never saves one: joining changes none of them. **Now** reading a document's wires joins each wire end that lies on another wire mid-way, splitting that wire there (`Schematic::joinWireEnds()`), whichever line comes first. `set_schematic` lists the places as `joined` with a note, in both orders. A part's pin lying on a wire is left as it was (a wire must end at it, and *Check Schematic* says so), since a part's pins are read before the wires. |

**Files left from the check**: the reviewer's projects `lh_a`, `lh_b` and `lh_use`, and the `LH*`
libraries of `user_lib`, are in the Trash, as the check says. Nothing here touched them.

## Tests

- **`test_qucs_control`**:
  - `librariesComeInWhole`:
    - a SPICE library's includes are in `vend/`, and its include lines name them there;
    - two libraries including `models/rmod.inc` each keep their own, through a `replace`;
    - one brought in under another name: an include naming it back follows it, and a byte not UTF-8 is kept;
    - a `replace` that would trash its own include's folder is refused;
    - the missing files' wording, a SPICE attachment's and a Qucsator include's, with a SPICE model and without;
    - *Check Schematic* under ngspice and under Qucsator.
  - `nutmegWordsAreNoNetNames`:
    - `OR` and `Eq` refused, `order` and `nor` taken;
    - `set_label`'s and `rename_net`'s warning, none under Qucsator;
    - `check_schematic`'s error, none for a subcircuit's net;
    - the run's answer for `OR` and for no ground, without "Unknown error";
    - the node names listed without the incompatible part.
  - `aRunsNetlistErrorsAreSaidNotBoxed`: a subcircuit not there, under ngspice and Qucsator; each answers within seconds, says why once, leaves no box, and says no "text file"; Qucsator's run with its netlist's folder taken away first.
  - `aTeeIsJoinedWhicheverLineComesFirst`: both orders in `set_schematic` and in a file opened; a pin on a wire left.
  - `briefAnswersAndBeforeAndAfter`: `get_netlist` in a brief batch, whole.

The whole `ctest` passes (91 of 91), as do the scenario script (97 checks), `check-tool-arguments`
(99 tools, 0 disagreements) and `check-json-nesting`. The latter caught a test's
`QJsonArray{QJsonArray{400, 140}}`, a copy to some compilers: the expected point is built by
`append`.

**26 breaks, 26 caught**:
- the includes at their paths in the shared folder again;
- the include lines not rewritten;
- the library itself not followed under a new name;
- a library read as UTF-8 (its `µ` lost);
- `replace` trashing the folder its own include is in;
- a refused run said as "Unknown error" again;
- the check case-sensitive;
- `check_schematic` saying nothing of the name;
- a subcircuit's net flagged too;
- `set_label` not warning;
- a warning under Qucsator too;
- the incompatible parts among the node names;
- a Qucsator include said as a SPICE one;
- a SPICE model said when the part has none;
- *Check Schematic* naming no simulator;
- `import_library` naming none;
- "Cannot simulate a text file!" again;
- Qucsator's netlist errors lost;
- Qucsator's run boxed (the answer at the timeout, "finished": false);
- ngspice's run boxed (the test hung);
- the netlist's error said four times;
- `brief` cutting a read to a line;
- a T not joined;
- a join made while reading not told;
- a pin on a wire joined too;
- the netlist's folder not made.

The first run of the breaks let one through, a subcircuit's net flagged too: the test's
`check_schematic` counted the subcircuit's findings without listing them. It now asks for
them (`subcircuits`), checks that the subcircuit was read, and catches it.
