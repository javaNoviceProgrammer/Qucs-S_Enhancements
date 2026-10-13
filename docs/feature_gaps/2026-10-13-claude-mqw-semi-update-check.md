# Feature gaps: the MQW and semiconductor update, checked independently

*13 October 2026 - on `408f07e` (v27.0.0).*

The review re-checked last rounds' quantum-well and semiconductor physics against
references of its own:
- an independent k·p solver;
- the ionization integral;
- a charge-neutrality solve;
- its own Kramers–Kronig integration;
- the 83-check calibration suite.

All of these agreed (its part A). Its part B left five things open:

- **B1:** Δn below the edge depended on the energy window.
- **B2:** Schrödinger–Poisson had one isotropic electron mass, so silicon's and
  germanium's valleys could not be represented.
- **B3:** four display and unit bugs.
- **B4:** no example used the semiconductor features.
- **B5:** a help line on line shapes for gain was asked for.

All five are done. One fault was found along the way: the default plots of a quantum
physics without holes plotted the holes.

## The review's items (B) against now

| Review | Now |
|---|---|
| **B1.** Δn(TE) of `gaas_eam_excitons` between 0 and 50 kV/cm at 1.40 eV: −6.7e-3 with the energies to 2.0 eV, −11.2e-3 to 3.0 eV. A field removes or weakens 2D steps, which extend to every energy, so the Kramers–Kronig integral never converges. Proposed: fade the steps into the barriers' 3D absorption, so α at high energy no longer depends on the field; then report Δn's convergence. | **Done as proposed.** With `"continuum": "bound"`, past the barriers' gap the wells' steps fade over `fade` (0.1 eV) into the wells' own bulk absorption. Past the fade, α is the bulk's alone and the same in any field. The same case gives **−5.253e-3, −5.254e-3 and −5.254e-3** to 2.0, 2.5 and 3.0 eV. `qw.dnTEtail` and `qw.dnTMtail` report what lies above the window, and the warning says when the top is still short of the fade's end. See "Δn and the window". |
| **B2.** One `me` per material. Si(100) needs two valley sets: two valleys of 0.916 m0 along the axis and 0.190 in the plane, and four of 0.190 along and 0.417 in the plane. Proposed: valley sets per material (mz, in-plane DOS mass, degeneracy), each solved on its own, densities summed in `qw.n`; Si(100), Si(110) and Ge(111) defaults. | **Done.** `"valleys"` on a Quantum Well Material or on the material itself takes Si(100), Si(110), Si(111), Ge(100), Ge(110) or Ge(111), from Stern and Howard's rule (each with `ml mt` if wanted), or sets of your own, `g mz md [dE]; ...`. Each set is a ladder of states of its own; the ladders are merged by energy, and their carriers are summed in `qw.n`. The new globals give each state's, each set's and all carriers per area. See "Valleys". |
| **B3.1.** Energies shown in aJ; `fem_evaluate` with `unit: "eV"` ignored. | **Done.** Every qw energy shows in eV, in the solve's globals, `fem_evaluate` and Derived Values, and `unit` converts it (meV, J). |
| **B3.2.** `qw.k` shown as its SI value with a 1/nm label (`2e+08 1/nm`). | **Done:** 0.2 1/nm. A global's display unit is now a unit of its dimension, converted, not a label. |
| **B3.3.** The Kramers–Kronig warning printed energies as volts. | **Done:** "between 1.4 eV and 2 eV"; the quasi-Fermi levels and exciton lines in the log too. |
| **B3.4.** A Derived Values global in a spectrum study repeated at every photon energy (3612 values). | **Done.** A global the same at each output of a point (each time, frequency, photon energy or mode) is given once for the point, with a note saying so: the review's four excitons' globals over `gaas_eam_excitons` are 12 values, not 3612. Several expressions take a unit each (`qw.Eb; qw.Ex` in `meV; eV`). One unit for several applies to those it fits; it is an error when it fits none. |
| **B4.** No example of impact ionization, BTBT, Fowler–Nordheim, interface traps, Schrödinger–Poisson, the implant, electrothermal coupling or transients. | **Eight examples**, each checked by a test against an analytic result. See "Examples". |
| **B5.** One help line: with a Lorentzian line the k·p gain crosses zero about 10 meV below ΔF; recommend the Gaussian or sech for gain spectra. | **Done**, in the line shape's tooltip, the README and the agent reference. |

## Δn and the window (B1)

**Why it diverged.** Each transition's 2D step is a constant above its edge, out to any
energy. A field shifts and weakens the steps, so Δα between two fields is a near-constant
offset at every energy above the barriers. In the review's case it was −3.5k/cm at
2.0 eV and still −2.1k/cm at 3.0 eV. The Kramers–Kronig integral of such an offset grows
with the window's top.

**The change.** With `"continuum": "bound"`:

- Each step is multiplied by cos²(π/2 · (E − Eg,b)/fade) between the barriers' gap Eg,b
  and Eg,b + `fade`, and by nothing past it. This holds for the parabolic bands' steps and
  for each k·p pair at each k, their broadened lines and the tails past their grids
  included. The default `fade` is 0.1 eV.
- In the steps' place, the wells' own 3D absorption fades in (1 − cos²), from their gap,
  over the wells' thickness.
- The barriers' bulk absorption now counts over their thickness, the line less its walled
  wells. Before, it counted over the whole line.
- So past the fade, α is the barriers' and the wells' bulk absorption, per the wells'
  thickness: the same in any field.
- The reach of the parabolic steps' cells and of the k grid is carried to the fade's end
  and 20 broadenings past it.

Results:

| Check | Result |
|---|---|
| The review's case: `gaas_eam_excitons`, Δn(TE) at 1.40 eV, 0 to 50 kV/cm, the energies to 2.0 / 2.5 / 3.0 eV | −5.253e-3 / −5.254e-3 / −5.254e-3 (before: −6.7e-3 and −11.2e-3 to 2.0 and 3.0 eV) |
| α near the window's top, 0 and 50 kV/cm | 5.3041e6 / 5.3039e6 /m (2.0 eV), 7.8103e6 both (2.5 eV), 8.464e6 both (3.0 eV) |
| An 8 nm GaAs well in Al0.3GaAs, 2.4 eV (559 meV above the barriers' gap) | TE = TM = 66009.9 /cm, the barriers' bulk over 20 nm plus GaAs's over 8 nm, to 1e-3 (a Gaussian line: TE/TM to 1e-9) |
| Δn at 1.46 eV, the window to 2.0 or 2.6 eV (a Gaussian line) | equal to six digits; α at the top equal between the fields to 1e-6 |
| A Lorentzian line | α at 1.70 eV the same whether the window ends at 1.80 or 2.2 eV, to 1e-6, parabolic and k·p; at 2.2 eV between fields 3e-5 (parabolic), 7e-5 (k·p): the slow tails of the steps below the fade |
| A barrier slab of one gap (nothing bound) | its bulk whole from its gap, through the fade, at 0 and 100 kV/cm, to 1e-3 |
| Below the first unbound-to-bound transition (1.551 eV here) | "bound" equals the box's states to 7e-16 |
| γ2 = γ3 = 0 at 77 K, the window 0.3 eV short of the fade's end | the k·p spectrum is the parabolic one's to 1.0e-5 |

**Reporting convergence.** `qw.dnTEtail` and `qw.dnTMtail` give, at each energy, what the
absorption above the window would add to the index if it stayed at the top's value all
the way: (c ħ/π) α_top ln((top + E)/(top − E))/2E. Between two fields their difference is
what Δn leaves out. When the window's top is past the fade, the log says so instead of
warning, because the top is the bulk's alone and the same in any field. When the top is
short of the fade's end, the warning names the energy to widen the window to (Al0.3GaAs:
1.9408 eV).

**What it does not do.**

- **No sum rule:** a step's oscillator strength is not carried into the bulk's; the
  fade's shape is a choice.
- **Unbound-to-bound transitions:** "bound" leaves out the transitions of an unbound
  electron to a bound hole, or the other way. They start at Ec(barrier) − Ev(well), below
  the barriers' gap. In the GaAs case that is 1.67 eV, and the box's states differ from
  "bound" by up to 2 % between there and the gap.
- **Lorentzian tails:** they reach past the fade at the 1e-4 level; a Gaussian or sech
  line has none.

**The proposals not taken.** The review also offered:

- fixing the set of transitions over a sweep at the reference field;
- forcing Δα's tail to zero past a stated energy.

The fade was chosen because past the barriers' gap the states are the barriers' 3D
continuum, where a field's effect is Franz–Keldysh's and small. α there is then the
same in any field without the user choosing an energy or a reference field.

## Valleys (B2)

**The presets.** Stern and Howard's rule:

- along the growth axis, 1/mz = cos²θ/ml + sin²θ/mt;
- in the plane, the density of states' mass is md = √(ml mt²/mz), the in-plane mass
  tensor's determinant once z is taken out;
- the valleys of one mz form a set, their degeneracies summed;
- silicon's six Δ valleys use ml 0.916 and mt 0.190; germanium's four L valleys (eight
  half valleys) use ml 1.59 and mt 0.0815;
- the heaviest set comes first.

| Surface | Sets: g, mz, md (m0) | Ando, Fowler and Stern |
|---|---|---|
| Si(100) | 2, 0.9160, 0.1900; 4, 0.1900, 0.4172 | 0.916, 0.190; 0.190, 0.417 |
| Si(110) | 4, 0.3147, 0.3241; 2, 0.1900, 0.4172 | 0.315, 0.324; 0.190, 0.417 |
| Si(111) | 6, 0.2582, 0.3579 | 0.258, 0.358 |
| Ge(111) | 1, 1.5900, 0.0815; 3, 0.0911, 0.3405 | - |
| Ge(100) | 4, 0.1192, 0.2977 | - |
| Ge(110) | 2, 0.2218, 0.2182; 2, 0.0815, 0.3600 | - |

**The solve.**

- Each set is solved as its own ladder: the quantization mass is mz where the set is
  given, and the domain's `me` where it is not (an oxide or barrier of one valley). Its
  edge is raised by `dE`.
- Each state keeps its set (`qw.Ve1`...) and its degeneracy g, averaged over |ψ|² where
  the set is given.
- Its carriers are g md kT/πħ² ln(1 + e^η) per |ψ|².
- The ladders are merged by energy: `qw.Ee1` is the lowest of all, and "states" asks for
  that many in each ladder.
- The "ask for more states" warning checks each ladder's highest state.
- Sets are matched across domains by their order.
- Not supported: an optical spectrum with valleys (an indirect gap) is refused, and so
  is a 2D wire's.
- **New globals**, with `etan` or `etap` in a 1D well: `qw.Ne1`, `qw.Nhh1`... (each
  state's carriers per area), `qw.Ns` and `qw.Ps` (all of them), `qw.Nv1`... (each
  set's), shown in 1/cm².

**Checks.**

| Check | Result |
|---|---|
| Si(100) at a wall in 50 MV/cm: each ladder's levels against Airy's of its mz, E − Ec(wall) = (ħ²/2mz)^⅓ (qF)^⅔ aₙ | 6 levels of 2 ladders, interleaved (Δ2 1, Δ4 1, Δ2 2, Δ2 3, Δ4 2, Δ4 3), each to 2e-5 (−0.3900521 eV both) |
| Each state's electrons per area at a flat Fermi level | g md kT/πħ² ln(1 + e^η) to 1e-9; the sets' and all their sums; `qw.n` integrated along the line to 1.2e-4 |
| Sets of one's own, the second raised by dE 0.05 eV | its three levels Airy's raised by 0.05 eV, to 2e-5 |
| GaAs with `"valleys": "2 me me"` on its material | the levels the same to 1e-9 eV, each state's electrons twice to 1e-9 |
| The MOS capacitor at 2 V (p 1e17 cm⁻³, 10 nm oxide), Schrödinger–Poisson | `semi.n` = `qw.n` to 1e-3; **66.0 % in the two valleys, 34.0 % in the four**; the centroid **2.309 nm**, where one valley of 0.916 m0 puts it at 1.963 nm. Fang and Howard's 3/b of each set's mz, by the sets' electrons, gives 2.198 nm (5 % off); Ninv 2.976e12 against one valley's 3.014e12 cm⁻² |
| The holes' MOS (n 1e17 cm⁻³, −2.5 V) | `qw.Ps` the holes integrated across the layer, to 1 % (1.686e12 cm⁻²) |

## Units and display (B3)

- **A global's unit.** A unit of the global's own dimension is now its display unit
  (`eV` for J, `1/nm` for 1/m, `1/cm^2` for 1/m²), converted wherever it is shown:
  - the solve's globals;
  - `fem_evaluate` with no `unit`;
  - Derived Values.

  A unit of another dimension stays a label (dB/m).
- **The energies.** Every qw energy (states, transitions, the gap, the direct
  transition, the photon, the broadening, the excitons, the quasi-Fermi levels, the
  valence bands) uses eV. `qw.rsp` and `qw.Rsp` show in 1/(s cm³ eV) and 1/(s cm³).
- **The log's energies.** The Kramers–Kronig warning, the quasi-Fermi levels' separation
  and the exciton's binding are in eV (or meV, µeV) where they had been V.
- **Derived Values.** A global the same at each output of a point is given once for the
  point, with a note. Several expressions take one unit each (`meV; eV`). One unit for
  several applies to those it is a unit of, the rest keep their own, and it is an error
  when it fits none.

## Examples (B4)

Each example is solved by `test_fem_devices::theExamples` as shipped. In a sanitized
build it uses fewer points, a coarser mesh or coarser times. Each is checked against
what its description says.

| Example | What it shows | Checked against | Release |
|---|---|---|---|
| `avalanche_breakdown` | A p⁺n junction (1e19 on 1e16 cm⁻³) swept to 54 V, its ionization integrals | Breakdown where `semi.In` reaches 1: **51.56 V**; the depletion approximation's triangle, same rates: 51.66 V (2 %); `semi.Ip` 1.0005 there | 2 s |
| `zener_tunnelling` | A p⁺-i-n⁺ diode's band-to-band current (Hurkx's) reversed to 2.5 V | 0 at 0 V; from 2 V, q ∫ G to 1 % and 0.51–0.65 of Kane's generation alone at each triangle's field; ×4.9e4 from 1.5 to 2.5 V | 1 s |
| `fowler_nordheim_oxide` | A 10 nm gate oxide's leakage from 6 to 12 V | J = A E² e^(−B/E) at each point to 1 % (1.0e-10 to 0.27 A/cm²); the Fowler–Nordheim plot's slope 241.6 MV/cm, B's 241.6 | 0.4 s |
| `interface_traps_cv` | A MOS C-V stretched by 5e11 cm⁻² eV⁻¹ of traps, at rest and 1 MHz | Through depletion each gate voltage Vfb + ψs − (Qs + Qit)/Cox to 2 mV; the quasi-static C to 0.5 %; ψs from −0.9 to 0 V 0.504 V | 1 s |
| `si_inversion_valleys` | Silicon's inversion layer in its two valley sets (Schrödinger–Poisson), 1 to 2 V | At 2 V as in "Valleys": 66.0 %, 2.31 nm against 2.20, `semi.n` = `qw.n` to 1e-4 | 3 s |
| `boron_implant_anneal` | Boron through a 0.4 µm window, 30 min at 1000 °C | Dose 1e14 cm⁻² to 2e-5; at the range 5.136e18 cm⁻³ (the formula with the window's erf, 5.140e18); the edge 0.5066 of the middle (0.5063); the junction 0.4302 µm (0.4300) | 0.7 s |
| `self_heating_resistor` | A silicon bar heating itself, 1 to 4 V (`TL = ht.T`) | The one-dimensional −k T'' = J² ρ(T) at each voltage: current and middle temperature exact to the digits shown (0.38077 mA, 330.45 K at 4 V; 15 % below Ohm's) | 0.4 s |
| `diode_reverse_recovery` | A pn diode switched from 0.65 to −1 V, lifetimes 0.1 and 0.3 ns | The reverse current 125 and 183 times the forward one; between the lifetimes the charge out 0.899 of the charge stored's difference (the depletion layer's change cancels) | 12 s |

The README's examples paragraph now lists them. It also lists last round's two MQW
examples, which it had left out.

## The help line (B5)

The line shape's tooltip, the README and the agent reference now say: with carriers, a
Lorentzian's slow tails move the gain's zero some 10 meV below the quasi-Fermi levels'
separation and part the emission from the gain, so use a Gaussian or a sech for gain
spectra.

## Found along the way

- **The default plots of a quantum physics plotted holes it did not solve.** A
  Schrödinger–Poisson region of electrons alone got a "Heavy Hole Probability" plot of
  `qw.P_hh` ("defined nowhere") and the holes' levels in its band diagram. They are now
  made only for the carriers the physics solves.

## Tests

`test_fem_quantum`:
- **`aBoundContinuum`:** the well past the fade, the slab through the fade, and "bound"
  equal to the box below the first unbound-to-bound transition.
- **`electrorefractionOfAnyWindow`:** Δn the same with either window; the top's α the
  same in either field; the note; the warning with the fade's end; a fade below 0
  refused.
- **`fadingInEveryPath`:** parabolic and k·p with a Lorentzian line; α independent of the
  window; `qw.dnTEtail`'s formula.
- **`kpAndParabolicFadeAlike`:** the reach of both paths to the fade's end.
- **`valleyPresets`:** the six surfaces, masses of one's own, sets of one's own, four
  refused texts.
- **`valleyLadders`:** Airy's levels and the occupations; `qw.n`; dE; a material's
  valleys; the per-ladder warning; a spectrum, a wire and a bad text refused.

`test_fem_devices`:
- **`schrodingerPoissonValleys`:** the Si(100) MOS.
- **`schrodingerPoissonHoles`:** now `qw.Ps` too.
- **`exampleBreakdown`, `exampleZenerTunnelling`, `exampleFowlerNordheim`,
  `exampleInterfaceTraps`, `exampleInversionInValleys`, `exampleImplantThroughAMask`,
  `exampleSelfHeating`, `exampleReverseRecovery`:** the eight examples, one function
  each.
- **`selfHeating`:** its one-dimensional reference is now shared with
  `exampleSelfHeating`.

`test_multiphysics_doc`:
- **`claudesToolsShowTheWellsInEV`:** B3's display, units and Derived Values.
- **`quantumWells`:** an electrons-only well's default plots.

In Release, `test_fem_quantum` runs its 34 functions in 64 s (with the ASan builds running beside it); the eight example functions of
`test_fem_devices` run in 31 s.

Under the sanitizers (a Debug build with ASan and UBSan) there were no sanitizer
reports in any run. Each function stays under QtTest's 300 s:

- **`test_fem_devices`:** its other 22 functions pass. The examples first ran as one
  function, which went past 300 s there, so they are now one function each: 49, 32, 48,
  130, 68, 14, 46 and 184 s.
- **`test_fem_quantum`:** the functions before this round's pass as before. Of the new
  ones, `valleyLadders` takes 105 s and `valleyPresets` 0 s. `at77K`, `gain` and
  `theExamples` take 9, 9 and 113 s. Three needed changes there:
  - **`electrorefractionOfAnyWindow`** read Δn at 1.45 eV, which is off a sanitized
    build's 0.02 eV grid (NaN). It now reads 1.46 eV, on both grids. Its six spectra on
    a 0.1 nm line went past 300 s, so there it uses the 0.25 nm line, as the k·p tests
    do, with 4 states and a 15 meV line: 23 s.
  - **`fadingInEveryPath`:** its k·p half went past 300 s, so there it runs its
    parabolic half alone: 183 s.
  - **`kpAndParabolicFadeAlike`:** with 41 values of k there, the k·p integral's own
    error is some 1e-3. It runs for the sanitizers' sake, its agreement there 1e-2:
    134 s.
- **`test_multiphysics_doc`:** 41 functions in 384 s.
- **The three tasks after this round:** the legend's 7 functions pass in 3 s, the Git History's three in 18 s, and
  `claudeStopsASolve` in 60 s. Before this, a sanitized run of that last one found
  `fem_stop` waiting out the whole of a long step at rest. A study looked for a cancel only
  between its steps, and a semiconductor's way to its contacts' voltages runs inside a step's
  setup and solve. Each problem is now asked between its Newton steps: a solve that runs
  27 s whole stops 30 to 60 ms after it is asked (its own commit).

## Breaks

Each change was broken in turn, rebuilt, and its tests run: 49 breaks.

| Of | Breaks |
|---|---|
| The fade (B1) | the steps never fading; the wells' own bulk left out; the barriers' bulk over the whole line; the wells' bulk not fading in; a parabolic step's cells not faded; a parabolic step exact, unfaded; a parabolic step short of the fade's end; a parabolic step's tail not faded; a k·p pair not faded; a k·p pair's tail not faded; the k grid short of the fade's end; the tail's index over E, not 2E; warned past the fade too; the fade 0.2 eV by default; the barriers' gap the edges' least |
| Valleys (B2) | the in-plane mass the axis' one; the valleys' g not in `qw.n`; g not in the sheets; a set's dE left out; md = mt; valleys alike not one set; Ge's half valleys two each; the presets Si's masses for Ge; the ladders not merged by energy; a material's valleys not read; a spectrum of valleys not refused; a wire's not refused; only the last state checked for its carriers; the sheets' Fermi level the band's; `qw.Ps` the last state's |
| Units and Derived Values (B3) | no global shown in its unit; the solve's text in SI; `fem_evaluate` not in the global's unit; one unit for each not split; one unit for several fitting none; a global repeated at every output; the warning in volts; `qw.k` in SI; the state energies in J |
| The default plots | the holes' probability plotted without holes; their levels drawn without holes |
| The examples (B4) | the breakdown junction 2e16; the Zener diode Kane's generation alone; the oxide's mass 0.5; the traps half as dense; silicon one valley; the anneal 20 minutes; the resistor's mobility not at its temperature; the diode switched over 100 ps |

Four were not caught at first:
- **The wells' own bulk left out:** the break itself did not build (an `if` left with
  nothing to do); remade, it is caught.
- **The k grid short of the fade's end:** the holes' reach (0.15 eV and 15 kT) carried
  the light-hole test grid past the fade anyway. The test now uses heavy carriers (0.6
  and 1 m0) and a window ending at 1.62 eV, so both paths' grids stop short of the
  fade's end (2.0 eV) without the extension; all four reach breaks are caught.
- **The fade 0.2 eV by default:** the solver's own default was dead code, since a node's
  `fade` always has the model's default. It is removed; the break is now made on the
  model's default, and caught.
- **The state energies in J:** the test read the transitions' energies only; it now reads
  `qw.Ee1` too.

All 49 are caught.

## The whole

- **ctest:** the full ctest passes in Release, 116 tests in 284 s, with this round, the
  three tasks after it, and the stop's fix.
- **MCP scenarios:** the script's 97 checks pass.
- **The DMG**, built at the head of these commits, and again after the stop's fix:
  - verified, signed, with no Homebrew links in its 79 binaries;
  - `--version` names its commit, and the netlist smoke test passes;
  - its twelve checks pass, as do the History check (a window of its own now) and the
    progress check.
  - On the packaged app this round's checks pass too: Δn the same to 2.0, 2.5 and
    3.0 eV; the eight examples solved and every plot drawn; `fem_stop` stopping a solve.
- **Nothing outside:** no run wrote to the workspace, the caches or the Trash.
