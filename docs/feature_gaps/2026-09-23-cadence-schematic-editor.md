# Feature gaps: what Cadence's schematic editors have and Qucs-S lacks

*23 September 2026 — Qucs-S 26.1.2 (`71e1236`).*

A list of the features of Cadence's schematic tools that Qucs-S does not
have, as a menu for the next enhancements. "Cadence" covers Virtuoso
Schematic Editor (IC design), OrCAD Capture (boards) and their simulation
front ends, ADE and PSpice. Each gap was checked against the Qucs-S source
(no bus syntax, no global nets other than ground, no no-connect marker, no
reference renumbering, no bill of materials) and against what this
repository has already added (Check Schematic, net highlighting, operating
points, find and replace, CDL export, ...).

Effort: **S** days, **M** a week or two, **L** more.

## Connectivity

1. **Buses** (L) — bus nets written `D<7:0>`, bus rippers, and arrays of
   instances such as `I1<3:0>` (Virtuoso) or bus entries (Capture). Qucs-S
   has no buses: every bit is a wire of its own. The largest gap, and it
   touches the netlister throughout.
2. **Global nets and power symbols** (M) — a net called `vdd!`, or a VDD
   power symbol, is the same net on every sheet and inside every
   subcircuit. In Qucs-S only ground works this way; a supply has to be
   brought into a subcircuit through a port.
3. **No-connect marker** (S) — an X on a pin that is meant to stay open, so
   Check Schematic stops reporting it.
4. **Multi-sheet designs** (M–L) — one circuit spread over several sheets,
   joined by off-page connectors (Capture). Qucs-S has one sheet per file.

## Hierarchy

5. **A view per instance** (M) — one subcircuit symbol can stand for the
   transistor schematic, a Verilog-A model or a SPICE model, chosen per
   instance without editing the parent (Virtuoso's configurations and
   Hierarchy Editor). In Qucs-S a symbol points to one file.
6. **Descending into one instance** (M) — opening a subcircuit shows that
   instance's parameter values and its operating point. Descending in
   Qucs-S opens the file with no instance context.
7. **Net highlighting through the hierarchy** (M) — a highlighted net
   lights up through the ports into the subcircuits. Ours stops at the
   sheet's edge.
8. **Hierarchy tree** (S–M) — a searchable tree of the design's instances.
   The Content panel lists files, not instances.

## Editing

9. **A docked property editor** (M) — stays open, edits several selected
   parts at once, and has a spreadsheet view of every part's properties
   (Virtuoso's Edit Object Properties, Capture's Property Editor). Qucs-S
   has a modal dialog per part; a bulk edit goes through Find and Replace.
10. **Renumbering reference names** (S) — renumbers R1…Rn in order and
    fixes duplicates (Capture's Annotate). Qucs-S names a part when it is
    placed and only reports duplicates.
11. **Selection filter** (S) — select only wires, only instances or only
    labels, or select by a property value.
12. **Scripting the editor** (L) — Cadence has SKILL (and Tcl in Capture).
    Our Python dock cannot place parts, set values or run a simulation.

## Checks

13. **Checks that use pin directions** (M) — two outputs driving each
    other, an input nothing drives, and a symbol whose pins do not match
    its schematic (Virtuoso's cross-view check). Check Schematic is
    structural only: names, open pins, ground, the simulation block.

## Simulation environment (ADE / PSpice)

14. **Probes that plot** (M) — drop a voltage or current marker on a net or
    a pin and it is plotted after the run (PSpice markers), or click a net
    afterwards to plot it (ADE's direct plot). A Qucs-S probe only creates
    a variable; a diagram then has to be placed and the variable picked.
15. **Run history and overlays** (M) — earlier runs are kept and can be
    overlaid on the current one. Qucs-S overwrites the dataset.
16. **Measurements with pass/fail** (M) — expressions such as bandwidth,
    overshoot or phase margin, evaluated after every run and shown in a
    table against their specs (ADE outputs and specs).
17. **Waveform calculator and viewer tools** (M) — delta markers, pan, a
    library of measurement functions, eye diagrams. Qucs-S has markers and
    drag-to-zoom.
18. **Corners and Monte Carlo** — *covered.* The custom ngspice
    (Ngspice_OpenVAF_Enhancements) has both as commands, `corners` over the
    corners the Verilog-A models declare and a packaged `montecarlo` with
    specs and a yield, and Qucs-S has them as the NgCorners and
    NgMonteCarlo simulation components: the values and waveforms per
    corner or per sample in the dataset, a histogram per recorded value,
    the yield in the status log.

## Board design (OrCAD Capture)

19. **Bill of materials, footprints, PCB netlists and variants** (M) — a
    parts list, a footprint property per part, a netlist for PCB tools and
    assembly variants. Qucs-S exports a CDL netlist, for IC tools, and
    nothing for PCB tools.

## What to do first

By value against effort, and by what the code already has:

- **Probes that plot (14)** — the largest everyday gain; it builds on the
  existing probe components and diagrams.
- **No-connect marker (3) and renumbering (10)** — quick, and they round
  off Check Schematic.
- **Global nets and power symbols (2)** — removes a real annoyance in
  hierarchical designs.
- **Measurements with pass/fail (16) with run history (15)** — together an
  ADE-style testbench.
- **Buses (1)** — the most Cadence-like feature and the most expensive; best
  done on its own once the others are in.
