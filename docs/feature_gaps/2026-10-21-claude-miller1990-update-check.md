# Quantum wells and drift-diffusion: the Miller update check of 21 October

The check (`qucs-reviews/2026-10-21-claude-miller1990-update-check.md`) went over 68e5f16, the commit that answered
[the Miller 1990 Fig. 3 review](2026-10-21-claude-miller1990-fig3-mqw-absorption.md). It ran in the app against the
reviewer's own references: a two-band Kane well, a Kronig-Penney superlattice, and Aspnes and Studna's GaAs.

**Its passes:** all five bugs of that review were fixed, and the AlGaAs gap fit was right. The new features
(periodic ends, the bulk's excitons, Urbach's tail, `linewidthslope`) also agreed with the references.

**Its issues:** seven, found as it went on:
- in the quantum wells;
- then in a drift-diffusion run of Miller's 65-period p-i-n (section 6);
- then in a reproduction of Weiner, Miller and Chemla's 1987 Δn against field (section 7).

Each is answered below. Every number was checked on the reviewer's models, copied to a scratch folder:
- `bulk_gaas_check.qfem`;
- `fig3_period1.qfem`;
- `pin_dd/miller_pin_65.qfem`;
- `weiner1987/weiner_fig2.qfem`.

## The issues

### 1. A flag given as a parameter was silently false

`"bulkexcitons": "ex"` with `ex` swept 0, 1 gave the free carriers at both points: 0.681 of Aspnes and Studna's
at 1.63 eV each time.

**Now** a flag (any true/false property: `excitons`, `bulkexcitons`, `conservearea` ...) may be an expression of the
parameters. It is read at each point of a sweep, true where it is not 0. On the reviewer's model the sweep now gives:

| ex | 1.55 eV | 1.63 eV | 1.70 eV |
|---|---|---|---|
| 0 | 0.681 | 0.678 | 0.682 |
| 1 | 1.121 | 1.011 | 0.966 |

These are the reviewer's `true` (1.12, 1.015, 0.965) and `false`.

A flag that is not a constant there is refused, the flag named:
"Optical Spectrum: its "bulkexcitons" is "maybe" - true, false or an expression of the parameters: 'maybe' is not a
parameter, a variable or a function here". A mistake in another study's flags is that study's alone.

### 2. The doc's n = 3 "line at 1.745 eV, 35 meV high"

`linewidthslope` widens the lines to some 18.5 meV at 1.72 eV, which moved the step's maximum 14 meV up.

**Now** the earlier doc's row and its paragraph are corrected, linking here: n = 3 is at 1.729 eV, 9.3 ×10³/cm, some
25-30 meV above the measured with the 10 meV offset, and about half as high. The help of "linewidthslope" says it: "A
step broadened so has its maximum the higher the wider it is: Miller's n = 3 at 0.05 some 14 meV above its edge".

### 3. n = 3 and the barriers' edge in one run

Under `continuum: bound` the barriers' excitonic edge was there but not the e3 miniband, which lies above the
barriers. Under `continuum: states` it was the other way round.

**Now** `bulkexcitons` and `urbach` act under `continuum: states` too. The box's states above the barriers stand
for the barriers' free carriers, so the barriers' bulk adds only its excitons' excess:
- Elliott's absorption with Urbach's tail;
- less the free carriers' √ε, broadened by the spectrum's own line as the box's states are.

So nothing is counted twice. Urbach's width set under `states` without `bulkexcitons` is said to be unused.

On `fig3_period1.qfem` (×10³/cm):

| | 1.70 | 1.72 | 1.729 | 1.745 | 1.76 | 1.77 | 1.78 |
|---|---|---|---|---|---|---|---|
| states | 7.45 | 8.17 | **9.31** | 8.57 | 8.62 | 8.42 | 8.53 |
| states + bulkexcitons + Urbach 10 meV | 7.45 | 8.18 | **9.33** | 8.64 | 8.94 | 9.28 | **10.84** |
| bound + bulkexcitons + Urbach 10 meV | 7.27 | 7.13 | 7.34 | 7.13 | 7.52 | 7.93 | 9.56 |

The second row has both: n = 3 at 1.729 eV and the barriers' edge rising from 1.76 eV.

### 4. The coupled excitons' continuum warning overstated

"152 meV apart, not the 2.75 the line's width asks" appeared for a spectrum that a size of 4000 moved by 1 % at
most.

**Now** the warning is by its effect: the ripple measured on a 10 nm GaAs well's 4x4 bands, at the ripple's peaks
±11 % at 0.5 meV, ±4 % at 1 and ±1.5 % at 2 (past 2 meV on that slope). It is said where the ripple is 2 % or more:
"... its absorption ripples about its trend some ±4 % at the ripple's peaks at this line's 1 meV - a wider line
smooths it, a larger size little". The Fig. 3 model's 5.5 meV (some ±0.4 %) says nothing.

### 5. The default spectrum plot in 1/m

**Now** "The Spectrum", made after a solve, plots α (or the gain) in 1/cm, as the panel and the plots made by hand
do.

### 6. A two-parameter sweep stopped when its outer parameter stepped

"Parametric Sweep stopped at Nb = 1e+15 1/cm^3, Va = 0 V (point 5 of 12): ... no equilibrium potential found in
200 steps". The next doping's 0 V started from −10 V.

**Now** the first point of an outer value (every combination, the last list the innermost) starts from that inner
value's solution at the outer value before. A point that finds no solution from where it starts is solved once more
from the start before the sweep stops, and said: "Nb = 1e+15 1/cm^3, Va = 0 V: no solution from the point before
(...): solved from the start".

The reviewer's 65-period p-i-n, sweeping Nb 1e14, 1e15, 3e15 and Va 0, −2, −5, −10 V: all 12 points in 6.5 s.
- With the start rule undone, the retry solved point 5.
- With both undone, the sweep stopped at point 5 again.

A single 9.8 nm barrier between 100 nm contacts does the same in 0.5 s, and is the test.

**6a. The results at the line's resolution.** A semiconductor solved in 1D (`h1d` 0.5 nm) was read at a point from
the 2D mesh's nodes, up to 5 nm apart. Now V, n and p are read along its line at any point:
- V linear between the line's nodes;
- the carriers in their logarithm;
- the field, the line's slope;
- the bands, of V.

On the test's coarse mesh (nodes 70 nm apart) they match the layered 2D reference between the nodes to rounding
(V 2e-15 V, n and p 8e-14, Ey 2e-14). Before, n 3 nm from the anode read 1.0e9 per m³ against the reference's
8.5e12. The currents and a coupled physics are still the mesh's, and the warning says so.

**6b. A field export along a line.** A grid of one column (`nx` 1) or one row is taken at the corners' x (or y),
their middle where they differ. The corners may then share that coordinate. It was refused, and `nx` 1 would have
divided by nought. On the reviewer's model, the export from [5, 0] to [5, 2270.8] writes its column.

### 7. Kramers-Kronig Δn in a field

**7a. `continuum: bound` lost a state's strength in a field.**

*Why.* `bound` kept a pair only when both its states were bound. In a field a bound electron's strength moves to hole
states that the field frees. They were dropped, and the barriers' bulk, the same in any field, did not give it back.

*Now.*
- A bound electron's pairs are kept with every hole: its strength Σ|⟨e|h⟩|² is 1 over them whatever the field.
- An unbound electron's pairs are the barriers' bulk's. So each electron is counted once and the total is kept.
- A bound hole's pairs with unbound electrons are not added, since the bulk already counts those electrons.
- In a field, a state is bound too when the field tilts it past its lower barrier's edge but it stays below the
  higher one's and its well holds it (its share of |ψ|² there at least "pmin", 0.6 unless given, now read under
  `bound` too). The spectrum says which: "in this field past their lower barrier's edge, below their higher one's
  and held in their wells, so bound: e3 (...)". The Weiner model's states up to 150 kV/cm need none of it: the pair
  rule above is what keeps its strength.
- A box's state above both barriers stays the continuum's, held or not. A slab of one gap holds nothing.

On the reviewer's Weiner model (coupled excitons, 3/8 subbands), the TM area over 1.38-1.95 eV (10³/cm·eV) and
qw.dnTM(1.40 eV) less its zero-field value:

| F (kV/cm) | 0 | 65 | 150 |
|---|---|---|---|
| area, before | 5.16 | 4.87 | 4.60 (−11 %) |
| area, now | 5.21 | 5.11 | 5.09 (−2.2 %) |
| Δn, before | 0 | −1.4×10⁻³ | −3.8×10⁻³ |
| Δn, now | 0 | −0.36×10⁻³ | +0.59×10⁻³ |
| Δn, now, 8/20 subbands | 0 | −0.05×10⁻³ | +2.43×10⁻³ |
| Weiner et al. | 0 | +0.4×10⁻³ | +2.3×10⁻³ |

With too few subbands a bound electron's partners in a field lie past them, and their strength is left out. 7c says
so.

The check's note "under `continuum: bound` the one-period spectrum has a bump at 1.63 eV (8.6 against 7.7 under
`states`)" goes with the same change: it reads 7.7 there now, as under `states` (the reviewer's saved spectrum:
8.67 at 1.62, 8.60 at 1.63). Which pair made it was not followed.

**7b. A local Δn.** On a spectrum in a sweep, `dnwindow` (two energies) gives qw.dnTEloc and qw.dnTMloc. They are
the Kramers-Kronig transform of Δα, each point against the first, over that window alone. With `conservearea`, each
point's α there is first scaled to the first point's area there: Weiner, Miller and Chemla's method.

On the Weiner model (1.38-1.50 eV, area kept), qw.dnTMloc(1.40 eV) is:
- +0.63×10⁻³ at 65 kV/cm;
- +4.27×10⁻³ at 150 kV/cm.

The reviewer's own script of the paper's method gave a fit of 1.54×10⁻¹³ cm²/V² on QFEM's α, which is 0.65×10⁻³ at 65
kV/cm. It is the same whatever the spectrum's top, and its first point is nought. A window not of two rising energies
is said, and none made.

**7c. A sum-rule warning.** After a sweep of a spectrum, each point's ∫α dE over the photons' window is compared
with the first point's, warned past 3 %. The reviewer's setup with free pairs, under the old rule (both states
bound), gets:

"qw.: the absorption's sum over the photons' window (∫α dE) at F = 15 MV/m is 0.839 of the first point's (TM): its
strength not kept - a state's partners in a field past the subbands taken (more keep them: "cstates", "vstates"), or
a box's states above the barriers moving. The index between the points (qw.dnTE, qw.dnTM) moves with it, not with
the field's shift alone; a local one: "dnwindow", "conservearea"".

**What changes for a model as it is.** Under `continuum: bound`, a bound electron's transitions to unbound holes are
now in the spectrum, at every field. On Fig. 3's one period they move the region below the barriers' gap by up to
0.5 ×10³/cm (1.76 eV: the check's 7.0, now 7.5). At zero field below a barrier gap's 30 meV nothing changes in the tests' wells
(`aBoundContinuum`: the box's to 7e-16). A semiconductor solved in 1D reads finer at points between its mesh's
nodes. Nothing else changes values.

## Not done

- **The equilibrium solve from a start 10 V away** finds no potential in 200 steps (the single barrier of the test
  does too). The sweep no longer starts it there, and a retry from the start rescues it. A damped equilibrium walk
  that recovers by itself was not attempted.
- **Under `continuum: bound` the e3 miniband** above the barriers stays out, as before: it is unbound. Item 3's
  `states` with the barriers' edge is the way to have both.

## Checks

| Item | Test |
|---|---|
| 1 | `test_fem_exciton flagsOfTheParameters`: "bulkexcitons": "ex" swept 0, 1 gives `false`'s and `true`'s spectrum exactly; "ex > 0.5" as true; "maybe" refused, the flag named; another study's "maybe" not this one's |
| 3 | `test_fem_exciton theBarriersEdgeUnderStates`: the barriers' edge under `states` adds 50/60 of a barrier slab's (Elliott's with Urbach less the free carriers') at 52 and 42 meV under its gap and 150 meV above (0.8333 each), Urbach's e^(10/6) apart; Urbach without bulkexcitons under `states` said, with them not |
| 4 | `test_fem_exciton theContinuumWidenedSaid`: ±4 % at 1 meV said, 2 meV nothing |
| 5 | `test_multiphysics_doc aWellsStudiesThroughTheTools`: "The Spectrum"'s α in 1/cm |
| 6 | `test_fem_modulator aSweepsOuterStepStartsWell`: Nb 1e14, 1e15 by Va 0, −10 V all solved, nothing retried, the third as solved alone (1e-6 V); specified pairs −10 V then 0 V solved from the start and said |
| 6a | `test_fem_modulator semiconductorIn1D`: on the 70 nm mesh, between its nodes, V, Ec, n, p and Ey the 2D reference's to rounding |
| 6b | `test_fem_modulator fieldOnAGrid`: a column at x 0.5 (corners 0.5 and 0.5, 0.3 and 0.7), the values the solution's; two columns at one x refused |
| 7a, 7c | `test_fem_quantum aBoundStatesStrengthInAField`: a 9.4 nm well, 0 and 150 kV/cm, 16 states: ∫α dE TE 0.980 and TM 0.978 of the zero field's, not said; 2 states: said, the subbands named |
| 7b | the same: qw.dnTMloc nought at the first point, +5.5×10⁻⁴ at 1.40 eV and 150 kV/cm; the same to 1e-9 with the spectrum to 1.70 or 1.95 eV (the whole window's moves); another without its area kept; `conservearea` as an expression; a window turned about said |
| 7a (held) | the same: at 250 kV/cm e3, past its lower barrier's edge and below the higher one's, held and said; not with "pmin" 0.99; a 30 nm well between 3 nm barriers at 10 kV/cm holds none of the box's states above both |
| 7a (a slab) | `test_fem_quantum aBoundContinuum`: a slab of one gap in a field holds nothing |

**Breaks.** 28, all caught:
- 1: the flags not read; one read as false; another study's refusing this one;
- 3: the barriers' excitons not added under `states`; the free carriers not taken off (counted twice); taken off
  with Urbach's tail rather than the spectrum's line; Urbach's width unused not said;
- 4: the ripple said at any width; its law's 1 meV point 5 %;
- 5: the spectrum plotted in 1/m;
- 6: the outer step from the point before; no retry from the start;
- 6a: the carriers, the bands, V and its field read from the mesh's nodes again; the carriers linear along the line;
- 6b: one column taken as two; refused;
- 7a: a pair kept only when both are bound; none held in a field; held above both barriers; a slab held; "pmin" not
  read under `bound`;
- 7b: the area not kept; the local index over the whole spectrum; `conservearea` as an expression false; a window
  turned about not said;
- 7c: the sum said past 50 %.

Four were not caught at first:
- **Another study's flag:** this study had none as an expression, so its flags were never read. It has one now.
- **None held in a field:** no test's well tilted a state past an edge at 150 kV/cm. The spectrum now says the
  states it holds, and the test tilts e3 at 250 kV/cm.
- **Held above both barriers:** the rule was missing, found while making the test above. Without it a 30 nm well between thin
  barriers took e8 to e16, box states, as bound at any field. The rule and its test were added.
- **A slab held:** the rule above already holds nothing where a stretch has no barrier, so the separate check went.
  Its break skips the rule for a slab.

The field test's `QLatin1String` of "∫α dE" never matched (a UTF-8 literal read as Latin-1), so its negative
check passed whatever was said; it is a `QStringLiteral` now.

**Suite.** The whole suite passes in Release (135). The changed and new functions pass under ASan with no report.
The longest, `aBoundContinuum` and `fadingInEveryPath` at 185 s each, took 194 and 188 s before this round. The CI
lints pass.
