# Bug hunt: crash hardening, round two - solvers, the converter, saved settings

*10 November 2026 - Qucs-S at `c6ce80cc` (main, with the first round's fixes), a Release build and an
ASan/UBSan (Debug) build of the worktree `crash-hardening-58de8d`, macOS 27.2, Qt 6.12. The user: "do
another round of bug hunting for one hour". Found and documented here; then all fixed (the user's "Fix them
all"), each with a test that fails on the code before it (the table's last column).*

The first round ([2026-11-09-crash-hardening.md](2026-11-09-crash-hardening.md)) damaged files' bytes, and
most of them stopped at the reader. This round goes past the readers: whole files with hostile *values*,
then solved; the converter qucsator_rf and the program run on the user's data; the program started on
damaged settings.

## Summary

| | Finding | Kind | Where | Fixed |
|---|---|---|---|---|
| **S1** | Closing a 2D model while its mesh builds or its study solves blocks for as long as the job does not reach a check: `~MultiphysicsDoc` cancels its job, then waits with no deadline (the first round's H2, fixed then only in the 3D model's document). Seen four times: the ray tracer, which never looks at the cancel flag (`grin_lens.qfem` with one corner at `-4e-300` instead of `-4` traces for more than 7 minutes, and `fem_stop` cannot stop it); a mesh still placing its boundaries' nodes (S2's loop: `boron_implant_anneal` at `hmax` `1e-7`, `vcsel_modes`); a quantum spectrum with an infinite linewidth (`gaas_eam_excitons`). Closing the tab, or quitting, freezes the window | hang (GUI thread) | `multiphysicsdoc.cpp:345`; `fem_rays.cpp` `trace()`; `fem_mesh.cpp:636-665` | yes: a closed tab waits 2 s, then leaves its job to end (as the 3D one since H2); the ray trace and the boundaries' walk ask whether to stop - `test_multiphysics_doc`, `test_fem_engine` |
| **S2** | A 2D mesh with a tiny `hmax` (`1e-7`; a typo for `1e-3`) allocates without limit: each boundary gets `lround(length / h)` nodes with no cap, before the mesh's 4-million-vertex budget is ever checked. `annulus_laplace` with `hmax` `1e-7` reaches 3.4 GB in 12 s and keeps growing; a 3D model's base mesh goes the same way (`tsv.qfem3d` with `hmax` `1e-308`: 8 GB in 1.3 s). Seen seven times. The first round's C3 gave only the 3D extrusion a budget | memory (to `bad_alloc` or the machine's swap) | `fem_mesh.cpp:646` | yes: the boundaries' and layered regions' nodes counted before any is placed, the mesh refused past its 4,000,000 - `test_fem_engine` |
| **S3** | A periodic port at a frequency of `1e30` Hz (`grating_orders.qfem`): the count of propagating orders overflows an `int`, so no orders are listed, and the solve reads the fed port's empty list - SIGSEGV on the solve thread, the program gone | crash (SIGSEGV) | `fem_waves.cpp:3153`, `:3732` | yes: a port's orders counted in doubles, a cell past 500 propagating orders refused, the specular order checked - `test_fem_engine` |
| **R1** | qucsconv_rf, VCD: `$upscope` before any `$scope`, or `$dumpvars` before the first `#time`, dereferences NULL | crash (bundled converter) | `parse_vcd.ypp:137`, `:279` | yes: an `$upscope` with no scope passed over; values dumped before a time stamp the ones at 0 - `test_qucsator_robustness` |
| **R2** | qucsconv_rf: names from the file `sprintf`ed into 256-byte buffers. SPICE: a line it cannot translate (a `B` source, for one) with a name over about 250 characters overflows `char str[256]` on the stack - Release aborts on the stack guard; the program runs qucsconv on a SPICE netlist part's file. The same in CITI's variable names (`citi_check`, on the stack) and the Qucs netlist's instance names (`netlist_instance`, a static buffer), where Release runs on with its memory overwritten | stack and global buffer overflows | `check_spice.cpp:1324`, `:1335`; `check_citi.cpp:236`; `qucs_producer.cpp:118` | yes: `snprintf` to the buffer's size (a name cut at 255), two buffers sized for their numbers - `test_qucsator_robustness` |
| **R3** | qucsator_rf and qucsconv_rf: a Touchstone file of two bytes, `#` and a newline, dereferences NULL (`touchstone_idents`). A simulation with an S-parameter file part (`SPfile`) on such a file ends with SIGSEGV | crash (bundled simulator) | `check_touchstone.cpp:426` | yes: `#` alone the defaults' (GHz, S, MA, 50 Ω), as Touchstone has it - `test_qucsator_robustness` |
| **R4** | qucsator_rf and qucsconv_rf: a CSV column whose name begins with a NUL byte: `csv_validate_str` writes `_` over the terminator and walks on - ASan's heap-buffer-overflow. A file-driven source (`Vfile`, `Ifile`) reads its CSV through it | heap overflow | `check_csv.cpp:72` | yes: an empty name left empty - `test_qucsator_robustness` |
| U3 | qucsconv_rf, ZVR: a points count past an `int` cast from `double` (UBSan) | undefined behaviour | `parse_zvr.ypp:129` | yes: a count past an `int` none - `test_qucsator_robustness` |
| U5 | A quantum spectrum's linewidth of 1e308 eV (`3[meV]+1e308`), finite but past any spectrum's width: `int(std::ceil(top / step))` of infinity, clamped only after the cast (UBSan, three places) | undefined behaviour | `fem_quantum.cpp:3816`, `:4780`, `:4886` | yes: clamped as doubles before the cast (and a fourth, `:4718`); a broadening that is not finite refused - `test_fem_engine` |
| U4 | qucsator's string hash shifts a negative `int` left (a name of 25 characters or more shifts it until negative; UBSan) | undefined behaviour | `hash.cpp:43` | yes: hashed unsigned - `test_qucsator_robustness` |
| N6 | A Touchstone file of Z or Y data with a reference impedance of `1e-320` makes the conversion to S invert a singular matrix: `assert(MaxPivot != 0)` aborts a Debug build (`matrix.cpp:820`); Release, without asserts, goes on with what it computes | note (Debug only) | - | - |
| N5 | A 2D solve has no memory estimate before it starts: `quantum_wire.qfem` with `hmax` `1e-30` meshes to the budget (1.09 million nodes) and its eigensolve passes 15 GB (four times: `quantum_wire` three, `mos_capacitor` one). A 3D model estimates first (`tsv.qfem3d`: "12 GB" for the direct solver) | note | - | - |

S1 to S3 and U5 are in Qucs-S's own code; R1 to R4, U3 and U4 are upstream qucsator's. S3 and R1 to R3 end
a process: S3 the program, R3 the simulator, R1 and R2 the converter. S1 leaves the window frozen with no way
out but to force it to quit, and S2 takes the machine's memory.

Each new test was run on the code before its fix, the fixes' sources put back to `c6ce80cc`'s. In
Release: R1's, R2's and R3's failed on a signal; S3's ended in SIGSEGV; S1's two never ended (the close
stopped at 150 s, the trace at 120 s); S2's budget test filled memory until stopped at 400 s, and its
boundary walk was not stopped (it meshed for 119 s). On ASan/UBSan, R4's, U3's, U4's and U5's failed
(Release does not see them) and R1-R3's again. With the fixes, Release's 190 suites pass through ctest,
the four lints among them. In the last full run one timing check (`test_markdown_doc`'s
`longDocumentsRenderAtOnce`, beside the ASan suites) was slow; run alone, that suite passes. On ASan/UBSan
the 16 suites the fixes touch pass: `test_fem_engine`, `test_fem3_mesh`, `test_fem_rays`, `test_fem_waves`,
`test_fem_quantum`, `test_fem_quantum_2`, `test_fem_exciton`, `test_fem_semiconductor`, `test_fem_laser`,
`test_fem_process`, `test_multiphysics_doc`, `test_multiphysics3d_doc`, `test_multiphysics_converge`,
`test_running_jobs`, `test_qucsator_robustness` and `test_library_export`. `test_fem_engine` passed once
a fourth mesher cast was fixed: UBSan stopped on it in the new budget test.

## How it was fuzzed

Three new fuzzers, in [2026-11-10-crash-hardening-2/](2026-11-10-crash-hardening-2/), with the first
round's client (`mcp.py`) and mutations (`mutate.py`):

| Run | Build | Time | Amount | Events |
|---|---|---|---|---|
| `fuzz_solve.py`: the examples' 52 2D and 6 3D models, an RF 2.5D model and 193 schematics (`examples/qucsator`, `examples/ngspice`) with 1-5 of their numbers made hostile (0, -1, 1e308, NaN, inf, 1e-308, empty, `x*1e300`, `x/0` ...), the file kept whole; then geometry, mesh, `fem_solve`, `fem_evaluate`, `fem_plot` (`rf25_solve`; `simulate` with qucsator or ngspice and `get_dataset`), and closed. A server past 8 GB killed and kept | Release, 2 seeds; ASan, 1 | 35-38 min each | 13,234 cases (Release 12,460: 9,651 schematics simulated, 2,465 2D models, 288 3D, 56 RF 2.5D; ASan 774) | S1 x4, S2 x7, S3, U5, N5 x4; one 3D solve past the fuzzer's 8 GB within its own estimate (12 GB) |
| the same, 3D and RF 2.5D models alone (`ONLY=.qfem3d,.qrf25`), a server killed past 12 GB | ASan | 28 min | 345 cases (295 3D, 50 RF 2.5D) | none |
| `fuzz_conv.py`: qucsconv_rf, damaged files of each input format it has - SPICE (13 netlists from the library and examples), VCD, Touchstone (1, 2, 3 ports), CITI, ZVR, MDL, CSV and Qucs datasets - into each output it pairs them with | Release, 2 seeds; ASan (VCD left out), 2 | Release 3 min (stopped once R1 filled it), ASan 25 min each | 62,700 runs Release, 151,240 ASan | Release 26: R1 x20, R2 x5, R3. ASan 46: U3 x23, R2 x16, R4 x2, R3, U4, N6 x3 |
| `fuzz_settings.py`: the program started on a `qucs_s.ini` of hostile values for the 267 keys its sources read (fonts and colours that are not, byte arrays of noise for saved window state, numbers past range, lists of 20,000 paths, 100,000-character strings), then a schematic opened, drawn, simulated, its display opened and a screenshot taken | ASan, 2 seeds | 25 min each | 225 starts | none |

## S1. Closing a 2D model whose solve does not stop

`grin_lens.qfem` traces 13 rays through a graded-index lens in 0.4 s. With its air rectangle's corner at
`(-4)*1e-300` instead of `-4`, the trace runs on: 37 % after 5 s, 10 % after 40 s in another run, more
than 7 minutes in all. `fem_stop` answers "Asked to stop, still at work after 30 s". Then
`close_document` - the tab's close - never returns, and nothing after it is answered (stopped after 400 s).

A sample of the process: the pooled threads in `RayProblem::solve`'s worker, each in `trace()`'s RK4 loop
(up to a million steps per segment, up to `maxinteractions` segments per ray, five index lookups a step).
Nothing in `trace()` reads the job's cancel flag. `~MultiphysicsDoc` (`multiphysicsdoc.cpp:341-347`) sets
`cancel` and then `job->thread->wait()` with no deadline - the pattern the first round fixed in
`~Multiphysics3DDoc` (H2: wait 2 s, then leave the thread to end and delete itself).

The fuzzer closed three more 2D models under a job that did not end. On ASan, `boron_implant_anneal` and
`vcsel_modes` with tiny mesh sizes, their meshes "still being built after 60 s" at "8 % - Boundaries"
(S2's loop, which asks nothing). On Release, `gaas_eam_excitons` with its linewidth `3[meV]^1000`, its
optical spectrum at 12 % after 40 s. Each close waited past the fuzzer's 120 s.

**Fixed**, both ways. `~MultiphysicsDoc` waits 2 s for each job, then leaves it to end on its own: the
thread holds its job (a `shared_ptr`) and everything it reads, and deletes itself when it finishes. Its
work lambdas capture values (a `Model`, tags), never the document. The ray tracer asks whether to stop:
per ray, every 64 interactions and every 4,096 RK4 steps, the study's `cancelled()` asked by one thread at
a time and kept once it is yes. The mesher's boundary walk asks per boundary and every 65,536 steps.
Tests: `test_multiphysics_doc`'s `closedWhileItTraces` (the lens closed after 1.5 s of tracing, "Stop and
Close" answered: closed in 51 ms); `test_fem_engine`'s `aRayTraceStops` (stopped 5 ms after it was asked)
and `aMeshStopsWhileItWalksItsBoundaries`.

## S2. A 2D mesh without a budget for its boundaries

`annulus_laplace.qfem` with `hmax` `1e-7`: `fem_build` mesh goes to 3.4 GB in 12 s, 4.5 GB at 14 s
(sampled in `Mesher::run`). Each boundary's node count is the integral of `1/h` along it, `n =
max(1, int(lround(f)))` (`fem_mesh.cpp:646`); the walk that integrates it stops at 2 million steps, but
`n` has no cap, and its `n - 1` positions are pushed into `where` and then into the nodes. A circle of
radius 1 at `h = 1e-7` is about 6e7 nodes - one boundary. The 4-million-vertex budget (`:888`) is the
refinement's, checked after. At `hmax` `1e-308` the `int` of `lround` is undefined as well.

Seen seven times: `annulus_laplace` twice (`hmax` `1e-7`), `drum_modes` (`1e-308`, ASan), `kp_laser_gain`
(`1e-7`), `ingaas_mqw_qcse` (a well 1e15 nm thick), `vcsel_laser` (a layered region's lateral size
`0.1*1e-300`: its columns, `fem_mesh.cpp:504`, are capped at 100,000 per region but not in all), and
`tsv.qfem3d` (`hmax` `1e-308`: the 3D model's plan mesh is this 2D mesher; the first round's prism budget
comes after it).

**Fixed**: each boundary's count is taken as a double and added up before any node is placed. Past the
mesh's 4,000,000 vertices (the refinement's budget, now one constant) the mesh is refused: "the mesh would
have about 3.14e+07 nodes on its boundaries, more than 4000000: a larger size there", or "countless" at
`1e-308`. A layered region's grid is counted the same way before its nodes are made. The counts that
feed these - a layered boundary's columns from its lateral size (`fem_mesh.cpp:1696`, an `int` of 4e301
at a lateral size of `1e-301`, which UBSan stopped on in this test), its rows, a region's columns and a
size field's samples - are clamped as doubles before they are cast. A test:
`test_fem_engine`'s `boundaryNodesPastTheBudgetRefused` - the annulus at `1e-7` and `1e-308`, the VCSEL
with a lateral size of `1e-301`, each refused in well under a second; the annulus as it is, meshed.

## S3. A periodic port at 1e30 Hz

`grating_orders.qfem` with its study's `frequencies` `1e30` (for `c0/lambda0`): `fem_solve` ends the
program after 11 s (Release: SIGSEGV in `std::complex` multiplication in `WaveProblem::solve`).
UBSan: `fem_waves.cpp:3153`, "3.33564e+15 is outside the range of representable values of type 'int'",
then "2147483647 + 1 cannot be represented". The `propagating` order count wraps negative, `M` with it,
and the loop over `-M..M` lists no order. At `:3730` the fed port's specular order is looked for in its
empty list, and `fp.beta[specular]`, `fp.c[specular]` read past it (ASan: SEGV at address 0).

**Fixed**: before the orders are made, each periodic port's propagating orders are counted in doubles. A
cell past 500 is refused: "periodic port ... is ... wavelengths wide at ...: about ... orders propagate,
more than the 500 solved" (each order is two vectors of the system's size, and an ordinary grating has
tens). `periodicOrders` clamps its counts as doubles too (the scan's calls), and the fed port's specular
order is checked before it is read. A test: `test_fem_engine`'s `periodicOrdersPastTheLimitRefused` - the
grating at 1e30 and 1e20 Hz refused, at its own frequency solved.

## R1-R4, U3 - qucsator's readers

The converter is the program's for importing data (Touchstone, CITI, CSV, VCD, ZVR, MDL) and SPICE netlists
(the SPICE file part); the simulator reads Touchstone and CSV through the same code for `SPfile`, `Vfile`
and `Ifile`. Each reproducer is beside this file:

- **R1** `r1_vcd_upscope.vcd` (`$upscope $end` first: `vcd->currentscope` is NULL at
  `parse_vcd.ypp:137`) and `r1_vcd_dumpvars.vcd` (`$dumpvars` before any `#0`: `vcd->changesets` NULL at
  `:279`). Release: SIGSEGV. 20 of the converter's 26 Release events.
- **R2** `r2_spice_long_name.cir`: `B` with 300 `x` after it. `spice_untranslated_text` `sprintf`s the
  line's type and name into `char str[256]` and then each value into it too. Release: abort (stack guard).
  The fuzzer found the same in `citi_check` (`txt`, a variable's name with its package's) and in
  `netlist_instance` (`static char ret[256]`). Read for the same, two more: a `<Component %s>` line in a
  1,024-byte buffer, and a voltage source's name (`%sV%d`) allocated for one digit of its number.
- **R3** `r3_touchstone_hash.s1p` (`#\n`): its option line has no options, `touchstone_idents` is never
  made, and `touchstone_check` asks it its length. `r3_spfile.net` simulates an `SPfile` on it:
  qucsator_rf SIGSEGV.
- **R4** `r4_csv_nul_name.csv`: a quoted name whose first byte is NUL reaches `csv_validate_str` as `""`,
  which writes `_` over the terminator and reads on. `r4_vfile.net`: a `Vfile` on it, the same overflow in
  qucsator_rf (ASan; Release runs on).
- **U3** a ZVR file's points count `1e20`: `(int) $8` at `parse_zvr.ypp:129`.
- **U4** a node named with 40 letters: `hash_code` shifts its `int` left once a character, and past about
  25 the sign bit is set and the next shift is undefined.

**Fixed**, each with as small a change as the upstream code allows, marked "Qucs-S:":

- R1: `$upscope` with no scope open is passed over. A `$dumpvars` before any time stamp makes the change
  set at 0 that it belongs to.
- R2: every `sprintf` of a name into a fixed buffer is now `snprintf` to the buffer's size, so a name is
  cut at 255 characters (these are comments, instance names and variable names). The two allocations
  sized for one digit are sized for any number.
- R3: an option line with no options makes the empty list, which is the defaults (GHz, S, MA, 50 Ω), as
  the Touchstone format has it.
- R4: an empty name is left alone (and `isalpha`/`isalnum` take an `unsigned char`).
- U3: a points count that is not a number in an `int`'s range is 0 (the count is not used past the
  parser).
- U4: the hash is shifted as an `unsigned int`.

Tests: `test_qucsator_robustness`, which now runs the converter too (`QUCSCONV`): `aTouchstoneFileOfAHashAlone`
(the simulator's `SPfile` and the converter, and `#` with data converted as the defaults),
`aCsvColumnNamedNothing` (the converter and a `Vfile`), `vcdCommandsOutOfOrder`, `longNames` (a SPICE
comment, an instance, a library component of 1,100 characters, a CITI variable) and `undefinedBehaviour`
(ZVR, the hash).

## Checked and found right

- 9,651 schematics of the examples (Release; 600 more on ASan) with hostile part and simulation values -
  `0`, `nan`, `1e308`, empty, `x/0` in resistances, sources, sweeps' points and steps - simulated with
  qucsator or ngspice, their datasets read and displays opened: no crash, hang or report. Each simulator's
  failure came back as the run's error.
- 3D models: 602 on Release and ASan with hostile values, built, meshed, solved and closed. The first
  round's prism budget refused every tiny `hmax` of the extrusion, and the 3D document's 2 s close
  (the first round's H2) let every busy model close. The only 3D event was S2, in the plan mesh before
  the budget.
- RF 2.5D: 108 models with hostile stack, layout-map and solver values, solved quasi-statically and in
  full wave (and a hostile frequency now and then): none.
- Settings: 225 starts on ASan with hostile values for every key the sources read (fonts, colours,
  saved window state, recent files, numbers past range): each start answered and drew a schematic.
- The converter's MDL and Qucs-dataset readers (into CSV, Touchstone and MATLAB), and CITI apart from R2's
  buffer: no event in 151,240 ASan runs.

## Reproducing

```
QUCS=build-rel/qucs/qucs-s.app/Contents/MacOS/qucs-s PATH=build-rel/qucsator_rf/src:$PATH \
  python3 fuzz_solve.py SEEDS OUT 1 2300
QUCSCONV=build-asan/qucsator_rf/src/converter/qucsconv_rf python3 fuzz_conv.py SEEDS OUT 3 1500
QUCS=build-asan/... python3 fuzz_settings.py KEYS SEEDS OUT 1 1500
QUCS=build-rel/... QUCSATOR=build-asan/qucsator_rf/src/qucsator_rf \
  QUCSCONV=build-asan/qucsator_rf/src/converter/qucsconv_rf EXAMPLES=qucs-s-26.1.1/examples \
  python3 repro_all.py SCRATCH
```

`fuzz_solve.py`'s seeds are the example folders copied whole (each schematic with what it uses beside
it), qucsator on the PATH (the program finds it there). `KEYS` is the keys the sources read, one per line:

```
grep -rhoE '(settings|qs|s|Settings|set|conf|st)(\.|->)value\(\s*(QStringLiteral\()?"[^"]+"' --include='*.cpp' qucs-s-26.1.1/qucs \
  | sed -E 's/.*"([^"]+)"$/\1/' | sort -u
```

`repro_all.py` runs S1-S3 in a server each (S1's close and S2's mesh past 8 GB stopped and said) and R1-R4
in the converter and the simulator. On `c6ce80cc` every one reproduces: S1 "HANG at close_document"
(125 s), S2 "MEM past 8419 MB at fem_build" (17 s), S3 "EOF at fem_solve", and the seven files of R1-R4
end in signal 6 on the ASan converter and simulator (in Release: R1, R3 SIGSEGV; R2 the stack guard's
abort; R4 silent). With the fixes every one is answered. S1's close returns at once after its solve's 5 s
wait (5.2 s in all). S2 is refused in 0.2 s ("about 3.14e+07 nodes on its boundaries, more than 4000000"),
S3 in 0.1 s ("3.34e+15 wavelengths wide at 1e+30 Hz: about 6.67e+15 orders propagate, more than the 500
solved"). The converter and the simulator exit cleanly on ASan, R3's file with "checker error, no data in
touchstone file".
