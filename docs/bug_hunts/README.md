# Bug hunts

Systematic sweeps over one area of Qucs-S, each written up with what was
checked, how, what was found (with the evidence and a proposed fix) and what
was found right — so the next sweep can start where the last one stopped.
Findings here are documented, not necessarily fixed; a fix references the
entry it closes.

| date | area | report |
|---|---|---|
| 2026-09-21 | every built-in component and the netlist it generates (ngspice, Xyce, Qucsator) | [2026-09-21-component-netlists.md](2026-09-21-component-netlists.md) — A/B fixed in `984660c`, C/D in `db4f676` |
| 2026-09-24 | stress: a random walk over the main window, schematics far beyond the usual size, the fuzzers again | [2026-09-24-stress.md](2026-09-24-stress.md) — crashes and hangs fixed in `53bbb26` (C); A (quadratic edits and loads) fixed in `46332a5`; B (healer invariants) in `ef4694f` |
| 2026-09-26 | the new features: paintings, File Browser drag and drop, "Any folder is a project", the Claude Code dock and its git bar, spreadsheets, Markdown and PDF documents (one hour: probes, fuzzing, the monkey, reading) | [2026-09-26-new-features.md](2026-09-26-new-features.md) — 29 findings; F1 (CI timeout) fixed in `7c8dd74`, A (data lost or corrupted) in `119974d`, B (security) in `01ccb44`, C (memory and time) in `c9e7afc`, D and E in `ef4694f` |
| 2026-09-28 | the Claude tools (the MCP server), and the changes of `e6df622` to them (one hour: a tool fuzzer on the ASan build, pipelined calls, probes, reading) | [2026-09-28-claude-tools.md](2026-09-28-claude-tools.md) — 32 findings and 14 minor notes; all fixed in `1be88f0` |
| 2026-09-29 | Check Schematic, the electrical rule check: every shipped example for its own simulator with the ground required and not, ngspice asked where the check assumed, the rules read against the netlister | [2026-09-29-schematic-check.md](2026-09-29-schematic-check.md) — 7 findings (A1-A4, B1-B2, C1) fixed in `5d7c0cf`; the examples of D left as they are |
