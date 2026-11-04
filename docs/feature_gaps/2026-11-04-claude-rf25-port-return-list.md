# RF 2.5D: a port's returns given as a list - GSG probe landings, ground-signal, ground rings

The reviewer's feature request of 4 November (`~/Desktop/qucs-reviews/RF2p5D/2026-11-04-claude-rf25-feature-port-return-list.md`,
priority high: "an important feature we should implement"): a port that returns through other ports or terminals, as
EMX lets a signal port name a list of returns. Until now a port returned through the ground plane, the faces beside it
on its edge's line, or the metal under it; "P1 = the signal pad's tip, returning through the two ground pads' tips"
needed a second metal, a bar and four vias per probe, whose own L and C were in the answer and which stalled the
compressed solve.

| Asked | Now |
|---|---|
| **A terminal node, `rf_terminal`** under Ports: `metal` (or empty for whichever is there), `x`, `y`, `width`, `height` (0 x 0 the cell under the point), or `face` +-x/+-y; it drives nothing and is no port of the S matrix | **Done.** A Terminal (T1, T2...) is the cells of a metal under a rectangle about its point - each by its share of the area, the one cell under a point - or, with **Of** `auto`, `+x`, `-x`, `+y`, `-y`, the face of the conductor at its point, each cell by its share of the face's length, as an edge port's face. Width and height shown only for a rectangle. The tab draws it by its name; it is in no S matrix |
| **Every port gains `return: terminals`** with `returns`, a list of names - terminals, or other ports | **Done for both kinds.** An edge port's Return has a third choice, "Through the terminals and ports it names (a probe's tips)"; an internal port gains a Return of its own, `from` (its From metal or the ground, as before, the default) or `terminals` - its From hidden then. **Returns** is a list of texts in the file (`["T1", "T2"]`); a text of names apart by commas or spaces is read too, as a file written by hand gives it |
| All the listed returns tied: one voltage, the current shared by the solve; on another metal, joined by vias or not | **Done**: each return is a column of its own, the port's cells less the return's, the columns of a port side by side as a coplanar port's two grounds are; a return's metal is its own |
| The positive side: an internal port's rectangle on `to`, an edge port's face | **Done**, both as before; on a metal solved as its two faces the port's cells and its returns' are both faces' half each (`put()`), as an edge port's face - with no path up from below, nothing makes one face its own |
| **Refusals with their names**: a return on the port's own net; one that is another port's positive side unless intended; an empty list; a terminal over no cell | **Done**, each by name: "P1's return T2 is on the same conductor as P1 (Au): it would short the source"; "P1's return TB is on the cells P2 drives: name P2 itself among P1's returns, if P1 is to return through what P2 drives (the two in series), or put TB elsewhere"; "...names none"; "P1's return T3, a 20 by 20 µm rectangle at 40, 900 µm, is over no cell of M1"; a point terminal on no shape of its metal (the nearest cell would otherwise have been taken without a word), a face terminal on no face, a port named that is not there, the port itself, a name twice |
| **Another port's positive side as a return** (EMX allows it), the pairwise check becoming a rank check - "suggest phase 2" | **Done now**: a port named among another's returns is in series with it, its cells that one's return and it a port of its own. The pairwise "drive the same face or cell" check stays; beside it, a combination of the columns that comes to nothing **and that is a current into a port** is refused, naming the ports ("P1, P2 drive no cell between them: ..."): two ports that return through each other, three round a ring. One that is no port's current - round the ties of two ports returning through the same two conductors, a GSSG's two signals - is let through, as the reduction has always taken it (refusing it too broke the GSSG tests: break 6) |
| Solver: `own` as now, a column per return with `put()`, `portOf` per column, no `PortPath` | **Done**: an ideal source between the cells, with no path up to them; columns made once every port's own cells are known, so that a port may name one after it |
| No ground plane: a port whose returns are all terminals returns through the layout | **Done**: the plane check passes them; over a plane they solve as they are, the plane no return of theirs |
| Feeds and calibration refused at first, with the reason | **Done**: `calibrate` (or a move along the feed) refused by name - a source between its cells and its returns' is not what strips of a feed are; moving its plane along a given line is allowed |
| Quasi-static unchanged; terminals are not nets | **Unchanged** |
| **Reports**: `portsSaid`, the Touchstone comment and `rf25_solve` per port, the cells driven and against which terminals ("..., tied: a GSG probe's"); each return's share at the top frequency | **Done.** "P1 drives the 6 cell(s) of Au under its 20 by 20 µm rectangle at 30, 0 µm against T1 (2 cell(s)) and T2 (2 cell(s)), tied: a ground-signal-ground probe's." The solve's lines and the Touchstone add "At 60 GHz P1's current comes back 50.0 % through T1, 50.0 % through T2." (a phase where it is a twentieth of a degree or more); the Touchstone's reference planes say "P1's at the cells it drives and those it returns through, a source between them: the probe's own loop not in it"; `rf25_solve` gives each port "returns through", "returns" (name, cells, share, phase degrees) and "shares at" - a coplanar port's two grounds too. Kept in the `.solution` (format 2; a format 1 file still reads) |
| `show_layout` draws the terminals and a line from each port to its returns | **Done**: the tab draws each terminal (its rectangle, or its face) by name and a thin line from each port to each of its returns; `show_layout`'s answer now lists what is drawn over the layout (`overlays`: each by name, its shapes and labels) |
| The Internal Port tool: the signal's rectangle, then Shift-drag the returns; the panel's Return list offers terminals and ports by name | **Done**: a Shift-drag (or Shift-click) with the tool puts a terminal on the topmost metal under it among the returns of the port put last, one step of Undo; the panel's **Returns** is a line of names with a menu of the model's terminals and other ports ("P2 (a port, in series)"), each taken in or out. A renamed terminal or port is renamed in the returns (and pairs) naming it |
| MCP: the ports with `return: terminals`/`returns`, `rf_terminal` in `fem_describe`-style format help | **Done**: `fem_describe` with `what` format and `space` "RF 2.5D" - or a .qrf25 named or in front - gives the RF 2.5D kinds in its line format (the terminal and both ports' Return and Returns among them) and a paragraph of the file. `rf25_solve` and `show_layout`'s descriptions say so (2039 and 2038 of 2048 characters) |

## The acceptance tests

On copies of the reviewer's models (`rf25_zotero_benchmarks/gsg_l2l`): CPW S 50 / G 20 µm, 2 µm Au sheet, 500 µm quartz
with nothing below, 80 µm pads, 80 µm tapers, lines of 0.5 and 1 mm; 2 to 60 GHz, 30 points. Every model below was
solved through this build's `--mcp-server` (`rf25_solve`, `rf25_export`; `make_models.py`, `run.py`, `compare.py` and
`bridges.py` beside this file, the reviewer's `l2l_extract.py` and `twoline.py` copied beside them; `results.txt` has
every answer's lines). The edge-port reference is the reviewer's own models, solved again here: today's meshes grade
the narrow gaps (47798db), so the reviewer's numbers of the morning (2.451, 77.6 Ω at 10 GHz) are of another mesh.

1. **The same as the coplanar edge port where the terminals are its faces** (`test_rf25_returns`, not `test_rf25_wave`,
   which is of the Green's functions): edge ports returning through face terminals on the grounds at their edge's line
   against the same ports returning beside - **S apart by 0 on a sheet, 5.7e-16 as two faces**, Z by 8.9e-22 and
   1.5e-15 of its largest; the grounds' cells alike, each return a half of the current.
2. **A GSG landing mid-pad, L-2L** - a 20 x 20 µm patch at each signal pad's centre against 20 x 20 µm terminals at
   the ground pads' centres: **eps_eff within 0.034 % of the edge port's over the whole sweep** (asked: 0.2 %), **Z0
   (the mean of the two pad-box orders) within 0.5 % to 58 GHz** (0.485 %), 0.512 % at 60 (asked: 0.5 %):

   | GHz | edge eps_eff | Z0 (Ω) | tips eps_eff | Z0 (Ω) | d eps_eff | d Z0 |
   |---|---|---|---|---|---|---|
   | 10 | 2.4699 | 76.11 | 2.4691 | 76.11 | -0.032 % | +0.002 % |
   | 30 | 2.4483 | 76.14 | 2.4475 | 76.25 | -0.034 % | +0.137 % |
   | 60 | 2.4272 | 77.35 | 2.4273 | 77.75 | +0.007 % | +0.512 % |

   Z0's difference grows as f², the pad box's second order: at 60 GHz the two orders themselves disagree by ±8.2 %
   (edge) and ±6.9 % (tips) about their mean, where eps_eff - from T_2L T_L⁻¹, any reciprocal box cancelled exactly - is
   the same line to 0.03 %. The solves: 3 and 8 s, dense, passive and reciprocal; the tips 50.00 / 50.00 %.
3. **Raw S against the bridge workaround** at bridge heights 20, 10 and 5 µm: at 60 GHz the 1 mm structure's |S11|
   is -14.18, -11.90 and -10.30 dB, the terminal port's -9.66; |S21| -0.38, -0.50 and -0.64 dB, the terminal's -0.68
   - each nearer as the bridge comes down (at 10 µm the reviewer had -11.4 dB, on the earlier mesh). The complex S
   does not close up, though: some 0.03 apart at 10 GHz at every height, 0.17 to 0.19 at 60. The bar over the signal
   pad is a capacitance across the port that grows as 1/h as its loop's inductance falls (20 x 80 µm at 5 µm, some
   3 fF): the bridge never becomes the ideal source - its own parasitics, as the request says, both ways.
4. **Current share**: the symmetric GSG 50.00 / 50.00 % (0.00°) at both ends of both lines; the lower tip moved off its
   pad's centre to |y| = 180 µm: 50.14 / 49.86 % on the 0.5 mm line, 48.46 / 51.54 % (±0.25°) on the 1 mm, S changed
   (L-2L eps_eff still within 0.042 %); one tip dropped, a ground-signal probe: "a ground-signal probe's", T1 100 %, S
   changed. All said in the answer and the Touchstone. (`test_rf25_returns` holds the same on a lossless line: 51.93 /
   48.07 % with a tip at its ground's far edge.)
5. **No plane, a terminal on another metal**: a patch on M2 against a terminal on M1 under it, no via, is today's
   internal port from M1 without its path - **the same S, to 0** (a test switch, `FullWaveControl::portPaths`, leaves
   that port's path out). With its path the port is the path's reactance more: +0.016j Ω at 10 GHz, +0.048j at 30,
   up the 5 µm between the metals.
6. **Refusals**, by name, in the solver, the tab and through `rf25_solve`: a return on the port's own conductor, an
   empty list, a terminal over no cell (and a point on no shape, a face on none), `calibrate`, a port not there or
   itself, a name twice, a terminal on another port's cells, ports round a loop.
7. **The compressed solve**: the test 2 structure at 6 cells across held compressed - **GMRES converging at every
   frequency to 60 GHz, 47 and 51 iterations at the most, residual under 1e-10**, passive and reciprocal, 5 and 10 s.
   Its L-2L against the edge port's at 6 cells across: eps_eff within 0.078 %, Z0 within 0.5 % to 58 GHz (0.493 %),
   0.522 % at 60. (The bridge models at 3 cells across: the 1 mm ones past the dense limit, compressed, 142 to 287
   iterations.)

## What the ideal source leaves: a port with its returns on one side, and no ground plane

A port that names its returns is a source between its cells and theirs, as the request asks: no path up to them, so
that the probe's own loop is not in the answer. The current a probe carries from its returns to its tip then has no
field here - G9's lesson again, horizontal this time: the scalar potential alone takes a current's radiation the wrong
way, -R/2 of it. Over a ground plane the current's image takes that radiation away, and nothing of it shows (every
port kind below gave back nothing to 60 GHz, lossless). With no plane it can: on the lossless 600 µm test line of
`test_rf25_returns` (S 50 / G 25 µm, 500 µm of 3.8, nothing below),

| S gives back, lossless, no plane | 20 GHz | 40 GHz | 60 GHz |
|---|---|---|---|
| coplanar edge ports (as before) | 1e-8 | 1.1e-6 | 1.5e-5 |
| GSG tips 125 µm either side | 1.6e-8 | 1.7e-6 | 2.3e-5 |
| GSG tips 60 µm either side | 8.9e-9 | 1.0e-6 | 1.6e-5 |
| ground-signal tips 125 µm away, both ends | 4.5e-5 | 9.1e-4 | 4.7e-3 |

So a GSG's two returns, alike either side, leave what the coplanar port's own gap current always has - it has no field
either - and the gold and the quartz's loss covers it: the request's structures are passive at both meshes. A
one-sided landing's dipole does not cancel, and is not covered: on the request's own gold and quartz the 0.5 mm line's
ground-signal probes give back 5.3e-3 at 60 GHz. Such a solve now says so, with the cause and the remedy, among its
warnings: "At 60 GHz S gives back 0.0053 more than it is given: with no ground plane, P1, P2 - returning through the
terminals and ports named, an ideal source between its cells and theirs - has none of the field its own current, from
its returns to its cells as a probe carries it, would radiate; most where its returns lie on one side of it, little
where they lie alike either side. Returns either side, nearer, or a ground plane under the stack make it less."

The cure, left for the user to choose: give each column's source current - from its returns' cells to its own, the
dipole the probe carries - the radiating part of its field alone (the vector kernel less its static 1/R part, the
`smooth` term `Kernels::inductance` already has), so that the loop's inductance stays out and its radiation is in: the
static part is a lossless reactance, so taking it out keeps reciprocity and passivity. It would change the coplanar
port's answers by some (k gap)² too, and must, or the first acceptance test's equality goes. Or the request's own later
`path: probe`, the tip loop as filaments at a given height.

## What changed

- **rf25_fullwave**: `PortTerminal`, `FullWavePort::terminals`/`returns`; the ports' columns made in two passes, each
  port's own cells first (`owns`), then a column per named return, `put()` throughout; the refusals; the loop check
  over the columns' Gram matrix, only of combinations that are a port's current; `FullWave::returnsOf`
  (`PortReturn`: name, cells, share) and `namesReturns`, shares from the reduction's own `x z` at the top frequency, a
  coplanar port's too; `returnSharesOf()`; the no-plane warning; `FullWaveControl::portPaths` (tests).
- **rf25_model**: `rf_terminal`; the Return choices and **Returns** (a list of names, `PropertyDef::Objects`) on both
  ports, an internal port's From shown only for `from`; T1, T2... named; `namesOf()`, `portName()`,
  `Model::renamePort()`; `problems()` checks every return's name, a terminal's metal, a name a terminal and a port
  share; `renameLayer()` renames a terminal's metal.
- **rf25doc**: `portsOf()` reads the returns, each terminal or port by name; the terminals and the lines to the
  returns drawn; `placeTerminal()` and the tool's Shift (`LayoutView::pickModifiers()`); the Touchstone's plane and
  shares. **rf25panel**: the Returns editor and its menu; a renamed port or terminal followed; `metal` picked from the
  stack. **rf25_kept**: format 2, the returns kept.
- **qucscontrol_rf25 / _fem / _layout / qucscontrol**: `rf25_solve`'s returns; `fem_describe`'s RF 2.5D format
  (`propertyLine()` now shared by both formats); `show_layout`'s `overlays`; the descriptions.

## Tests

- **test_rf25_returns** (new: 7 functions, 0.4 s in Release, 43 s under ASan and UBSan with Eigen's LU): the
  coplanar equivalence on a sheet and two faces; a probe's tips sharing its current, moved, dropped, the shares at a
  sweep's top in either order, the no-plane warning and where it is not; the series pair's Z the transformed pair's
  to 3.6e-15; a GSSG's two signals against the same two tips at each end, paired - the loop round their ties let
  through, reciprocal, passive, no mode turned into the other (beyond a GSG, as the request lists); the other metal;
  the refusals; compressed against dense (1.1e-8).
- **test_rf25_model** `aPortsReturnsAreTheTerminalsAndPortsItNames`; **test_rf25_doc**
  `aPortReturnsThroughTheTerminalsItNames` (the file both ways, the drawing, the solve, the Touchstone, the kept
  solution, the rename, the Returns menu, the Shift-drag and its Undo, a refusal through the tab);
  **test_qucs_control** `anRf25PortsReturnsAreNamedThroughTheTools` (`rf25_solve`, brief too, `show_layout`'s
  overlays, a refusal, `fem_describe`'s RF 2.5D format); `theSolutionIsKeptBesideTheModel`'s "later build" is format 3.
- Every rf25 suite in Release; under ASan and UBSan with Eigen's LU the changed suites (test_rf25_doc 65 s,
  test_rf25_model 4 s, the two tool functions 15 s), test_rf25_fullwave's two parts (103 and 100 s) and
  test_rf25_deembed (199 s), no report; **28 breaks, all caught** (`breaks.py` beside this file) - the first run
  missed one, the shares taken at a sweep's first frequency, because the test's sweep listed its top first: it now
  solves both orders; the four CI lints; the full ctest, 162 of 162 (323 s).

## Left

- The no-plane radiation of a one-sided landing (above): warned, not cured.
- A return on the port's own conductor is refused, as asked. A full-wave port across one conductor - an inductor
  between its two ends, a shorted stub through a GSG's tips - is physically a port (the conductor's own impedance),
  and lifting the refusal would allow it; the coplanar port has always passed over its own net too.
- The tip loop as a path (`path: probe`), the request's own later option.
