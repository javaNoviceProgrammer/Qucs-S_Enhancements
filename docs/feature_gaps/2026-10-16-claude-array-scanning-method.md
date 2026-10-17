# The waves physics: one source in a periodic structure from its unit cell

The user, after the solver review (`2026-10-16-claude-fem-solver-gpu-vs-sparse-lu.md`), named four features worth adding to the multiphysics:

1. a dipole or line-current source in the native waves physics, complex-valued, with the PML built in;
2. the mesh and the matrix's ordering reused across a sweep, where only material values change;
3. the array-scanning method for periodic structures: a few unit-cell Floquet solves a wavelength, in place of the 30 µm window;
4. for a quick answer, fewer wavelengths or a narrower window (the Purcell factor needs a few µm; the wide window is there for the 5° far-field cone and the glass/air fractions).

| | Where it stands |
|---|---|
| 1 | Done in d0d688b, written up in `2026-10-16-claude-fem-solver-sparse-lu.md` §2. The review's OLED model, rebuilt on the native physics, has 341,275 complex unknowns against 681,054 real ones. A wavelength takes 1.22 s against 4.08 s, and three take 2.08 s against 8.56 s. |
| 2 | **Already there.** A sweep builds the geometry and mesh again only where a parameter changes them. Accelerate's analysis (the ordering) is kept by pattern, so a sweep's next point refactorizes: 1.9 s, then 0.06 s (78951cd). On the native OLED model, each wavelength after the first takes 0.45 s, assembly included. Eigen's LU, below 20,000 unknowns, analyses again; its ordering is a small part of its time. |
| 3 | **This commit**, below. |
| 4 | A modelling choice, and the scan changes it. The Purcell factor now comes from one unit cell: the OLED's is 9,867 complex unknowns, not 341,275. The glass fraction is what the cell's top periodic port takes. What the scan does not yet give is the power by angle, for the escape cone and the 5° cone; see "Not done". |

## 3. The array scan

A Periodic Condition's **Periodicity: Array scan** solves the cell at kF across its Brillouin zone. The mean of the fields is one source's field: not that of a periodic array of sources, but of the single dipole in the periodic structure. For the field, (Λ/2π) ∫ u_k dk picks out the source's own cell and cancels its images. By Parseval, the power through a line of every cell is the mean of the cells' powers.

| | |
|---|---|
| Given | **kF points** (32) and **kF sampling**; a Point Dipole or a Line Current in the cell; PMLs, or periodic ports not fed, above and below. |
| Results | ewfd.Psource and ewfd.Purcell are the one source's. ewfd.Pport_<name> is what it sends out by each periodic port. The field shown is its own. |
| Refused | An odd number of points or fewer than two. A scan without a point source. Ports in the cell. Two scans. A fed periodic port or an incoming plane wave alongside the source (refused before, for any source). |
| Said | The anomalies' kF. Then Psource with every point and with half of them, and how far apart they are: the scan's measure of its own error. |

**A periodic port not fed, with a source, is new too.** Without a scan, a single Floquet solve at the condition's kF (or nought) is the field of a periodic array of sources. Its ports' ewfd.Pport_<name> are given as well. Before, every periodic port needed one fed.

### The sampling: about Rayleigh's anomalies

**Evenly**, kF = (2π/Λ)(i/N − ½), the trapezoid rule of the zone, is exactly a supercell of N cells with one source. On the tests' meshes, which are layered so that a supercell's mesh is its cell's repeated, the two agree to 1e-10.

But evenly the scan did not converge. On the test stack (a dipole in a spacer over an absorbing film, a glass tooth on it each 0.4 µm, at λ0 1 µm) the Purcell factor came out as:

| kF points | 8 | 16 | 32 | 64 | 128 | 256 |
|---|---|---|---|---|---|---|
| Evenly | 1.198 | 1.112 | 1.090 | 1.124 | 1.130 | 1.131 |

It was erratic at every count. The integrand, printed over the zone, was smooth but for a sharp peak at kF = ±k0. That is a Rayleigh anomaly: an order grazing an outer medium. Here the glass's −1 order and the air's 0 order both graze there, since Λ = λ/2.5. The cell's response has a square-root singularity in kF there, which uniform points sample at random.

**About the anomalies (the default):**

- **Where the anomalies are.** They are known before solving: kF ≡ ±k of each lossless outer medium, folded into the zone. The outer media are the periodic ports' media and the media of the PML domains before stretching; the latter needed a field of the medium's own, unstretched k².
- **How it samples.** Between the anomalies, round the zone (the integrand is periodic in kF), it uses tanh-sinh's rule. That clusters kF at both ends, and its error falls as e^−cN for a square root there. The anomalies themselves are never solved (their weight would be nought).
- **The error estimate.** Every other point is the rule of half as many, so the estimate costs nothing more.

| kF points (solved) | 8 (14) | 16 (18) | 32 | 64 (60) | 128 (126) | 256 (252) |
|---|---|---|---|---|---|---|
| Between periodic ports | 1.1246 | 1.1295 | 1.1344 | 1.1317 | 1.1317490 | 1.1317490 |
| Half of them, apart | 12.8 % | 3.0 % | 1.1 % | 1.0 % | 0.0023 % | 1.3e-9 |
| With the PMLs | 1.1417 | 1.1417 | 1.1395 | 1.1300 | 1.1314273 | 1.1314375 |

**The window agrees.** The same dipole in a window of cells, PMLs beside it (as the OLED model was built), gives 1.1320, 1.1325 and 1.1320 with 12, 24 and 48 cells. The scan with the same PMLs gives 1.13144, 0.1 % from the 24-cell window. The window of 48 cells takes 0.42 s; the scan of 128 kF takes 0.03 s.

**Found doing it: the first window comparison was 2 % off.** The test had left the dipole in the first cell of the window, 0.13 µm from its side PML. In the window's middle it agrees.

### The OLED's unit cell

The review's model, the same layers and mesh, one period of the pattern (360 nm, one SiN pillar), the dipole at (0, 330 nm), the glass's PML above:

| kF points | 32 | 64 | 128 | 256 | 512 | 1024 |
|---|---|---|---|---|---|---|
| Purcell | 0.9501 | 0.9936 | 1.01289 | 1.013674 | 1.0138157 | 1.0138161 |
| Half apart | | | | 0.066 % | 0.0039 % | 3.9e-7 |
| Time | 0.20 s | 0.28 s | 0.46 s | 0.89 s | 1.76 s | 4.0 s |

The 30 µm window gave 1.016823, 0.30 % above the converged scan, in 1.22 s. At the window's accuracy the scan takes 128 kF and 0.46 s, about 2.6 times faster. At ten times the window's accuracy it takes 256 kF, in about the window's time.

**Why not "a few" kF.** The OLED's guided modes lose little: its organic and ITO layers guide light, and the aluminium carries a surface plasmon. Each is a narrow peak in kF (printed: at about ±0.09 and ±0.19 of the zone). Tanh-sinh clusters points at the anomalies, not at the peaks. Each cell is 9,867 complex unknowns. Eigen's LU takes some 55 ms a cell, with 16 threads solving cells at once. Accelerate, refactorizing one cell after another on its own threads, took 12 ms a cell and 6.3 s for 512 kF, against Eigen's 1.76 s. The cells stay on Eigen's LU, on threads, below Accelerate's 20,000 unknowns.

**Each thread's LU is analysed once.** The pattern is the same at every kF; only the ties' phases and the orders' values change. The matrix without the periodic ports' conditions is also built once: 12 % faster.

## Not done

- **Points where the guided modes are.** An adaptive rule, splitting the intervals whose half-rule error is largest (as QUADPACK's), would put points at the OLED's peaks. 256 kF for 1e-4 would perhaps become about a hundred.
- **The power by angle.** This is the escape cone (the glass's power at |kx| < k0, what leaves into air) and the 5° far-field cone. Each order's power at each kF is the angular spectrum; the scan gives each periodic port's total alone.
- **A path off the real axis.** Moving it off the axis (k = t + jδ sin Λt, above the forward poles) would make the field's integrand analytic on it, and the field and Psource would converge as e^−cN. The ports' powers are quadratic in the field, so they need the real axis.
- **A two-dimensional lattice.** The scan is along one period alone.

## Tests

- **test_fem_waves `anArrayScanIsASupercell` (new):**
  - 4 kF evenly on a cell, against a supercell of 4 with continuity: Psource to 1e-9 (agreeing to 1e-10), the field at three points to 1e-8, the mesh four times its cell's;
  - the log's line;
  - lossless between periodic ports not fed, 8 kF: the two ports take Psource to 1e-8 (1.4e-14).
- **test_fem_waves `anArrayScanAgainstAWindow` (new):**
  - the window of 24 cells, its Purcell factor to 1e-6 of 1.1324522 (a sanitized build compares to that value: the window is 22,000 unknowns, some 40 s with Eigen's LU sanitized);
  - 128 and 256 kF between periodic ports agree to 1e-6, their half estimates below 1e-3 and 1e-6 (sanitized, 256 left out);
  - at 16 kF the half's estimate above 1e-3 and above the error;
  - 128 kF with the PMLs, and between ports, within 1e-3 of the window;
  - evenly at 64 kF off by more than 0.3 %, and its half saying so;
  - the anomalies said.
- **test_fem_waves `arrayScanRefusals` (new):**
  - 7 and 0 points;
  - no source;
  - a single Floquet solve at a given kF, the source between periodic ports not fed: they take Psource to 1e-8.
- **The rest of test_fem_waves (34 of 34).** The periodic cells' Fresnel, thin films and gratings are unchanged after periodicOrders was split (media once, orders at a kF from the mesh alone) and periodicMap took kF.

Twelve changes broken on purpose:

- **10 caught at once:**
  - the mean without the weights;
  - the scan's ties without kF (continuity);
  - no clustering (evenly);
  - the ports' powers unweighted;
  - odd points allowed;
  - a scan without a source allowed;
  - a single Floquet solve's ports' powers not computed;
  - the PML's medium taken stretched (no anomaly found);
  - tanh-sinh's weight without its cosh τ;
  - the field the even points' alone.
- **1 caught after a test was added:** the half rule the full one. Nothing read the clustered rule's estimate where it should be large; at 16 kF it now must exceed the error.
- **1 equivalent, so not caught:** the anomalies not folded into the zone. The intervals then wrap with signed lengths and still cover the zone once (its integrand periodic), the points clustered at the same folded kF.

Runs:

- **Timed under the sanitizers with Eigen's LU, as CI's Linux has it:** 11, 11 and 1 s for the three new functions (the window comparison 52 s before it took the Release value).
- **The Release suite:** 127 of 127.
- **Under the sanitizers, no report:** the new functions; the threads read the cell's matrix, mesh and media and write their own results alone.
- **The lints pass.**
