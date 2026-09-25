# Bug hunts

Systematic sweeps over one area of Qucs-S, each written up with what was
checked, how, what was found (with the evidence and a proposed fix) and what
was found right — so the next sweep can start where the last one stopped.
Findings here are documented, not necessarily fixed; a fix references the
entry it closes.

| date | area | report |
|---|---|---|
| 2026-09-21 | every built-in component and the netlist it generates (ngspice, Xyce, Qucsator) | [2026-09-21-component-netlists.md](2026-09-21-component-netlists.md) — A/B fixed in `984660c`, C/D in `db4f676` |
| 2026-09-24 | stress: a random walk over the main window, schematics far beyond the usual size, the fuzzers again | [2026-09-24-stress.md](2026-09-24-stress.md) — crashes and hangs fixed in `53bbb26` (C); A (quadratic edits and loads) and B (healer invariants) open |
