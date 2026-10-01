# Proposal: the synthesis tools as Claude's tools

*1 October 2026 - Qucs-S 26.1.5. A proposal, not built yet: parked by choice (group 4
of `qucs-s-claude-access.md`, "what Claude still needs for full control").*

The Tools menu starts programs of their own: Filter synthesis, Active filter
synthesis, Line calculation, Attenuator synthesis, Power combining, the S-parameter
Viewer and the Receiver calculator. `trigger_action` starts one, and from then on it
is outside Qucs-S. Claude can't read its window or fill it in, and gets nothing back.
The result goes to the clipboard as schematic text, for the user to paste. This
proposal plans how Claude could design with them. The facts below were checked in
this tree on 1 October 2026.

## What there is

| Tool | Where | Size | Its result | What its calculation needs |
|---|---|---|---|---|
| Filter synthesis | `qucs-filter/`, a program | 6,300 lines | `.sch` text to the clipboard | `LC_Filter::createSchematic(tFilter*, bool)` and the line filters' `createSchematic(tFilter*, tSubstrate*, bool)` return the text, with no window. But they include `qucsfilter.h`, which declares a `QucsSettings` of its own, the same name as Qucs-S's. |
| Active filter synthesis | `qucs-activefilter/`, a program | 5,500 lines | `.sch` text to the clipboard | `Filter(func, type, FilterParam)` and its `createSchematic(QString&)`: Sallen-Key, MFB, Cauer. Clean of the window. |
| Attenuator synthesis | `qucs-attenuator/`, a program | 2,000 lines | `.sch` text to the clipboard | `QUCS_Att::Calc(tagATT*)` and `createSchematic(tagATT*, bool)`. Clean of the window. |
| Power combining | `qucs-powercombining/`, a program | 2,300 lines | `.sch` text to the clipboard | Its calculations are inside the window's class (`qucspowercombiningtool.cpp`). Each writes its text straight to the clipboard, so they would have to be taken out. |
| Line calculation | `qucs-transcalc/`, a program | 5,500 lines | numbers in its window | Each `transline` (microstrip, coplanar, coax, ...) reads its values through the window (`getProperty("Er")`, `setApplication(QucsTranscalc*)`). A table of values in place of the window would have to stand in for it. |
| Matching Circuit | `MatchDialog`, **in Qucs-S** | - | a schematic in the document | Already in reach: `trigger_action` "Tools > Matching Circuit", then `get_dialog` / `set_dialog`. |
| S-parameter Viewer | `qucs-s-spar-viewer/`, a program | 148 files, about 100,000 lines | a viewer | Out of scope. `import_data` (Touchstone) and `get_dataset` already read S-parameters as numbers. |
| Receiver calculator | `rxcalc`, not in this tree | - | - | Out of scope: it is not built here. |

Only `qucs-transcalc` reads its command line, and that is only a file to load. None of
them has a mode without a window.

## Three ways

1. **The calculations in Qucs-S, as tools of Claude's** (the audit's first choice).
   The calculation files are built into Qucs-S as well as into their programs. A tool
   then calls them and puts the result where Claude asked: a new schematic, or the
   document in front at x, y, as one undo step (as `set_schematic` puts text). The
   programs stay as they are for the user.
   - **Cost:** the filter's `QucsSettings` is renamed or put in a namespace, so that
     the two don't collide. The power combiner's calculations are taken out of its
     window, and transcalc gets a table of values in place of its window. A test
     shows each tool's output is what its program writes.
   - **Gain:** no process, no clipboard, nothing left on the user's screen. Each
     answer is checked and numbers come back typed.
2. **A `--json` mode in each program:** a spec in, the schematic text or numbers out,
   run as a child process by a tool.
   - **Cost:** a parser and a writer in each of five `main.cpp` files. The same window
     coupling as in option 1 still has to be undone for transcalc and the power
     combiner, and each program is started per call.
   - **Gain:** less change inside Qucs-S. But there are five programs to keep in step
     with the tools' descriptions.
3. **A control tree like `get_dialog` for each window.** The windows are in other
   processes, so this needs a channel to each of them (a local socket, and a server in
   each program). It is the most work for the least: Claude would still type into
   forms and read the clipboard.

**Recommended: 1**, in order of cost:
- attenuator (clean);
- active filter (clean);
- LC and line filters (one rename);
- line calculation (a table of values);
- power combining (taking it out of its window).

Each step is a tool on its own, worth shipping alone.

## The tools

Each tool returns what it designed, with its values, and where it put it:

- **`synthesize_filter`** `{kind: "lc" | "line" | "active", response: "butterworth" |
  "chebyshev" | "bessel" | "cauer" | ..., type: "lowpass" | "highpass" | "bandpass" |
  "bandstop", order? | spec: {fc, ripple, atten, fs}, impedance, topology?, substrate?,
  into: "new" | {x, y}}`
  - returns the parts and their values, the order it chose, and the new document's
    name (or the parts placed);
  - with `spec` and no `order`, the order is worked out as the program does.
- **`synthesize_attenuator`** `{topology: "pi" | "tee" | "bridged-tee" | ..., attenuation,
  z_in, z_out, into}` returns the resistors.
- **`synthesize_power_combiner`** `{topology: "wilkinson" | ..., f, z0, ways, ...,
  into}`.
- **`line_calc`** `{type: "microstrip" | "coplanar" | "coax" | ..., substrate: {er, h,
  t, tand, ...}, f, z0 | w, l?}` returns `{w, l, z0, e_eff, loss}`. It runs forward
  (analyze) or backward (synthesize), as the window's two arrows do.

## Tests

From the audit:

- **A filter:** a 5th-order 1 GHz Chebyshev LC low-pass, made into a new schematic,
  simulated, and measured at -3 dB with `get_dataset`.
- **A line:** a 50 Ω microstrip on FR4 at 2.4 GHz, with its width the same as the
  Line calculation window gives.

Also:

- each tool's text the same as its program's for the same spec (the program's own
  `createSchematic` called on both sides);
- a spec a program refuses refused the same way, with why.

## Open questions

- Should the programs themselves go on as they are? Option 1 keeps them; nothing the
  user does changes.
- `into` a document: one undo step, and refused while a dialog waits, as the other
  editing tools are.
- The filter's own help text (`helpdialog.cpp`) could become each tool's description.
