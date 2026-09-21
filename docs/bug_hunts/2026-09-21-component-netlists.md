# Bug hunt: the built-in components and the netlists they generate

*21 September 2026 — Qucs-S 26.1.2 (`d4c6746` plus the audit harness of this commit).*

Every component the Components panel offers was placed on a schematic, netlisted
in every flavour the code has (ngspice, Xyce, CDL, Qucsator, model cards,
`.control` fragments), and checked three ways:

1. **By eye against the simulator's syntax**, with the component's default
   properties and again with every property set to a value of its own (so a
   property the netlist ignores, or reads twice in place of its neighbour,
   shows up).
2. **Through ngspice 46** (with the XSPICE code models) as one deck per
   component, and through `qucsator_rf` where a reference behaviour was needed.
3. **Round trips**: schematic save → load, and the properties dialog opened
   and applied without a change, must leave properties and netlist alone.

The harness is `qucs/tests/test_netlist_audit` (part of `ctest`); with
`QUCS_NETLIST_AUDIT=<dir>` it also writes the surveys, and
`scripts/netlist-audit.sh <build> <out>` runs the decks through ngspice. It
covers 211 components in ngspice mode, 202 in Xyce mode and 180 in Qucsator
mode (the panel filters by simulator).

Xyce is not installed here: Xyce findings are from reading the emitted text
against the Xyce reference guide, and say so.

*Status:* sections A and B were fixed in `984660c` (each entry says so);
`qucs/tests/test_netlist_fixes` holds one case per fix. Sections C and D are
open.

---

## A. Wrong netlists (the simulation runs, the answer is wrong)

### A1. 4-terminal transmission line: the two ports pair the wrong pins

> **Fixed** in `984660c` — `T p1 p4 p2 p3`.

`components/tline_4port.cpp` emits

```
T<name> p1 p3 p2 p4 Z0=… TD=…
```

so SPICE's port 1 is (pin 1, pin 3) = (left-top, **right**-bottom) and port 2 is
(pin 2, pin 4). The symbol and the Qucsator model (`tline4p.cpp`: DC short
1↔2 and 3↔4, S-matrix couples (1,4) and (2,3) as ports) have port 1 = pins
(1, 4), the left pair, and port 2 = pins (2, 3), the right pair.

With both bottom pins grounded nothing is visible, which is why it went
unnoticed. Floating use (a balanced line, a line used as a transformer) gives
a different answer. Reference run, left pair driven with 1 V on a 5 V
common-mode offset, right pair loaded with 50 Ω:

| | V(p2) − V(p3) |
|---|---|
| qucsator (`TLIN4P:Line1 p1 p2 p3 p4`) | 1.000 V |
| ngspice, emitted `T p1 p3 p2 p4` | 0.136 V |
| ngspice, `T p1 p4 p2 p3` | 1.000 V |

**Fix:** `.arg(p1).arg(p4).arg(p2).arg(p3)`. Same line is used for Xyce.

### A2. Symmetric transformer: T1 and T2 are applied to the wrong windings

> **Fixed** in `984660c` — W1 is pins 1 and 6 with T1, W2 pins 5 and 4 with T2.

`components/symtrafo.cpp`: winding W1 is built from ports 4 and 3 (pins 5 and
4, the **lower** left winding) with `RATIO = Props[0] = T1`; W2 from ports 0
and 5 (pins 1 and 6, the **upper** winding) with `T2`. The symbol prints T1
next to the upper winding, and `qucsator_rf/strafo.cpp` has
V(1,6) = T1·V(2,3), V(5,4) = T2·V(2,3).

Run with T1 = 2, T2 = 3, 1 V on the right winding:

| | V(1) − V(6) | V(5) − V(4) |
|---|---|---|
| qucsator | 2.00 | 3.00 |
| ngspice from the emitted netlist | 3.00 | 2.00 |

**Fix:** W1 (pins 5, 4) gets `Props[1]` (T2), W2 (pins 1, 6) gets `Props[0]`
(T1) — or swap which pins each X line uses.

### A3. 3 Mutual Inductors: K13 is given the value of k12

> **Fixed** in `984660c`.

`components/mutual2.cpp`:

```cpp
s += QStringLiteral("%1 %2 %3 %4\n").arg(k13).arg(l1).arg(l3)
        .arg(spicecompat::normalize_value(getProperty("k12")->Value));   // k13 wanted
```

With k12 = 14, k13 = 15, k23 = 16 the netlist reads `K13_Tr1 LTr1_L1 LTr1_L3 14`.
The k13 property never reaches SPICE. (L1/L2/L3 node pairs match qucsator's
`mutual2.cpp`: L1 = 1–6, L2 = 5–4, L3 = 2–3.)

### A4. I(TRNOISE): RTSCAPT and RTSEMT are both the value of RTSAM

> **Fixed** in `984660c` — properties 5 and 6; the current also flows with the arrow now.

`spicecomponents/iTRNOISE.cpp` reads `Props.at(4)` three times:

```
Rtsam=15 Rtscapt=16 Rtsemt=17  →  TRNOISE(11 12 13 14 15  15 15)
```

The voltage version (`vTRNOISE.cpp`) reads indices 4, 5, 6 and is right. So an
RTS (burst) noise current source always has capture time = emission time =
RTSAM.

(The same file draws its arrow toward pin 1 like `Idc` but nets `I1 n1 n2`,
where `Idc`/`Iac`/`Ipulse` net `n2 n1`: its current flows against the arrow.
Harmless for noise, but inconsistent.)

### A5. Xyce transient sensitivity: `.TRAN` arguments in the wrong order

> **Fixed** in `984660c` — `.tran <step> <stop> <start>`.

`spicecomponents/sp_sens_tr_xyce.cpp`:

```cpp
s = QStringLiteral(".tran %1 %2 %3").arg(start).arg(stop).arg(step);
```

Xyce's form is `.TRAN <initial step> <final time> [<start time> [<step ceiling>]]`,
so the defaults produce `.tran 0 1M 5U`: initial step 0, output starting at
5 µs, and the user's Step is not the step. The ordinary transient block
(`tr_sim.cpp`) gets it right: `.tran <step> <stop> <start> <step>`.

### A6. Digital source: the pattern is played once, and ends low whatever `init` says

> **Fixed** in `984660c` — `spicecompat::togglingPWL()` builds `PWL(...) r=0` with each change ending at its nominal time (exact period); checked in ngspice.

Qucsator (`digisource.cpp`, `t = t − T·floor(t/T)`) and the VHDL/Verilog
generators repeat the `times` list forever. `components/digi_source.cpp` emits
a single period as a PWL and forces the tail to 0:

```
init=high, times="1ns; 2ns"  →  PWL(0 1 1e-09 1 1.01E-09 0 3E-09 0 3.01E-09 0 3.02E-09 0)
```

From 3 ns on the source stays low; with `init=high` it should have returned
high at 3 ns and repeated. ngspice's `PWL(...) r=0` repeats a PWL from t = 0;
the tail then has to end at exactly T with the initial level.

### A7. Time-controlled switch: an even-numbered time list is not repeated

> **Fixed** in `984660c` — same builder; even lists get `r=0`, the change is at most `MaxDuration`.

Same class as A6. The `time` property says "even numbered lists are repeated"
and `tswitch.cpp` does so; `components/switch.cpp` emits one pass:

```
time="1 ms; 3 ms"  →  VS1 … PWL(0 0 0.00099 0 0.001 1 0.00399 1 0.004 0)
```

and stays off after 4 ms. Also the transition takes 1 % of the *current*
time value (10 µs at 1 ms) — `MaxDuration` (1 µs default) and `Transition`
are ignored, where qucsator uses min(smallest time/100, MaxDuration).

### A8. VDMOS: the model card pins the device temperature even with `UseGlobTemp = yes`

> **Fixed** in `984660c` — `Temp` is excluded from the card.

`components/vdmos.cpp` excludes `Type Thermal Mul UseGlobTemp LibName CompName`
from the model card but not `Temp`, so every VDMOS card ends with
`Temp=26.85 Tnom=26.85`. ngspice accepts an instance parameter in a `.MODEL`
line as the instance default, and it wins over the circuit temperature:

| model card | `.option temp=100` | Id |
|---|---|---|
| without `Temp` | yes | 3.965 mA |
| with `Temp=26.85` | yes | 2.998 mA (= the 26.85 °C value) |

The diode, BJT, JFET and MOSFET netlisters all keep `Temp` out of the card.

### A9. Values without a unit: Qucs `M` is mega, SPICE `M` is milli

> **Fixed** in `984660c` — a bare number with a Qucs prefix maps `M` → `Meg` and `c` → `e-2`; the unit forms accept a sign.

`spicecompat::normalize_value` rewrites `M` → `Meg` only when it recognises a
unit suffix (`Ohm`, `F`, `H`, `V`, `A`, `Hz`, `S`, `s`, `dBm`). A bare value
goes through untouched:

```
R = "10M"      →  R1 n1 n2 10M        (ngspice: 10 mΩ; Qucs GUI, misc::str2num: 10 MΩ)
L = "10 cm"    →  … 10CM              (ngspice: 10; Qucs: 0.1)
R = "-1 MOhm"  →  -1MOHM              (ngspice: −1 mΩ; the regexes need a leading digit)
```

The GUI itself (sweeps, unit display, `str2num`) treats `M` as 1e6 and `c` as
1e-2, so a schematic that reads "10M" simulates 1e9× off in ngspice and Xyce,
silently. The shipped examples avoid bare `M`, which is probably why it has
survived.

### A10. Values that are expressions and start with a digit are not braced

> **Fixed** in `984660c` — braced like the letter-first ones.

Same function: `Rload` becomes `{RLOAD}`, but `2*Rload` (no letter first)
becomes `2*RLOAD`, and ngspice stops with "Error on line …". Users have to
know to write `{2*Rload}` themselves.

---

## B. Netlists the simulator refuses (with default properties)

### B1. Relay: default `Ron = 0` cannot be simulated

> **Fixed** in `984660c` — `Ron ≤ 0` is netlisted as `1e-9` (the switch's default); Xyce's `von` is Vt + Vh.

`.MODEL MOD_S1 sw vt=0.5 vh=0.1 ron=0 roff=1E12` — ngspice's switch takes
1/Ron, and a closed relay makes the operating point fail ("Dynamic gmin
stepping failed … Transient op failed"). Xyce (`vswitch … ron=0`) divides the
same way. Qucsator's relay is fine with 0. The two shipped ngspice examples
that use the relay both changed Ron (to `1e-3` and `1e-20`) — the workaround
users find by themselves. The time-controlled switch defaults to `Ron = 1e-9`
and has no such problem; the relay netlister should clamp 0 to a small value
the same way.

### B2. VDMOS: `RQ=0.0 VQ=0.0` switch on quasi-saturation and break the OP

> **Fixed** in `984660c` — RQ/VQ are emitted only when both are non-zero.

The card always carries `RQ` and `VQ`. In ngspice, giving both (whatever the
value) sets `VDMOSqsGiven`; with the default `Rd = 0` the drain conductance
then becomes 1/0 and the operating point fails — the VDMOS placed from the
panel does not simulate until the user sets Rd > 0 or removes RQ/VQ. Bisected
one parameter at a time: `RQ=0 VQ=0` alone fail, either alone is fine,
`RQ=0 VQ=0 Rd=0.1` is fine. **Fix:** emit RQ/VQ only when both are non-zero.

### B3. JFET for Xyce: `UseGlobTemp={YES}` lands in the model card

> **Fixed** in `984660c`.

`components/jfet.cpp`, Xyce branch of the exclusion list: `"UseGLobTemp"`
(capital L). The property is then netlisted like any other:

```
.MODEL JMOD_T1 NJF (VtO=-2.0V … Tnom=26.85 UseGlobTemp={YES} )
```

Xyce rejects unknown model parameters. The ngspice branch spells it right.
*(Xyce not run here.)*

### B4. Components offered in Xyce mode whose netlist is XSPICE

> **Fixed** in `984660c` — their `Simulator` masks drop Xyce.

Registration filters the panel by `Component::Simulator`, and these carry the
Xyce bit but emit XSPICE `A` devices and `.MODEL … filesource/s_xfer/pwl`
cards, which Xyce does not have:

| component | `Simulator` | emits |
|---|---|---|
| File Based Voltage/Current Source (`Vfile`, `Ifile`) | simAll | `AV1 %vd([n1 n2]) mod_… filesource(…)` |
| XSPICE devices / SDTF | ngspice+xyce+spiceopus | `.MODEL … s_xfer (…)` |
| XSPICE devices / XAPWL | ngspice+xyce+spiceopus | `.MODEL … pwl (…)` |
| XSPICE generic device | ngspice+xyce+spiceopus | `A1 %v(n1) …` |

The digital gates and flip-flops handle this the other way — see C2.
*(Xyce not run here.)*

---

## C. Silently dropped

### C1. Equation Defined Device, implicit type

`EqnDefined::spice_netlist` returns an empty string for `Type = implicit`.
`SpiceModel` is `B`, so the kernel's compatibility check does not flag it
either: the device just disappears from the ngspice/Xyce netlist.

### C2. Digital gates and flip-flops in Xyce mode

`GateComponent::spice_netlist` returns `QString()` for Xyce; `d_dff` and the
others likewise. The gate is gone from the netlist with no message.

### C3. Winding without its core

`magnetics/winding.cpp` looks the `CORE` component up by name in the schematic;
when it is not there the H and B node names are empty and the line reads
`X_W1 n1 n2   winding N=10 Rs=0.1`. ngspice fails on the subcircuit's pin
count, with nothing pointing at the missing core.

### C4. Properties the SPICE netlist ignores although the dialog shows them

Found by the marker survey (`marker_audit.txt`). Not counting Qucsator-only
properties that are hidden by their `simulators` mask (the transient block's
`reltol`, `IntegrationMethod`, … are masked correctly).

| component | ignored | note |
|---|---|---|
| dc simulation `.DC` | `Temp reltol abstol vntol MaxIter` | shown for every simulator, only `op` is emitted; no `.options` / `.temp` |
| ac simulation `.AC` | `Noise` | ngspice has `.noise`, not an AC flag |
| Diode | `Isr Nr` (ngspice and Xyce have ISR, NR), `Cp` | |
| JFET | `Betatce` (ngspice BETATCE), `Xti`, `N`, `Isr`, `Nr`, `M` | |
| MOSFET | `Nrd Nrs` (SPICE instance params), `Rg`, `N`, `Tt` | |
| Transmission Line, 4-terminal line | `Alpha` | a lossy spec becomes lossless |
| Switch (time) | `MaxDuration Transition` | see A7 |
| Inductor/Capacitor with Q | `Mode` | only the Linear (constant R) form |
| Potentiometer | `Taper_Coeff Conformity Linearity Contact_Res Temp_Coeff` | |
| VCCS/CCCS/VCVS/CCVS | `T` (delay) | |
| Power Source | `Temp` | |
| Coupled Transmission Line | — | ngspice/Qucsator only, correct |
| Twisted-Pair | everything | no SPICE model at all; `SpiceModel` is empty so the kernel reports it |

---

## D. Lower severity

* **Relay for Xyce** (`relais.cpp`): `von = Vt`, `voff = Vt − Vh`. The `sw`
  semantics it mirrors for ngspice are on above Vt + Vh and off below Vt − Vh,
  so the Xyce relay closes at 0.5 V where the ngspice one closes at 0.6 V.
* **Time-controlled switch for Xyce** emits `.model … sw vt= ron= roff=`, the
  ngspice model type; the relay's Xyce branch deliberately emits `vswitch`
  instead. Whether current Xyce accepts `sw` was not checked here (no Xyce).
* **`.CSPARAM` section** carries the Xyce bit; `.CSPARAM` is ngspice-only.
* **Qucsator netlist of the 3-pin BJT and MOSFET** (`BJTsub`, `MOSFET_sub`)
  includes `UseGlobTemp="yes" LibName="Generic" CompName="Generic"`;
  `qucsator_rf` prints three "extraneous property" checker warnings per
  device and carries on. The 4-pin variants and `Component::netlist()` strip
  them.
* **I(SFFM)** has `Model = "I"` (the file line is `<I I1 …>`), default
  `Fc = 1` with `Fs = 500`, which ngspice reports as "MDI limited to FC/FM";
  V(SFFM)'s defaults (Fc = 1k, Mdi = 10, Fs = 500) trigger the same warning.
  `I0`/`Ia` values carry a trailing space.
* **SPICE library device** with an empty `File` emits `.INCLUDE "<cwd>"`.
* **Subcircuit with an empty `File`** called `misc::properName("")`, which
  indexed an empty string (a Qt assertion in debug builds; undefined
  behaviour otherwise). *Fixed in this commit:* `properName` returns the empty
  name.
* **Voltage probe** adds `R… n1 n2 1E8` across the probed nodes — a 100 MΩ
  load; documented behaviour, but worth knowing for high-impedance nodes.

---

## E. Checked and found right

So the next hunt does not repeat them: node order and polarity of R, C, L,
IndQ/CapQ (series/parallel loss formulas), potentiometer (B–W = R·Rot/Max),
MUT and N mutual inductors (dots match qucsator's voltage-source orientation),
Transformer (XFMR pin order and RATIO), gyrator (both B-source equations
against `gyrator.cpp`), current/voltage probes, Vdc/Idc/Vac/Iac (SIN phase and
`AC mag ACPHASE deg` both verified in ngspice), Vpulse/Ipulse/Vrect/Irect
(PW and PER arithmetic), Vexp/Iexp, AM sources (ngspice ≥ 40 argument order
`AM(VO VMO VMA FM FC TD)` verified by sampling the waveform), SFFM, TRRANDOM,
V(TRNOISE), Pac (ngspice `portnum … z0` carries the source impedance: 0 dBm
gives 0.316 V across a matched 50 Ω), all four controlled sources (numerically
identical to qucsator), Diode/BJT/JFET/MOSFET pin order and model cards (parse
and run in ngspice; `Cj0` accepted; Tbv → `Tcv`/`Tbv1`, Trs → `Trs`/`Trs1`),
OpAmp (input polarity, `u()`/`stp()`), EDD explicit (with the G–L–B charge
trick), TLIN (`TD = L/c`), RLCG (LTRA card), COAX and its subcircuit, MLIN/
MCOUPLED/MOPEN XSPICE cards, all XSPICE digital gates and flip-flops (load and
run with the code models), Subcircuit, S-parameter file wrapper, every
simulation block (`.TR` step arithmetic and UIC, `.AC`/`.SP` lin/log, `.SW`
control loop, `.FOUR`, `.NOISE`, `.FFT` step/stop from BW and dF, `.DISTO`,
`.PZ`, `.SENS`, `.SENS_AC`, Xyce `.HB`/`.LIN`/`.STEP`), the `.PARAM`,
`.CSPARAM`, `.GLOBAL_PARAM`, `.IC`, `.NODESET`, `.FUNC`, `.MODEL`, `.INCLUDE`,
`.LIB`, Nutmeg equation and `.OPTIONS` sections (the latter after the fix in
`d4c6746`), and every component's two round trips (file, dialog) under
ngspice, Xyce and Qucsator settings.

---

## Reproducing

```bash
cmake -S qucs-s-26.1.1 -B build && ninja -C build test_netlist_audit
NGSPICE_CM_DIR=/path/to/ngspice/codemodels scripts/netlist-audit.sh build /tmp/audit
```

`/tmp/audit/netlist_audit_ngspice.txt` is the survey (with the file line of
each component, handy for writing scenario schematics), `marker_audit.txt`
the ignored/repeated properties, `ngspice_complaints.txt` what the simulator
said per deck. The one-off reference runs above (TLIN4P, sTr, VDMOS bisect,
relay, AM sampling, controlled sources) are small hand-written decks; their
text is in the sections that cite them.
