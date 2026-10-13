# Bug hunt: CI's Linux job red since v27.0.0

*13 October 2026 - Qucs-S 27.0.0, `367419b` (the Release build, the ASan/UBSan build, and
a Release build without fused multiply-adds). The findings written up as they came. A1 and
A2 fixed in `404a858`, A3 in `8f4ca6c`, A4 in `8dd47de`.*

CI's Linux job (GCC on x86-64, Debug with ASan and UBSan, Ubuntu 24.04) failed on
`1381057` and on the v27.0.0 tag. macOS's Release job passed, and so did the local
ASan build. Five checks failed, all in the semiconductor's tests:
- `test_fem_devices`: `aSweepStartsFromTheLast`, `anAvalancheGain`, `fowlerNordheim`,
  `exampleFowlerNordheim`;
- `test_fem_optoelectronics`: `aPhotodiodesStepResponse`.

There is no Linux here (no container, no GCC), so each failure was read from CI's log and
looked for on this machine by other means:
1. **The log**, for each failure's numbers, against the same test's here.
2. **The mesh moved**: each failing model solved on 7 to 24 meshes a little different
   from its own (its size or width changed by 0.1 to 2 %). The two platforms make
   slightly different meshes, so CI's failure should show among them if it comes from
   the mesh.
3. **The code as CI ran it**: `1381057` had not yet the Newton fix of `8f4ca6c`. Its
   stopping rule was put back behind a switch, for a probe only.
4. **A build without fused multiply-adds** (`-ffp-contract=off`), to try to make Linux's
   meshes here. It did not: its meshes are a third set (below).
5. **Two diagnostic runs on CI's Linux**, from ci/ branches whose workflow built the two
   tests alone (`ci/e949e4f`, `ci/b72287f`): the photodiode's numbers, its mesh printed
   whole, and this machine's mesh solved there. With Linux's mesh, CI's failure was then
   reproduced here.

Nothing was written to the real workspace, cache or trash (checked with `find -newer`).

## Summary

| | Finding | Since |
|---|---|---|
| **A1** | "1 of 615 edges not Delaunay": a right angle's rounding taken for an obtuse angle - a false report, nothing left out | `18b5157`, fixed in `404a858` |
| **A2** | `semi.Ifn`, Fowler-Nordheim's current, from the first insulator triangle's field: 1.5 % low and ±1.2 % from mesh to mesh (Linux's 3.8 % and 7 % low); the tests' reference noisy too | `ed50888`, fixed in `404a858` |
| **A3** | Beyond breakdown, a solve "converged" on Linux: Newton stopped on small steps alone | `1ef149d`, fixed in `8f4ca6c` |
| **A4** | A photodiode's step settled 1.2 % short on Linux: a node between germanium and silicon took its first triangle's medium, as the mesh's order had it | `1ef149d`, fixed in `8dd47de` |
| C1 | None of the five checks had run on Linux before: 34 commits between the last green run and `1381057`, none run to its tests | `1b71490`..`1381057` |

## A. Wrong results and false reports

### A1. A right angle's rounding reported as an edge not Delaunay

`aSweepStartsFromTheLast` failed on Linux because the solve's log said "Semiconductor: 1
of 615 edges not Delaunay (their currents left out)". The test asks that a strip's right
angles not be counted.

**Here.** The diode solved on 18 meshes between 0.097 and 0.12 µm: 11 of them reported 1
to 3 such edges. The ones printed were exact right angles, of two shapes:
- a boundary's edge with one triangle, its angle 90° (cotangent sum -2.8e-17);
- an inner edge between two right angles (-4.5e-16).

After the fix none of them is reported, so every one was within rounding of nought.

The tolerance was -1e-9 times the sum of the cotangents' sizes, and at a right angle that
sum is nought too: any rounding below nought counted. Linux's arithmetic rounds the
0.1 µm mesh's right angle below; macOS's rounds it above.

The edge's weight is max(sum, 0) - nought either way. **No current was lost**: only the
log line was wrong, with the check that reads it.

**Fixed in `404a858`.** The tolerance is now measured against the cotangents and the weight both,
Σ(|cot| + 1)·½·weight, never nought. After the fix, none of 24 meshes from 0.04 to
0.2 µm reports one. A mesh made to have a genuine obtuse angle is still reported (the test
below).

**Test** (`aRightAngleIsNoEdgeLeftOut`). The diode's mesh, with one node across a
boundary's edge moved:
- to make the angle there 90° and 1e-12 rad: nothing said;
- to make it 118° (cotangent -0.53): "1 of 556 edges not Delaunay".

A node is chosen whose move leaves every other edge Delaunay and every triangle turned as
before. Both cases are deterministic on any platform.

### A2. Fowler-Nordheim's current from the first triangle's field

`fowlerNordheim` on Linux: `semi.Ifn` 3.1605e-6 A against J·W 3.2864e-6 at the oxide's
field (3.8 % low, 1 % allowed). `exampleFowlerNordheim` at 5.96 MV/cm: 9.696e-11 against
1.043e-10 A/cm² (7 % low).

**Here.** I/(J·W) moves from 0.970 to 1.009 over 21 meshes of the test's capacitor (its
width ±1 %, the interface's element size ±0.5 %). The test passed on macOS's own mesh by
chance, at 0.994. The cause is in the field.

A planar capacitor's oxide field should be uniform along it. Measured:

| Depth into the oxide | Field along the interface (MV/cm) |
|---|---|
| 0.2 nm | 9.80 to 10.00 |
| 2 nm | 9.92 to 9.96 |
| 5 nm | 9.939 to 9.947 |

The interface's potential goes 0.623 to 0.648 V from node to node: about Vt. At 10 MV/cm
the inversion layer is about 0.1 nm thick, far thinner than the 2 nm mesh. Its charge is
lumped into each interface node's box, and each node's potential moves by Vt ln of its
box's depth. The first triangles' fields move with them (±1 %). J changes 2 + B/E ≈ 26
times as fast as E, and 42 times at 6 MV/cm (the example's 7 %).

The tests' reference was noisy too: the field read at one point is 0.04 % from the next
point's, 1 % in J.

Against the oxide's field averaged along its middle, each candidate field gives
(21 meshes, as a fraction of J·W):

| Field | I / J(⟨E⟩)·W |
|---|---|
| The first insulator triangle's (as it was) | 0.968 to 0.993 |
| At 2 nm | 0.999 to 1.002 |
| At the classical turning point, φB/E deep | 0.999 to 1.002 |
| The barrier's mean, φB over the depth where V has moved by φB | **0.9999 to 1.0007** |

**Fixed in `404a858`.** `semi.Ifn` now takes each edge's field as the barrier's mean. From the edge's
middle, the walk goes along its normal into the insulator, triangle by triangle, to where
the potential has moved by φB/q: E = φB/(q x_t). This is the triangle that Lenzlinger and
Snow's J is for, fitted to the barrier's width. Where the insulator is thinner than the
barrier (direct tunnelling), E is the potential's change across it over its thickness. A
ray through a corner goes on in the triangle just ahead.

The help (Fowler-Nordheim Tunnelling), the global's description and the README say so.

**Tests.**
- `fowlerNordheim` and `exampleFowlerNordheim` read the oxide's field as its mean along
  its middle (200 points), and hold `semi.Ifn` to J·W within 0.3 % (it was 1 %). Seven
  fields, 6 to 12 MV/cm: within 0.05 % as printed.
- `fowlerNordheim` adds 2 nm of oxide at 2 V and at -3 V (accumulated): the barrier,
  3.2 and 3.5 nm, is wider than the oxide, so each edge's field is the potential's change
  across it. Its current agrees with Σ J(ΔV/t)·L from the edges' middles and the gate
  to 1e-6.

### A3. Beyond breakdown, a solve "converged"

`anAvalancheGain` solves the diode at -60 V, past its breakdown (51.7 V), and asks for
"no solution found". On Linux it returned no error at all.

**Here.** With the current code the -60 V solve fails, as asked, on all 15 meshes from
0.115 to 0.125 µm. With `1381057`'s stopping rule put back (Newton converged when its
steps were small, its equations unasked), 12 of the 15 "converge": the cathode's current
anywhere from 8e-14 to 148 A.

**Fixed in `8f4ca6c`** (13 October, after `1381057`; not yet run by CI then). Newton now
stops only when each node's equations are met to 1e-6 of the flows through it.

**On Linux** (the diagnostic run `ci/e949e4f`, with `404a858` and `8f4ca6c`): A1's, A2's
and A3's five checks all pass - `aRightAngleIsNoEdgeLeftOut`, `fowlerNordheim`,
`exampleFowlerNordheim`, `aSweepStartsFromTheLast`, `anAvalancheGain`.

### A4. A photodiode's step settled 1.2 % short on Linux: a node between germanium and silicon took its first triangle's medium

`aPhotodiodesStepResponse` (the sanitized build's coarser path) on Linux: the step's
current at 100 ps 1.1231e-6 A, its rest lit 1.1372e-6 (0.9876; 1e-3 allowed). Its
frequency step at 10 GHz was 0.9225 against the step's own 0.9349.

**Here.** The same path, on the ASan build and on the Release build, with the stopping
rule of `1381057` and of `8f4ca6c`: on 7 meshes from 0.099 to 0.101 µm, and on 12 of the
no-FMA build's from 0.095 to 0.11 µm. Every step settles within 6e-4 of its rest, both
stopping rules alike, and the 10 GHz frequency step is 0.934 to 0.959.

Neither the time stepping nor the small-signal solve is fragile here:
- the step's end is the same (1.1372e-6 A) from rtol 5e-4 to 5e-3, in 109 to 67 steps;
- the frequency step's solve leaves a residual 1e-15 of its right side, and one round of
  refinement changes it by 2e-11.

**On Linux, a first diagnostic run** (`ci/e949e4f`: the two tests built alone, the step's
numbers printed). Both of its solves show a slow part that this machine's mesh has not:

| | Linux | here |
|---|---|---|
| The light's response at 1 GHz, of 1 MHz's | 0.9923 | 0.99937 |
| at 5 GHz | 0.9549 | 0.9870 |
| at 10 GHz | 0.9225 | 0.9515 |
| The step at 20 ps, of its rest | 0.957 | 0.980 |
| at 40 ps | 0.973 | 0.999 |
| at 100 ps | 0.988 | 0.99984 |

From 40 to 100 ps Linux's step falls short by 2.7 % down to 1.24 %: a part of about 4.5 %
of the photocurrent, collected with a time constant of 79 ps. The frequency step shows the
same part (0.8 % lost by 1 GHz). So the two solves agree with each other on Linux, and it
is the device on Linux's mesh that has the slow part.

**A second diagnostic run** (`ci/b72287f`) printed Linux's mesh whole (697 nodes and 1330
triangles; this machine's has 685 and 1308) and solved the photodiode on this machine's
mesh on Linux:
- **on Linux, on this machine's mesh**: this machine's numbers to every digit printed -
  at rest 1.143914e-6 A, 0.999373 at 1 GHz, the step 0.999836 at 100 ps. The solver is
  the same on both platforms; the mesh is the difference.
- **here, on Linux's mesh**: Linux's numbers to every digit - 1.1435848e-6 A, 0.992305 at
  1 GHz, the step 0.987611 at 100 ps. CI's failure, reproduced here.

**The cause.** A node shared by two semiconductors took the medium of the first triangle,
in the mesh's order, that touched it: its band edges, its ni, its lifetimes. Along the
germanium stripe's foot on the silicon slab, the nodes came out a mix, in a pattern set
by the order of the triangles:
- this machine's 15: 8 silicon, 7 germanium;
- Linux's 17: 10 silicon, 7 germanium.

The interface's band edges were jagged from node to node. The carriers still missing at
40 ps, on this machine's mesh, are in the silicon slab: the holes at its far end, the
electrons under the n⁺ corner. The slow part fills through that interface. A larger share
of the nodes doesn't settle it either: on a straight interface the two sides' shares are
nearly equal.

The rules tried, on both meshes:

| Rule | at rest, this machine's / Linux's | the step at 100 ps, this / Linux's |
|---|---|---|
| The first triangle's (as it was) | 1.1439 / 1.1436e-6 A | 0.99984 / 0.98761 |
| **The narrower gap's** | 1.1578 / 1.1559e-6 A | 0.999997 / 0.999994 |
| The wider gap's | 1.1414 / 1.1408e-6 A | 0.99922 / 0.99934 |
| The larger share's | 1.1421 / 1.1453e-6 A | 0.99967 / 0.99940 |

Refined to 0.0125 µm, the narrower and wider gaps' rules still differ by 0.9 % in the
current. Both converge smoothly at 10 GHz (0.9615 to 0.9702, 0.9598 to 0.9686), where the
first triangle's jumps.

**Fixed in `8dd47de`.** A node between two semiconductors now takes the narrower gap's medium (an
absorber's, a well's, a channel's: their carriers on their own bands to their edge), and
between equal gaps the lower domain's - whatever the order of its triangles. Since
`1ef149d`; it mattered from `aaf8b8e` (heterojunctions).

**What moved** (the Release build; the examples as shipped):

| | as it was | now |
|---|---|---|
| The modulator example's loss at 0 V | 0.649 dB | 0.615 dB |
| its extinction at -3 V | 5.643 dB | 5.646 dB |
| its photocurrent at 0 V | 0.168 mA | 0.160 mA |
| The photodiode example | 1.149 A/W | 1.155 A/W |
| its dark current at -1 V | 7.20 nA | 7.37 nA |
| its 3 dB through 50 Ω at 25 µW | 33 GHz (its description's) | 34.9 GHz |
| Virot et al.'s photodiode, 40 µm | 1.248 A/W | 1.248 A/W |
| Liu et al.'s modulator's extinction | 6.51 dB | 6.52 dB |

On the sanitized path's meshes, 0.08 to 0.12 µm, the figures now hardly move:

| | as it was | now |
|---|---|---|
| The modulator's loss at 0 V | 0.619 to 0.745 dB | 0.602 to 0.610 dB |
| its photocurrent at 0 V | 160 to 189 µA | 157 to 159 µA |
| The photodiode | 1.138 to 1.154 A/W | 1.148 to 1.156 A/W |

Two examples' descriptions (`ge_photodiode.qfem`, `gesi_eam.qfem`), the README, and the
tests' figures were updated to match.

**Test** (`aHeterojunctionsNodesWhateverTheirOrder`). A silicon-germanium diode, its
germanium in two domains (1 ns lifetimes in its first 0.1 µm, germanium's own beyond):
- its current is the same, to 1e-9, on its mesh with the triangles reversed, and sorted
  by domain one way and the other;
- at rest, each node between silicon and germanium has germanium's band potential
  (Vt ln n - V: 0.866 V, not silicon's 0.942).

Broken on purpose:
- the first triangle's medium: caught (the reversed mesh's current 1.6 % off);
- the wider gap's: caught;
- ties to the first triangle's: caught;
- ties to the higher domain's: not caught - equally independent of the order, a
  convention.

## C. CI

### C1. The five checks had never run on Linux

The last green run on main was `1b71490` (12 October, 22:06 UTC). The 34 commits after it,
to `1381057`, went to main in five pushes (`96a327c`, `27ddc46`, `408f07e`, `315caa2`,
`1381057`):
- each of the first four runs was cancelled by the next push before its tests ran;
- none of the commits went through a ci/ branch.

So the first Linux run to reach these tests was the v27.0.0 tag's. The failing checks came in with
those commits:

| Check | Commit |
|---|---|
| `aSweepStartsFromTheLast`, `anAvalancheGain` | `18b5157` |
| `fowlerNordheim` | `ed50888` |
| `exampleFowlerNordheim` | `c4da797` |
| `aPhotodiodesStepResponse` | `164ef3d` |

## Checked and found right

- **The mesher keeps semiconductors Delaunay.** Every edge reported over the 24 diode
  meshes was a right angle's rounding, none really obtuse. `aSemiconductorsMeshIsDelaunay`
  still finds none in the two photodiode examples.
- **Eigen is the same on both platforms**: it is vendored, `third_party/`.
- **CDT's refinement is deterministic**: its random numbers are a fixed SplitMix64, and
  it sorts the encroached edges it finds in its hash set.
- **The solver is the same on both platforms.** On one mesh, Linux and this machine give
  the photodiode's numbers alike to every digit printed (A4): only the meshes differ.
- **The meshes still differ between platforms.** The Ge photodiode's dark current at the
  sanitized path:
  - 6.566 nA here;
  - 6.301 nA here without fused multiply-adds;
  - 6.372 nA on Linux.

  Arm64's contracted a·b + c is one difference; the C libraries' hypot, pow and exp are
  others. No build here makes Linux's meshes, so a check that holds only on its own
  mesh's numbers fails there. The fixes above make the checks hold on any mesh near the
  model's. (The dark currents above are the first triangle's rule's.)
- **No compiler warning** in the multiphysics code on CI's GCC, beyond missing field
  initializers in `fem_solver.cpp`.
- **The time**: `fowlerNordheim` with its thin oxides (meshed at 1 nm) takes 43 s under
  ASan here (QtTest allows 300); `aRightAngleIsNoEdgeLeftOut` takes 2 s. The whole of
  `test_fem_devices` on the ASan build: 34 functions in 1768 s (ctest allows 3600).
- **The rest of the suite** (with `8dd47de`): the Release build's ctest, 118 of 118; the
  multiphysics tests on the ASan build, 14 of 14 (`test_fem_devices` 1803 s of its 3600,
  `test_fem_optoelectronics` 1001 of its 1800, `test_fem_quantum` 1285, run four at a
  time); the end-to-end scenarios, 97 checks, none failed.

## Reproducing

Beside this report:
- `probes.cpp`: slots of `TestFemDevices` (paste them into `tests/test_fem_devices.cpp`);
- `probes_opto.cpp`: slots of `TestFemOptoelectronics` - the photodiode's mesh printed
  (`zzMeshOut`), solved from a printed one (`zzSolveOnMesh`), its carriers missing at 40 ps
  (`zzWhereSlow`), refined (`zzRefine`), the examples' figures (`zzExampleFigures`);
- `mesh_linux.txt`: the photodiode's mesh as CI's Linux makes it (`zzMeshOut`'s lines);
- `interface.py`: a printed mesh's Ge/Si nodes, by the first triangle and by their shares;
- `switches.diff`: switches for probes only - `PROBE_NO_IMBALANCE` puts `1381057`'s
  stopping rule back, `PROBE_STRIPE_H` sizes the photodiode's mesh, `PROBE_EX_H` the
  examples' on the sanitized path;
- `breaks.py` and `breaks_interface.py`: each change broken on purpose, its tests run.

```
# the diode at many sizes
PROBE_H=0.0999,0.1001,0.1005 ./build/qucs/tests/test_fem_devices zzProbeDiode
# Fowler-Nordheim's fields and their estimates over meshes
PROBE_W=0.1,0.1002 PROBE_F=0.002,0.00199 ./build/qucs/tests/test_fem_devices zzProbeFn
# past breakdown with the old stopping rule (switches.diff applied)
PROBE_NO_IMBALANCE=1 PROBE_H=0.1201,0.1195 ./build/qucs/tests/test_fem_devices zzProbeBeyond
# the step response on another mesh, the sanitized path (switches.diff applied)
QUCS_TEST_SLOW=1 PROBE_STRIPE_H=0.0997 ./build/qucs/tests/test_fem_optoelectronics aPhotodiodesStepResponse
# A4 here: the photodiode on Linux's mesh (probes_opto.cpp pasted)
PROBE_MESH=docs/bug_hunts/2026-10-13-ci-linux-red/mesh_linux.txt ./build/qucs/tests/test_fem_optoelectronics zzSolveOnMesh
python3 docs/bug_hunts/2026-10-13-ci-linux-red/interface.py docs/bug_hunts/2026-10-13-ci-linux-red/mesh_linux.txt
# the no-FMA build
cmake -S qucs-s-26.1.1 -B build-nofma -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt -DCMAKE_CXX_FLAGS=-ffp-contract=off -DCMAKE_C_FLAGS=-ffp-contract=off
cmake --build build-nofma --target test_fem_devices test_fem_optoelectronics
```
