# Feature gaps: the multiphysics solver against a paper

*11 October 2026 — Qucs-S 26.1.8.*

The check (`2026-10-11-claude-fem-pn-junction-paper-check.md`) was written on the
installed build of `73bf6fe`. It modelled Sugesh and Sivasubramanian's corrugated pn
junction phase shifter (*Silicon*, 2021) with Claude's `fem_*` tools.

- **Its result:** a V_πL of 0.86 V·cm at 5 V, against the paper's 0.75.
- **How:** the semiconductor physics was not in that build, so the carriers came from
  the heat physics used as a nonlinear Poisson solver.
- **What it found:** one bug, one trap and four minor notes.

Each was first reproduced through `qucs-s --mcp-server`, with its own HOME, settings,
trash and cache, on a copy of the reviewer's model at one bias (5 V).

Reproducing showed the bug to be wider than reported:

- `fem_solve`'s answer had no warnings at all.
- A Newton's method that did not converge was said only in the log.
- The same one-step clamp hit every step built since without a Maximum iterations:
  Quantum States, Optical Spectrum and Eigenvalue.

All of it is done here, on top of `13c0dc4`. One more problem was found on the way.

## 1. A mode step gave the physics it solves first one Newton step

| Review | Now |
|---|---|
| A mode step with `"physics": ["ht", "emw"]` solved the heat (the Poisson stand-in) with one Newton step. `setupStep` took `maxiter` from a node that has none: 0, clamped to 1. The modes were then solved on that field without an error (T_min 97.3 K, not a physical potential). | A step without a Maximum iterations keeps the step's default, 50. The reviewer's mode step now converges in 18 Newton steps, T_min 99.46 K - the field a Stationary step before it gives. |
| Give the mode and frequency steps `maxiter` and `nonlinear`. | The Mode Analysis, Quantum States, Optical Spectrum and Eigenvalue steps have **Maximum iterations** (50) and **Nonlinear method**, for the physics at rest they solve first. The Frequency Domain step had a Maximum iterations, and now has a Nonlinear method too. `fem_edit` takes them. |
| A Newton's method that does not converge should fail the step, or at least show in `fem_solve`'s warnings, not only in the log. | **Both.** A mode, states, spectrum or eigenvalue step whose physics at rest did not converge **fails**. It names that physics and says what to do: raise the step's Maximum iterations, or solve the physics in a Stationary step before it. What it solves would otherwise be of a field not solved. Every warning the physics put in the log is now also the **study's warning**: a Newton's method that did not converge, a coupling that did not agree, an eigensolver's restarts. Each is said once in the window's messages and in `fem_solve`'s answer. |
| The time-dependent step has the same pattern and may have the same problem. | It has its own Maximum iterations (25), so it was not clamped. A Newton's method that does not converge in it now shows among the study's warnings too. |

Found on the way: **`fem_solve` never returned warnings.** A coupled study that did not
agree, or a sweep's point that warned, was said in the window's messages only. The answer
has a `warnings` list now.

## 2. The contact blocks guide light, and the mode search found them first

| Review | Now |
|---|---|
| The P+ and N+ blocks (1 µm × 220 nm) guide modes of their own (n_eff 2.815, 2.810), above the rib's TE0 (2.566). With `modes: 1, neff: 2.8` the solver gave a block's mode: 259 dB/cm, its index moved by the bias by 10⁻⁹. The rib's TE0 was mode 5 of 6, and its number changes with the geometry. Suggested: a selection by field overlap, `"track": {"only": ["ntop"]}`. | The Mode Analysis step's **Keep the modes most in** (`track`), a domain selection. Of the modes found about the index (at least n + 6 of them for n asked), those with the most of their \|E\|² in the domains tracked are kept. They are then ordered by effective index. The reviewer's study, `modes: 1` about 2.8 with `"track": {"only": ["ntop"]}`, now gives the rib's TE0: n_eff 2.567, 8.99 dB/cm at 5 V. The log gives each kept mode's share. |
| A per-mode confinement global, `emw.Gamma_<selection>`, so a sweep can be filtered afterwards. | **`emw.Gamma`**: the mode's share of \|E\|² in the tracked domains (0.188 for TE0 in the rib's top 80 nm). It is a global like `emw.neff`, at each mode, sweep point and frequency. A step tracks one selection, so the name has no suffix. |

## 3. Minor

| Review | Now |
|---|---|
| A `global` derived value with an empty `study` was evaluated against the first study (no modes): "'emw.neff' is not a parameter, a variable or a function here". Default to the study that has the variable, or name the study tried. | Both. Without a study, a derived value the first study cannot evaluate is evaluated with the first computed study that can. Its answer says which (`note`: "of Modes vs bias: the first study has not what it reads"). When none can, the error names the first study and says to give the node its Study. `fem_evaluate` without a `study` does the same, and says which study it used. |
| `fem_evaluate` with `expression: "a; b; c"` and `at` points failed with "unexpected ';'". | Split at the points as for globals. Each expression is evaluated at each point, and each value is named. |
| `emw.loss` is labelled `1/m`; it is dB/m. | **dB/m**. A global may now carry a unit its dimension cannot say, and `emw.loss` does. `fem_solve`, `fem_evaluate`, `fem_describe` and the derived values show it in that unit. A unit asked for (`1/cm`) still converts it. |
| The `integral` of `fe(T)` was reported in m². That is correct, as the function has no unit, but easy to misread. | The answer has a `note`: "fe has no unit (its Function's unit): what is integrated is a number, the integral in m² - the area's unit alone". It is given when an integral of a number calls one of the model's functions that has no unit. |

Found on the way: **`fem_evaluate` without a `study` refused when the first study was not
solved**, though another was ("The study is not solved yet"). It now takes the first solved
study and says which.

## The review's "not tested": drift-diffusion against heat-as-Poisson

The semiconductor physics is in `1ef149d`. The reviewer's own drift-diffusion model,
`corrugated_ps_dd.qfem`, was run here at 0 and 5 V through `fem_solve`, the rib's TE0
tracked as in section 2 (26 s):

| Quantity | Drift-diffusion | Heat as Poisson (the review's) |
|---|---|---|
| n_eff at 0 V | 2.5664370 | 2.5664278 |
| Δn_eff, 0 to 5 V | 4.477 × 10⁻⁴ | 4.515 × 10⁻⁴ (+0.8 %) |
| V_πL at 5 V | 0.866 V·cm | 0.858 V·cm |
| Loss at 0 / 5 V | 17.49 / 8.89 dB/cm | 17.66 / 8.99 dB/cm |

The two agree within 1 %, as the reviewer reasoned they should under reverse bias, when
almost no current flows. The gap to the paper (0.75 V·cm, and the loss 1.7 times the
paper's) is the model's doping, not the solver's.

The paper's Gaussian implants were not run here. They need nothing new: the semiconductor's
doping takes a box with Gaussian tails, or any expression of x and y.

## Tests

- **`test_fem_waves`:**
  - `modeStepSolvesWhatItReads`: a core heated through a k(T) oxide, its mode
    thermo-optic. A mode step solving `["ht", "emw"]` converges, to the same T and n_eff
    as a Stationary step before it (10⁻⁶ K, 10⁻⁹). Asked for one Newton step, it fails,
    saying why. A Stationary step's non-convergence is among the study's warnings.
  - `trackingAMode`: a wide block and a narrow wire 2 µm apart. About n 3 the block's mode
    comes first; tracking the wire gives the wire's (its index the wire's alone, to
    3 × 10⁻⁵), with its `emw.Gamma`.
- **`test_multiphysics_doc`, `aPaperChecksNotes`:** through Claude's tools:
  - `emw.loss` in dB/m in the modes and the globals;
  - `fem_solve`'s `warnings` for a heat solve in one Newton step;
  - the global with no study finding the modes' study;
  - the integral's note;
  - `fem_evaluate` with two expressions at two points;
  - without a study, a solved study taken, and the first study named when nothing has
    the expression.

Each of the 13 changes was broken in turn, and its test failed.

The default of 50 is a second guard. No step lacks a Maximum iterations now, so broken
alone it changes nothing. Broken together with the mode step's new property, the test fails
with the step's new error ("Heat Transfer in Solids did not converge in 1 Newton steps…").

The full ctest, the MCP scenario script and the DMG's MCP check (now with this round's
scenario) pass.
