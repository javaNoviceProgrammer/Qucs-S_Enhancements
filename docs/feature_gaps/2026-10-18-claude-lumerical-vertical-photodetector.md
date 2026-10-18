# Lumerical's vertical photodetector: the four issues against now

The reviewer's "Lumerical CHARGE Vertical photodetector, reproduced in Qucs-S" (`2026-10-13-claude-lumerical-vertical-photodetector.md`, a build of 13 October) rebuilt Lumerical's Ge-on-Si vertical p-i-n (Liow et al. 2010) from its `.ldev`. Their results stood:
- dark current within 3-4 % at every reverse bias;
- responsivity within 1.5 %;
- the dark current against temperature on the measured curve;
- the 3 dB bandwidth within 7 %.

They listed four Qucs-S issues. Three came the next day in `2e349c6` (Lumerical's projects in and out); the rest is done here.

| Issue | Then | Now |
|---|---|---|
| 1. SRH lifetimes of position or doping | `taun`, `taup` refused x and y; no Fossum | **Done in `2e349c6`:** lifetimes of x and y, and by doping (Fossum's, with Hurkx's tunnelling) |
| 2. A surface recombination velocity | emulated with interface traps, S = N_it σ v_th | **The feature done in `2e349c6`** (Surface Recombination: Sn, Sp, its level); **its "Apply to majority carriers" switch now** |
| 3. Generation maps | a 2D table as a rank-8 SVD in 16 one-variable tables | **Done in `2e349c6`:** an interpolation of two arguments on a grid. A reader of CSV, NPZ or `.mat` files not done (below) |
| 4. The generation's help | "optical generation, say" | **Done:** the frequency step's modulation (semi.Hop_*, semi.Gop, semi.Iop) |

## 2. Apply to majority carriers

**Lumerical's switch** (its surface recombination boundary condition):
- **On:** both carriers recombine. This is what it advises at an insulator's or another semiconductor's interface.
- **Off:** the minority carriers alone. At a metal contact the majority carriers are held at equilibrium and the minority ones flow into it at the surface's rate. This is how Lumerical sets a new one.

**Before:** Qucs-S's Surface Recombination was the "on" case alone. At an ohmic contact both carriers are held at equilibrium, so it did nothing there, as its help said. A contact of a finite recombination velocity could not be modelled.

**Now** a flag, "Apply to majority carriers" (`"majority"`, on by default):
- **Off at an ohmic contact:** the minority carrier's equation at each of the contact's nodes is freed. Its flux into the contact is q U, with U = (n p - ni²)/((n + n1)/Sp + (p + p1)/Sn). The majority carriers stay at equilibrium. The minority carrier is the one below its equilibrium partner there (the contact's own values).
- **On at an ohmic contact:** nothing, as before. The log says so.
- **Off where no ohmic contact is:** refused. A sink of one carrier with nothing to take the other is not a boundary condition. The error says to use "on" at an insulator's or another semiconductor's interface.
- **At a Schottky contact:** none either way, and said. The contact's thermionic velocities are its recombination already. (Before, a surface's recombination there was added to them.)
- **The small signal** frees the same rows. A contact's excitation drives only the rows it holds.

**(T/300)^η**, the review's other request for this feature: a velocity is a constant of the parameters, so it is written as an expression of the temperature parameter that the physics' T names. The help now says so. The importer has written Lumerical's temperature model this way since `2e349c6` (each carrier's η, `S*(T/300[K])^(η)`).

**Lumerical's projects:**
- **The export** now writes the switch for every surface. It never did before: Lumerical's default for a new one is off, so an exported oxide interface recombined its minority carriers alone in Lumerical. That was a latent bug.
- **The import** reads it:
  - at a metal it becomes Surface Recombination with the switch off, and is noted. Before, it was left out with "their carriers at equilibrium already".
  - "on" at a metal is noted, and the minority carriers alone are taken: the contact holds the majority.
  - "off" at an insulator is noted, and both carriers are taken, as Lumerical advises.
  - a metal not in the region is still left out, with why.

## The reviewer's model on this build

The reviewer's own `vpd.qfem` and `vpd_bw.qfem` were run through the tools on scratch copies. They were run three ways:
- as written: interface traps for the two surfaces, and the rank-8 fit for the generation;
- with Surface Recombination in place of the traps (Ge/SiO₂ S = 2.25e5 (T/300)^-3.8 cm/s, Si/Ge 5e4 cm/s, midgap);
- with the map itself as one grid function, `gmap(x, y)` (1904 points), in place of the fit.

| | The review, 13 Oct | Now, traps | Now, Surface Recombination | Now, the map's grid | Lumerical |
|---|---|---|---|---|---|
| Dark current, -1 V | 0.333 µA | 0.3341 µA | 0.3344 µA | | 0.344 µA |
| Dark, -2 / -0.5 V | 0.419 / 0.258 µA | 0.421 / 0.260 | 0.4216 / 0.2599 | | 0.437 / 0.268 |
| Forward, +0.1 / +0.25 V | 1.90 / 91.6 µA | 1.915 / 91.45 | 1.917 / 91.47 | | 1.93 / 89.6 |
| Responsivity at 1.2 mW, 0 / -0.5 / -1 / -2 V | 0.856 / 0.906 / 0.931 / 0.960 A/W | - / 0.9067 / 0.9319 / 0.9603 | - / 0.9070 / 0.9321 / 0.9606 | 0.8569 / 0.9070 / 0.9321 / 0.9606 | 0.867 / 0.919 / 0.945 / 0.974 |
| Dark vs T at -1 V (27 to 87 °C) | 0.33, 0.73, 1.57, 3.23, 6.35 µA | 0.334, 0.737, 1.58, 3.25, 6.38 | 0.334, 0.738, 1.58, 3.25, 6.38 | | measured 0.36, 0.75, 1.62, 3.28 (to 72 °C) |
| 3 dB bandwidth, -1 V | 3.60 GHz | 3.723 GHz | 3.731 GHz | 3.730 GHz | 3.87 GHz |
| At 20 GHz | -7.80 dB | -7.74 dB | | -7.75 dB | -7.74 dB |
| q ∫G in the germanium (both halves) | 1.273 mA | | | 1.2732 mA | |
| The 24-frequency small signal | 112 s | 12 s | | | |

**Surface Recombination gives the traps' answer:** within 0.1 % at every bias and temperature, 0.2 % in bandwidth. The traps had N_it so small that their charge did not count, as the reviewer said. Now the feature says what it is.

**The map as a grid gives the fit's answer** to the fourth digit. The reviewer's 1.5 % shortfall in responsivity is unchanged: about 2 % of Lumerical's map lies just outside the germanium, which the generation's selection leaves out.

**The bandwidth rose from 3.60 to 3.72 GHz** between 13 and 18 October, on the same files. This is 4 % below Lumerical's 3.87 (grad Phi). It was not bisected. Over the same days the germanium photodiode example's capacitance under light also moved (`2026-10-18-claude-semiconductor-capability-recheck.md`).

## Found on the way

The Lumerical importer's fuzzer (`QUCS_LUMERICAL_FUZZ`) was run against the importer changed here. It found two old faults, both from damaged MQW projects:
- **A crash.** A stored result's second dimension was overwritten with 0xFFFFFF0000000001 and its numbers read as complex. The dimensions' product wrapped past 2^64, and its bytes past a qint64 to a negative number, so it was checked as fitting. Then a vector of 2^63 doubles was allocated: length_error, and Qucs-S gone. Now each dimension, and their product, is checked against the blob's bytes; a damaged result is left out and the project still reads.
- **A hang.** The layer table's count 3 was overwritten by the bytes "lue\0", making it 1819632896. An empty layer was pushed for each, minutes in a loop. Now each layer listed must be there; otherwise the import is refused with "its MQW solver's layer table lists 1819632896 layers, 3 there".

After both fixes: three seeds of 150 s each, about 200 000 scripts and 51 000 damaged projects, nothing. A sanitized 120 s run, no report.

## Not done

- **A generation read straight from a CSV, NPZ or `.mat` file.** The review offered it as an alternative to a 2D interpolation, which `2e349c6` gave. A map goes in as an Interpolation of two arguments: rows of x, y and G, any order. The importer makes one from Lumerical's own generation object.

## Tests

- **test_fem_semiconductor `aContactsSurfaceRecombination` (new).**
  - An n bar under uniform generation, its left ohmic contact recombining its holes at Sp = 1e4 cm/s. Δp at 0, 5 and 30 µm matches the closed form within 5e-3 (5.154e18 against 5.149e18 at the contact). The electrons at the contact are at equilibrium. Under 1 kHz modulated light the contact's current is its dc one (0.016684 of q Gop against 0.016663).
  - S of 1e12 cm/s gives the plain ohmic contact; "on" gives none, said. "Off" on a side with no contact is refused.
  - A short diode whose anode recombines its electrons at Sn = Dn/W: its current 0.62153 of the ohmic one's, against (Dn/(W + Dn/S) + Dp/W)/(Dn/W + Dp/W) = 0.62162. Its small-signal conductance at 1 kHz is the dc slope within 2e-3.
- **test_fem_semiconductor `aSchottkyDiode`:** a surface's recombination at the Schottky contact leaves its current exactly as it was, and is said.
- **test_fem_lumerical `aGradedDiodeRoundTrip`:**
  - a back contact of Sn = Sp = 1e5 m/s with the switch off, out and back in. The script sets the switch to 1 for the oxide and 0 for the contact.
  - the bare import (no `#qucs-s` line) gives the switch off at the metal, on at the oxide, and the note. It solves to the same current within 1e-3 (5.9194e-8 against 5.9223e-8 A).
- **test_fem_lumerical `damagedInputsAreRefused`:** the fuzzer's two projects, rebuilt from `lumerical_mqw_offsets.ldev`. The stored results damaged: the project reads. The layer count damaged: refused, with why, in under 5 s.

## Breaks

Fourteen changes broken on purpose, all caught:
- **The solver:**
  - the minority carriers not freed in Newton;
  - the majority freed too;
  - "off" allowed away from a contact;
  - the recombination on the majority's row;
  - in the small signal, the minority row held;
  - in the small signal, the minority row driven;
  - a Schottky contact's surface recombining too;
  - the switch off when not given.
- **Lumerical's projects:**
  - the export's switch inverted;
  - the import's "on" at a metal;
  - the import's surface at a metal left out;
  - the import's note not said.
- **The fuzzer's two:** the size check removed (length_error); the layer check removed (hung, killed at 30 s).

"In the small signal, the minority row held" first got through: the diode's conductance moved 0.04 %. The electrons the cathode injects keep the anode's quasi-Fermi level near the cathode's, so holding its change at nought was all but right. The bar under modulated light catches it (1.4e-5 of q Gop against 0.0167).

## Runs

- **Release:** the suite 129 of 129.
- **Under the sanitizers, with Eigen's LU (as on CI's Linux):** no report.
  - `aContactsSurfaceRecombination` 31 s on a coarser diode there (198 s on the Release mesh);
  - `aSchottkyDiode` 143 s, `surfaceRecombination` 7 s;
  - `aGradedDiodeRoundTrip` 107 s, `damagedInputsAreRefused` 0.1 s.
- **The lints pass.**
