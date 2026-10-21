# A photodiode under strong light, 8: an avalanche diode past its wall

The last of the nine missing pieces of a photodiode's physics (`2026-10-20-claude-photodiode-circuit-and-slices.md` lists them): the convergence wall the APD round found on 18 October (`2026-10-18-claude-lumerical-apd.md`, "Not fixed: a wall").

**Before:** the APD round's case found no solution past −22.7 V, its breakdown 51.7 V away:
- the diode: one-sided silicon, p⁺ 1e19 cm⁻³ (0.5 µm), n 1e16 cm⁻³ (4 µm), meshed at 0.1 µm;
- velocity saturation along the current, impact ionization;
- lit past 3 µm (1e24 pairs/(m³ s)), beyond its depletion.

Swept from −10 V, it stopped at −22.7 V with Fermi-Dirac statistics and at −25 V with Boltzmann's. A single point at −30 V failed outright. This held on this round's build before the fix and on s27.0.1 (82669a9) alike, the same model through each one's tools.

**Now:** it sweeps to −50 V, 1.7 V short of breakdown, in under half a second a sweep. Its multiplication is McIntyre's.

## The cause

**Impact ionization is counted by triangle:** (αn |Jn| + αp |Jp|)/q, its currents fitted from its three edges' current densities. Each edge's current density is its flux over its dual face:
- the flux is the continuity equation's own, (Σ cot µ) q Vt (n_j B(d) − n_i B(−d)): each triangle's cotangent times its own mobility, the finite elements' form;
- the face is (Σ cot) h.

Their ratio is the edge's mobility only where its two triangles have the same one. With saturation they do not: each triangle's mobility is its own field's.

**One edge of the 0.1 µm mesh** (from node 69 at (1.5, 0.2) µm to node 183 at (1.396, 0.135) µm) had its two cotangents all but cancel. Its face was 1.4·10⁻²³ m², against 10⁻⁸ for its neighbours, and its ratio was then anything. The triangle's fitted holes' current was 7·10⁵ A/m², in a diode carrying 0.04.

**Newton's method** settled the potential to 10⁻¹² Vt. The electrons at the top edge's node beside it, (1.4, 0.2) µm, then had only a negative solution:
- they were cut to a twentieth (the positivity clamp) every step;
- their equation stayed 15 % off;
- the node below cycled with them, six steps a turn.

(The round's note had a hole equation at that edge 2·10⁻⁴ off. That was the same noise, at another step.)

Without saturation both triangles have the same mobility, and the ratio is exact. That is why the wall needed saturation.

## The fix

Where a current density is taken from an edge (impact ionization's fit), its mobility (Σ cot µ)/(Σ cot) is bounded by its triangles' largest. Where the two are the same, the result is the same to its rounding.

The continuity equation's flux is unchanged: it never divides by the face. The hot carriers' gift, the other fit of the edges' currents, already weighs each edge by its face (`2026-10-21-claude-photodiode-hot-carriers.md`).

## Checks

The test is `anAvalanchePastTheWall` in `test_fem_photodiode_power`. The multiplication M is the current over the same diode's without ionization's pairs in its continuity (Impact Ionization's "In the carriers' continuity" off). McIntyre's is for holes entering the field, 1/(1 − Ip), Ip the solution's ionization integral (`semi.Mp`).

| Va | Current | Without the pairs | M | McIntyre's |
|---|---|---|---|---|
| −10 V | 5.564 nA | 5.563 nA | 1.0002 | 1.0002 |
| −20 V | 6.703 nA | 6.640 nA | 1.0095 | 1.0096 |
| −25 V | 7.424 nA | 7.217 nA | 1.0286 | 1.0289 |
| −30 V | 8.362 nA | 7.808 nA | 1.0710 | 1.0720 |
| −40 V | 12.52 nA | 9.194 nA | 1.3614 | 1.3687 |
| −50 V | 63.95 nA | 10.90 nA | 5.867 | 6.645 |

- **To −40 V** the two agree to 0.5 %.
- **At −50 V** they differ by 2.4 % in the integral, 1 − 1/M. M magnifies this near breakdown. It converges with the mesh: 0.8 % at 0.05 µm (M 6.24 against 6.52) and 0.3 % at 0.025 µm (6.35 against 6.47).
- **Fermi-Dirac's** current is the same to 0.6 %: the n side is not degenerate.

The test runs on every build: 1.3 s in Release for its three sweeps. Sanitized builds skip Fermi-Dirac's: 65 s there for the other two, 114 s with all three. `test_fem_photodiode_power` took 1050 s sanitized beside ten other tests. Its ctest limit is now 1800 s, times CI's scale of 3.

## Break checks

| Break | Caught by |
|---|---|
| The edge's mobility unbounded (as before) | `anAvalanchePastTheWall`: stopped at −25 V |

**Not caught:** a sliced device's bound kept with each slice's weights. No test has an avalanche diode in slices. The bound travels with the weights wherever a slice's are set.

## Next

None of the nine remain.
