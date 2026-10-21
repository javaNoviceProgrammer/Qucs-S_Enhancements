# Mobility models: the update check of 21 October

The check (`qucs-reviews/2026-10-21-claude-mobility-update-check.md`) ran the mobility commit b7df5f3 live through the tools. It compared QFEM with the reviewer's own calculations: Python formulas from the papers, and a P1 FEM of a rectangle with a Hall-angle conductivity tensor. It also set Crosslight's mobility slides beside QFEM.

**Its passes:** Arora's, Dorkel and Leturcq's and Yamaguchi's low field matched to 7 digits on a grid of dopings at 300 and 400 K. Klaassen's matched within 2e-5. Also matched:
- an expression (Masetti's ⊕ Conwell-Weisskopf) in a lit bar;
- the exponential saturation velocity, to 1e-9;
- the Hall bar's geometric magnetoresistance, against its own FEM at matching meshes.

**Its issues:** six. Each is fixed below.

## The issues

| Review | Now |
|---|---|
| 1. Klaassen's F(P) has r3 = 6.5502; the paper and Crosslight's table give 6.5505 (2e-5 on minority mobilities). The test copied the code's constant, so it could not catch it | **6.5505** (`klaassenF`). The test's own formula has the paper's value, and its dark cases are held to 1e-9, not 1e-4. They match to 1e-15, and the old constant fails them. Minority electrons in p⁺ 1e19: 205.85191 cm²/(V s) |
| 2. Yamaguchi's model ignores temperature without saying so: at 400 K the 300 K values to every digit | **Said**, as a warning in the solve's answer: "Semiconductor, domain 1 (bar): Yamaguchi's mobility is of no temperature - its 300 K fit's at 400 K". Under a lattice temperature it names TL's range: "... at a lattice temperature of 350 K". At 300 K nothing is said. Not scaled: the model is a fit at room temperature with no temperature law of its own, so a (T/300 K)^-ζ factor would be another model under its name |
| 3. "surface scattering, but no semiconductor meets an insulator: none" appears with Yamaguchi's model, or with an expression of n and p alone, while no surface scattering is on | **The line names what reads the interface:** "Yamaguchi's field across the interface, but no semiconductor meets an insulator: none", likewise "Lombardi's surface scattering", "Shirahata's surface scattering" and "the mobility's expression of Eperp or dsurf (0 and 1 m)". Only an expression that reads Eperp or dsurf looks for the nearest interface now. One of n, p, Epar, N or T alone does not, and says nothing (it was `perpendicular()`: any expression of the state) |
| 4. The library Silicon's Masetti takes the lattice mobilities, 1400 and 450 cm²/(V s), where Masetti's fit has 1417 and 470.5. Masetti at 1e17 gave 718.65, not 727.05; holes at 1e15 were 4 % low | **Masetti's model has its own µmax:** Electrons' µmax and Holes' µmax (`mmaxn`, `mmaxp`). The library Silicon gives Masetti's 1417 and 470.5, of T as its lattice mobilities are ((T/300 K)^-2.5 and ^-2.2). Left empty, which is the default on every other material, µmax is the lattice's µn and µp. That is the Lumerical Masetti model's, so a Lumerical import gives what it gave. Now 727.0544 at 1e17 (Klaassen's majority electrons there: 727.0544) and holes 461.05 at 1e15. The material visualizer's Masetti curve takes it too |
| 5. The mobility's unit is shown as s²·A/kg | **m²/(V·s)**: in `fem_describe what=variables`, in `fem_evaluate`'s values, and typed back in brackets |
| 6. A mesh's hmax does nothing without "custom": true: hmax 0.1, 0.05 and 0.025 all gave the same 234 triangles, and nothing said so | **Said.** `fem_build what=mesh` has "warnings": "Mesh's hmax is not used: Customize is off ("custom": true) - the normal preset's sizes taken". A solve that meshes has it among its warnings, and so does the window's message list. The same goes for hmin, growth, curvature and narrow. They are still not used without Customize: the panel shows them only with it, so a hidden value never takes effect |

**What changes for a model as it is.** Masetti's model on the library Silicon is up to 1.3 % higher for electrons and 4.6 % for holes at low doping, and less as the doping rises (electrons 0.65 % at 1e19 cm⁻³). A Masetti model on any other material, or imported from Lumerical, is unchanged. Nothing else changes values.

## Not done

- **Crosslight's Brooks-Herring with Fermi-Dirac statistics.** Its F_-½ has no function in expressions. With Boltzmann statistics it can be written as an expression of n, p, N and T. A new function changes the parser, the compiled form, the units' checks, the completions and the tools' description of expressions, so it is a change of its own.
- **A general "set but not used" warning** for any property whose condition to be shown is unmet. A shown condition is not always a used one. For example, a Material Model's µn set on the feature is used without "source": "user", since a feature's own values come before its material's. So a general rule would warn about values that are taken. The mesh's sizes are the case the check met, and they are said.

## Checks

| Check | Result |
|---|---|
| `test_fem_mobility klaassenMajorityAndMinority`: n-type 1e15 to 1e20, p-type 1e19, dark and lit | The paper's formula to 1e-9 dark (1e-15 measured), 1 % lit |
| `masettisOwnMaximum`: 1e15, 1e17 and 1e19 cm⁻³; at 350 K; µmax empty; µmax given | Masetti's formula to 1e-9 in each: µmax 1417 and 470.5 (at 350 K, (T/300 K)^-ζ of them); the lattice's 1400 and 450 empty (718.65 at 1e17); 1000 and 400 given |
| `yamaguchiSaysItHasNoTemperature`: 300 K, T 400 K, TL 350 K | The same mobility each time; the warning at 400 K and at the lattice's 350 K, nothing at 300 K |
| `interfaceReadersAreNamed`: a bar with no insulator, with Lombardi's, Yamaguchi's, an expression of Eperp, an expression of n | Each named once; the expression of n says nothing |
| `mobilityIsShownInItsUnit` | The variables semi.mun and semi.mup in m²/(V·s); 0.14 shown "0.14 m²/(V·s)"; "m²/(V·s)" typed back |
| `test_fem_materials curves`: a material of Masetti's parameters, µmax absent and 1417 | The visualizer's curve Masetti's to 1e-9 with each |
| `test_multiphysics_doc aMeshsUnusedSizesAreSaid`: a 40 by 1 µm sample, hmax 0.1 µm without Customize, then with it | 234 triangles, as the check had, and the warning; customized, 13313 and no warning |

Under the sanitizers (Debug, as CI's Linux job) the new and changed tests pass:
- the five mobility tests together: 25 s;
- `curves` and `siliconAsItWas`: 7 s;
- `masettiMobility`: 9 s;
- `aMeshsUnusedSizesAreSaid`: 5 s.

The whole suite passes in Release (135 of 135).

Two existing tests held the old Masetti µmax and now hold the new one:
- `test_fem_semiconductor masettiMobility`: its expected mobilities were of 1400 and 450 cm²/(V s). They are of 1417 and 470.5 now: 727.0544 at 1e17 for electrons, 319.0981 for holes, by semi.mun and by the bar's current alike.
- `test_fem_materials siliconAsItWas` holds every model parameter of the library Silicon equal to the feature's default, so that a model of it solves as it did. Masetti's µmax is the exception now. The feature's default is empty (the lattice's), and Silicon's is 0.1417 and 0.04705 m²/(V s).

Through the tools (the Release app, the check's bar):
- Yamaguchi's mobility was 991.7 cm²/(V s) at 300 and 400 K, the warning at 400 K;
- the no-insulator line named Yamaguchi's field;
- Masetti's model at 1e17 gave 727.1;
- semi.mun and semi.mup were listed in m²/(V·s);
- an expression of n gave no line, one of Eperp named it;
- `fem_build` of the mesh with hmax but no Customize had the warning.

## Break checks

Each fix undone on its own, its tests built and run (Release):

| Break | Caught by |
|---|---|
| 1. Klaassen's r3 6.5502 | `klaassenMajorityAndMinority`: 2e-5 off at 1e-9 |
| 2. Yamaguchi's at the physics' T not said | `yamaguchiSaysItHasNoTemperature`: no warning at 400 K |
| 2. Yamaguchi's at a lattice temperature not said | the same: no warning at a TL of 350 K |
| 3. The interface sought for any expression of the state | `interfaceReadersAreNamed`: the expression of n given a line |
| 3. The line not naming what reads the interface | the same |
| 4. Masetti's of the lattice's µ, not its µmax | `masettisOwnMaximum`: 1342.42 against 1358.69 at 1e15 |
| 4. The library Silicon without Masetti's µmax | the same |
| 4. Silicon's µmax not of T | the same: 727.05 against 502.98 at 350 K |
| 4. µmax not a key a material gives | the same: 1342.42 against 1358.69 |
| 4. An empty µmax not the lattice's | the same: 727.05 against 718.65 |
| 4. The visualizer's Masetti of the lattice's µ | `test_fem_materials curves` |
| 5. The mobility's unit unnamed | `mobilityIsShownInItsUnit` |
| 6. An hmax not used, not said | `aMeshsUnusedSizesAreSaid` |
| 6. The mesh job's warnings not kept for the answer | the same |
| 6. `fem_build`'s mesh answer without its warnings | the same |

**Not caught, one:** `perpendicular()` true again for any expression of the state. It cannot change a result. Now only a medium that reads the interface (surface scattering, Yamaguchi's, an expression of Eperp or dsurf) makes the solver look for one. `perpendicular()` then only chooses which triangles get their nearest interface computed, and an expression that reads neither Eperp nor dsurf never uses it. The change saves that work and says what the function means.
