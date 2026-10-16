# Update check 2026-10-16 (09c4d91 .. 68c6656): each open item

The reviewer's update check (`2026-10-16-claude-update-check-68c6656.md`, its details in
`~/QucsWorkspace/update_check_1016/*/REPORT.md`, read, not changed) passed the commits and listed
eleven open items and some smaller ones. Each was reproduced first (the reviewer's models copied
to a scratch workspace, solved by the build's app through its tools), then fixed and tested.

## The open items

| # | Feedback | Now |
|---|---|---|
| 1 | qwabsorption Δn 20-50 % off when the photon is within rounding of a Spectrum energy (the photon appended beside it: an interval of no width) | Reproduced exactly: 8.95e-4 against the spectrum's own 7.44e-4. Three things, the last the largest: the photon not appended when one of the energies is it to rounding (1e-9); the Kramers-Kronig integrand at E (and at a twin within rounding) its limit α'(E)/2E - it was nought there, an error of order h, and a quotient of two roundings beside the twin; and the integral's own energies between those asked, at most an eighth of the line's half width apart (the absorption there alone, not points of the spectrum). On the reviewer's grid (10 meV, a 6 meV Gaussian) Δn at 1.46 eV is now 1.05546e-3 against 1.05512e-3 converged (4801 energies, 0.125 meV) - it was 7.44e-4, 29 % low, whatever the photon's digits. The quantum test's own reference had the first flaw (its \|x² - E²\| < 1e-40 J² left ±1.2 meV about E out - 1.1 % off beside a step, as the solver's own): corrected, the solver within 8e-6 of it on 61 and on 241 energies. A laser's gain table, needing α alone, takes no extra energies. |
| 2 | In0.53Ga0.47As dn/dT -2.0e-4 per K (wrong sign); 1.3Q +9.7e-5 (half); binaries flat in T; compounds no dndT | A compound's index at its temperature: its 300 K index and the change of its oscillator with its gap and E0 moved together by the gap's (Varshni's) shift - all the gaps move alike; its gap alone moved, the index below the gap hardly did and above it (held there) fell. A compound's dndT that derivative, so the Thermo-optic option takes compounds. Against Franta et al.'s GaAs at 300 and 370 K: 2.09e-4 per K at 1.55 µm (theirs 2.25e-4), 1.86e-4 at 5 µm (1.87e-4); 14-20 % low within 0.1 eV of the gap. In0.53Ga0.47As +2.4e-4 per K at 1550 nm; the 1.3Q InGaAsP 2.07e-4. The library's binaries (GaAs, InP...) keep their 300 K index and their measured dn/dT through the Thermo-optic option - an index of T there too would count it twice -, said in their descriptions and the format text. |
| 3 | fem_model new + replace on an open path: a second tab, the tools on the stale one | That tab takes the new model (one tab of the file, the tools on it); a tab with changes not saved refused, not thrown away. fem_model import alike. |
| 4 | choose "te" picks a mode below the cladding's index; track drifts along a sweep; fem_solve's sweep summary names another mode | te (tm): the TE-like (TM-like) mode of the highest index - its TE fraction over (under) a half -, the most TE (TM) when none is: on the reviewer's rib TE0 at 3.2276 (TE 0.9993) over the MQW's slab mode at 3.177 (0.9996). Tracking domains, a step searches about its own neff, not the last point's (said): the ridge swept to 2 µm gives 3.1667, Γ 0.19, as the 2 µm ridge alone (it drifted to 3.103, Γ 0.096). The summary: each point's globals its chosen mode's - a mode step leaves its chosen mode's fields after its last mode's snapshot, and the snapshot cache took them as that snapshot's (Solution::unselect()); fem_solve selects each point's chosen snapshot. |
| 5 | VCSEL LP11 splitting not mesh-converged; seven figures overstate it | Not a solver fault - the oxide aperture's corner converges slowly -, so said: fem_solve's resonator output has "figures" (the solve's on this mesh; its error the mesh's, larger; solve again finer before reading splittings), the format text likewise, and the 68c6656 write-up carries the reviewer's mesh table. |
| 6 | Resonator input taken silently: a mode out of range clamped (and kept in a plot group), m 1.5 as 0, count 0 or 500 as 1 or 200, order 3 as 2, a mode step as an eigenfrequency step; low-Q Mie poles never returned; the PML's strength not settable | Each refused, said: fem_evaluate's and fem_plot's mode from 1 to the solution's count; m a whole number; count 1 to 200; any physics' choice not one of its kind's (order 3 for every physics, not only the resonator); a Mode Analysis step on a resonator. A pml's "reflection" its strength (1e-6, e^-13.8, as before; 0.1 moves a Mie sphere's Q from 46.9 to 51.7). Modes of Q near 1 are mostly in the layers and left out with the layers' own - said in the format text. The default plots of a resonator no arrows (the example's taken out too). |
| 7 | Near-gap index held flat inside the range: AlSb +0.45, AlAs +0.26, InGaAsP +0.10, GaAs +0.08 | The oscillator's gap term, ln((Ef² - E²)/(Eg² - E²)), broadened by 20 meV (the real part of its log at E² + 2iEΓ): finite at the gap and on above it, not held at 0.995 Eg, where its spike stood. Adachi's InGaAsP of the photon's energy E + i·50 meV (f's real part; broadening only its √(1 - χ) was 0.007 off at 2 µm). The range from 0.2 eV above the gap (0.1 before), or from 0.15 eV below the direct gap for an alloy mostly (0.6) of AlAs, AlSb, GaP, AlP, whose oscillators leave the measured near their direct gap. In range now: GaAs within 0.03 of Aspnes's and Jellison's (Papatryfonos's 0.06 above - the references 0.08 apart), InP 0.016, AlGaAs x 0.315 0.008, InGaAsP (y 0.24) 0.035 of Adachi's tables, AlAs 0.04, AlSb 0.05. The 1.3Q at 1550 nm 3.386 (3.389 held). |
| 8 | 1D semiconductor 3-10 times slower than a 2D layered mesh, its unknowns miscounted; profiles only as fine as the 2D mesh, silently; a mesa's current 4-35 % off, silently | Its strip two columns across, not eight (the eight kept its middle from the sides' leaning dopings - item 9 is gone): the reviewer's PIN swept over 8 voltages 1.0 s at 5 nm and 3.6 s at 1 nm (4.1 and 22.7 s before; its 2D Layered Region at 1 nm 2.5 s here, 2.3 the reviewer's), its current at 0.8 V 1.819 A, the reviewer's own Scharfetter-Gummel. Its unknowns counted on its strip (the log, fem_solve). Said: its "at" not given (the domains' middle taken), a stack wider than its narrowest domain (a 2D solve's current spreads there: a mesa), this mesh along its line coarser than its own elements (what reads it here sees it that coarse - a Layered Region along the line keeps its detail). |
| 9 | Layered Region side walls: a node where two layers meet takes 1/3 and 2/3 of their dopings, 8.5 mV across a uniform PIN | A node's doping its domains' share of its circumcentric (Voronoi) box - a mixed one where a triangle is obtuse -, not of its triangles' areas: a rectangle halved by its diagonal gives each corner a quarter, so a side's node as a middle's. The reviewer's PIN: Ndop 5.001e23 at both walls and inside (3.33e23 and 6.67e23 at the walls before), V level across; the 1D and 2D solutions equal to 1.6e-15 V at every node, the sides' too. |
| 10 | grating_orders' "Each order" errors from 15°; R_n2, T_n3 missing; orders not propagating should read 0; one at its cutoff listed in the substrate, not in the air; fem_solve's seconds the last point's | Each order that propagates at some angle (|m| up to (k + k_in) L/2π for its port's medium) has its global at every angle, nought where it does not; the example's Each order lists R_n2 to R_1, T_n3 to T_2 and reads at all nine angles. An order at its cutoff (β² nought to 1e-9 of k²) closed in both media alike. fem_solve's seconds the sweep's, every point's together. |

## The smaller ones

| Feedback | Now |
|---|---|
| InGaAsP y 1 3.612 against InGaAs 3.514 | At 1550 nm (above both gaps) now 3.522 and 3.568; at 2 µm 3.471 and 3.480 (0.03 apart before). An alloy's oscillator bowed as half its gap: In0.53Ga0.47As within 0.006 of Le et al.'s measured at 16.7 µm (0.027 under), Ga0.51In0.49P between Ferrini's and Kaneko's (0.03 apart). Its ends its binaries' own: AlGaAs at x 0 and 1 GaAs and AlAs (0.006 and 0.03 apart), GaInP at x 0 InP (0.005). |
| SiGe untouched: constant n, no κ, 4.275 at x 1 | Silicon's and germanium's dispersions mixed by x, its ends theirs (at 1550 nm the same as before); its range 1.9-14 µm, where both are smooth - nearer the gaps it is too high (Jellison's ellipsometry 0.04 at 0.84 µm and 15 % Ge, 0.26 at half), said; κ none, said (its edge moves with x). |
| ALD Al₂O₃ and sputtered HfO₂ promise κ, give none | Their κ the measured (Rodriguez de Marcos's, Bright's), within 2 % of their points. |
| Silver against Johnson and Christy worse; 6.5 % at 0.41 µm against a 3 % claim | The claim corrected: n within 3 % of Yang's from 0.45 µm (8 % at 0.4, its interband edge), κ 3.5 %; Johnson and Christy's films' κ within 4 %, their n (0.03-0.15) as much as twice off - said in its description. |
| InP against Pettit 0.0054 at 0.95 µm (1e-5 before); its description "from 0.95 µm", its range 0.859 | Pettit's Sellmeier alone from 0.95 µm (the oscillator below 0.92, blended between): 1e-5 again; the description its range (0.80 µm, Pettit's from 0.95). |
| 1.3Q InGaAsP 3.3889 against a 3.393 claim; Si against Li 0.0032 against 0.001 | The 1.3Q 3.386 now (its edge broadened), its test so; silicon's claim was the commit message's (its description says 0.007) - corrected here. |
| 20 Sellmeier entries NaN outside their range | Each index or κ that fails past its range (40 points each side, to a fifth of its start and 1.5 times its end) held at its end on that side - silica beyond 6.7 µm, its band of n² < 0 from 8.3 to 9.9 µm; within its range unchanged. Not past 1.5 times: a crystal's index beyond its phonon is the Reststrahlen's. |
| GaInP 0.03-0.07 below Schubert's | Schubert's an ordered sample (its gap 1.85 eV); said in its description - give Eg and n for an ordered one. |
| VπL in m with a unitless bias | In cm, "cm (times the bias's unit)". |
| "selected" undocumented | In the format text: a plot group's or an export's own point, time, mode. |
| An explicit choose "first" not a choice | Given, it is: a derived value's "auto" its mode alone. |
| composition "gap" takes λg alone | An Eg (eV) as well. |
| κ 0 above the gap: Δα held at it, Δn the whole bleaching | Said in the log: band filling bleaches more than the material absorbs there - its Δα held, its Δn the computed edge's (Kramers-Kronig of its own absorption, not held). |
| A stepped wall refused as "6 boundaries make 3 runs" | Said where each wall is, and that a wall in steps is one with its steps chosen. |
| .npy without coordinates | A grid's x and y beside it, <file>_xy.npy (2, ny, nx); the nodes' export had them. |
| An array's input object named: "there is no object bar" | Said: array bars' input, its copies bars_1... chosen together by bars. |
| A missing "at" taken as the centre, silently | Said (item 8). |
| The vurgaftman band set's own InGaAsP bowing | As Vurgaftman's tables have it (its quaternary bowing): the 1.3Q's composition differs from the measured set's, as it should. |

## Breaks

Forty-six changes broken on purpose (each built, its tests run, the file restored): 42 caught at
once, 2 after the tests were made to see them, 2 not.

Caught at once: the Kramers-Kronig integrand nought at E, the integral without its own energies;
the index held at 0.995 Eg, the gap unbroadened, of T its gap alone moved, a compound without
dndT, an alloy not bowed, AlGaAs's ends and GaInP's InP not their binaries', the range from 0.1 eV
over the gap, Adachi's edge unbroadened, a failing formula not held, ALD alumina without κ, SiGe a
constant; fem_model new a second tab, its unsaved changes lost; TE the most TE of all, a sweep's
summary its last snapshot's, fem_evaluate's and fem_plot's mode not checked, an explicit first no
choice; m 1.5, count 0 and 500, an order 3, a mode step on a resonator taken, the layers'
reflection ignored, no word on the seven figures, the arrows back; the doping's box by thirds, the
strip eight columns, a 1D's unknowns its mesh's, its "at", a mesa's spreading and a coarse mesh not
said; the orders' globals only where open; fem_solve's seconds the last point's; VπL of a plain
bias in m; a composition's Eg refused; band filling's over-bleaching not said; a stepped wall's
message as before; a grid's .npy without coordinates; an array's input named as nothing.

Caught after the tests were made to see them:
- **Tracking followed along a sweep**: the test's widths (0.8, 1.4, 2 µm) never drifted; the
  reviewer's (from 0.6 µm, a ridge 1.5 µm high meshed finer) do - now in the test.
- **The mode step's selection left as its last snapshot's** (Solution::unselect()): fem_solve
  now selects each point's chosen snapshot itself; the fault shows where the last mode is asked
  right after a solve - its fields the chosen mode's, said as the last's. Now tested.

Not caught, and why:
- **The photon appended beside its rounding twin**: the Kramers-Kronig integrand's limit at E now
  covers a twin within rounding too, so Δn is right either way - the guard redundant, kept.
- **An order at its cutoff open** (β² > 0 rather than > 1e-9 k²): no test reaches a cutoff whose
  rounding leans open - the four exact ones tried (orders 1 and 2 grazing air and n 1.5) all round
  closed under the old rule as well; the reviewer's came at one angle of a sweep. The rule is one
  for the listing and the power now.

## Not done

- **Solutions kept with the model** (the reviewer's: the bench's 1-6 minutes again after a reopen):
  no physics keeps its solution in the .qfem or beside it; a sidecar of the fields and snapshots,
  rebuilt against the model on opening, is a feature of its own for every physics.
- **A 1D semiconductor's profile at its own resolution where it is read**: it is set on the
  model's mesh, as other physics read it; now said, with the remedy (a Layered Region along the
  line).
- **InGaAs above its gap**: 0.12 above Adachi's model (not a measurement) at 1.3 µm; no measured
  data there to settle it.
- **SiGe's κ** and its index near its gaps.
- **Low-Q Mie poles** (Q near 1): left out with the layers' own modes, said.
