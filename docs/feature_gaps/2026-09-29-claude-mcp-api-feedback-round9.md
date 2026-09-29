# Feature gaps: Qucs-S's MCP tools, the ninth round of feedback

*29 September 2026 — Qucs-S 26.1.4.*

Claude (Fable 5.1) reviewed `994c4ec` in the evening
(`qucs-mcp-api-feedback-round9.md`). Every item of round 8 checked out:
the Rg placement, the quiet negative-supply note, the bench numbers with
units, the 741 bench as scenario s8 (11 of 11) and the guide in the
instructions. Its own seven scenarios passed, 49 of 49. It found nothing
of substance and one cosmetic: on the arranged bench, "the VEE wire from
the left column runs just under Vin's ground symbol; it is clear of it,
but a grid step lower would read better".

## The finding

| Feedback | Now |
|---|---|
| **The VEE wire runs just under a ground symbol.** | Reproduced on the bench as s8 builds it. The symbol is VCC's ground, not Vin's. The two layouts differ. **Without `straighten`**, VCC's ground is right under VCC, and the VEE wire leaves VEE's + pin (facing up) and turns right at y = 220, 15 below the ground's lowest bar. A grid step lower is y = 230, where Vin's + pin and the + input's wire are: to pass Vin the wire must run at 220 or above, or go round below Vin and its ground with two more bends. So this row is the only one, and it is left as it is. **With `straighten`**, as the reviewer ran it, the layout was worse, and there was a fault behind it. `straighten` moved VCC 30 down to put its + pin level with the op-amp's VCC pin. Both pins face up, so the wire went over the top all the same. VCC's ground no longer fitted under VCC (VEE was there), so it went onto a jog to the right, and the VEE wire crossed that jog's stub: a crossing the check notes. |
| (the fault) **`straighten` took two pins as lined up when they merely shared a row or a column**, whichever way they faced. | Two pins count as lined up only when a straight wire can join them: they share a row and each leaves its symbol toward the other, or the same for a column. A pair whose pins cannot face each other is not tried. A pin's direction is its stub's, the line of the symbol that ends at it, from its other end to the pin. When no such line is straight, it falls back to the nearest side of the symbol's box, as the router does. The stub is needed for a coupled line: its pins sit at the corners of its box and would read as facing sideways, but its stubs run up and down. On the bench `straighten` now nudges nothing, VCC's ground stays under VCC, and the crossing is gone. |

## A correction

The assessment's write-up said of `straighten`: "On every shipped example,
as a preview: none failed, every net was kept, and 137 of 251 were
straightened." That was `keep_places`, a tidy of each drawing as it is,
and most of those nudges were of this false kind. Run again on the 252
examples by HEAD's `arrange` (`82bb631`, as `994c4ec`'s) and this one, built alike. The
installed app is not a fair baseline: the build tree has no
`share/qucs-s/symbols`, so a SPICE library part (`SpLib`) has another
box there, and the same code draws Lorenz with 9 crossings in the build
tree and 6 in the app. Each file is arranged with `straighten`, then
checked.

| | Before | Now |
|---|---|---|
| Examples arranged, every net kept | 246 of 246 | 246 of 246 |
| `arrange`: examples with a part nudged | 80 (104 parts) | 8 (8 parts) |
| `arrange`: crossings without a junction | 939 | 933 |
| `keep_places`: examples with a part nudged | 137 (223 parts) | 4 (6 parts) |
| `keep_places`: crossings without a junction | 159 | 161 |

After a full `arrange`, 10 examples have fewer crossings: the op-amp
templates, the charge pumps, `testCombLogic2` and `BPF_2000_Co-ax`. 3
have more: `singleOPV.sch` (both copies, +1 each) and
`BPF_1550_edge_cpld.sch` (+3). As a tidy, `pentode.sch` has 2 more. In
those, the old nudge of pins that do not face each other happened to
give the router a better start. The pentode's port S moved 40 down,
level with pins that face up, so its wire bent all the same. Now the
drawing's parts stay where they were drawn, unless a nudge makes a wire
straight. The eight nudges left after a full `arrange` each line up two
pins that face each other: R1 of `Amp_Two_Tone`, L4 of the two baluns,
P3 of `genericopa_sub`, C4 of `testTwoStageBJT`, and an op-amp in each
Bessel filter.

## Tests

- **`test_qucs_control`:**
  - `straightenLinesUpOnlyPinsThatFace`: two standing resistors whose top pins are joined over the top, 20 apart: nothing is nudged. A coupled line's corner pin (its stub going up) with a standing resistor 10 to its side, facing down onto it: the resistor or the line is nudged so the two pins share a column. And the reviewer's bench arranged with `feedback: below` and `straighten`: VCC is not nudged, its ground stands at its pin, and no crossing involves the ground net.
  - `straightenLinesUpTheWires` (the assessment's) passes unchanged.
- **`scripts/mcp-e2e-scenarios.py`:** s1 to s8, 60 checks, none failed.

Each change was broken on purpose. A pin's direction by the box alone,
and a direction read backwards, are each caught. The two guards,
`aligned()` and the search's skip of pairs that cannot face, cover for
each other: each broken alone changes nothing, since the other still
refuses the pair. Broken together they are the old behaviour, and it is
caught.

The full suite passes (78). `~/QucsWorkspace` and `~/Library/Caches/qucs-s`
are as they were before the runs; one project in the workspace,
`opamp741_prj`, was simulated during the work, at 19:34:57, by a session
that was not these runs (an AC and a transient analysis of
`ua741_noninv.sch`; none of these runs simulate that project).

The tools sent every turn are as they were; the change is inside `arrange`.
