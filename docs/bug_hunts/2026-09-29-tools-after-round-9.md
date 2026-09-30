# Bug hunt: the Claude tools and Check Schematic after rounds 7 to 9

*29 September 2026 - Qucs-S 26.1.4, `a643ac8`.*

One hour, 20:08 to 21:08, in the foreground, aimed at what came after the last two hunts
(the Claude tools at `e6df622`, Check Schematic at `d15ebe2`). That means the feedback rounds 7
to 9 and the assessment:
- Check Schematic as a design review: source loops, values, AC drive, NutmegEq names, bias,
  loads, supply polarity, subcircuits.
- `describe_part` and the library benches, and `max_chars`.
- `tune`'s `hold` and `compare`.
- `arrange`'s `feedback` and `straighten`.
- `create_subcircuit`'s port names and `make_symbol`'s sides.
- The markers' number format, and the diagram notation in the tools.
- The scenario script and the library test script.

The Release build of `a643ac8` drove the probes, and the ASan/UBSan build of the same commit ran
the fuzzers. Each `qucs-s --mcp-server` ran with its own workspace, settings and HOME in a
scratch folder, on macOS arm64 with ngspice 46. Methods:

1. **Probes**: the tools driven as an agent drives them, one question at a time (scripts
   beside this report). Where the check assumes what ngspice does, ngspice was asked.
2. **Sweeps**: `describe_part` on all 4,224 library parts, and their `place` handed to
   `add_component` (4,237 placements). `arrange` with `feedback` and `straighten` on every
   example, and `check_schematic` with `subcircuits` on every example. `max_chars` on every
   example, and the library test re-run on two libraries with `--merge`.
3. **Fuzzers on the ASan build**:
   - 1,185 examples with their values, wires and labels mutated, each checked, netlisted
     and arranged.
   - 934 examples with their marker lines mutated, each opened, saved and exported.
   - 4,308 random calls with junk arguments to the tools of rounds 7 to 9.
   - 2,189 random SPICE netlists imported, arranged and checked.
   - 7,918 marker and diagram edits on a simulated circuit, across every diagram kind.

   None found a crash or a sanitizer report.

*Status:* documented, not fixed. `~/QucsWorkspace` and `~/Library/Caches/qucs-s` were not
written by these runs. During the hour another session edited and simulated
`opamp741_prj/ua741_noninv.sch` there (20:24 and 20:27). No probe names that folder, and every
probe server had a workspace of its own.

| | severity | area | finding |
|---|---|---|---|
| A1 | medium | `create_subcircuit` | A moved pin named GND becomes a port and label "GND" - ground: refused, the message blaming the layout |
| A2 | medium | subcircuits | A schematic that uses itself (or a cycle of files) is not told; ngspice stops ("unknown subckt: x1.x1.x1...") |
| A3 | medium | Check Schematic | The source-loop rule misses current probes and controlled sources across a supply (ngspice: singular matrix) |
| A4 | medium | `tune` | A `hold` given as one object is ignored; a misspelt field in a hold's `measure` is ignored |
| A5 | medium-low | `simulate` | Its check first covers the top schematic only: a subcircuit's error behind a failed run is not said |
| A6 | low-medium | Check Schematic | The dV/dt note's numbers are a fixed example, not the circuit's |
| A7 | low | Check Schematic | The op-amp load note fires on a power amplifier's speaker (LM3886, 8 Ohm) |
| A8 | low | Check Schematic | In a subcircuit, a port named VEE does not quiet the negative-supply note |
| A9 | low | Check Schematic | The value it suggests for "+15 V" is "-+15 V" |
| A10 | low | `describe_part` | A transistor's bench that passed at its second point shows the failed first inside "passes", in alphabetical order |
| B1 | medium | `max_chars` | Below a few thousand characters the answer is cut as text: unparseable JSON |
| B2 | medium | `max_chars` | A JSON object's keys are never trimmed (a netlist map's nodes, an operating point's) |
| B3 | medium | `max_chars` | A `batch` answer is not trimmed at all |
| B4 | low | `max_chars` | Inside a batch's calls it is refused: the batch fails |
| C1 | low-medium | arguments | Values of the wrong JSON type are taken as the default, silently |
| C2 | low | arguments | Marker precision out of range, or not whole, is clamped silently (2.7 -> 1) |
| C3 | low | `edit_diagram` | A notation on a table or truth table is taken and never used or shown |
| C4 | low | tool search | The search hints were not updated: "notation", "feedback", "straighten" find nothing |
| D1 | low-medium | subcircuit files | A subcircuit with no file name: no error; the schematic's folder is "loaded" as one, with a message box |
| D2 | low | subcircuit files | A subcircuit file not found is loaded relative to the process's working folder |
| E1 | medium | instructions | The guide tells agents to mirror the uA741, whose inverting input is already the lower pin |
| E2 | medium | library test | A part missing from a night's results counts as passing: a library that no longer loads stays green |
| E3 | low-medium | scenario script | A second run on the same root fails (its files are there) |
| E4 | low | scenario script | It tests the source tree's library even when given a packaged app |
| E5 | low | names | Pin roles and `make_symbol` miss plain IN and numbered outputs (OUT1, OUTA) |
| F1 | low | library (upstream) | 13 part names appear twice within a library, with different models; the second is unreachable |

## A. Wrong answers, or silent

### A1. A moved pin named GND becomes a port and label "GND" - ground

Round 8 made `create_subcircuit` name a port after the moved part's pin on an unlabelled net
(INN, VCC). It checks the name is a word and that no net has it yet, in any case. It does not
check the names `set_label` refuses, and "gnd" among them means ground. A subcircuit with a
pin named GND (a local ground, wired in the parent to a mid-rail source VM) was turned into a
subcircuit. The new port and the parent's label were "GND", which joins VM's net to ground.
The net check caught it and nothing changed, but the answer is: "The instance would not keep
this schematic's nets - ... ground was on the net of SUB1.2 ... Move the parts apart (or their
wires) and try again." No move helps, and before round 8 the same call worked, with the port
named `wrap_n2`. (`h_gnd.py`.) *Fix:* leave out the names `set_label` refuses: gnd in any case,
the generated `net<n>` and `_net<n>`.

### A2. A schematic that uses itself, or a cycle of files, is not told

`self.sch` holds an instance of itself; `a.sch` uses `b.sch`, which uses `a.sch`. Check
Schematic with `subcircuits` says "0 errors, 0 warnings, 0 notes; in its subcircuits 0 errors,
0 warnings" of the cycle, and only "pin 1 is connected to nothing" of the self-use. The walk
keeps a visited set and does not loop, so Qucs-S does not hang. The netlist defines
`.SUBCKT a p` with `X1 p b` in it, and `.SUBCKT b p` with `X1 p a`. ngspice nests them until
it gives up: "Error: unknown subckt: x1.x1.x1.x1...", then "incomplete or empty netlist"
(`self.cir`). (`h_rec.py`, `h_cycle.py`.) *Fix:* the walk that already notices a file seen
before on its own path can say so as an error.

### A3. The source-loop rule misses current probes and controlled sources

A current probe (IProbe) straight across a Vdc passes the check, and so does a VCVS output
across it. In SPICE the probe is a 0 V source and the VCVS a voltage-defined branch. ngspice
says "singular matrix: check node v1#branch" for both (`amm.cir`, `vcvs.cir`). The rule counts
only independent voltage sources, and inductors, in the loops it looks for. *Fix:* the probes
(IProbe, the ammeters) and the voltage-output controlled sources (VCVS, CCVS) and their SPICE
forms join the loop.

### A4. `tune`'s `hold`: an object ignored, a misspelt measure field ignored

- `"hold": {"measure": {...}, "min": 1}`, one object in place of a list, is dropped without
  a word. The tune runs unconstrained and reports success.
- `"hold": [{"measure": {"operating_point": "top", "waht": "x"}, "min": 1}]` runs, and the
  misspelt `waht` is ignored.

A hold's own fields are checked ("hold[0] has no minn; it takes max, measure, min. Meant
min?"), and so is the top-level `measure` (round 6). With a dataset measure, a misspelt `what`
would measure something other than meant. (`h_tune.py`.) Minor: a hold on a node that does not
exist stops with "its nodes: i(v1), mid, top", which calls a branch current a node.

### A5. `simulate`'s check covers the top schematic only

A clean top schematic uses a subcircuit with two sources in parallel. `simulate` fails with
ngspice's words alone: "Transient op failed, timestep too small", "doAnalyses: OP: Timestep too
small; trouble with node v.x1.vx#branch". `check_schematic` with `subcircuits` names the
cause, "VX and VY are in parallel", but `simulate`'s check-first does not walk the subcircuits.
The guide says to check with `subcircuits` before `simulate`, which covers it when followed.
*Fix:* when a run fails, add the subcircuits' errors.

### A6. The dV/dt note's numbers are a fixed example

"C1 is straight across V1: nothing limits its current at V1's edges (C dV/dt: 5 A for 1 nF and
5 V in 1 ns)" is the same text for 1e308 F, for -1 nF (flagged as negative on the next line)
and for a rise time of 0, a negative one or a parameter. It is an illustration written into
the message (`edgeNotes`), but it reads as worked out. Round 8's reviewer took it as "the dV/dt
current worked out", and the assessment's write-up presents it so. Aside: the rule knows only
Qucs's Vpulse, Vrect and vPWL, so an imported SPICE PULSE source (S4Q_V) is not seen. *Fix:*
compute it from C, the step (U2 - U1) and Tr, or say "for example".

### A7. The op-amp load note on a power amplifier

An LM3886 (OpAmps, pins named POSRAIL, OUT, ...) drives an 8 Ohm speaker: "RSPK (8 Ohm) loads
U1's output to ground: op-amps are specified into 2 kOhm or so, and many limit their current
below 1 kOhm". That is its intended use. Any part with supply pins and an OUT pin is taken for
a small-signal op-amp.

### A8. In a subcircuit, a port named VEE does not quiet the negative-supply note

Round 8 quiets the note when the net's label, a pin on it or the source's own name says
negative supply. In a subcircuit a Vdc with its + on ground feeding the port VEE still gets
"its + is on ground, so VEE.1 is at -5 V - a negative supply is so", while a label `vee` on
the same net quiets it. The port's name is the net's name to the outside, but it is not read.
(A port named VCC below ground is not warned either.) (`h_sub.py`.)

### A9. The value it suggests for "+15 V" is "-+15 V"

The note and the wrong-way warning suggest the value of the other sign by prefixing or
stripping a minus in the text. U = "+15 V" gives "set U to -+15 V", and "+1.5e+1" gives
"-+1.5e+1". (`h_pol.py`.)

### A10. A bench that passed at its second point shows the failed first inside "passes"

22 transistors (2N3773, BFQ31, ...) passed at 0.9 mA after failing at 9 µA. `describe_part`
says: "its bench passes (... at 0.9 mA: Vbe 0.6401 V (0.1 to 1.6 V), beta 77.8 (3 to 5000), ...;
at 9 uA: Vbe 0.3366 V (0.1 to 1.6 V), beta 0.1 (3 to 5000), ...)". A beta of 0.1 against its
range sits inside "passes" with no word that that point failed. The points come in the order
QJsonObject sorts keys ("at 0.9 mA" first), not the order tried. This is round 8's
`benchNumbers`.

## B. `max_chars`

### B1. Below a few thousand characters the answer is cut as text

When halving the biggest lists and texts cannot bring a JSON answer within `max_chars`, the
answer is cut as text at the limit. The rest of the JSON goes, the `trimmed` field too, and
what is left does not parse. With 300 parts, `get_schematic` stops parsing below about 1,200
characters, `get_state` below 800 and `check_schematic` below 500. On the examples,
`get_schematic` with `max_chars` 1000 gives unparseable text for 246 of 251; at 3000 all parse.
`get_dataset` at 600 does the same. (`h_max.py`, `h_max2.py`.) *Fix:* drop whole entries to
"… n more" until it fits, and always keep the answer JSON with its `trimmed` field.

### B2. A JSON object's keys are never trimmed

`get_netlist` with `map` answers `nodes` as an object with a key per node. With 300 of them
the answer is cut as text at every `max_chars` up to 4000. The same happens to `get_dataset`
with `operating_point` (151 nodes) at 1000 and 3000; at 6000 it cuts `devices` and keeps the
object.

### B3. A `batch` answer is not trimmed at all

A batch answers in several content items ("2 of 2 calls done.", "[1] get_schematic:", then
the answer, ...). None is trimmed: asked for 2,000 characters it gives about 14,500, with no
`trimmed`. Every other tool answers in one item. (Screenshots add an image, which a character
count cannot cut.)

### B4. Inside a batch's calls it is refused

`{"calls": [{"tool": "get_schematic", "arguments": {"max_chars": 300}}]}` fails the batch:
"get_schematic takes no max_chars". The server takes `max_chars` out of a call's arguments
before the tool sees them, but not out of a batch's calls. The instructions say every tool
takes it.

## C. Arguments

### C1. Values of the wrong JSON type are taken as the default, silently

There is no check of an argument's type against the schema. A few are checked
(`name_shown`), most are read with a default:
- `arrange`: `straighten` or `keep_places` given 1 or "yes" is off, `feedback: 1` is inline,
  and `supplies: ["labels"]` is a column.
- `check_schematic`: `subcircuits` given "yes" or 1 is off.
- `get_netlist`: `map` given "true" or 1 gives no map.
- `import_netlist`: a `title` of 5 is dropped, and the netlist's own title is drawn.
- `make_symbol`: `sides` given a list, or a side given as a number, is ignored.

(`h_types.py`.) The same kind as A4's `hold`. *Fix:* one check of each argument's JSON type
against its schema, before the tool.

### C2. Marker precision out of range, or not whole, is clamped silently

`add_marker` and `edit_marker` take `precision` 1 to 12, as the schema says. 0 becomes 1, 99
becomes 12, 2.7 becomes 1 (toInt, then the clamp) and "5" becomes 1, all without a word.
The dialog allows 0.

### C3. A notation on a table is taken and never used or shown

`edit_diagram` with `notation` and `decimals` on a table (Tab) or a truth table answers as if
done and saves the values. Nothing there uses them, and `get_schematic` shows a notation only
for diagrams with axes.

### C4. The search hints were not updated

Deferred tools are found by their `anthropic/searchHint`. For `edit_diagram`, `edit_marker`
and `add_marker` there is no "notation", "number format", "scientific" or "engineering". For
`arrange` there is no "feedback", "straighten" or "supplies". A search in those words does not
find the tool that does it.

## D. Subcircuit files

### D1. A subcircuit with no file name

A Sub with `File` empty draws no error of its own. Check Schematic's `subcircuits` lists the
schematic's own folder as a subcircuit that "could not be loaded", and loading it raises a
message box ("Cannot load document: <folder>"). Under `--mcp-server` the box is closed and
reported; in the window it is a dialog.

### D2. A subcircuit file not found is loaded relative to the process's working folder

For `nosuch.sch`, not found beside the schematic, the walk tried
`<the process's working folder>/nosuch.sch`, outside the schematic's folder. A file of that
name there would be checked in its place. The error of the part itself, "not found (beside
the schematic, in the project or its user_lib)", is right.

## E. Scripts, instructions, names

### E1. The guide tells agents to mirror the uA741, whose inverting input is already the lower pin

Round 8's guide in the server's instructions says: "an op-amp whose - input is its upper pin
(ua741(TI), uA741) reads best mirrored". `describe_part` says the uA741's INN is "left,
lower", as its symbol draws it. ua741(TI) and ua741(boyle) have it on top. Mirrored, a
uA741's inverting input goes on top, the opposite of what the advice is for. Every session
reads the guide. Round 8's write-up says the same.

### E2. A part missing from a night's results counts as passing

`test-library-parts.py --baseline` reads `results.get(k, {}).get("passes", True)`. A part in
the baseline that the night did not test at all counts as passing, and so does a bench no
longer run. If a library file stops loading, its parts drop out of the run, and the nightly
job stays green. (The job's own `--out` is a separate file from its `--baseline`, so the
comparison itself is sound.)

### E3. The scenario script fails on a second run with the same root

`scripts/mcp-e2e-scenarios.py` never clears its root (`/tmp/e2e` by default). On a second run,
s1's `import_netlist` with `save_as`, s2's and s8's `save_document` refuse to write over the
files of the first run. Three scenarios end early ("ran to the end" FAIL, 3 of 4 checks fail).

### E4. It tests the source tree's library even when given a packaged app

The script sets `QUCS_LIBRARY_DIR` to the source tree's library whenever that exists,
whichever app `QUCS` names. A packaged app is never tested with its own library.

### E5. Pin roles and `make_symbol` miss plain IN and numbered outputs

The input names include in+, inp, inn, ..., and `make_symbol`'s own list has `v?in\d*`. The
output names are only out, vout and output. So:
- `describe_part`, the check's op-amp rules and `arrange`'s output search give no role to
  plain IN (LM317K-2, d_Divider, DirectionalCoupler), OUT1 or OUTA.
- `make_symbol` with ports in1..in3 and out1..out3 put out1 and out3 on the left among the
  inputs; out2 and OUTA went right by the alternation.

The library has no dual op-amp named that way; a user's subcircuit may.

## F. The library (upstream's)

### F1. 13 part names appear twice within a library

These names occur twice, and each pair has different models (BC847B: one from Diodes Inc.,
one contributed by G. Kraut):
- BJT_Extended: 2N3055, 2SA1302, MJK40, 2SC5171.
- JFETs: 2N4221.
- Transistors: 2N5551, BC846B, BC847B, BC848B, BC856A, BC857A, BC858A.
- Varactor: BB804.

Every lookup by name reaches the first. The second cannot be placed, and
`ngspice-tested.json`, one entry per name, never tested it.

## G. Checked and found right

- **`replace_component`'s `pins`**: two old pins to one new, pins that do not exist and junk
  are refused, each named. A named part to another with other names (ua741(TI) to LM3886) is
  refused with both lists. AD825 and ua741(boyle) map by name, each net kept.
- **`describe_part`** answers for all 4,224 parts; the 77 without pins are ferrite core
  materials. Its `place` given to `add_component` makes the part with the same pins and
  names, 4,237 times.
- **The library test** re-run on Diodes and JFETs with `--merge` gives the committed results
  part for part.
- **`arrange`** on every example with `feedback` below, `straighten`, and with `feedback`
  above and `supplies: labels`: every net kept, the slowest 0.14 s. The op-amp turned round
  (ideal and library) keeps Rg with Rf, above and below. `straighten` on 1,200 parts takes as
  long as before its round-9 change (1.2 s).
- **`import_netlist`**: `title` with slashes, dots, 300 characters, a newline or an emoji
  gives a safe file name. Two titles that clean to one name do not write over each other's
  library. Node names differing in case (out, OUT, Out) become one net, as SPICE reads them.
- **Check Schematic**:
  - The NutmegEq name rule leaves out hierarchical, generated and device names.
  - The verdict says when the subcircuits have findings.
  - A net label equal to a part's name is allowed, as in SPICE.
  - The value rules catch SPICE parts too (0 Ohm, 0 H, a negative C).
  - *Check Schematic and Subcircuits* on the recursive files ends at once.
- **`max_chars`** accepts 200.0 and refuses 199, 200.5, "300", true and a list. Cut texts keep
  emoji whole.
- **Markers**: 7,918 random marker and diagram edits on all five kinds of diagram, and 934
  mutated marker lines: no crash. A bad notation in a file reads as the diagram's.

## Reproducing

The probes are beside this report (`2026-09-29-tools-after-round-9/`). `mcp.py` starts
`qucs-s --mcp-server` with its own workspace, settings and HOME under `QUCS_PROBE_ROOT` (a temp
folder by default), and the source tree's library. `QUCS` names another binary, such as the
ASan build's.

```
cd docs/bug_hunts/2026-09-29-tools-after-round-9
python3 h_gnd.py        # A1
python3 h_rec.py        # A2 (self), and D1, D2
python3 h_cycle.py      # A2 (a cycle)
ngspice -b self.cir; ngspice -b amm.cir; ngspice -b vcvs.cir   # A2, A3
python3 h_tune.py       # A4
python3 h_sub.py        # A8
python3 h_pol.py        # A9
python3 h_max.py; python3 h_max2.py   # B1-B4
python3 h_types.py      # C1
```

The fuzzers (`h_fuzz.py`, `h_fuzz2.py`, `h_argfuzz.py`, `h_netfuzz.py`, `h_mkfuzz.py`) take the
binary, a seed and a time in seconds; those on examples want a copy of `qucs-s-26.1.1/examples`
at `$QUCS_PROBE_ROOT/exs`. For the ASan build:
`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 python3 h_argfuzz.py <build-asan qucs-s> 7 240`,
then look for "AddressSanitizer" or "runtime error" in `$QUCS_PROBE_ROOT/server.err`.
