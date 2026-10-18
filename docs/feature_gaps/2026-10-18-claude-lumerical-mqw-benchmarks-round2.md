# Lumerical MQW benchmarks, round 2: the five nits against now

The reviewer's round 2 (`2026-10-13-claude-lumerical-mqw-benchmarks-round2.md`, on `367419b`) ran all three Lumerical MQW benchmarks from models the importer made of Lumerical's own `.ldev` files. They are Ahn 1988 (4x4), Chuang and Chang 1997 (wurtzite 6x6) and De Franceschi 1999 (8x8). Their results stood; they listed five nits about the importer and the tools.

Each nit was checked first on scratch copies of Lumerical's three projects (`lumerical_mqw_bench/download/mqw_*.ldev`).

| Nit | Then | Now |
|---|---|---|
| 1. `import` with `to` in a missing folder failed | "cannot be written" | **Already fixed** in `2e349c6` (the folder made; a failed write leaves no tab), tested by `claudeImportsLumerical`. Reproduced not: the 6x6 imported into `out/new/`. |
| 2. The bands plot drew `qw.Ev1, Ev3, Ev5, Ev7` alone | the top four Kramers pairs: in three wells, the top two subbands | **Done:** every state found, through a new form, `qw.Ev*` |
| 3. The 6x6's bands step asked for 20 states | 7 of the paper's 9 subbands with three wells | **Done:** every state bound in the wells too, through a new option, "Every bound state too" |
| 4. The client's tool list lacked `import` | a client cache | **Not Qucs-S's**, as the reviewer thought (below) |
| 5. A notice of the import's own write, repeated on every call | "changed on disk … loaded again", not cleared | **Already fixed:** reproduced not, now tested (below) |

## 3. Every bound state (nit 3)

**Why not 30.** The importer took Lumerical's own count: "max num eigenvalues" over two, since Lumerical's runs count both Kramers partners. Its 4x4 run's 10 eigenvalues are five pairs, and its stored bands show the same four subbands in each spin block. Lumerical's own 6x6 run would find the same 20 pairs. Thirty would fit this file, but a stack of five wells would need more again.

**Now.** The Valence Bands step has an option, "Every bound state too" (`"bound": true`):
- The states asked for are found as before, or every state bound in the wells where those are more.
- "Bound" means above the valence band edge where the stack ends, at k = 0. This is the edge the solver already uses for a state's own "bound".
- At k = 0 more states are asked for, doubling, until the lowest found is not bound (120 pairs at most, said if reached). Then the bands are solved at every k with that many.
- The log says how many states are bound and above which edge.

Both MQW importers (an `.ldev` project and an `mqwindex` script) set it, beside Lumerical's own count.

**Lumerical's three projects, imported and solved through the tools** (Release, the solve's own time):

| Project | Wells | Lumerical's count | Bound at k = 0 | Solved | Before |
|---|---|---|---|---|---|
| 4x4 (Ahn, 50 kV/cm) | 1 | 5 pairs | 6 states (3 pairs): its field tilts the stack | 5 pairs, 3.2 s | 5 pairs |
| 6x6 (Chuang and Chang) | 3 | 20 | 54 (27 pairs: 9 subbands in each well) | 27 pairs, 11.9 s | 20 pairs, 7.4 s |
| 8x8 (De Franceschi) | 3 | 15 | 48 (24 pairs) | 24 pairs, 37.7 s | 15 pairs, 22.8 s |

**The 6x6 against the paper, straight from the import** (the reviewer's comparison script on a scratch copy; nothing edited by hand):

| Subband | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|
| rms (meV) | 0.19 | 0.25 | 0.34 | 0.44 | 1.01 | 0.92 | 1.80 | 3.91 | 4.12 |
| The reviewer's, states set by hand | 0.20 | 0.25 | 0.34 | 0.44 | 1.01 | 0.90 | 1.76 | 3.9 | 4.1 |

The 8x8's three subbands are 0.26, 0.32 and 1.14 meV rms, as before.

## 2. Each state drawn (nit 2)

**A new form for a family of globals:** in a graph's (or a Derived Values global's, or fem_evaluate's) list, `name*` alone stands for each of the solution's globals `name1`, `name2`… in their order (Ev10 after Ev9). `qw.Ev*` is every valence state the bands step found, however many "bound" made them.

The importers' Valence bands plot is now one graph of `qw.Ev*`. That is every state, both members of each Kramers pair: Lumerical shows its two spin blocks too, and in a field they part away from k = 0. The `mqwindex` script importer, which made a bands study but drew none, now draws it too. A star that names no family is left as it is, and said as an expression that does not read.

## 4. The tool list (nit 4)

The server lists `import` in `fem_model`'s actions, and the lints check the tools' arguments against their schemas.
- **In the dock**, `claude` gets the tools at its start.
- **Under `qucs-s --mcp-server`**, one process serves the client's session.

So a schema without `import` came from the client, as the reviewer suspected. Nothing changed.

## 5. The repeated notice (nit 5)

Not reproduced on this build:
- **The import's own write**, over the model open in its tab, is not told back.
- **A write from outside** (a scratch script) is told once, then not again.

A test now holds both: `claudeImportsLumerical`, a conversation's import with `replace`, then an outside edit.

## Not done

- The reviewer's "Not compared" items stay so: Ahn's figures 10 and 11, and Lumerical's 6x6 and 8x8 plots that exist only as images.

## Tests

- **test_fem_quantum `everyBoundState` (new).** GaAs 8 nm in Al0.3Ga0.7As, one state asked:
  - the bound states come back, 12 states (6 pairs), each above the edge;
  - the same states with two more pairs asked and no option, the next below the edge;
  - more asked than are bound, as asked;
  - `qw.Ev*` in its order (past Ev10), as a Derived Values global; a star of no family, `2*qw.k` and no solution as they are.
- **test_fem_nitrides `lumericalProjects`:** the imported project's bands step is bound and its plot `qw.Ev*`; solved, its bound states said, Ev10 there.
- **test_fem_lumerical `aMqwScript`:** an `mqwindex` script with `kt`: a bands step of 12 states and bound, drawn as `qw.Ev*`.
- **test_multiphysics_doc `claudesToolsShowTheWellsInEV`:** a 1D plot group of `qw.Ev*`: its dataset holds qw.Ev1 to qw.Ev4 in eV against k, no Ev5, no "qw.Ev*".
- **test_multiphysics_doc `claudeImportsLumerical`:** nit 5, as above.

## Breaks

Twelve changes broken on purpose, all caught:
- **The bands step:** the option not read; every state taken as bound; no more asked for; the bound ones not taken.
- **The family form:** sorted by name (Ev10 before Ev2); not in a Derived Values global; not in a graph.
- **The importers:**
  - the project's bands step without the option, and its plot drawing four states;
  - the script's bands step without the option, and its bands not drawn.
- **The notices:** a conversation's seen state not kept after a call.

## Runs

- **Release:** the suite 129 of 129.
- **Under the sanitizers, with Eigen's LU (as on CI's Linux):** no report.
  - `everyBoundState` 29 s, on a coarser line there (92 s at 0.25 nm);
  - `lumericalWurtzite` 49 s, its bands solved there without the bound ones (117 s with them: 9 pairs, 5 asked);
  - `lumericalProjects` 28 s, `aMqwScript` 8 s, `claudeImportsLumerical` 30 s, `claudesToolsShowTheWellsInEV` 108 s (its new graph only plots).
- **The lints pass.**
