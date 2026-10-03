# Proposal: Schematic Check Settings

*3 October 2026 - Qucs-S 26.1.5. A proposal, not built yet.*

Cadence's schematic editor lets the user set up each rule of its schematic checker:
how severe its finding is (error, warning or ignored), its limits, and which findings
are waived. Qucs-S's check (*Check Schematic*, the Problems tab) has fixed rules. This
proposal adds **Simulation > Schematic Check Settings...**, right after *Simulators
Settings...*, a dialog of several tabs that controls every aspect of the check. The
facts below were checked in this tree on 3 October 2026.

## The check today

`erc.cpp` (2,100 lines) has three entry points:

- `check()`: errors and warnings. The Problems tab, the status bar (as you edit), the
  check before every simulation, *Check Schematic and Subcircuits* and Claude's
  `check_schematic` all use it.
- `notes()`: findings worth a look, which would fill a drawing if they were warnings
  (a label on one pin, wires crossing without a junction, a time step long for a
  source's edges, ...). **Only Claude's `check_schematic` shows them**, never the
  Problems tab.
- `wiring()`: what a wire just drawn did. Claude's drawing tools use it.

Each rule's severity is fixed in the code. Two rules can be switched, both in
*Simulators Settings > Before a simulation*: *A schematic must have a ground symbol*
(on by default) and *Warn of commands a simulation runs besides the simulator* (off).
The check's limits are fixed too: an op-amp load below 2 kOhm, a transient crossing an
edge in fewer than five steps. Before a simulation, errors bring up the Problems tab,
and the run goes ahead anyway: the simulator has the last word.

## The dialog

*Simulation > Schematic Check Settings...*, after *Simulators Settings...*. Each tab
lists its rules, one row each:
- the rule's name, its tooltip saying what it finds and why;
- its severity: **Error**, **Warning**, **Note** or **Off**;
- its limit, where it has one.

A Note is shown in the Problems tab only while *Show notes* is on (General), and it is
always in Claude's `check_schematic`. Off skips the rule. Each tab has *Defaults*, and
the dialog *Restore All Defaults*. The defaults are today's severities, so nothing
changes until the user changes it.

In the tables, *Today* is the rule's severity now, and where it is shown: "check" is
the Problems tab and everywhere else, "notes" is Claude's `check_schematic` only.
"New" is a rule the check does not have.

### 1. General

| Setting | Today |
|---|---|
| Run the check before every simulation | always |
| On errors before a simulation: run anyway, ask, or refuse | runs anyway |
| Bring up the Problems tab: on errors, on warnings, or never | on errors |
| Check as you edit, in the status bar; the delay before it runs | always, no delay setting |
| Check on save: warn before saving with errors (Cadence's *Check and Save*) | new |
| Subcircuits in the check before a simulation: none, those used directly, all | none |
| Show notes in the Problems tab | new (notes are Claude's only) |
| At most so many findings of one rule in the Problems tab, then their count | new (all are listed) |
| Warn of commands a simulation runs besides the simulator; by kind: System command parts, ngspice's `shell`, `.spiceinit`, the Octave script after a run | one switch, in Simulators Settings |

### 2. Connectivity

| Rule | Today |
|---|---|
| A pin connected to nothing | Warning, check |
| A ground connected to nothing | Warning, check |
| A wire end connected to nothing | Warning, check |
| A part or a group of parts not connected to the rest (floating) | Warning, check |
| No ground; and whether a net named `0` or `gnd` counts as one | Error, check, when *require a ground symbol* is on (Simulators Settings) |
| A net reaching ground only through capacitors or current sources: no DC path | Warning, check |
| A net with one pin only: a label on a single pin | Note |
| A pin on another net's wire mid-way, not connected to it | Warning, check |
| A wire end touching a wire mid-way without a junction | Warning, check |
| Two nets' wires lying over each other | Warning, check |
| Wires of two nets crossing without a junction | Note |
| One net with two names (two labels) | Note |
| A label on a net with a ground symbol: the netlist calls it 0 | Note |
| Labels that differ only in case, for a simulator that reads names without case | Warning, check |
| A pin labelled `gnd`, or with another net's label: it gets a generated name | Warning, check |
| A subcircuit's ports sharing a number | Error, check |
| Pins allowed to stay open: by component type (a transformer's `nH`/`nB`, a test point) or by a name pattern (`^TP`) | new |

### 3. Electrical

| Rule | Limit | Today |
|---|---|---|
| A voltage source shorted: both its pins on one net | | Error, check |
| Voltage sources in parallel or in a loop | | Error, check |
| A loop of voltage sources and inductors, at DC | | Warning, check |
| A part's supply pins on nothing that powers them | | Warning, check |
| A supply the wrong way round: its value against its pins' or label's names (`VEE` at +15 V) | | Warning, check |
| A negative supply on ground, in case it was meant to be positive | | Note |
| A base, a gate or an op-amp input with no DC bias path | | Note |
| An op-amp output loaded below the limit | 2 kOhm | Note, the limit fixed |
| A capacitor straight across a source's edge: nothing limits C dV/dt | | Note |

### 4. Values

| Rule | Limit | Today |
|---|---|---|
| A negative capacitance | | Warning, check |
| A 0 Ohm resistor or a 0 H inductor: a short | | Note |
| Another negative value | | Note |
| SPICE reads another number than Qucs (`M` and `MEG`, `4k7`) | | new to the check: shown in the component dialog and in Claude's edit answers |
| A value naming a parameter no `.PARAM` defines | | new |
| A value outside its type's range: R below 1 mOhm or above 1 TOhm, C above 1 F, ... | user-editable, per type | new |
| A V(PRBS) value ngspice refuses: Tbit, Td, Order, Seed, an edge longer than Tbit | | Error, check |
| A V(PRBS) Coding neither NRZ nor PAM4; a pulse's or PRBS's edge below 0 | | Warning, check |

### 5. Analyses and simulator

| Rule | Limit | Today |
|---|---|---|
| No simulation block | | Warning, check |
| An AC analysis with no AC source; its only AC source of magnitude 0 | | Warning, check |
| A transient's MaxStep below 0 | | Error, check |
| A transient's Points not a whole number | | Warning, check |
| A transient's time step long for its sources' edges | steps per edge: 5 | Note, the limit fixed |
| A transient's integration method that ngspice does not take | | Note |
| A DC sweep of what ngspice's `dc` cannot sweep | | Error, check |
| An optimizer whose setup does not read, or names a simulation not in the schematic | | Error, check |
| A Nutmeg equation reading `v(x)` of a net that is not there | | Warning, check |
| An equation writing over a node's, a probe's or an axis's name | | Warning, check |
| A part not available for the simulator; one with no SPICE model; an implicit equation-defined device | | Error, check |
| Check against the simulator selected, or every SPICE simulator | | the selected one |

### 6. Names

| Rule | Today |
|---|---|
| A name used twice | Error, check |
| Names that differ only in case, for a simulator that reads names without case | Error, check |
| Naming rules: the characters allowed (a regular expression), the longest name, reserved names (`gnd`, `0`), a prefix per type (R, C, L, Q, M, X, ...) | new |
| Net label rules: the characters allowed, whether case matters | new |

### 7. Libraries and files

| Rule | Today |
|---|---|
| A library part that does not load | Error, check |
| A subcircuit: no file given, its file not found, a file that does not load, one that uses itself or one above it | Error, check |
| A SPICE library: no file given, not found, no such subcircuit in it | Error, check |
| A Verilog-A module in no library (`.osdi`) or source (`.va`) that Qucs-S loads | Warning, check |
| A winding without its magnetic core | Error, check |
| What a part reports of itself (its own check) | Error, check |

### 8. Waivers and rule sets

All new:
- **Waivers.** *Waive* in the Problems tab's menu: one finding, by its rule and part,
  with a reason, kept with the schematic. A tab lists them, to read or remove; the
  Problems tab shows waived findings as a count.
- **Presets:** Strict, Default, Relaxed.
- **Rules per project:** a rules file in the project overrides the application's, so
  that a team shares one set. A rule set exports and imports as JSON, as the other
  settings do.
- **Exported findings:** text, CSV or JSON, with subcircuits and notes or without.

## How it would be built

- **A name for each rule.** Each finding carries its rule's ID (`pin.unconnected`,
  `net.floating`, `source.loop`, ...): a field of `erc::Issue`. One table gives each
  rule its default severity (today's), its group (the tab) and its limit.
- **Severity at one place.** `check()` and `notes()` find as now. A last step gives each
  finding its rule's severity and drops the Off ones. With the defaults, the findings
  must be those of today: `test_erc`'s cases, and its survey of every shipped example
  (`QUCS_ERC_SURVEY=1`: each simulator, the ground required and not) run before and
  after, the same.
- **Kept with the settings,** as `ErcRules/<id>` (the severity) and `ErcLimits/<id>`.
  Export and Import Settings carry them with no more code: they walk every key. The
  two existing switches keep their keys (`RequireGround`, `CheckCommands`). Simulators
  Settings shows them as now and points to the new dialog, or they move there; both
  dialogs set the same setting.
- **Claude's tools follow it.** `check_schematic` reports each finding's rule and the
  severity set, so the window and Claude say the same. `get_settings` and
  `set_settings` reach the dialog as a scope of their own (`schematic check`), as they
  reach the other settings dialogs: they read a dialog's controls by tab and label.
- **Tests:** for each rule, a severity set changes its finding, and Off removes it.
  The defaults keep the examples' survey as it is. The limits take effect (an op-amp
  load at 1 kOhm with the limit at 500 Ohm says nothing). The dialog saves, restores
  its defaults and reads its settings back.

## Phases

1. **Tabs 1 to 7, with the rules there are:** the rules' IDs, severities and limits,
   the General tab, notes in the Problems tab, Claude's tools following it. About
   1,500 to 2,500 lines with the tests.
2. **The new rules:** naming rules, value ranges, SPICE's reading of a value,
   undefined parameters, check on save, every SPICE simulator.
3. **Waivers, presets, rules per project and exported findings.**

## Open questions

- Settings for the application only, or per project from the start (phase 3 sooner)?
- Should *refuse a simulation with errors* be offered? Today the run goes ahead,
  because the simulator decides what it can run; refusing would block circuits the
  check misjudges.
- An Off rule: hidden from Claude's `check_schematic` too, or still shown there as a
  note, so Claude sees what the user turned off?
