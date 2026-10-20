# Coupled excitons: the check of a4ff84a, and the tools around a well's studies (answered)

The reviewer's file: `~/Desktop/qucs-reviews/2026-10-20-claude-coupled-exciton-a4ff84a-check.md`. It checked the hh–lh mixing of the coupled excitons (a4ff84a; [its write-up](2026-10-19-claude-coupled-exciton-hh-lh-mixing.md)) against an independent real-space reference of its own. It then drew the well's band diagrams and E(k) in the app.

**The verdict:** all four findings of the last round are fixed. The mixing agrees with the reference: the hh1 exciton's binding to 0.02 meV (8.635 against 8.624 meV), the lh1 exciton's line to 0.1 meV. One nit was left in the excitons: the ripple the continuum's warning quotes reads low. Three tool nits came up while plotting, and one more was seen.

Each was reproduced first over `qucs-s --mcp-server`, with an isolated home and cache, on copies of the reviewer's `gaas10_kp4.qfem`.

## The excitons

| Feedback | Now |
|---|---|
| 1. The mixing in the attraction: pass. 2. Phases: pass (by construction). 3. No TM on hh1: pass (0.0035 of TE). | As they were. |
| **4. Nit: the warning's ripple figures read low.** "some 7 % at 0.5 meV, 2 % at 1" sits between the rms and the peaks the reviewer measured from 1.482 to 1.500 eV: TE ±13.5 % raw, ±9.7 % about a quadratic trend, 4.8 % rms at 0.5 meV; ±7.5, ±3.5 and 1.7 % at 1 meV. | **Reproduced to the reviewer's figures** on the current build, and measured at 2 meV too. The warning's figures were a third measure, the largest departure from a 2 meV local mean (7.1, 2.2, 0.7 %), and the warning did not say so. **Now it quotes the ripple's peaks about its trend**, the larger of TE's and TM's, and says what they are: "a line of 2 meV or more smooths it (a 10 nm GaAs well's 4x4 bands from 1.48 to 1.50 eV, at the ripple's peaks: some ±11 % at 0.5 meV, ±4 % at 1, ±1.5 % at 2; its rms under half that)". It said 1 meV, which still ripples ±4 %. The size's help says the same. |

The ripple on the reviewer's well, by each measure (the current build, 1.482–1.500 eV):

| line | TE raw | TE about trend | TE rms | TM about trend | TM rms | the old local measure (TE, TM) |
|---|---|---|---|---|---|---|
| 0.5 meV | ±13.5 % | ±9.7 % | 4.8 % | ±10.9 % | 4.7 % | 7.1, 6.7 % |
| 1 meV | ±7.5 % | ±3.5 % | 1.7 % | ±4.1 % | 1.8 % | 2.2, 2.3 % |
| 2 meV | ±5.6 % | ±0.7 % | 0.3 % | ±1.5 % | 0.6 % | 0.7, 0.7 % |

(The raw figures hold the continuum's own slope, which a ripple is not.)

## The tools

| Feedback | Now |
|---|---|
| **6.1 `fem_evaluate`'s `point` ignored on a Valence Bands study:** 21 and 41 both answered at k = 0.8 nm⁻¹, the last. | **Reproduced:** 21, 41 and 99 all at 0.8 nm⁻¹. A bands step's values of k are one solution's outputs, not a sweep's points, and `point` picked only those; with one solution it fell to the last output. **Now `point` picks a bands step's k by number**, as it already picked a Laser L-I's points: 21, 41 and 81 give k = 0.2, 0.4 and 0.8 nm⁻¹ (Ev1 −0.81342, −0.81869, −0.85196 eV). A 1D plot group's `point` takes it too. **And one past the last is refused**, not answered with the last: "There is no point 82: its valence bands have 81 values of k (from 1)". The same for a sweep ("its sweep has 2 points") and a Laser L-I, and for a study of one solution ("the study has one solution, no sweep"). |
| **6.2 `fem_plot` adds to a stale data display.** `gaas10_kp4_pg1d2.dpl` held an older group's diagram of six traces (another `pg1d2`, from the day before); the new diagram went beside it, four of the old one's traces "no data". | **Reproduced**, on a copy of the reviewer's display with an older "Band Diagram (qw)" diagram of six traces put back in. The group's own display replaced only its diagrams of the group's title, and the older group had another. **Now a diagram of the group's display with a trace the newly written dataset lacks is removed**: it can only be an older group's of that tag, or traces renamed since. `fem_plot` says so: `"removed from its display": ["“Band Diagram (qw)” (its qw.Ev, qw.wave_hh, qw.wave_lh, qw.Elh not in gaas10_kp4_pg1d2.dat)"]`, and the model's log too. The new diagram takes the old one's place. The user's own diagrams of the dataset's traces stay: the reviewer's restyled one, its title changed, was kept. |
| **6.3 Solving a new study adds the default plot groups again**: after `std2` and `std3`, "Electron Probability (qw)" and "Heavy Hole Probability (qw)" three times each, "The Spectrum" twice, and a growth axis, a band diagram and the states beside the reviewer's own. | **Reproduced**, and found two causes and a third fault. (1) A study was given defaults unless a 2D plot group pointed at it: the reviewer's 1D groups for `std2` and `std3` did not count. (2) Each study's defaults were all made again, one set per study solved. (3) The Valence Bands study was given "The Spectrum" of `qw.alphaTE; qw.alphaTM`, a spectrum's absorption: a bands solution is in the frequency domain, and was taken for a spectrum. **Now:** a study with plot groups of its own, 1D ones too, gets none. A default of a name the results already have (the same physics' field, of another study) is not made again, so there is one set per kind of study: one of each probability, the states' band diagram and levels, the spectrum, and the bands' **"The Valence Bands": `qw.Ev*` in eV, E(k)**. A band diagram made again along a growth axis that is there draws along it, not a new one. **The reviewer's model, as it was before solving** (std1's defaults, their own cut line and three 1D groups): solving `std2` and `std3` adds nothing. **The same model without the reviewer's groups**: std1's three, then for `std2` the growth axis, the band diagram and "The States", for `std3` "The Valence Bands"; no copies. The copies already saved in `gaas10_kp4.qfem` (pg3–pg6, cln2, pg1d5–pg1d7) stay until deleted. |
| **Also seen: `fem_model save` in a brief batch said "done (2 fields)"**, so which model was saved was not told. | **Reproduced.** A brief batch's line took a few known keys, and `fem_model save`'s answer (`saved`, `model`) had none of them. **Now `saved` is one**, and a path cut to fit keeps its end, with the file's name: the reviewer's (86 characters) reads "saved …/meisam/QucsWorkspace/sorianello2020_graphene/hhlh_exciton_check/gaas10_kp4.qfem". Every value was cut to its first 80 characters, so the file's name was what went. **An answer with none of the keys** names the node it made or set ("[2] fem_edit: set My levels, again (mine)"), else its first short fields ("[1] fem_build: boundaries 10, domains 3, points 8"; "fem_solve: seconds 0.336, study std2, triangles 505"), not their count. |

## Tests

- **`test_fem_exciton`:** `theContinuumWidenedSaid`, the warning's new figures and wording.
- **`test_multiphysics_doc`:** `claudesToolsShowTheWellsInEV`, a point past a sweep's last refused; `aWellsStudiesThroughTheTools` (new), on a small well with four studies (a spectrum, two of states, valence bands), driven through the tools:
  - each study's defaults: the spectrum's three; none for a study with a 1D group of the user's; the states' band diagram and levels, not the probabilities again; the bands' "The Valence Bands", `qw.Ev*` in eV; one of each;
  - the states' groups deleted and solved again: made again along the growth axis that is there;
  - `point` 2 on the bands, k = 0.25 nm⁻¹; 6 of 5 refused; 2 on a states study refused;
  - the bands' display with an older group's diagram (a trace not in the dataset) and the user's own: the older one removed and said, the user's kept;
  - a brief batch: the save's file, a node set by name, a build by its counts.

Fifteen changes were broken on purpose, and all fifteen were caught:

| Break | Caught by |
|---|---|
| X1 the ripple's old figures | theContinuumWidenedSaid |
| X2 a study's 1D groups not counted | aWellsStudiesThroughTheTools (the user's study given defaults) |
| X3 every default made again | aWellsStudiesThroughTheTools |
| X4 a new growth axis, one there | aWellsStudiesThroughTheTools (the band diagram along another line) |
| X5 the bands given a spectrum's absorption | aWellsStudiesThroughTheTools |
| X6 the bands' group called "The Spectrum" | aWellsStudiesThroughTheTools |
| X7 the bands' k not by number | aWellsStudiesThroughTheTools (point 2 at the last k) |
| X8 a point past the last answered | aWellsStudiesThroughTheTools |
| X9 an older group's diagram kept | aWellsStudiesThroughTheTools |
| X10 its removal not said | aWellsStudiesThroughTheTools |
| X11 a node set not named in a brief line | aWellsStudiesThroughTheTools |
| X12 `saved` not among the brief line's keys | aWellsStudiesThroughTheTools |
| X13 a saved path cut at its end | aWellsStudiesThroughTheTools (the file's name cut off) |
| X14 no short fields: their count | aWellsStudiesThroughTheTools |
| X15 a trace naming its dataset (`set:var`) taken for one it lacks | aPlotGroupShownAgain (the user's diagram removed) |

X15 came from the full suite. The first form of the stale rule looked a trace's name up as it was, so a user's diagram of `guide_again_pgl:Loss` was taken for an older group's and removed. Now a trace of this display's dataset is looked up by its variable, and one of another dataset is left alone.

Not tested: the same one-of-each rule for a PDE's "Solution" group along its line (the code is the band diagram's).

## Runs

- **Release:** the changed functions pass, and the Release suite 131 of 131.
- **The scenarios** (`scripts/mcp-e2e-scenarios.py`, the build's app): 97 checks, none failed.
- **The reviewer's model over `qucs-s --mcp-server`**, on the final build: each item as in the tables above.
- **Sanitizers** (ASan and UBSan), with no report: `theContinuumWidenedSaid`, `aWellsStudiesThroughTheTools` (53 s alone), `claudesToolsShowTheWellsInEV`, `aPlotGroupShownAgain`.
- **`test_multiphysics_doc`'s time on CI's Linux job** went from 1131 to 2254 s of its 2700 (900 s times the scale of 3) over the last five runs. This round adds some 150 to 250 s there, so its TIMEOUT is 1200 now (3600 s there). The job's limit, 330 minutes, has room.
