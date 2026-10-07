# Feature gaps: the check of Verilog-A library export and import

*7 October 2026 - Qucs-S 26.1.7.*

Claude checked the export and import of libraries that bring Verilog-A
(`qucs-s-va-library-export-import-2026-10-07.md`, on the app of `6c1889e`), with four
subcircuits that use Verilog-A through `SPICE_dev` and `.MODEL` cards: a resistor, an amplifier
whose source is in a folder with a space and includes a header, a file with two modules, and a
nested part. Each library was exported, copied away and imported, as a colleague would receive
it.

The confirmed behaviour:
- **Values** after export and import match the source project's.
- **Naming:** nested parts are named for their library, two levels deep too.
- **Embedding:** a header in a subfolder, two modules in one file and a file name with a space
  are all embedded.
- **Clashing file names** in one library are refused.
- **Updates** replace the old copy and recompile.
- **A library made without its Verilog-A** fails with a clear ngspice error.

It found five problems and three nits. Each was reproduced first, through `qucs-s --mcp-server`
with its own HOME, settings, trash and cache, on copies of the reviewer's projects `vx_src`,
`vx_use` and `vx_bare`. Each is now done. Two were worse than written:

- The stray model of finding 1 was a hazard in the source project too, not only in the library
  made from it: a run there loaded whichever definition was compiled last.
- Under finding 3, every Qucsator DC run listed no variables, not only the one with a Verilog-A
  part.

Every change has a test, and each was broken on purpose to see its test fail.

## Findings

| Check | Now |
|---|---|
| **1. Export bundles every project `.va` that defines the module name.** `embedVerilogA` collected every `*.va` in the project and embedded each one that defined a module the `.model` cards name, matching by name only. `wrap.sch` holds one `VXLib`/`vxhier` part, which uses `vx_res` (2·R). An unrelated `stray/other_res.va` defines `vx_res` as 10·R. The export said "Embedding Verilog-A: other_res.va, vx_res.va". Imported elsewhere, the run gave 0.5 V or 0.1667 V depending on which `.osdi` was built last, with no warning. | Reproduced: the same two files and the same `<SpiceAttach>`. **Now** a module that a library part in the subcircuit uses comes from that part's own library, the files it brings. The project's files serve only the subcircuit's own cards. `wrap`'s library takes `vx_res.va` alone. If two sources still differ for one module, the export is refused, naming the module and both files ("Error: the Verilog-A module good is defined differently by …"). A copy with the same bytes is one source. The library being made again is not a candidate either, since its folder holds the old library. **Worse than written:** the same choice by name was made in the project itself. A run of a `VXLib` part in `vx_use`, with `stray/other_res.va` compiled and newer, loaded whichever was built last. **Now** a run prefers the model of a placed part's own library (`osdi::needed()`'s `preferred`). It gives VXLib's 0.1429 V, and the netlist notes the file it did not take. |
| **2. The same module name in two libraries.** `VXLib` (`vx_res`, 2·R) and `VXOld` (`vx_res`, 1·R) side by side: both gave 0.333 V, and `VXOld/vx_res.va` was never compiled. There was no warning from `import_library`, `check_schematic` or `simulate`. | Reproduced (here 0.1429 V twice; VXOld's part should give 0.25 V). ngspice loads one module of a name, so one circuit cannot run both. **Now** each of the three says so. `import_library` warns that the library "brings Verilog-A modules that another library defines differently", lists them under `modules defined differently`, and compares against every library of the Libraries panel, the project's included. *Check Schematic* gives an error under ngspice: "X1 (VXLib) and X2 (VXOld) bring the Verilog-A module vx_res, defined differently (…): ngspice loads one of them, and every part using vx_res runs it". A part whose own library brings two definitions (a library made before the fix) is told to make it again. A run notes it in the netlist ("* OSDI: vx_res is defined differently by …: one of them is loaded"), even when nothing is compiled. `simulate` gives that as a warning. The reviewer's old `VXW`, which brings both `vx_res.va` and `other_res.va`, now draws the error and the warning. |
| **3. Qucsator with a Verilog-A library part runs as an open circuit and "succeeds".** The part's Qucs model was an empty `.Def`. `simulate` reported success, every node 0 V, with no message. In the project, `check_schematic` with `subcircuits: true` flagged it, but `simulate`'s pre-run check did not look into subcircuits. A library part gave nothing to check. Side effect: `variable count: 0`. | Reproduced, all of it. **Now:** `create_library` notes it ("has no Qucs model (its devices are SPICE's or Verilog-A): Qucsator leaves this part out"), and the answer has a `warning` field with every "Warning:" or "Note:" line of the messages. A library part whose Qucs model (with the files it includes) has no device of Qucsator's but whose SPICE model does is an error under Qucsator: "not available for Qucsator - its library part (VXLib) has a SPICE model only" (`LibComp::qucsatorSimulates()`). `simulate` looks into its subcircuits before every run, not only after one that failed. Their errors are under `before the run` → `errors in its subcircuits`, each with its file, and the run's log begins with them ("Check Schematic, of its subcircuits: error in vxres.sch: X1: not available for Qucsator …"). The check is also made for the run's own simulator: a run with `simulator: qucsator` was checked for the one in the settings. **The side effect was wider:** Qucsator writes a DC operating point's node voltages as independent vectors, so every Qucsator DC run listed none. An independent vector that nothing depends on is now listed (`V1.I`, `out1.V`). |
| **4. An `` `include `` from outside the source's folder.** `va2/vx_up.va` includes `../common/up.vams`. The export said "Warning: … not embedded" in its messages, then "Successfully created library." There was no `warning` field, no `missing` entry on import, nothing from `check_schematic`, and OpenVAF failed where the library was used. | Reproduced. **Now** embedded and rewritten, as the reviewer suggested: the file goes below `vx_up.includes/`, at its path from the folder the source and it share (`vx_up.includes/common/up.vams`). The `` `include `` lines that name it are rewritten in the library's copies, the source's and those of the files it includes. Other lines, `` `include "disciplines.vams" `` included, stay as they were. The library compiles where it is brought: `vxup` gives 0.0625 V in `vx_bare`, as in `vx_src`. For a library made before: a source's include that leads to no file (OpenVAF's own headers aside) counts as missing (`osdi::missingSourceIncludes()`). `import_library` lists it in `missing`, and `describe_part` says it. *Check Schematic* gives an error: "the Verilog-A of its library … includes what the library does not have (vx_up.va's include ../common/up.vams): OpenVAF fails on it here - make the library again". |
| **5. Export without the Verilog-A records no dependency.** With `embed_verilog_a: false` the `.lib` had the `.MODEL … vx_res` cards and nothing else. Import into a clean project gave no warning, nor did `check_schematic`, and the run's error didn't say that the library was made without its Verilog-A. | Reproduced. **Now** Create Library records the modules each part needs, whether it embeds them or not: `<VerilogAModules "vx_res">`, the modules its `.model` cards name that a Verilog-A source or compiled model defines. `import_library` lists those that neither the library nor the open project defines (`modules not here`) and says it was made without its Verilog-A. *Check Schematic*, under ngspice, names the cause before the run's generic error: "X1: its library VXNE2 was made without the Verilog-A of vx_amp, which no source or compiled model here defines …". In `vx_bare`, `vx_res` is defined by the imported `VXW`, so only `vx_amp` is said. A library made before carries no record, and nothing is guessed for it. |

## Nits

| Check | Now |
|---|---|
| `create_library replace` without `descriptions` drops the replaced library's descriptions. | **Now** a part with no description given keeps the one it had in the library it replaces, by part name. One given replaces it. The window's dialog shows them, to keep or change, when it rewrites a library. |
| The operating point labels a Verilog-A instance parameter `gain` (=3) with "S" and `vmax` with "V", guessed from the names. | **Now** a device loaded with OSDI (ngspice's "A simulator independent device loaded with OSDI") has no unit guessed for its own values. They are listed, without one. ngspice's `dt`, `dtemp` and `temp` keep theirs, and a built-in device's `gm` stays a conductance (`oppoint::unitOf(const Device&, …)`). |
| The `check_schematic` verdict for `bench2.sch` under Qucsator: "Nothing wrong found" with "in its subcircuits 8 errors". | Reproduced. **Now** "Nothing wrong in it, but its subcircuits have 8 error(s): 'subcircuits': true lists them." |

The reviewer's projects `vx_src`, `vx_use` and `vx_bare` in `~/QucsWorkspace` are as they were.
Everything here ran on copies.

## Tests

- **`test_library_verilog_a`**:
  - `aPartsModuleComesFromItsOwnLibrary`: a part's module from its library, a stray file left out, two differing sources refused.
  - `twoLibrariesDefiningAModuleDifferentlyAreSaid`: the check's error and the netlist's note, nothing for one library alone.
  - `anIncludeFromOutsideItsFolderIsEmbedded`: embedded and named, its line rewritten, OpenVAF's own left; one made without it reported missing.
  - `aLibraryRecordsTheModulesItNeeds`: `<VerilogAModules>` with embedding off, the check where nothing defines it, nothing where the project does.
  - `aReplacedLibraryKeepsItsDescriptions`.
- **`test_qucs_control`**:
  - `verilogALibrariesSayWhatTheyLack`:
    - `create_library`'s note and `warning`;
    - `import_library`'s `modules defined differently` and `modules not here`;
    - the check's error and a run's warning for two libraries' `tm`;
    - under Qucsator, the part's error, the verdict, and a run's check of its subcircuit.
  - `aRunIsCheckedFirstAndTakesAnotherSimulatorOnce`: a Qucsator DC run lists its variables.
- **`test_osdi_selection`** `oneLibraryForEachModuleUsed`: a placed part's library first, else as before.
- **`test_operating_point`** `aVerilogADevicesValuesHaveNoGuessedUnits`.

**The results:**
- **Breaks:** 25, each a fix undone or bent in the source, every one caught by its test. That includes a subcircuit's error told twice.
- **Full suite:** 92 of 92, with its own HOME, settings, trash and cache.
- **CI's source checks:** `check-tool-arguments.py` (99 tools, no disagreement) and `check-json-nesting.py` pass.
- **The MCP scenarios** (`scripts/mcp-e2e-scenarios.py`, on the built app): 97 checks, none failed.
- **The reviewer's repros**, run again on the last build, on copies:
  - finding 1: `vx_res.va` alone, 0.1429 V;
  - finding 2: the import warning, the check's error and the run's warning;
  - finding 3: the error before the run, both subcircuit errors before the in-project run, and three variables;
  - finding 4: 0.0625 V in `vx_bare`, and the old library's include reported missing;
  - finding 5: the record, the import's `modules not here` and the check's error;
  - the nits as described.
- **The first full run** caught a fault of the first version. `simulate` had already looked into the subcircuits after a failed run, under `errors in its subcircuits`. The new check before the run gave the same errors again, under `errors`. Now there is one check, before the run, in the shape that was there: `whatTheHuntAfterRoundNineFound` passes as it was written.
