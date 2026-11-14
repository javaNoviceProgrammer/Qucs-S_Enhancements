# RF 2.5D's metal Solved as Automatic

Phase 3 of the plan for RF 2.5D's accuracy limits.
- **Phase 2** (`2026-11-12-rf25-walls-and-loss.md`) made a metal solved as its two faces right: its walls carry
  current and charge, and its loss is its edge's own.
- **A sheet**, the default, has no thickness in the field and no walls. On thick metal in narrow gaps its Z0 reads
  some per cents high. The benchmark's 2 µm gold in 20 µm gaps is 4.8 % high as a sheet: Palace's thin metal reads
  73.72 Ω, its 2 µm 70.3.

The user chose a new "Automatic" for a metal's Solved as, which solves such metal as its two faces. On where it
switches, the user said "Yes, use 1/40" of the gap, relayed by the parent session. B2's quarter is where a sheet's
capacitance across the gap goes visibly wrong; Z0 is out by a per cent long before that.

## What changed

- **The metal's Solved as** has three choices: Automatic, a sheet, its two faces. Automatic solves the metal as its
  two faces where it is at least a fortieth as thick as the narrowest gap beside it, and as a sheet elsewhere.
- **A new metal is Automatic** (`Model::make()`), and so is the starter model's (`Model::blank()`). A file that says
  nothing is a sheet, as it was, so no model saved before this solves differently. The kind's default stays "sheet"
  because a model's file writes only the properties set.
- **`Layer::automatic`**, read from the file's "automatic".
- **The mesh keeps each layer's narrowest gap** (`Mesh::gapByLayer`, µm), which the mesher already measured for B2's
  warning.
- **`thickened()` decides first.** On a copy of the stack each Automatic metal becomes two faces or a sheet, so
  everything after reads the decision, the film rule included. Each is said:
  - "M1 is solved as its two faces (Automatic): 2 µm thick beside gaps as narrow as 20 µm, a fortieth of them or
    more.";
  - "... a sheet (Automatic): 0.5 µm thick, its narrowest gap 50 µm, over forty times as wide.";
  - "... a sheet (Automatic): no gap faces it."
- **B2's warning** reads an Automatic metal past its quarter as two faces. Its text now says the walls facing across
  the gap hold charge, as phase 2 made them.
- **The stack drawing's key** says "solved as its two faces where it is thick beside its gaps (Automatic)". The stack
  file's note counts an Automatic metal among those its format has no place for. A stack read back keeps a metal's
  Solved as by its name, Automatic too.

## Checked

- **`test_rf25_mesh`** `aMetalSolvedAsAutomaticIsItsTwoFacesWhereItIsThickBesideItsGaps` (new):
  - 2 µm in 20 µm gaps is two faces, and said;
  - 1 µm in 40 µm gaps is two faces, and in 41 µm gaps a sheet;
  - 0.5 µm in 50 µm gaps, and a strip with nothing beside it, are sheets, and said;
  - the mesh keeps the layer's gap (20 µm);
  - a file that says nothing is a sheet;
  - a new metal and the starter model's are Automatic.
- **`test_rf25_mesh`** `aMetalThickBesideNarrowGapsIsSaid`: B2's warning has its new words.
- **`test_rf25_accuracy`**'s benchmark CPW is now solved as Automatic. It reads phase 2's two-face figures: Z0 +0.37,
  +0.37, +0.48 % at 10, 30, 50 GHz against Palace 2D and QFEM.
- All 26 RF 2.5D suites and `test_qucs_control` pass under ctest at `-j4`. Models the tests build through `make()`
  and `blank()` with thick metal in narrow gaps are now two faces, and their checks hold. The four lints pass.
- `breaks.py` undoes each piece. Its results are in `results.txt` beside it.

## Not done

- A stack read from a file makes a metal it does not already have a sheet, as the file says nothing of how to solve
  it.
- Automatic decides per metal, by the narrowest gap anywhere on its layer: one narrow gap makes the whole layer two
  faces.
