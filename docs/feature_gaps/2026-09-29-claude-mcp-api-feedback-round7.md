# Feature gaps: Qucs-S's MCP tools, the seventh round of feedback

*29 September 2026 — Qucs-S 26.1.4.*

Claude (Fable 5.1) reviewed `e5978a4` in the afternoon
(`qucs-mcp-api-feedback-round7.md`). It drove the tools headless, and
read the three commit messages and the round's write-up. Every item of
rounds 5 and 6 checked out, and the end-to-end scenarios passed, 49 of
49. It reported nothing silent: three things by eye and one by wiring,
plus two small ones. Each was reproduced before it was changed. On a
741 bench built as the report describes, the drawing was worse than it
said, and the cause was in the router, not in `arrange` alone. The
tables say what was done.

## Findings

| Feedback | Now |
|---|---|
| **2.1 `arrange feedback: below` leaves Rg in the load column**, and the feedback node's wire runs the width of the schematic. | A two-pin part from the feedback part's input end to ground now goes with the feedback network. The input end is the end on the net of the host's pin that is not the outermost, which is the op-amp's inverting input, not its output. Rg of a non-inverting amplifier is such a part. It stands under Rf's input end, just under Rf's texts, with its ground below. With `feedback: above` it stands under the op-amp's input pin. The column's other parts move down to make room, the ground symbol's room included. The bench's columns are now `[Vin] [U1, Rf, Rg] [RL]`, and RL lines up with the output. |
| **2.1 The label goes on Rf's wire, over the drawing**; it should go on the longest free segment of its net. | After the wires are drawn, each label `arrange` put on a pin moves to the longest stretch of its piece's wiring. A straight horizontal stretch is tried first. The label goes at the stretch's middle, with its text above it to the right, below it, or above it to the left, wherever the text keeps clear of the parts, their texts, the wires and the other labels. A label with no such stretch stays at its pin. The supplies' labels stay at each of their pins (`supplies: labels`). The nets are compared once more afterwards. On the bench, `out` sits above the long wire to RL. |
| **2.2 A wire drawn through a ground symbol.** With the feedback below and the supplies in a column, the VEE wire passed through Vin's ground symbol's stem. | On the bench it was worse than that. Rf's wire went up through the op-amp to reach INN, and INN's went across the op-amp's body. The cause was the router's strict pass, which is clear of every part's symbol. It could not leave most pins at all. A library part's box is four points wider than its pins (`LibComp` enlarges it), and an op-amp's supply pins lie inside its box. So every clear way was refused at its first step, and the fallback pass, which may go over anything, drew the wire. Three changes. **A pin's way out:** in the strict pass, a piece that leaves a pin (or reaches one) may cross that pin's own part's box, when it goes straight out, toward the box's nearest side, as the pin's stub goes. **A way a step at a time:** when none of the few shapes the router tries (two- and three-step bends, and U-turns further and further out) is clear, an A* search on the grid finds one. It leaves each end's symbol straight out first, then goes round every part's box. It passes through no pin or wire end of another net, never runs along another net's wire, and never turns on one. A bend costs three steps, a crossing two, and a step over a part's texts two. It searches within 30 grid steps of the two ends, and gives up after 60,000 steps. **Tidy first:** in `arrange` and in a `connect` without `side` or `via`, a shape clear of the other parts' texts as well is taken before the step-at-a-time way, and one that only avoids their symbols after it. Only then does the router go over the parts. A `via` is the caller's own way: it is taken or refused, never routed round. A `side` keeps its order. `aDividerInARowKeepsItsNets` and the other wiring tests pass unchanged. |
| **2.3 `replace_component` from named pins to unnamed ones maps by number and says nothing.** ua741(TI) → uA741: INP went to the uA741's pin 2, its output, and the amplifier sat at −6.4 V. | Refused, when a wired pin of the old part has a name the new part lacks and `pins` is not given. The answer lists both parts' pins, each with the side of the symbol it is on ("1 (left, upper), 2 (left, lower), 3 (right) …"). `pins` says which is which; `"pins": "by number"` takes the numbers when they match. The uA741 also names its pins now, as the report suggested. Its model's port nets in `OpAmps.lib` are `_netINN`, `_netOUT`, `_netINP`, `_netVCC`, `_netVEE`, so the replacement maps by name and gives 1.1045 V. AD825's wrapper names its pins the same way: INP, INN, VCC, OUT, VEE, in the ADI order its model uses. Its gain of 11 is 1.087 V wired by name. A library part's pins are now named from the part's own subcircuit (`Lib_Comp`), not from the first one its model defines. AD825's and LM3886's inner models come first, and LM3886 names its pins through its wrapper: POSIN, NEGIN, POSRAIL, OUT, NEGRAIL, MUTE. POSRAIL and NEGRAIL count as supply names, for Check Schematic and for `arrange`. |
| **2.4 `add_component` with `near` returns no note when nothing was nudged.** | It always says where the part went: "Placed below RA, 40 from its symbol, centred on it". **A correction:** the sixth round's write-up said `near` slid a part along the side when its pins would land on something. It did not; nothing did. It does now. When the centred place has the part's symbol over another part's, or a pin on a wire or another pin, the part slides a grid step at a time, both ways, up to 30 steps. The note says so: "…, slid 20 right of its centre: there its symbol met another part's, or a pin came down on a wire or pin". `edit_component`'s `near` says the same ("Moved above RA, …"). Its own wires, which go with it, are not in its way. |
| **2.4 `find_library_component` gives a one-line part's `place` as `type: Lib`.** | Unchanged, since `add_component` makes the component from it. A new field says what it becomes: `"placed as": "Diode"` for 1N4148. It is outside `place`, so `place` can still be handed to `add_component` whole. |

## Checked beyond the tests

- **Every shipped example**, arranged as a preview by the build before
  (`e5978a4`) and after. The same 246 of 252 keep every net; the other
  six are symbol sheets with no parts with pins, and one file that does
  not open, the same with both. All 252 take 2.1 s now against 2.7 s.
  `audio_amp.sch` takes 0.07 s against 0.49 s: before, the first spacing
  found no way for every wire and it was laid out again with more room.
  Now it succeeds at once, with 189 wires against 209. Two examples
  needed that second try before, none now.
- **The reviewer's end-to-end scenarios** (`qucs-mcp-e2e-scenarios.py`,
  run from a copy with a root of its own) on the new build: 49 checks,
  49 passed. The LM386 schematic of s7 needs the libraries, so s7 was
  run with `QUCS_LIBRARY_DIR` as well.
- **Every library part under ngspice** again (the pins' names and
  `OpAmps.lib` changed): 4,039 of 4,224, part for part as committed.
- **The 741 bench by pin name**: ua741(TI) 1.1008 V; uA741 1.1045 V;
  AD825 1.0871 V; ua741(TI) → uA741 by name, 1.1045 V.

## Tests

- **`test_qucs_control`:**
  - `theFeedbackNetworkIsDrawnAsOne`: the report's bench (ua741(TI) wired by pin name with `connect`, Vin, Rf, Rg, RL, supplies in a column, a transient block, labels on the output and on VEE's net) arranged with `feedback: below`. Checked: `connect`'s wires over no part's symbol; after `arrange`, Rg in U1's column and not the load's, standing under Rf's input end with its ground pin below; no wire over any part's symbol, other than one leaving a pin straight out of its own, a ground's included; no wire over the texts of a part with no pin on its net (a wire from an op-amp's VEE pin may have to pass the op-amp's own name, which sits right under it); each label on the longest horizontal wire of its net, its text clear of every part; and every net as it was.
  - `theSeventhRoundsSmallThings`: the pin names of uA741, AD825 and LM3886; LM3886's POSRAIL and NEGRAIL on nothing warned; ua741(TI) → ua741(mod) refused, with both lists and their sides; ua741(TI) → uA741 by name (`XU1 0 inn out inp vcc vee`); `"pins": "by number"` taken; `near` centred and said, slid off a wire and said, `edit_component`'s said; `placed as` for 1N4148 and not for an LED.
- **`scripts/ci/check-tool-arguments.py`:** 67 tools checked, 0 disagreements.

Each change was broken on purpose, one at a time: 20 breaks, each built
and its tests run by the script of the sixth round, which put each file
back and checked it was as before. Three were not caught at first:

- **No maze in `connect`** (the ways `wireAlong` tries): the test looked
  at the wires only after `arrange`. It now checks the bench's
  hand-connected wires too; without the maze, the wire from V2 to VEE
  goes over Rf.
- **A label on the shortest stretch instead of the longest:** the output
  net had one horizontal stretch long enough, so shortest and longest
  were the same. VEE's net, whose two long stretches differ, is labelled
  as well now.
- **A label's text box taken as its bottom left corner** (it is its top
  left): not caught. The place chosen is the same where both readings
  are clear, as on the bench.

The test's check of wires over texts was first "a part none of whose
pins the segment ends on". That failed on a crossing no router can
avoid: U1's name sits right under its VEE pin, the full width of the
op-amp. It is now "a part with no pin on the wire's net". With that,
one more break is not caught: **the tidy pass of `arrange`** (a shape
clear of other parts' texts before the step-at-a-time way). On this
bench the shapes that avoid the symbols cross only the op-amp's own
texts, which the check allows. The maze's weight for texts is caught:
without it the VEE wire goes through Vin's name.

The full suite passes (77), under ASan and UBSan too, with no sanitizer
report. `~/QucsWorkspace` and `~/Library/Caches/qucs-s` are as they
were before the runs.

The tools sent every turn grew by 447 bytes (111,320 to 111,767): the
refusal and `"by number"` in `replace_component`'s description (+326),
and `placed as` in `find_library_component`'s (+121).
