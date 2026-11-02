# RF 2.5D, 2 November: the Green's functions kept between solves

The user's "Do this task here", on the follow-up G9's write-up left (`2026-11-02-rf25-port-paths.md`, "Not done"):
every full-wave solve built its stack's Green's function tables afresh at each frequency, and since a port's path up
from the ground has a field those tables were twice what they had been - under the sanitizers most of a solve.

## What was built again and again

`Kernels::Kernels` builds, at every frequency of every solve, a `WaveGreen` for each pair of metal heights and a
`ViaGreen` for each vertical span, each tabulating Sommerfeld integrals over distance out to the layout's own extent.
Through two dielectrics that was some 38 s a frequency under the sanitizers. And the same tables were built over and
over:

- **A calibration** solves three strips of a port's feed, and shorter ones beside them where they disagree, all at the
  same frequencies on the same stack.
- **A two-line check** solves two lines.
- **The tab, and the tool,** solve a model again after a port's reference or a sweep's end changes.

Each of these differs from the others only in how far its tables reach.

## One grid for every extent

A table's distances began at the least of a thousandth of the farthest distance it was wanted to, a thousandth of
the wavelength over 2 pi, and a hundredth of the nearest image's distance. The first made every layout's tables a
grid of their own, and nothing could be shared.

They begin now at the least of the other two alone, which are the scales the remainder changes over. Below the first
distance it is read straight in rho², and a function of sqrt(rho² + d²) is within (rho/d)^4 of that: a part in 1e8
at a hundredth of d. From there on the grid is the same sequence however far it goes, so a table kept to a further
distance holds a nearer one's nodes exactly. A kernel is handed the nodes its own table would have had, and no more
of them. What it reads is the same to the last digit whatever was solved before it.

The grid's new start moved nothing a solve gives that could be seen. On the reviewer's four models through
`rf25_solve`, against 9603502's DMG (f717c20's solver, with the viewers' pens), Z at the top frequency is the same to
the last digit, and S21 agrees within 6e-12 dB over every sweep. A realistic stack's grid began at its image or its
wavelength already, and a thousandth of the farthest distance decided only for small layouts at low frequencies.

## The tables kept

`tableOf()` in `rf25_wave.cpp` keeps each kernel's table, and `prepare()` takes it from there.

- **What a table is kept by.** The stack (each slab's heights, eps_r, loss tangent and conductivity, the half-spaces,
  the ground), the frequency, and the grid's start and step. Then the two heights for a `WaveGreen`; for a
  `ViaGreen`, its own parts, those of the vias it couples to, and the metal heights.
- **Growing.** A table wanted further than it is kept is grown by its further distances alone, under that kernel's
  own lock. A second solve of the same kernel waits for the first rather than does it again. Different frequencies
  never wait on each other.
- **The budget.** 256 MB unless set, and the least lately used is forgotten past it.
- **The interface.** `keepGreens(bytes)` sets the budget, and nought keeps nothing, every table built as it is
  wanted. `forgetGreens()` forgets everything; `greensKept()` says what is kept and how many distances were built
  and reused.

## A race the kept tables laid bare

The full ctest, six suites at a time, crashed the full-wave suite's second part once: the allocator found its free
list corrupted, at an allocation of no interest. So I loaded the machine the same way, four copies of part 2 at once
for three rounds, with the allocator's guards on:

- **In Release,** two of the twelve runs crashed the same way, and in a third a compressed line came out with eps_eff
  16.03 for 6.57.
- **Under the sanitizers,** a compressed tee's S was 5.2e-5 off the dense solve's, where it is 2e-14, and nothing was
  reported at all.

A kernel's tables are `mutable`, and `WaveGreen::fromTable()` and `ViaGreen::fromTable()`, both `const`, called
`begin()`, `end()` and `last()` on them. Those are the non-const overloads, which detach a list that is shared. A
compressed solve's threads read one kernel at once as they fill their blocks. While each kernel's lists were its own,
detaching did nothing; once the store handed each kernel a list it still shared, the readers detached it under one
another.

They read through const views now, and the store hands out copies of its own; either alone would do. Twelve runs
under the same load came back clean, and four more under the sanitizers.

## What it gives

In Release, each suite run alone:

| Suite | Before | Now |
|---|---|---|
| test_rf25_fullwave | 96.9 s | 50.0 s |
| test_rf25_deembed | 29.6 s | 9.3 s |
| test_rf25_doc | 15.5 s | 10.7 s |
| test_rf25_wave | 3.7 s | 3.0 s |

The reviewer's models through `rf25_solve`, against 9603502's DMG:

| Model | Before | Now |
|---|---|---|
| MOM, calibrated: 60 frequencies, three strips and the shorter ones | 91.0 s | 23.6 s |
| GCPW, calibrated | 116.6 s | 105.4 s |
| Microstrip, 1500 µm, calibrated | 2.5 s | 1.0 s |
| MOM, raw | 14.5 s | 14.6 s |

A raw solve has nothing to share between its frequencies; a calibration's strips share everything.

Under the sanitizers, as CI's Linux job builds it, each through ctest with its limits:

| Suite | Before | Now |
|---|---|---|
| test_rf25_fullwave, part 1 and part 2 | 562 s and 603 s | 293 s and 403 s |
| test_rf25_deembed | 731 s (396 before the ports' paths) | 314 s |
| test_rf25_doc | 273 s | 145 s |
| test_rf25_wave | | 127 s, its two new functions in it |

The de-embedding's suite gains the most: a calibration's strips, and the shorter ones beside them, now build one set
of tables between them.

## Tested

| Test | What it holds to |
|---|---|
| `aKeptTableIsTheOneItsKernelWouldMake` (`test_rf25_wave`, new) | A wave kernel kept to 10 mm, then one wanted to 2 mm: handed its first part, nothing built, its own table bit for bit; one kept to 2 mm grown to 10 mm by the further distances alone, the same as its own; keeping nothing keeps nothing; another frequency or height its own; a via's the same, and one coupling to two vias a table of its own; room for two of three, the least lately used forgotten |
| `aSolveReadsTheSameWhateverWasSolvedBefore` (`test_rf25_fullwave`, new) | A line solved keeping nothing, and after one twice as long kept its tables: Z the same to the last digit, the second solve building nothing, and again |
| `aKernelReadFromManyThreadsReadsAsFromOne` (`test_rf25_wave`, new) | One kernel read from eight threads at once, 100 rounds, its lists shared with a copy of it afresh each round: bit for bit what one thread reads |

## Broken on purpose

`2026-11-02-rf25-greens-kept/breaks.py` holds ten:

- **The grid:** a wave kernel's from how far it is wanted again.
- **What a table is kept by:** the stack alone, not the frequency or the grid; a wave kernel without its heights; a
  via's without the vias it couples to.
- **What is handed over:** the whole kept table rather than the kernel's own part; kept though nothing is to be.
- **The counts:** what was built and reused not counted.
- **The budget:** past it nothing forgotten; past it the most lately used forgotten.
- **Reading from many threads:** a wave kernel read through its mutable lists, which a reader detaches. Caught three
  times in three: the race is a matter of chance, and a hundred rounds of eight threads give it plenty.

All ten were caught, after the tests were made to hold three of them:

- **The old grid rule** changed nothing for the cases first chosen. The 3 µm slab's image set the grid's start in
  both, and on the solve test's alumina at 10 and 20 GHz the wavelength did. The kernel test went to 1 GHz on
  254 µm, where a thousandth of 2 mm and of 10 mm would have begun its grids at 2 and 5 µm; the solve test went to
  1 and 2 GHz.
- **A via's key without the vias it couples to** still counted them, and the two cases differed in that count. A
  via coupling to another of the same count, and different spans, was added.
- **Two heights as far from the slab's faces** did not collide without the heights in the key. 254 less 204 µm is
  not 50 to the last digit, so their grids differed there. They are now two heights either side of the 3 µm slab,
  100 and 103 µm, whose distance to the nearest face is one subtraction both ways.

Rebuilt after the breaks: the full ctest (152) twice, six at a time as where it crashed, and the four lints green.

## Not done

- **The full-wave suite's split.** Its two parts take 293 and 403 s sanitized now, 696 together, and one entry
  would fit its 900 s again, about half of 900 times 3 at CI's rate. They stay two entries until the user says
  otherwise.
- **A stack's tables across processes.** They are kept for the program's run, not on disk. The tables of a model
  reopened tomorrow are built again.
- **The quasi-static Green** reads its mutable table the same way (`Green::reflectedByTable()` in `rf25_green.cpp`).
  That is harmless while one thread reads it, as one does now.
