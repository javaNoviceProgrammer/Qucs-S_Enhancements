# A process flow for the multiphysics FEM

The proposal: `~/Desktop/qucs-reviews/2026-10-15-claude-process-flow-proposal.md` - a Process Flow in the model, an ordered list of steps on a 2D cross-section whose result is the geometry, materials and doping the physics read. The user asked for its Stages 1 and 2 whole (Stage 3 is a set of projects of its own, left out as the proposal itself says).

Stage 1 (shape steps and masks) first, then Stage 2 (the dopants: implants, anneals, oxidation, the III-V steps, their hand-off to the device physics and the tools).

## Stage 1: shape steps and masks

### What it is

A geometry feature, **Process Flow** (`"process"`), holds its steps in order; when the build reaches it, it runs them and each region it leaves becomes an object of the geometry, named by the step that made it (its label, else its tag; an epitaxial stack's by its rows). The materials and the physics select them by those names, as they do a layout cross-section's rows.

- **Its window:** x from `x0` across `width`, or along a layout's cut (`file`, `cell`, `start`, `end`, `origin`), as the Layout Cross-Section takes it; the surface at y = 0 to start.
- **The steps** (`process/...`):

| Step | What it does |
|---|---|
| Substrate | A crystal across the window, `thickness` below y = 0; its `orientation` (100, 110, 111) is what a crystal-plane etch follows |
| Epitaxial Stack | Its `layers` [name, material, thickness] from the bottom, the top's surface at y = 0 |
| Deposit | `conformal` (its thickness from every surface: the solids offset, round), `planar` (filled to its thickness above the highest point) or `directional` (its thickness straight up on what the sky sees: nothing under an overhang); masked, a lift-off |
| Etch | A front from the surface into each material at its rate - 1 for those it etches (`materials`; empty: all but its stop and mask), 1/S for a `selectivity` S, `stopratio` for its stop, `maskratio` for the resist - for a `depth`, or until its `stop` shows in every opening, then its `overetch`. `anisotropic` (straight down), `isotropic` (round, under the mask's edges), `tapered` (`angle` from the surface: under 90° narrowing, over 90° a dovetail), `crystal` ({111} sidewalls: 54.74° on (100), upright on (110), a V where they meet; another plane's angle given) |
| Planarize (CMP) | Everything above a `height`, or above its stop's (`to`) highest point, cut away |
| Strip | A material removed wherever the air touches it; a buried piece stays (said) |
| Epitaxy | A crystal grown straight up on the exposed crystal - `selective` (on crystal alone: a substrate's, a stack's, epitaxy's) or not |
| Fill | A polymer or glass filling every open space to a `height` (or its `thickness` above the highest point) |

- **Masks:** a step's `mask` is a layout's `layer` along the cut (open where it is `drawn`, or where it is not: `polarity`) or typed `openings` ("1, 2; 4, 5"), each opening grown by its `bias`. A layer is "1/0", or a name the layout's `.lyp` gives (beside it, as the layout viewer finds it: one map for the viewer, the cross-sections and the masks). For an etch, a mask is a resist of its `resist` thickness over the surface where it is closed, for that step alone.
- **Robustness:** the regions are Clipper polygons on a 64th of the **process grid** (1 nm by default); after each step they are snapped to the grid, cleaned, and rid of slivers - a piece thinner than the **minimum** (1 nm) everywhere, or one the grid takes away - each dropped piece said in the step's warnings. An etch's front keeps only what it touches: a step of the front longer than a stop layer or a resist is thick does not go through it (a 2 nm buried oxide under a 500 nm etch is kept).

### The state after each step

- **In the window:** choosing a step in the Multiphysics panel shows the structure as that step leaves it ("Show After This Step"); any other node shows the whole geometry again. A step that makes a region has its domains highlighted.
- **Claude's tools:** `fem_build` with `until` a step's tag (or any geometry feature's) builds to it, shows it, and answers each step's report (what it did, its time, its warnings) and the regions then (name, material, area, where; crystalline marked). `fem_describe what=format` describes the flow and every step.

### With the layout viewer

- **A flow from a ruler:** the layout tab's menu, with a ruler drawn, has "Process Flow to Multiphysics" beside the cross-section: a new model of a flow along the ruler, a silicon substrate and its material to begin with.
- **The cut follows the ruler:** the flow's "Cut Along the Layout's Ruler" takes its layout tab's last ruler (or an open layout's, when it has none): its file, cell, start and end.
- **A step's mask over the layout:** "Show Its Mask in the Layout" shows that layer alone in the layout, the cut on it.
- **Cross-probing:** with the flow or a step chosen, a click in the cross-section shows that point in the layout - the cut drawn, the point marked across it, the shape there selected. A shape picked in the layout says, in its description, the open models' steps it is the mask of ("the mask of rib (heater_flow.qfem)").
- **Its materials:** "Make Its Materials" makes a material for each material its regions are of, from the library, on their regions by name - or adds them to a material of that library entry already there.

### How it was checked

`test_fem_process` (new):

| Check | Result |
|---|---|
| A 1 µm trench etched 0.5 µm through a mask, then 0.1 µm conformal oxide | the silicon's area 7.5 µm² to 1e-6, its corners on the mask's edges; the film 0.3 + π·0.01/2 + 0.1 + 0.08 µm² (flat tops, a quarter disc at each convex corner, the walls, the floor between them) to 1e-3 |
| The same, planar 0.2 µm | the trench filled, its top flat at 0.2 µm, 1.3 µm² |
| SOI's top silicon etched to its buried oxide, over-etched 20 % | the oxide untouched; at a twentieth of the rate, over-etched by half, it loses 2 × 0.3 × 0.5 × 0.05 µm² |
| A 2 nm buried oxide under a 0.7 µm etch, its steps 4 nm | not gone through: the handle untouched |
| An isotropic etch, 0.5 µm, through a 0.2 µm opening | a stadium, 0.2 × 0.5 + π·0.25/2 µm² to 3e-3; 0.5 µm under the mask at either side |
| A crystal-plane etch of (100) silicon through a 2 µm opening | a V of √2 µm² - its walls at 54.74° (to 0.1°, the proposal's target) |
| A tapered etch at 80° | the trapezoid's area to 3e-3 |
| A directional deposit over an undercut hard mask; one lifted off | metal on the cavity's floor under the opening, none under the overhang; lifted off: in the opening alone |
| Selective epitaxy in an oxide window, planarized to the oxide | grown in the window alone, crystalline; cut to the oxide's top |
| A strip of buried resist; of resist opened to the air | kept, said; stripped |
| A mask from a layout's layer along the cut (written with gdstk) | open where drawn, where not, biased by 0.1 µm: the areas exact |
| A film thinner than the grid; a flow without a substrate; an unknown stop; an etch of nothing | dropped and said; refused, each said |
| The regions as the geometry | objects named by their steps, three domains, a selection by name, built up to a step, the file read back as it was |

`test_multiphysics_doc`: `aLayoutsProcessFlow` - a flow from the ruler of the thermo-optic heater's layout, an SOI process whose rib etch's mask is the waveguide's layer by its `.lyp` name; the waveguide left where the layer is; the materials made (oxide new, the substrate's silicon extended to the core); the mask shown alone in the layout with the cut; a shape picked there naming the rib; the cut along another ruler; the state after the etch.

## Stage 2: the dopants

### What it is

A flow's dopants live on a grid of their own over its window: rows `dy` tall (2 nm by default) from the top down to `depth` (2 µm) below the lowest surface, taller below; columns `dx` wide (the window's 200th). The grid follows every shape step: a cell an etch opens loses its dopants (said, as a dose over the window), a deposit's cells start clean, rows are added above a new top, and the fine rows are extended down when an etch goes below them.

- **Doped as made:** a substrate, a stack's row (two more columns: dopant, concentration), a deposit (a doped glass, a source for a drive-in) or epitaxy.
- **Implant** (`process/implant`): `species` (an element's symbol, or BF2, whose boron gets 11/49 of the energy), `energy`, `dose`, `tilt`; a mask's openings and its resist.
  - Along each column of the beam, a Pearson IV profile in each layer it crosses, with that material's own moments. From layer to layer the profile is dose-matched: entering a material with a fraction G still moving, the ion is placed at the depth of that material's own profile that leaves G beyond (Ryssel et al.'s method). What the profile puts above its surface is folded back below it, the Implant profile's image.
  - Across the beam, a Gaussian whose straggle varies with depth: σ⊥² + slope (z − Rp), the slope from the ⟨x y²⟩ moment.
  - The resist stops ions; a tilted beam is shadowed by sidewalls.
  - `profile`: Pearson IV, Gaussian, or a dual Pearson whose channelled part (its fraction and moments) is given per material. There are no defaults for that part: it depends on the tilt, twist and screen oxide.
- **The moments:**
  - **Boron, phosphorus and arsenic in silicon** come from a table: Rp and ΔRp every 10 keV, read off the curves of Plummer, Deal and Griffin's EE212 handout (LSS tables) by their pixels. Boron is tabled to 140 keV, the others to 200. An energy outside the table is said to be extrapolated.
  - **Their skewness, kurtosis and lateral straggle, and every other ion and target,** come from transport theory. The moment equations of Lindhard, Scharff and Schiøtt are solved upward in energy, on the ZBL universal potential's exact scattering (Gauss–Mehler quadrature of the scattering integral, a table over ε and b). Lindhard and Scharff's electronic stopping is used, Bragg's rule for compounds.
  - Collisions that give less than a tenth of the energy are taken to second order in it; the rest are taken exactly. To second order throughout, a heavy ion's skewness came out twice a Monte Carlo's.
  - The tabled ions' electronic stopping is fitted to their table (×1.6 for boron, ×1.4 for phosphorus, ×1.1 for arsenic: Rp's rms 2.5, 5 and 3.6 %). In other targets, their moments are the theory's scaled as their silicon's are to the table.
  - Light ions faster than v0 Z1^(2/3), where Lindhard and Scharff's stopping no longer holds (hydrogen past about 25 keV), are said: their range comes out short.
  - The handout's antimony curve is not taken: it lies deeper than arsenic's below 140 keV, and the theory and a Monte Carlo of the same collisions both put antimony 25 to 30 % shallower.
  - A table of moments given per material overrides any of them.
- **The targets:** a material's compound (GaAs, InGaAs at x, InGaAsP, AlGaInAs, SiGe...) gives its elements by sublattice, and its atoms come from its density or lattice. Otherwise its name is looked up: silicon, its oxide and nitride, germanium, the high-k oxides, sapphire, lithium niobate and tantalate, a novolac resist (C8H8O at 1.2 g/cm³), PMMA, BCB, polyimide, the metals.
- **Damage:** the energy into nuclear collisions, ν, comes from the same equations (local deposition), deposited as a Gaussian about its own mean depth. A crystal goes amorphous where it passes Dennis and Hale's critical density (6×10²³ eV/cm³). There is then no channelling, and the step says how deep the amorphous layer is. Beyond it, Giles's +1 interstitials are held in clusters (`plus`).
- **Diffuse** (`process/diffuse`): a `temperature` for a `time`, or a `recipe` of rows (time, temperature) with ramps between.
  - **The models:**
    - `fermi` (the default): in silicon, Fair's diffusivities (R. B. Fair, in Impurity Doping Processes in Silicon, 1981, as Campbell tabulates them; boron's sum the Implant profile's 0.76 cm²/s), D = D⁰ + D⁻(n/ni) + D⁼(n/ni)² for a donor and D⁰ + D⁺(p/ni) for an acceptor. n comes from all the active dopants (ni 3.87×10¹⁶ T^1.5 e^(−0.605 eV/kT) cm⁻³, Morin and Maita). Each dopant drifts in the field their gradients make (Scharfetter and Gummel's fluxes).
    - `constant`: n = ni and no field.
    - `defects`: the Fermi level's model times f_I s + (1 − f_I)/s, where s = CI/CI* is the free interstitials' supersaturation and the vacancies are taken in equilibrium with them. The +1 clusters release interstitials with a lifetime (10 minutes at 800 °C by default, 3.8 eV, Eaglesham et al.'s dissolution energy); they diffuse (TSUPREM-4's D_I and CI*, as Heitzinger lists them) and are lost at silicon's surfaces and interfaces.
  - **Interfaces:** segregation C_a = m C_b, through a transport h when given. Silicon to its oxide: boron 0.3, phosphorus 10 (Grove, Leistiko and Sah), arsenic and antimony as phosphorus.
  - **Surfaces:** a `source` held at its `surface` concentration (a predeposition), or a dose lost at a rate (`loss`).
  - **Activation:** above a solid solubility given (Css0, Ecss) the rest is inactive, at once or with a time constant (tau0, Etau). The report says what was lost and what is inactive.
  - **Any parameter of a species in a material is a row of `parameters`:** D0, Ea and the charge states' terms, a square law's Dsq0, Esq, Cref, Css0, Ecss, tau0, Etau, loss, fI, m:<material>, h:<material>.
  - **No defaults:**
    - **Dopant solid solubilities:** none of the fits found could be checked against their sources here.
    - **Diffusivities in oxide and nitride:** a species there does not move, and the step says so.
    - **Zinc in InP:** its square law takes Dsq0 and Cref.
  - **The numerics:** Peaceman and Rachford's alternating directions on the grid, a tridiagonal solve per row and per column. A step whose diffusivity depends on the concentration, or with an interface moving, or with interstitials, uses backward Euler's directions instead, with Picard iterations to the step's end (Peaceman and Rachford's ring at such a front); those steps are at most a hundredth of the anneal. Steps start at a fifth of the finest cell's diffusion time, grow 5 % a step, and break at the recipe's points and within ramps.
- **Oxidize** (`process/oxidize`): `ambient` dry or wet; `rates` Massoud's or Deal and Grove's; a `recipe` or temperature and time; `pressure`; `factor` (HCl's); a mask and `beak`; or `B` and `BA` given.
  - **Dry, by default:** Massoud, Plummer and Irene's B and B/A, with Massoud and Plummer's thin-regime terms K1 e^(−t/τ1) + K2 e^(−t/τ2), as Hollauer's thesis tabulates them. Those terms are fitted from 800 to 1000 °C; outside that range they are left out and the step says so.
  - **Dry with Deal and Grove's rates:** their initial 25 nm (their τ), said. **Wet:** Deal and Grove's. (100) has B/A over 1.68; (110) takes (111)'s values where a set has none, said.
  - **Pressure:** both rates go as the oxidant's pressure (Henry's law).
  - **The shape:** it is planar, column by column. Silicon open to the oxidant (bare, or under oxide; not under nitride, nor under a mask) grows its own thickness from the oxide it already has: 0.44 of it is silicon consumed, and everything above is lifted. Under a mask's edge the oxide tapers as erfc(d/L), L the oxide's thickness unless given (an error function's taper, as Guillemot et al.'s LOCOS model has it): an analytic bird's beak flagged approximate.
  - **The dopants meanwhile:** they diffuse as in an anneal while the interface sweeps through them. A silicon cell becomes oxide as the interface passes its middle; the oxide takes C/m over its 1/0.44 of the silicon's thickness, and the rest is left in the silicon below, or taken from it. So boron is depleted and phosphorus and arsenic pile up.
- **Intermix** (`process/intermix`): a III-V stack's sublattices interdiffuse. Each element's share on its own sublattice diffuses in the III-V cells alone, group III's with `diii` and group V's with `dv` (at `tref`, their activation energies).
  - Default: Peyre et al.'s InGaAs/InGaAsP wells, one D for every atom (9.5×10⁻³ Å²/s at 650 °C, 2.54 eV).
  - Masked: `enhance` times D in the openings, no default (it depends on the cap).

### The hand-offs

- **A semiconductor's doping "From the process flow":** the flow's active dopants at each node, a donor or an acceptor as each species is in its domain's material (boron in silicon, zinc in InP; an isolation implant's H or Fe neither). The values are read from that material's cells alone, so a jump at an interface stays a jump. `species` lists which.
- **The quantum wells physics:** on an intermixed stack, each point's bands are the alloy's there. The local shares give the compound and its x and y; its unstrained edges are moved from the layer's. The masses stay the layer's.
- **The geometry's Topology** carries the flow's state; the physics read it from there.

### The tools

- `fem_build until` a dopant step: its report. An implant gives its moments and their source, where the ions stopped (the mask, each material, out through a sidewall), the amorphous depth. An anneal gives its model, steps, dose lost and inactive part. An oxidation gives the thickness grown and the silicon consumed.
- **A step's state as a solution:** a step's tag (or a flow's: its last step) answers where a study's would. Its variables are read where they are: pf1.B (all of it), pf1.B_a (active), pf1.Nd, pf1.Na, pf1.net, pf1.I, pf1.Icl, pf1.x_In, with the flow's tag as their prefix. Its globals are the values down a column at x (the window's middle):
  - pf1.xj, the first junction below the top semiconductor's surface;
  - pf1.Rs, the sheet resistance of the layer above it, with Masetti's silicon mobilities;
  - pf1.tox, the oxide over it;
  - pf1.dose_B, each species' dose kept over the window.
  It is built once per step and kept until the model changes; another x only finds the column again.
- **`fem_evaluate step`** (and `x`): any expression, at points, over domains, or the globals.
- **`fem_plot step`:** a colour map of `expression` (pf1.net in cm⁻³ by default). With `sims`, its profile down x, each species (and `active`) on a logarithmic axis (a 1D plot group's new **Logarithmic y axis**). With `measured`, a SIMS CSV is imported beside the display and each column becomes a trace on that diagram.
- **The panel:** a step or a flow has **Plot Its Dopants** and **Its Profile (SIMS)**.
- `fem_describe what=format` describes every dopant step and parameter.

### How it was checked

`test_fem_process`: each against what it must equal (the proposal's §5), its measured accuracy printed.

| Check | Target | Found |
|---|---|---|
| Pearson IV: its table's moments those it was made of, γ from −1 to 0.8 | 1e-3 | within it (12σ cut a skewed one's kurtosis 1e-3 short: its support is now found where it falls below 1e-15 of its peak) |
| The moments: B, P, As in silicon their table's; in oxide within a third of silicon's; Sb (theory) shallower than As; 300 keV boron said to be extrapolated; BF2's boron at 11/49 | exact, said | as said |
| A constant source: boron at 1e18 cm⁻³, an hour at 1000 °C, intrinsic D | erfc, 1e-3 of Cs | 3.2e-5 |
| A drive-in: a Gaussian (Rp 0.2 µm, ΔRp 20 nm), an hour at 1000 °C | the image's Gaussian of σ² = ΔRp² + 2Dt, 1e-3; its dose 1e-4 | 1e-4; 1e-10 |
| A shallow implant as implanted (Rp 40 nm, ΔRp 20 nm: 2 % of it above the surface) | the Implant profile's image (what falls above folded back), 1e-3 of its peak | 1e-4 (it found a bug: each sample along the beam took its material from its start, a rounding put the surface's first in the air, the top cell a third short - 0.32 off; now its middle's) |
| Implant + anneal through a mask (0.3–0.7 µm, 2 µm of resist), 30 min at 1000 °C | the Implant profile's formula, 1e-3 of its peak | 8.5e-5 on the grid; 1.1e-4 at a semiconductor's nodes (its doping From the process flow); the typed Implant's own 2e-10 |
| Segregation: boron in 0.2 µm of silicon under 0.1 µm of oxide, 10 h at 1100 °C | steady state C_Si = C0 t_Si/(t_Si + t_ox/m), 1e-3 | 1.1e-13 |
| Deal and Grove, wet, an hour at 1000 °C, their table's A and B given | x² + Ax = Bt, 1 %; 0.44 consumed | 0.43450 µm (0.43451); 0.4400 |
| Their own rates there: wet, dry from their τ | the table's thicknesses, 5 % | 0.4495 µm (0.4345: +3.5 %), 0.0698 µm (0.0686: +1.7 %) |
| Massoud's dry, 30 min at 900 °C, (100) | his closed form, 1 % | 0.01180 µm (0.01181) |
| The interface sweeping: boron and phosphorus at 1e17 cm⁻³, an hour of dry oxide at 1000 °C | B depleted, P piled up, each dose kept | as said, the doses to 1e-6 |
| Zinc in InP by a square law: its front (half the surface's) at 10 and 40 minutes | √t: twice as deep, 2 %; steep (from a half to a twentieth within a third of its depth) | 1.9992; 0.28 |
| The same with no tail, against the square law's similarity solution (−(η/2) f′ = (f² f′)′, f(0) = 1, η = x/√(Dsq t), shot in f: a half at η 0.8525, a fifth at 1.0534) | 1 % | 0.6588 µm (0.6603), 0.8158 (0.8160) |
| Quantum-well intermixing: 10 nm In0.53Ga0.47As in InP, √(Dt) 2 nm | the erf slab's shares at its middle, 1e-3 | In 0.56632 (0.56624), As 0.92273 (0.92290); the first electron state up 10 meV, the middle's edge 10.6 meV |
| TED: boron 2e14 cm⁻² at 20 keV, 30 min at 800 °C | deeper with interstitials | 0.62 µm against 0.20 (to 1e17 cm⁻³) |
| Masks, topography: phosphorus through an opening; arsenic at 10° into a 0.5 µm wide, 1 µm deep trench | nothing under the resist; the trench floor lit beyond its wall's shadow, dark within it | as said |
| LOCOS: 10 nm pad oxide, nitride opened over 1.5–2.5 µm, an hour of wet oxide at 1000 °C | grown in the opening as Deal and Grove from the pad (1 %), none far under the nitride, a bird's beak at its edge | 0.3818 µm (0.3820); 1e-17 µm at 1 µm in; 0.336 µm at 40 nm in |
| Channelling: boron 1e13 cm⁻² at 20 keV, a fifth channelled about 0.25 µm (50 nm); arsenic 1e15 cm⁻² with the same | the channelled peak's concentration (15 %); none once amorphous | 1.596e17 cm⁻³ (1.596e17); none, said |
| Doped as made: a stack's rows; boron from a doped glass | each row its own; boron in the silicon after a drive-in | as said |
| Transport theory against a Monte Carlo of the same collisions: P, As, Sb at 80 keV | Rp, ΔRp, lateral 4 %, γ 0.05, β 0.1 | within them |
| Arsenic at 1e16 cm⁻² by the Fermi level (Fair's box) | its front steeper and deeper than intrinsic | from a half to a tenth 0.36 of its depth (0.28 µm), intrinsic 0.52 (0.073 µm) |
| Amorphization | As 1e15 cm⁻² at 80 keV amorphous, B 1e13 at 40 keV not | as said |
| Activation and dose loss | a solubility given: that much active, the rest inactive, said; a loss given: the dose less, said | as said |
| End to end: the example's pn phase shifter built by its process (implants through masks, an anneal, the rib etched, clad), against the same shapes with its doping typed in as Implant profiles | n_eff at 0 V, Δn_eff from 0 to −2 V, 1 % | 2.56679586 (2.56679809); 7.6942e-5 (7.6889e-5: 0.07 %) |

The range moments were also checked against a Monte Carlo of the same collisions (a TRIM-like binary collision code with the same potential table and stopping, 20,000 ions, run while building this). P, As and Sb at 80 keV agree in Rp to 1 %, ΔRp to 4 %, and γ, β and the lateral straggle to a few hundredths. Boron's γ differs by the 1.3 % of ions the Monte Carlo loses backscattered.

Each check was broken on purpose to see it fail - 33 breaks, all caught: the Pearson support cut at 12σ, the table, every collision to second order, the profile renormalized instead of folded, a given lateral straggle's slope kept, a harmonic mean at a face, the surface's diffusivity an empty cell's, segregation's ratio inverted, an oxide grown from Deal and Grove's 25 nm instead of its start, the oxidation's rates read from the dopants' model key, one Picard pass, backward Euler's steps a twentieth of the anneal, no Fermi-level ratio, no TED factor, the resist as air, the tilt ignored, the amorphizing density a million times higher, no activation clamp, no dose loss, the anions not intermixed, no band shift, donors and acceptors swapped, the interface's ratio 1, nitride not blocking, no channelling, a stack's rows undoped, the junction 0.1 µm deeper, the column's x ignored, no log axis, the measured traces not added, no panel button, a sample by its start, a raised kurtosis said of the resist too (where the dopant is stripped with it). Five of the first run's breaks went uncaught; the checks were made stricter (the shallow implant, zinc's similarity solution, the junction's sign change) - one of them finding the sampling bug above.

`test_multiphysics_doc`: `aFlowsDopantsThroughTheTools` covers boron implanted into n-type silicon and driven in.
- `fem_build` to the implant reports Rp 0.145 µm from Plummer et al.'s table.
- `fem_evaluate` at the anneal gives xj, Rs and the dose kept (to 1e-3); the net doping changes sign 10 nm either side of xj; the same at another x, pf1.x that x.
- An unknown step is refused with the steps listed.
- The net doping at a point is negative (boron an acceptor).
- `fem_plot` makes the colour map and the profile, with its active parts and a measured CSV beside it.
- The panel's two buttons make the same.

### What it does not do (said where it matters)

- **No default solid solubilities, oxide or nitride diffusivities, or zinc D:** the parameters exist and the step says what is missing.
- **TED:** without {311} clusters' detail, boron–interstitial clusters, or a bulk sink, its size rests on the clusters' lifetime, an estimate. The check above went three times deeper, likely more than measured: calibrate it.
- **Lindhard and Scharff's electronic stopping** past its velocity (light ions fast) is said. The ions tabled are fitted, the rest uncorrected: compare them with a table and give their moments.
- **Oxidation is planar column by column:** a mesa's sidewalls and a trench's corners are approximate; the bird's beak is an analytic taper; there is no oxidation-enhanced diffusion. True 2D oxidation with viscous flow is Stage 3, as the proposal says.
- **The regions snap to the process grid (1 nm):** a thin oxide's thickness is good to it (the Massoud check uses 0.1 nm).
- **Intermixing moves the well's band edges** (unstrained), not its masses or strain.
