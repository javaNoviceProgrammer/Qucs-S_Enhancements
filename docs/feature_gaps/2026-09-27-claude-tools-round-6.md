# Feature gaps: Claude's Qucs-S tools, round 6

*27 September 2026 — Qucs-S 26.1.3.*

Claude worked in `project1_prj` on an op-amp inverter, a Verilog-A ideal
op-amp (`va/ideal_opamp.va`), a SPICE subcircuit wrapper for it
(`va_opamp.sch`) and an inverter built on the wrapper. Everything got done,
and it wrote down the places where one tool argument should have been
enough and was not (`qucs-mcp-wishlist.md`). Each wish was checked against
the source; the tables say what was done.

## The five gaps

| Wish | Now |
|---|---|
| **1. Flag options in equation blocks.** An `.OPTIONS` option with an empty value is written alone (`.OPTION noinit`), but `equations` took `""` for "take it away": `{"noinit": ""}` added nothing, and the line had to be rewritten with `set_schematic`. | `add_component` and `edit_component` take `flags` (`["noinit", "keepopinfo"]`). In `equations`, `true` means a flag (`{"noinit": true}`), and so does a name alone in a list (`["noinit", "reltol=1e-4"]`). `""`, `null` or `false` still take one away. Only an ngspice `.OPTIONS` block (`SpiceOptions`) takes a flag; the other blocks would write `name=`, which is not an equation, so they refuse with that reason. `get_schematic` marks a flag `"flag": true`, and `describe_component_type SpiceOptions` explains flags. |
| **2. Port names drawn on subcircuit symbols.** An instance drew each pin's name (the net's, from the `.PortSym` line) unless the symbol had a text of exactly that string. A symbol drawn with `+` and `-` still showed `inp` and `inn` over them. | A symbol's port has a **label**: what its instances draw beside the pin instead of the name. `"+"` for `inp`, `""` for nothing, `null` for the name again. `edit_painting` sets it on a port (`label`), `get_schematic` lists it, and an instance's pins in the tools' answers carry it. It is saved after the name, in quotes (`.PortSym 0 0 1 180 inp "+"`). The name stays the net's, which the netlist and the tools keep using. In the symbol editor, a double-click on a port asks what its instances show (the name, nothing, or a text), and the port reads "inp (shown as +)". An older Qucs-S shows the extra field beside the port in its symbol editor and drops it when it saves the symbol. |
| **3. The symbol view blocked the schematic tools.** After `add_painting` with `symbol: true`, every schematic tool refused ("shows its symbol"), and the error named "Edit Circuit Symbol", which the action is not called then. | A tool that changes the schematic switches the document back itself (as *File → Edit Schematic*, F9, does) and says so after its result: "… showed its symbol: it shows its schematic again, where this change is made (File > Edit Schematic, F9, switches between the two; the painting tools take 'symbol': true for the symbol)". A symbol file (`.sym`), which has nothing else, is refused with that reason. |
| **4. A first Verilog-A draft that compiles.** An attribute after a parameter (`parameter real gain = 1e6 (* desc="..." *);`) fails in OpenVAF, and there was nothing to start from. | `describe_component_type` with `"Verilog-A"` (or `verilog_a`, `new Verilog-A module`) gives a module to start from, with rules. The rules cover attributes before the declaration (OpenVAF's error when they are not), `desc`, `units`, `type = "instance"`, ranges, potential and flow contributions (a series resistance via the branch's own current), a DC path for every node, and the includes. Then the steps: save it, `build_verilog_a`, `describe_component_type` with the module's name. The template is an amplifier (`amp`: `inp`, `inn`, `out`) with an instance parameter. It compiles with OpenVAF as it is. In ngspice, an inverter of gain −10 built with it gives −1.0 V for 0.1 V in, without a warning. Without its input resistance, ngspice warns "no DC path from node … to ground" for an input reached only through a capacitor, which is what the rule explains. |
| **5. Knowing that instances took a changed symbol.** Only a screenshot showed whether an open schematic's instance had refreshed. | `save_document` of a subcircuit says which open schematics' instances took the new symbol ("Its instances took the new symbol: usestwopin.sch (X1)"). It says each pin that moved, from where to where, and whether it no longer meets its wiring ("X1.1 (a) in usestwopin.sch moved from … to … and no longer meets its wiring: connect it again, or move X1"). Otherwise it says "Their pins are where they were." |

## The smaller wishes

| Wish | Now |
|---|---|
| **Replace a component keeping its pin geometry.** Swapping the built-in `OpAmp` for a `Sub` meant reading pin offsets, choosing a mirror and rewiring by hand. | **`replace_component`** puts a part of another type in one's place, and its pins take the old pins' nets. `pins` maps them, old to new, by number or name (`{"1": "inn", "2": "inp", "3": "out"}` for the OpAmp, whose pin 1 is its − input). Without it they go by name when the old pins' names are all the new part's, else by number. It keeps the old name, so traces and equations that name it stay right (`rename` for another). It takes properties, equations and flags as `add_component` does. The part is placed as follows. The candidates are each turn and mirroring, at the old centre, so that one pin lands on its old place, and a few grid steps aside from those. They are tried in order: fewest other parts under the new symbol, fewest pins off their places, least distance, then the old part's own turn. For each, the old pins' leftover wires are taken back to where they meet something, and each new pin is wired to its net by a way that joins nothing else. The first way with no wire across the new symbol and nothing new for Check Schematic is kept; else the best of the first ten. A way is kept only if every net is as it was, checked as `edit_component` checks a turn. `rotation`, `mirror`, `x` and `y` choose the placement instead. It is refused when an old pin with something on it has no new pin, or when no way keeps the nets. The result is one step to undo, and it says which old pin went to which new one and how the part was placed. |
| **Text bounds of a component.** Moving `X1`'s label off a wire needed screenshots. | Each component in `get_schematic` (and in `add_component`'s and `edit_component`'s answers) has `texts`: its name and each property shown, each with its box `[x1, y1, x2, y2]` on the schematic. They are laid out as Qucs-S draws them, one under the other from the text's corner, for the simulator in the settings. |
| **Hidden properties that reach the netlist.** The OpAmp's `Umax` (15 V) clips its output and surprises people. | `describe_component_type` finds, for any type, the properties hidden on the schematic that the netlist line uses all the same. Each is given another value in turn and the line compared, so nothing is listed by hand. They are under `netlist` → `hidden but in it`, each with its default and the part of the line it changes, and a `hidden properties` line names them: for the OpAmp, `Umax`. |
| **Component-line field count on load.** `set_schematic` should warn of a line with more values than its type has properties. | `set_schematic` has refused such a line since round 5. A file opened with `open_document` is now told of too: "Look: R1 (R) has 8 property values, its type 6 properties: the last 2 were left out …". A file of an older Qucs may simply carry values its type has since dropped, so it is opened and not refused. |

## Tests

`test_qucs_control` (69 functions), new in this round:

- `optionsTakeFlags`: flags by `flags`, by `true`, by a name alone; taken
  away by `""` and `false`; `.OPTION noinit` in the block's netlist; marked
  in `get_schematic`; refused on `Eqn` and `R`.
- `aPortShowsALabel`: the line's name and label read and written; labels set
  with `edit_painting`, listed, saved in quotes, taken by an instance
  (`+`, `-`, nothing) and put back to the name; the symbol editor's dialog
  (a text, nothing, cancelled).
- `theSchematicToolsLeaveTheSymbol`: `add_component` on a document that
  shows its symbol switches it back and says so, once. `aSymbolIsDrawnToo`
  (round 3) expected the refusal; it now expects the switch.
- `aVerilogAModuleToStartFrom`: the template by each name, its rules, every
  attribute before its declaration. Where OpenVAF is installed, it is
  compiled with `build_verilog_a` and described (its instance parameter).
- `aSavedSymbolIsTakenByItsInstances`: an instance refreshed with its pins
  where they were, then a port moved and the pin said to be off its wire.
- `aComponentIsReplacedKeepingItsNets`: an OpAmp replaced by a subcircuit,
  each pin on its old net, no wire across the new symbol, the subcircuit's
  netlist line with its nodes in order (`XOP1 plus minus vo`), undone in
  one step. Also refusals (a pin the new part has not, a wired pin left
  out, a type there is not) and a resistor replaced in place by number.
  `replace_component` is also among the tools the fuzz test calls with odd
  arguments.
- `aComponentsTextsHaveTheirBoxes`, `hiddenPropertiesInTheNetlistAreFlagged`
  and `aLineWithAValueTooManyIsSaidOnOpening`.

Each new piece was taken out in turn and the tests run again: 15 breaks,
14 caught. The one not caught is taking the old pins' leftover wires back
before the new pins are wired. With the placement order as it is, the
test's circuit comes out the same either way, so that step is kept for
cleaner routes but is not proved by a test.

The tests found a crash in `replace_component` as first written: its pin
map iterated over two temporary copies of the JSON object. They also found
placements that ran a wire across the new symbol. A look at the pictures
led to the placement search and to the old pins' stubs being taken back
first.
