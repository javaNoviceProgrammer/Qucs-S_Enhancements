# Feature gaps: the check of 558ae40's library export

*6 October 2026 - Qucs-S 26.1.6.*

Claude checked commit `558ae40` (`qucs-s-558ae40-library-export-check.md`).
It found that both features work as the commit says:

- library subcircuits without a `gnd` pin unless asked;
- a marked part's Verilog-A loaded in every circuit of a project.

It ran each in the app under ngspice, a subcircuit with a parameter too. It
listed three nits and three code notes, needing no action. Each was
reproduced first, through `qucs-s --mcp-server` with its own HOME,
settings, trash and cache, and each is now done. Every change has a test,
and each was broken on purpose to see its test fail.

## Nits

| Check | Now |
|---|---|
| **1. Wrong menu in the tooltip.** The Ground pin tooltip says "Tools > Create Library"; the action is in the **Project** menu (`qucs_init.cpp:1160`). So do the Embed Verilog-A tooltip and a comment in `settingsdialog.cpp`. | Reproduced: those three, two test comments and the README's *Libraries that bring their Verilog-A* said Tools. All now say **Project**. Only the dated write-ups of earlier rounds still say Tools, as they were written. |
| **2. `get_settings` key.** The new checkbox was keyed by its whole tooltip paragraph instead of its row label. Not new: Embed Verilog-A and the others on that tab were keyed the same way. | Reproduced, and wider: **14 of Application Settings' 71 keys** were a tooltip, the ones whose check box has no text and whose label is in the grid cell before it. A control's name came from the label beside it only for a field, not a button: a button carries its own text. **Now** a check box with no text of its own is named by the label before it in its row, as a field is. When that row has no label, its tooltip still names it. Icon buttons keep their tooltips: a label beside one names something else. The 14 are now `Settings/Ground pin (gnd) in exported subcircuits`, `Settings/Embed Verilog-A files in exported libraries`, `Settings/Load documents from future versions`, `Appearance/Lock the toolbars` and the rest of their rows' labels. All 71 keys stay unique, and the Simulators and CDL settings are unchanged. No test, script or README text used the old keys. |
| **3. Values after a failed run.** After a failed `operating_point` run, `simulate` still listed device values (V1 at −0.25 mA), apparently the previous run's. | Reproduced: a 1 V source and 1 k ran at the operating point (R1 1 mA, V1 −1 mA). Then R1's value became `{nosuchparam}` and the operating point ran again. The failed run's answer listed **R1 at 1 mA and V1 at −1 mA as its operating point**, with no note. It also said "the schematic shows its bias now". The devices are the schematic's in memory, set when a DC bias run is read. A run that fails is never read, so the last run's stayed: in Claude's answer, and in *View → Operating Point*'s list. The node values come from files each run deletes first, so they were gone. **Now** a DC bias run forgets the schematic's devices as it starts: ngspice's where it deletes the files of the run before, and Xyce's as it starts (`AbstractSpiceKernel::forgetOperatingPoint()`). A run of the analyses keeps them, as before. The failed run's answer has no `operating point` and says "it failed, so the schematic shows no bias and there is no operating point to read". |

## Code notes

| Check | Now |
|---|---|
| `projectlibraries::alwaysLoadedModules()` runs on every ngspice netlist, for the OSDI loads and for the Verilog-A builds. Each time it looks for `*.lib` in the project and reads the marked parts' `.va` again, uncached. Fine for a small project; could slow a large one. | **Measured** on a project of 5,000 files with its own marked library, six operating-point runs each: 0.515 s per run without the library, 0.519 s with it. So it was under 10 ms a run. A run asks three times: its compile step, the netlist that step writes, and the netlist it runs. **Now** a kernel asks once (`Ngspice::a_alwaysLoaded`), and a `.va` is read again only when it changed, as a library's components and an `.osdi`'s modules already were. Afterwards 0.524 s, within the noise. |
| `takesGround()` returns true whenever the `.SUBCKT` pin count differs, not only when it has exactly one more. Harmless: such a library fails to netlist anyway. | Kept: a count that fits neither keeps the leading `0` as before. The code and its comment now say so in their own branch. A test holds it: a two-pin part whose `.SUBCKT` has four pins. |
| `readLibrary()` builds the subcircuit's name with `misc::properName(baseName + "_" + comp)`, while `createType()` also passes the Lib value through `properFileName`. A mismatch falls back to the last `.SUBCKT`, the component's own, so safe as written. | Both now call one function, `LibComp::subcircuitName(lib, comp)`. The name is the same by construction, not by agreement. The fallback would be wrong in one case: a component whose own `.SUBCKT` comes before one it places. A test now has one, and its own subcircuit is the one read. |

**Files left from the check**: the reviewer's projects `libexport_test` and
`libexport_user` in `~/QucsWorkspace` (one schematic in the Trash), and
`user_lib/LXVaMark.lib` with its folder. These are the user's to keep or
remove: nothing here touched them.

## Tests

- **`test_library_verilog_a`**:
  - the tooltips name the Project menu;
  - the subcircuit's own `.SUBCKT` is found when another follows it;
  - a count that fits neither keeps the ground pin;
  - a marked part's `.va` changed is read again (its module renamed and back).
- **`test_qucs_control`**:
  - `settingsAreReadAndSetByKeys`: the four keys above are their rows' labels, and no key holds "Create Library";
  - `theOperatingPointComesInOneCall`, under ngspice: after the good run, a failed one has no `operating point` and says it failed.
- **`test_operating_point`**: `aDcBiasRunForgetsTheRunBefore`. A DC bias run of ngspice's, and of Xyce's (with no Xyce here: it starts and fails), forgets the devices of the run before; a run of the analyses keeps them.

**11 breaks, 11 caught**:
- each tooltip naming Tools again;
- a textless check box named by its tooltip again;
- nothing forgotten;
- ngspice's start or Xyce's not forgetting;
- every run forgetting, an analyses run too;
- the failure not said;
- a `.va` never read again;
- the subcircuit looked for by another name;
- a count that fits neither taken as no ground pin.

The once-a-kernel cache changes how often, not what, so no test can see it
broken; the timings above are its measure.

The full suite passes, 89 of 89; under ASan, 89 of 89 with no report. The
scenario script passes 97 of 97. The user's `~/QucsWorkspace` and
`~/Library/Caches/qucs-s` are as they were before the runs.
