# A library material's keys said "not read"

The report (2026-10-17): *"in a pn junction simulation, the solve gave about 60 warnings like "Silicon: it has no property Cbgn – not read", even though the library Silicon node lists those properties. They are models this diode doesn't use (Auger, impact ionization, bandgap narrowing), so the results are unaffected. It does look like a false warning for library materials added with fem_edit."*

## Reproduced

`examples/multiphysics/pn_diode.qfem`, copied, with library Silicon added by `fem_edit` (`"type": "material:Silicon"`) and solved through the tools, gave **59 warnings**:

- the semiconductor models' parameters (Cbgn, Ebgn, Nbgn, caun, caup, copt, iian…, munmin, nrefn, vsatn, bbtA…, fA…);
- `nrange`;
- `sigmaFCn` and `sigmaFCp`.

The Multiphysics panel's Add Material from the library makes the same node (`materialNode`), so it gives the same warnings.

**Why.**
- A library entry's material node keeps those keys on purpose (`materialNode` keeps `propertyKeys()`: the basic keys and `semiconductorModelKeys()`). The physics read them from the material:
  - a semiconductor feature whose model is "From the material" (Auger's, impact ionization's, narrowing's parameters);
  - the waves physics' free-carrier absorption (`sigmaFCn`, `sigmaFCp`);
  - the index's range check and the material plot (`nrange`).
- The solve's property check (`checkProperties`, 8bd5709) said "not read" of every key the node's kind does not list, and the material kind lists only the basic properties.

**The same cause in `fem_edit`.** `set` refused those keys on a library material ("Material has no property caun"), so Claude could not change Silicon's Auger coefficients, though the material carried them.

## Now

- **A material node takes a library entry's keys** (`isMaterialKey`, `materialKeys()` in fem_materials.h):
  - the solve no longer says them "not read";
  - `fem_edit` sets them.
- **A key no material has is still said:**
  - "Silicon: it has no property Cbgnx - not read";
  - `fem_edit` refuses it, listing the library's keys with the kind's, so Cbgn is there for "Cbgnx";
  - a material's key on another node (Cbgn on an ohmic contact) is still said.
- Through the tools again: **0 warnings,** `caun` set, `Cbgnx` refused.

## Tests

- **test_fem_semiconductor `aLibraryMaterialsKeysAreItsOwn` (new):**
  - a bar of library Silicon put in as the panel and `fem_edit` put it, its keys on the node: solved, nothing said;
  - with Cbgnx: said;
  - Cbgn on an ohmic contact: said.
- **test_multiphysics_doc `claudesToolsBuildAndSolveAModel`:**
  - library Silicon's `caun` set through `fem_edit`;
  - Cbgnx refused, the message naming Cbgn and caun;
  - solved with nothing said.

Five changes broken on purpose, all caught:

- the keys said again;
- a material's key unsaid on any node;
- every key a material key;
- `fem_edit` refusing them;
- `fem_edit` not naming them.

## Runs

- **The Release suite (offscreen):** 127 of 127.
- **Under the sanitizers,** with Eigen's solvers as CI's Linux has them: both functions, no report. The new one takes 5.4 s, on a coarse mesh: it asks what is said, not the solution.
- **The lints pass.**
