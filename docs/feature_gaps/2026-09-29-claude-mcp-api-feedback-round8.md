# Feature gaps: Qucs-S's MCP tools, the eighth round of feedback

*29 September 2026 — Qucs-S 26.1.4.*

Claude (Fable 5.1) reviewed `4c0e137` in the evening
(`qucs-mcp-api-feedback-round8.md`). It drove the tools headless and read
the eleven commit messages since round 7 and the assessment's write-up.
Every item of the assessment checked out, and the end-to-end scenarios
passed, 49 of 49. It found one layout fault, one note that spoke too
often and one formatting nicety, and it asked for two things more: the
741 bench as an end-to-end scenario, and the way of working as a guide
in the server's instructions. Each fault was reproduced before it was
changed.

## Findings

| Feedback | Now |
|---|---|
| **2.1 `arrange feedback: below` on the non-inverting 741 bench:** Rg goes to a column of its own, `[[Vin], [U1, Rf, RL], [Rg]]`, and its wire crosses the output's without a junction. | Reproduced with U1 as a subcircuit (`create_subcircuit` of the ua741(TI), then `make_symbol`). The reviewer's check of the bench said "in its subcircuits …", which only a subcircuit instance gives. Round 7 took the host's output to be the pin of Rf's two that stands out furthest. On a subcircuit's box every pin stands out as far (±110), so the first pin won: INN. Rf's "input" end was then the output's, and RL, from the output to ground, was taken for Rg and hung under Rf. The output is now, in order: the pin named so (`OUT`, by the check's `erc::pinRole`); else the pin on a net named so (a label `out`); else the one standing out furthest; else the right-hand one of two as far. It is worked out once, and the shunt search, Rf's turn and Rf's place under the output all use it. The bench now gives `[[Vin], [U1, Rf, Rg], [RL]]`, with U1 as the library part and as the subcircuit in each of its boxes. The test's op-amp is a subcircuit box whose output ties with an input, with names and without. |
| (found while reproducing it) **`make_symbol` put `out` on the left**, among the inputs, in a box of five ports named `ua741_sym_n1` … `n4` and `out`. | Two changes. `make_symbol` sides a port by its name when its type (`analog`) does not say: inputs (in, vin, inp, inn, in+, noninv …) on the left, outputs (out, vout, output) on the right. And `create_subcircuit` names an unlabelled net's port after the pin of the moved parts on it, when one pin has a name and no net has it yet, whatever its case. Otherwise it is `<file>_n1` as before. So the ua741(TI)'s subcircuit has ports INN, INP, out, VCC, VEE, and its symbol has INN and INP on the left, out on the right, VCC on top and VEE below. The name check ignores case because two labels `inp` and `INP` are one net to SPICE. |
| **2.2 The note "VEE: its + is on ground, so … is at −15 V - a negative supply is so"** fires on every correctly drawn negative supply. | It is quiet when the net says it is a negative supply: a pin named as one on it (an op-amp's VEE, VSS, V−, NEGRAIL), a label named as one, or the source itself named as one (VEE, VNEG). Where the names say the other way, it is now a **warning**: a positive supply's pin below ground ("VCC puts U1.4 (VCC) at −15 V, though it is a positive supply's pin: the source is the wrong way round - set U to 15 V (edit_component)"), a negative one's above it, or a net labelled vcc below ground. A net nothing names keeps the note. The fix it gives is the value of the other sign: `edit_component` refuses to turn a wired source when the turn would join nets, as the scenario found. The ad822 of the op-amp templates names its pins by number (1, 2, 99, 50, 25), so their notes stay. |
| **2.2 `describe_part`'s bench numbers have no units or expected values** ("follower of 1 V 1, gain of 11 of 0.5 V 5.504"). | "follower of 1 V: 1.000 V (expected 1 V); gain of 11 of 0.5 V: 5.504 V (expected 5.5 V)". Every kind has its unit and the range the script checks: "at 9 uA: Vbe 0.6712 V (0.1 to 1.6 V), beta 213.5 (3 to 5000), Vce 8.008 V"; "Id 9.824 mA (1 to 10 mA)" (a JFET's from 0.001 mA); "Vf 0.6538 V (0.1 to 4.5 V)". **A bug on the way:** a transistor's numbers are one object per bias point, and they read as a number, so every bipolar transistor's bench said "at 9 uA 0". `find_library_component` shows the same text. |
| **4.2 The 741 bench as an end-to-end scenario.** | The reviewer's scenario script is now in the repository, `scripts/mcp-e2e-scenarios.py`, with **s8**. s8 describes the ua741(TI) (its pins by name, its bench with the expected values). It wires the bench by pin name in one batch and checks it: nothing wrong and no supply note. It sets VCC to −15 V and checks for the warning, then undoes it. It arranges the bench with the feedback below (Rg with Rf, every net kept) and simulates: 1.1 V peak for a gain of 11 of 0.1 V. It keeps that run, sets Rf to 20k and compares: 2.1 V. It makes U1 a subcircuit (ports INN, INP, out, VCC, VEE) and draws its symbol (sides by name). It checks with `subcircuits` (nothing in either), arranges again, and simulates again: 2.1 V. The script's known faults are fixed: its root is made before the server's log is opened; the server runs with a HOME of its own, so nothing reaches the user's caches; the build tree's app gets the source's library; s7 takes its project from `QUCS_E2E_PROJECT`, and is skipped when there is none. |
| **4.3 The way of working, in the instructions.** | The instructions end with "How to work, as it has worked best", in six steps. (1) `describe_part` before wiring a library part, then wiring by pin name in one batch and powering every supply pin. (2) `arrange` with `feedback`, `mirror` for an op-amp whose − input is its upper pin, `supplies: labels`, `straighten` and `preview`. (3) `check_schematic` with `subcircuits` before `simulate`, and what its errors, warnings and notes mean. (4) Judge by `get_dataset`'s numbers; `keep_as` and `compare` for a before and after. (5) `tune` with `hold` and `compare`. (6) `brief`, the tools' filters and `max_chars` for long answers; `undo` and `undo files`. A test checks that every tool and argument the guide names exists. |

## What layout is left

The reviewer's advice was to fix 2.1 and then stop. One crossing is
left, and it is the symbol's, not `arrange`'s. The ua741(TI) and the
uA741 have their − input as the upper pin, so with the feedback below,
the − wire crosses the + input's wire, and with the supplies in a column,
their wires as well (four crossings on the bench). `feedback: above`
does not help, because Rg still stands below. Mirrored (`mirror: true`),
with `feedback: below` and `supplies: labels`, the bench draws with no
crossing at all: + on top, Rf and Rg under the op-amp, as a textbook has
it. The guide says so, rather than `arrange` mirroring parts by itself.

## Checked beyond the tests

- **The end-to-end scenarios**, s1 to s8, on the build tree with the
  source's library: 60 checks, none failed. 12 of the 13 project
  schematics of s7 simulate; `diffpair.sch` has no analysis.
- **The survey of the shipped examples** (`test_erc surveyOfTheExamples`):
  the errors and warnings are as before, and no example is found with a
  supply the wrong way round. Two runs give the same output byte for
  byte. 23 of the negative-supply notes remain, each on a net that
  nothing names.
- **Every library kind's bench text**, on the uA741, 2N2222, BU508DR,
  BSS123, 2N2608, 1N4148, S2K and BFS20.

## Tests

- **`test_qucs_control`:**
  - `theOutputIsFoundByNameWhenThePinsTie`: a subcircuit of an ideal op-amp between ports inn, inp and out, used with no symbol drawn (inn and out both at x −30). With the label `out`, arranged with the feedback below: Rg with the subcircuit and Rf, not with RL. `make_symbol` then sides inn and inp left and out right. With every port put on the left and no label, the pins' names decide: Rf's output end under the output pin, and Rg standing under its other end. `create_subcircuit` of the instance names the ports after its pins, except one whose name a label has in another case (`INP` beside `inp`), which becomes `fbwrap_n1`.
  - `supplySignsAreRead`: a part with pins VCC and VEE. Drawn right, nothing is said. VCC's source at −15 V and VEE's at +15 V are warned, each with the value to set. An unnamed net below ground keeps the note; labelled vee it is quiet, labelled vcc it is warned. A source named VNEG is quiet.
  - `theEighthRoundsSmallThings`: the bench text of an op-amp, a bipolar transistor, a MOSFET, a JFET and a diode, each with its units and range; and each tool and argument the instructions' guide names is one there is.
  - `suppliesAreChecked` (round 6): the negative source on the VEE pin labelled `vee` is now quiet.
- **`scripts/mcp-e2e-scenarios.py` s8**, as above.
- **`scripts/ci/check-tool-arguments.py`:** 68 tools checked, 0 disagreements.

Each change was broken on purpose, one at a time, with its test run: 21
breaks, and each was caught. They covered the output found by the pin's
name, by the net's, by the tie, and Rf's place under it; `make_symbol`'s
inputs and outputs by name; `create_subcircuit`'s names, and the case of
a name in use; each way the supply check is quiet, each warning, the
fix's value, and the warning left as a note only; the bench text as it
was, without the expected value, and with a MOSFET's range for a JFET;
and a stale tool and a stale argument in the guide.

The full suite passes (77). `~/QucsWorkspace` and `~/Library/Caches/qucs-s`
are as they were before the runs.

The tools sent every turn grew by 114 bytes (114,333 to 114,447): how
`create_subcircuit` names its ports and how `make_symbol` sides them.
The instructions, sent once, grew by 1,495 characters (4,329 to 5,824)
with the guide.

## A correction

*30 September.* "What layout is left" says the ua741(TI) and the uA741 have
their − input as the upper pin, and the guide said to mirror both. The
uA741 does not: its symbol has INN on the left, lower (`describe_part` says
"left, lower"), so mirrored its − input goes on top, the opposite of what
the advice is for. The ua741(TI) and ua741(boyle) have it on top, as do
op27(boyle), opa27(TI), tl081(TI), tl071(TI), OP07(TI), OP37(TI) and
mc1458(TI); the AD825 and the LM3886 have it below. The guide now says to
read the side from `describe_part`, with an example of each, and a test
checks each part it names against `describe_part` (the bug hunt of
29 September, E1).
