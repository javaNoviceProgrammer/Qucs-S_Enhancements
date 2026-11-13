# Bug hunt: a stopped routing run kept lines too near each other

*12 November 2026 - Qucs-S on `claude/busy-nightingale-d92e81` (on `469ebb7b`), a Release build, macOS
27.2, Qt 6.12. Started from a full clean check of `main` at `65373813`: with about ten heavy suites
running at once, `test_layout_routing`'s `theToolsCoverEveryFeature` failed once on a finding that is
never meant to be there. It passed alone, and in 72 later runs on a lighter load. Found, fixed and
tested here.*

The failing check (`test_layout_routing.cpp`, the run past its timeout) sets up 24 nets on a 0.25 µm
grid, calls `route_run` with `timeout` 1, stops the run with `route_stop` if it is still going, and
asks that every finding of `route_report what=findings` be `unroutable`: nets not routed are allowed,
lines that break the rules are not. Under load, one finding was:

```
m17 and m18 are 1.75 µm apart on M1 at (-18.75, 31.875) (2 µm needed)
```

On an idle machine the run converges within its second, so the stop path (`route_stop`) hardly ever
runs. The router code had not changed since 8 November.

## Summary

| | Finding | Where | Fixed |
|---|---|---|---|
| **F1** | After a stop (or a stall, or the last pass), the clean-up routes each net still contested again around the others, and keeps it without testing what it drew. The search keeps a net's lattice cells clear of the others, but the tails it draws from a cell into its terminals are not cells, so a tail can land too near another net. The failing finding: m18's tail into its start, 1.75 µm from the M1 landing of m17's via | `router_engine.cpp`, `runAll`'s clean-up | yes: what a re-routed net drew is tested both ways, its cells against the others' metal and the nearby nets' cells against its own, and a net that would leave any pair too near is reported as not routed |
| **F2** | `conflicted()`, the negotiation's test of whether a net is too near another, was meant to test its vias' landings too, and never did: it looked for a via as two neighbouring path entries on the same cell, and the path is sorted plane first, so these entries are never next to each other. A landing too near another net's tail went unseen (stopped after batch 40: m14's M2 landing 1.75 µm from m3's tail) | `router_engine.cpp`, `conflicted` | yes: the landings are tested where the net drew its vias (`NetResult::vias`) |
| **F3** | A route kept from the last run is not routed again and is not in the run's order, so when a new net's tail came too near it, only the kept route was in conflict, and nothing gave way | `runAll` | yes, untested: after the negotiation a net near a kept route in conflict counts as contested itself |
| N1 | Testing landings as "the same cell on two metals" (as `stillClear` does) is too strict: a net's lines on both metals may cross one cell with no via there. Used in the negotiation, this made the test's whole run drop a net (23 of 24) that it routes cleanly | `stillClear` | F2 doesn't use it. `stillClear` still does, for routes kept from the last run, where it can only mean a route is routed again when it didn't need to be: left as it is |
| N2 | Two tails of different nets too near each other are seen by neither net's lattice test (neither is a cell). The exact check on the drawn shapes reports it. Not seen in this job, or at any of its 168 stop points | - | no |

## How it was found

`Progress` gets a `batch` callback, called from the routing thread after each batch of nets is drawn.
A test can set `cancel` there and so stop a run after any batch it chooses, with no timing involved.
The new `test_layout_router aRunStoppedAnywhereKeepsOnlyLegalRoutes` builds the failing job directly:
the same 24 nets, technology and chip shapes (the five 6 µm pads, the placed block, the etch obstacle),
with no app and no tools. It counts the batches of a whole run (168), then stops the run after each one.

Before the fix, the stop after batch 25 gives the clean check's finding exactly:

```
pass 1: 24 routed, 21 contested
pass 2: 24 routed, 21 contested (cut short)
clean-up: 13 routed, 11 not routed - m17 and m18 among those routed
```

m18's start is at (-20, 30) and its lattice cells end at (-19.75, 29.75), from where it draws a tail
along y = 30 into the start. m17 goes up M2 at x = -20 and changes to M1 through a via at
(-20, 34.75), whose 4 µm M1 landing reaches down to y = 32.75. The tail's top edge is at y = 31, which
leaves 1.75 µm. m17 was re-routed first, then m18 strictly around it: m18's cells keep clear, but its
tail is not one of them, and the clean-up tested nothing after the re-route.

With F1 fixed, the stop after batch 40 failed on m3 and m14, 1.75 µm apart on M2. m14's via at
(-24.75, -44.5) has an M2 landing whose right edge is at x = -22.75, and m3's tail into its start
(-20, -45) has its left edge at x = -21. `conflicted(m14)` should have found the landing too near,
and it has a test for that, but the test never runs (F2).

The first fix for F2 tested a landing wherever the path has the same cell on two metals, as
`stillClear` does. The whole run then converged at pass 7 and then dropped one net: m21's M1 stub and
its M2 line share three cells at (59.25, -44.5 to -44) with no via there, and a landing at those cells
would have been too near m4 (N1). With the landings tested at the vias as drawn, the whole run is as it
was before: 7 passes, converged, 24 of 24 routed, 168 batches.

## The fix

- `conflicted(n)`: a net's cells against the other nets' stamps, as before, and then each of its vias'
  landings (the lower and upper landing types at the via's cell) against them too.
- After the negotiation, once the best pass is back (or the pass a stop cut short is kept), every
  routed net is tested again. A net near a kept route that is in conflict counts as contested too (F3).
- The clean-up: each contested net is routed again strictly around the others, as before. Its result
  is kept only if `conflicted` passes for the net and for every routed net whose metal comes within
  reach of its own. Otherwise it is taken out again and reported as too crowded, with where it was stuck.

This doesn't drop routes after the fact using the exact check: what is kept is what the lattice tests
pass, the same rule the negotiation goes by, now applied after the clean-up's re-routes as well.

## Tests

- `test_layout_router aRunStoppedAnywhereKeepsOnlyLegalRoutes` (new). The whole run must be clean with
  all 24 nets routed. Then it stops after every third batch of the first two passes (1, 4, ... 46, which
  includes batches 25 and 40), and after each stop every finding must be `unroutable` and every net
  either routed or reported. `QUCS_ROUTER_STOPS="1-168"` stops after each batch of a range instead.
  QtTest's 300 s watchdog allows about 40 to 100 stops in one call, so the full sweep runs as four ranges.
- `test_layout_routing theToolsCoverEveryFeature` is unchanged: what it asks for is now true whichever
  path its run takes.

## Results

| Check | Result |
|---|---|
| Every stop point, `QUCS_ROUTER_STOPS` 1-40, 41-80, 81-120, 121-168 | all 168 legal (106, 50, 146 and 106 s) |
| `test_layout_router` (37 cases, the new one about 15 s) and `test_layout_routing`, through ctest | pass |
| `test_pen_reading` (the other suite that runs the route tools) | pass |
| [`breaks.py`](2026-11-12-router-stopped-run/breaks.py) (through ctest): F1, F2 and N1 undone one at a time | 3 of 3 caught ([results](2026-11-12-router-stopped-run/results.txt)): F1 at batch 25 with the original finding, F2 at batch 40, N1 by the whole run dropping m21 |
| The 200-net chip (`QUCS_ROUTER_BENCH="10/4 random"`), before and after | the same: 3 passes, converged, 200 of 200, 524 vias, 992.8 mm, 445,719,401 states searched; 42.4 s before, 42.8 s after |
| The four lints (`scripts/ci/check-*.py`) | pass |

F3 has no test. Making it happen needs a route kept from the last run that a new net's tail comes too
near, and this job never gives one.
