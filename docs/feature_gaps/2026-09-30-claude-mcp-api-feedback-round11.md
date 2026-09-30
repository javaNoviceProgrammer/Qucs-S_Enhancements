# Feature gaps: Qucs-S's MCP tools, the eleventh round of feedback

*30 September 2026 — Qucs-S 26.1.4.*

Claude verified `9329124` with the Qucs-S window open
(`qucs-s-mcp-round11-verification-2026-09-30.md`). The round-10 findings are
fixed: a preview of a diagram on a missing data display now says it would
make one and leaves nothing behind; an untitled schematic runs in
`Scratch/untitled`; a schematic outside the project runs in a folder of its
own, not the project's. `clean_scratch` on that folder trashed it whole,
which the reviewer judged right, since it belongs to that schematic alone.
The scenario script passed, s1 to s9, 71 checks. One new finding.

## The finding

| Feedback | Now |
|---|---|
| **`max_chars` is refused on a direct call from the panel.** `get_netlist … max_chars=1000` answered "max_chars is a whole number of characters, 200 or more"; the same value inside a `batch` worked. No tool's schema declares `max_chars` (the server takes it out of the arguments before the tool sees it), so the client has no type for it and sends the digits as a text, `"1000"`, and `readMaxChars` took only a JSON number. | Reproduced by sending `"1000"`: refused, while `1000` worked. `readMaxChars` now also takes a text of digits (spaces around them allowed), with the same rule: a whole number, 200 or more. `"1000"` and `" 400 "` work as the numbers do, in a direct call and in a `batch`'s call. `"abc"`, `"4e3"`, `"-500"` and `"150"` are still refused, and the refusal now says what was given: "max_chars is a whole number of characters, 200 or more (1000, or \"1000\"), not the text \"abc\". Nothing was done." The same text comes from a `batch`'s call. The round-8 test had checked that `"4000"` was refused; it now checks that a text of digits cuts exactly as the number does. |

The other suggestion, declaring `max_chars` in every tool's schema, is not
taken. It would add the same property to 67 schemas sent every turn, and the
instructions already say once that every tool takes it. With a text of
digits accepted, a client that sends it untyped is served either way.

## Tests

- **`test_mcp_server`**, `everyToolTakesMaxChars`: `"300"` and `" 300 "` cut
  `get_netlist` exactly as `300` does; `"300"` in a `batch`'s call cuts it;
  `"abc"` there is refused, naming it. 50, 2500.5, `"abc"`, `"4e3"`,
  `"-500"`, `"150"` and `true` are refused on a direct call, each named, and
  nothing is done.
- **`scripts/mcp-e2e-scenarios.py`**: s1 to s9, 71 checks, none failed.

Each change was broken on purpose, five breaks in all, and each is caught: a
text of digits refused, any text read as a number, a value under 200 taken,
the server's refusal and a `batch` call's refusal back to the old words.

The full suite passes (79). `~/QucsWorkspace` and `~/Library/Caches` are as
they were before the runs (checked against a marker file).

The tools sent every turn are as they were.
