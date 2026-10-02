# Feature gaps: `edit_component` with `flags` alone

*2 October 2026 - Qucs-S 26.1.5.*

Claude found that `edit_component` given only `flags` answered as done and changed nothing
(`qucs-s-edit-component-flags-bug.md`, severity low). It was reproduced first with the report's
calls, made in `test_qucs_control` to the same tool code `qucs-s --mcp-server` runs:

- the answer listed `"equations": ["temp=27"]`;
- the block was as before, so the netlist had no `.OPTION noinit`.

## The bug and the suggested tests

| Report | Now |
|---|---|
| `edit_component {"name": "SpiceOptions1", "flags": ["noinit"]}` answers with success and changes nothing. `equations: [{"noinit": true}]` works, as does `flags` with another argument beside it. The cause: the test before `setListsOf` in `editComponent` named `equations`, `records` and `specs`, not `flags`. | **Fixed.** `flags` alone sets the option: the answer lists it, the netlist has `.OPTION noinit`, and one `undo` takes it out. |
| Fix: one helper that says whether a call has list arguments, so the test and `setListsOf` cannot drift apart. | Done as `givesLists()`: `equations`, `flags`, `records`, `specs`. `editComponent`'s test and `setListsOf` both ask it. |
| Test 1: `flags` alone on a block with `temp=27`. | `flagsAloneAreSet`: the answer's equations are `["temp=27", "noinit"]`, and the netlist has `.OPTION temp = 27` and `.OPTION noinit`. |
| Test 2: `flags` alone on an empty block. | The same test: a SpiceOptions placed with no equations, then `flags: ["keepopinfo"]`, which the netlist has. |
| Test 3: `flags` with `equations: ["reltol=1e-4"]`. | The same test: both applied. |
| Test 4: undo. | The same test: one `undo` gives the schematic back as it was, and a netlist without `noinit`. |
| Test 5: each argument `edit_component` takes, given alone, either changes the component or is refused. | `eachArgumentAloneChangesThePart`. It reads the arguments from the tool's own schema (`describe_tool`), so an argument added later fails the test until it is given a value there. Each argument alone either changes the schematic, and one `undo` gives it back as it was, or is refused, leaving the schematic as it was. The test found a second argument that did nothing (below). |

## Found while fixing

- **`replace_equations` alone did nothing either.** It makes an equation block's list exactly
  the `equations` given with it. Given alone, `setListsOf` returned before reading it, and the
  call answered as done.
  - Now refused without `equations` or `flags`, by `set_schematic`, `add_component`,
    `edit_component` and `replace_component`: "'replace_equations' goes with 'equations' or
    'flags': it makes the block's list exactly those given."
  - With `flags` alone, it makes the block exactly those flags.
  - Its description in the tools' schemas says so.

Each of the other 14 arguments changes the part when given alone: `rename`, `properties`, `x`,
`y`, `near`, `rotation`, `mirror`, `active`, `shown`, `name_shown`, `text_at`, `equations`,
`records`, `specs`.

## Checked

- Each part of the fix was broken on purpose, 6 breaks, and each was caught:
  - `editComponent`'s test as it was;
  - `flags` left out of `givesLists()`;
  - `records` left out of `givesLists()`;
  - the `replace_equations` refusal turned off;
  - `shown`, `name_shown` and `text_at` not applied: the general test names `name_shown`;
  - `active` not applied: the general test names it.
- The suite passes 85/85, the new tests under ASan and UBSan, and the e2e scenarios 77/77.
- Run after the rest of `test_qucs_control`, the general test first failed on `active`:
  - the schematic after the `undo` differed only in its `<View=...>` line, which records
    where the view is scrolled to;
  - an undo puts the parts back, not the scroll;
  - the tests now compare the schematic without that line.

## Workaround

Not needed with this build. Before it, the options were `equations: [{"noinit": true}]`, or
another argument in the same call.
