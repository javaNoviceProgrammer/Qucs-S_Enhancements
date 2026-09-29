# Feature gaps: Qucs-S's MCP tools, the assessment after seven rounds

*29 September 2026 — Qucs-S 26.1.4.*

Claude (Fable 5.1) wrote an assessment of the tools after rounds 1 to 7
(`qucs-mcp-api-assessment-and-wishlist.md`), with a wishlist in order of
value and a probe of `check_schematic` with twelve faulty circuits. It
was written at `e5978a4`, before round 7's fixes (`8c89aac`), so some of
it was done already; the tables say so. The rest is in `fa660c7` (the
check), `2ee8079` (the tools) and `f6d7c1f` (the benches, `straighten`).
Each change was reproduced before it was made, has a test, and was
broken on purpose to see the test fail.

## What is still thin

| Assessment | Now |
|---|---|
| **Layout is functional, not fluent**: `feedback: below` leaves Rg in the load column and puts a label on a wire. | Round 7: Rg stands under Rf's input end, the label goes on the longest free stretch of its net, and wires go round every symbol, grounds included. New: `arrange`'s `straighten` (wishlist 1, below). |
| **One silent path remains**: `replace_component` from named pins to unnamed ones maps by number. | Round 7: refused, both parts' pins listed with their sides, until `pins` says which is which or `"by number"`. |
| **The library is the weak link**: the nightly test proves a part converges, not that it behaves. | Wishlist 3, below: a bench for each op-amp, transistor, FET and diode, its numbers checked against a range. 12 models fail theirs. |

## The wishlist

| Wishlist | Now |
|---|---|
| **1. Layout that knows the idioms**: the feedback network (Rg under Rf), labels on the longest free segment, wires clear of ground and supply symbols, and a `tidy` that straightens stubs and aligns parts to their neighbours' pins. | The first three were round 7's. **`straighten`** is new: `arrange` nudges a part up to four grid steps so the two pins of a wire between two parts line up and it runs straight. It takes the move that lines up the most such wires, then the next, until none helps, and never moves a part's symbol or texts onto another part's. With `keep_places` it is the tidy; after a full `arrange` it straightens what the columns left. The answer names each part moved and by how much ("R1 10 down"). On every shipped example, as a preview: none failed, every net was kept, and 137 of 251 were straightened, mostly one or two parts by 10 to 40. |
| **2. No silent by-number mapping**, and pin names for the transistor-level models. | Round 7: the refusal, and the uA741, AD825 and LM3886 name their pins. `get_netlist`'s map now also lists the parts of more than two pins none of which has a name, each pin with its side: `OP1: 1 (left, lower), 2 (left, upper), 3 (right)`. |
| **3. Reference circuits per library**, run nightly, their numbers checked against a range. | `scripts/ci/test-library-parts.py` puts each part that passes the smoke test and is of a kind it knows into a bench. **An op-amp** (its inputs, output and supplies named): a follower of 1 V and a gain of 11 of 0.5 V on ±15 V, the output within 5 %. **A bipolar transistor**: 10 V, the base through 1 MΩ and the collector through 1 kΩ (9 µA in), or 10 kΩ and 10 Ω (0.9 mA in, for a power part whose model is fitted at amps). At either point, Vbe must be 0.1 to 1.6 V (germanium from 0.15) and β 3 to 5000, or saturated. **A MOSFET** must conduct more than 1 mA at \|Vgs\| = 10 V; **a JFET** must conduct with its gate at its source. **A diode**'s forward drop at about 1 mA must be 0.1 to 4.5 V. 1,798 parts have a bench, and 1,786 pass. The 12 that fail run and give nonsense: S2K's saturation current is written `1.3` (amperes) and it drops 65 µV; MMST4401's Nf is 410 (its β, a field over); Transistors.lib's BFS20 has Ise = 15.6 pA (the BJT_Extended one has 1 fA) and a β of 0.0; 818E drops 27 mV; the five KT630s, KT608A and KT373G, small-signal parts, give a β of 0.0 to 1.0 at both points; BU508DR's Vbe is 33 mV. `describe_part` and `find_library_component` report the bench ("its bench passes (op-amp: follower of 1 V, gain of 11 of 0.5 V, on +-15 V: follower of 1 V 1, gain of 11 of 0.5 V 5.504)", or "but its bench FAILS (diode: …): its forward drop is 6.462222e-05 V, not 0.1 to 4.5 V - the model runs and does the wrong thing"), and a part whose bench fails is not `tested`. The nightly job fails when a bench that passed fails. `--merge` runs some libraries into the results file and keeps the rest. |
| **4. `describe_part`** for library models. | New tool. For `library` and `part`: each pin in order with its name, the side of the symbol it is on and its role (input, output, supply, from the names the check reads, `erc::pinRole`); the supply pins; what the model is, with the count of its elements; how the tests found it; its description, a note and `place` for `add_component`. The model is one of: one component, placed as that component with the library's values; a macromodel of controlled sources; or transistor level. The uA741: INN input (left, lower), OUT output (right), INP input (left, upper), VCC and VEE supplies, "transistor level (21 transistors, 0 controlled sources, 0 diodes, 12 R/C/L, 0 subcircuit calls)". A part whose pins have no names says it is wired by number. |
| **5. `tune` with constraints and reporting**: a `hold` list kept within bounds, and a table of every measurement before and after. | `hold`: `[{"measure": {...}, "min": 50e3, "max": ...}]`, measured by every run, of the same kind as the target (operating point or analyses). Only a value that keeps every hold is set. One that reaches the target but breaks a hold is named under `held back`, with the hold it breaks and its value ("R1 = 2.0057k gives the target, but b is 2.496, below 2.6 - not taken"), and not set. This works with one knob or with several. `compare` (on with `hold`) runs the values as they are first. `before and after` then gives each measurement, the target's and each hold's, as it was and with the value found, with the change. Each run in `runs` shows its hold values and whether it kept them. Both tune paths now measure through one function (`measureRun`). |
| **6. A budget on the whole answer**: `max_chars` on every tool. | Every tool takes `max_chars` (200 or more). It belongs to the server, not the tool, and is taken out of the arguments before the tool sees them. A longer answer has its biggest list or text halved, then the next, until it fits. Lists end "… 59 more" and texts "… (n more characters)". A `trimmed` field then says what was cut and to what size ("to 2000 characters or less of …, as max_chars asks: components cut - the tool's own filters ask for less, a larger max_chars for more"). A text answer is cut at its end. A value that is not a whole number of 200 or more is refused and nothing is done. It is said once, in the server's instructions, not in 68 schemas. |
| **7. `check_schematic` as a design review.** | Below. |
| **8. Small ones**: `near` says where; `find_library_component` says "placed as"; the map marks unnamed pins; `import_netlist` takes a `title`. | The first two were round 7's. The map lists parts with unnamed pins (item 2). `import_netlist`'s `title` replaces the netlist's first line: it is drawn as a text above the circuit, and it names the subcircuits' library. The netlist's own title is drawn too now. |

## check_schematic: what it missed

Each case was run through ngspice 46 first, to give it the severity of
what the simulator does with it.

| Circuit | Then | Now |
|---|---|---|
| Two DC sources of different values in parallel | nothing | **Error**: "V1 and V2 are in parallel: voltage sources in a loop fix one voltage twice, and the operating point fails (ngspice: singular matrix)". The same for any loop of voltage sources. ngspice: "DC solution failed". |
| A voltage source shorted by a wire | nothing | **Error**: "V1 is shorted: both its pins are on a, and a voltage source across a wire has no solution". ngspice: "instance v1 is a shorted VSRC", fatal. |
| An inductor straight across a DC source | nothing | **Warning**: "V1 and L1 are in parallel: at DC an inductor is a short, so this loop of voltage sources and inductors has no operating point" (inductors in parallel too). ngspice: singular matrix, then nonsense. |
| A capacitor straight across a pulse source, no series R | nothing | **Note**: "C1 is straight across V1: nothing limits its current at V1's edges (C dV/dt: 5 A for 1 nF and 5 V in 1 ns)". ngspice drew 5 A. A capacitor across a DC supply is decoupling, and not told. |
| R = 0 and C = −1 nF | nothing | **Warning** for a negative capacitance: a transient ran away to −8.9e8 V. **Notes** for 0 Ω, 0 H and negative R or L. ngspice runs R = 0 as 1e-12 Ω, and four shipped examples keep 0 Ω jumpers. |
| An AC analysis with no AC source | nothing | **Warning**: "AC1 has nothing to drive it: no Vac, Iac, Pac or SPICE source with an AC value, so every voltage and current of it is 0". This is not told when a subcircuit, library part or SPICE text could hold the source. |
| A NutmegEq naming a node that does not exist | nothing | **Warning** (SPICE simulators): "NutmegEq1 reads v(nothere), but no net is labelled nothere". A probe's name and the equations' own vectors count as names. A diagram's traces are not looked at: `v(s_1_1)`, `v(nf)` and a sensitivity's names are the analysis's own, and read that way 18 of the shipped examples gave 26 false warnings. |
| Two labels on one net (`in` and `vin`) | nothing | **Note**: "one net has 2 names, in and vin: the netlist keeps one of them, and a plot or an equation of another finds nothing". |
| Labels `Out` and `out` on different nets | a single-pin note | Done in `5d7c0cf`: one net for ngspice and Xyce, and a warning when the wires keep them apart. `r1` beside `R1` is an error: ngspice stops, "device already exists". |
| An op-amp input floating, output loaded | found | As it was. New **notes**: an op-amp input, a base or a gate whose only DC path runs through its own part. An op-amp's own output counts as a path (feedback), and a current source biases. So "Q1: its base has no DC path but through Q1 itself - no bias current reaches it, so Q1 sits off". Also a load under 1 kΩ on the output of an op-amp that has supply pins. |
| **The check does not descend into subcircuits** | not told | `check_schematic` gives a line of counts for each subcircuit it uses, at any depth, and `"subcircuits": true` lists their findings with their files. The walk is Check Schematic and Subcircuits', now one function (`erc::checkSubcircuits`). |

Its description says what it is: "A topology and netlist check: nothing
found does not mean the circuit works."

Found on the way, and fixed:
- **A pin joined only by a label counted as on nothing.** A floating group wired by labels was never told, and the bias note skipped such pins.
- **The findings' order followed a hash's order.** Qt seeds hashes anew in each process, so the order, and which label named a net of two, could change between runs (VCO_100's "net L7.1" was "net C5.1" on another run). Now the survey of the examples gives the same output byte for byte on two runs. A test runs the check under eight hash seeds.

**The shipped examples**: the survey (`test_erc surveyOfTheExamples`),
with the notes now, gives the same errors and warnings as before these
rules. The only new findings are four 0 Ω jumpers, as notes.

## What to keep doing

The reviewer's three: the scenario suite on every commit, each fix broken
on purpose, refusing rather than guessing. In this round 49 breaks were
made, and each was caught. Two were caught only after the test was made
stronger: an op-amp's own output counting as a DC path, and a two-knob
hold that was not measured. That second one was first caught by a crash:
the check read past a list the break left empty, and it is guarded now.
A third break hit a line that did nothing: a NutmegEq's `out=pos-neg`
is read as the property `out`. That line is gone.

## Tests

- **`test_erc`:** `theDesignRulesFindWhatTheSimulatorWouldTrip` (each case above in its circuit, and the circuit put right); `theFindingsDoNotDependOnAHashsOrder`.
- **`test_qucs_control`:** `theCheckGoesIntoSubcircuitsAndReviewsTheDesign`, `theAssessmentsSmallThings` (the title, the map, `describe_part`, the benches' report), `tuneHoldsAndCompares`, `straightenLinesUpTheWires`.
- **`test_mcp_server`:** `everyToolTakesMaxChars`.
- **`scripts/ci/check-tool-arguments.py`:** 68 tools, 0 disagreements.

The full suite passes (77). `~/QucsWorkspace` and `~/Library/Caches/qucs-s`
are as they were before the runs.

The tools sent every turn grew by 2,566 bytes (111,767 to 114,333). That
is the new `describe_part` (960 bytes), plus `tune`'s `hold` and
`compare`, `arrange`'s `straighten`, `check_schematic`'s `subcircuits`
and `import_netlist`'s `title`. `max_chars` is in the instructions, sent
once.
