# A table right after a line of text, in the Claude panel

The review of 2026-10-18 (`~/Desktop/qucs-reviews/2026-10-18-claude-panel-table-after-text-line.md`) reported that two tables in one reply in the Claude panel showed as raw pipes, while a third rendered. Each broken one had its header row right under a line of text:

```
**Archived:**
| File | Why it counts as closed |
|---|---|
```

GitHub renders all three as tables, and a "**Label:**" line above a table is common in replies.

## The cause

`qucs_s::markdown::setMarkdown` hands the text to `QTextDocument::setMarkdown(…, MarkdownDialectGitHub)`, and Qt reads it with md4c. md4c starts a table only where the header row is its paragraph's only line. A header row under a line of text continues that paragraph, so the table becomes a paragraph of pipes. GitHub's reader takes the paragraph's last line as the header instead.

The review's repro (plain Qt 6.11.2, no Qucs-S) gives the same counts here:

| Case | Above the table | Tables |
|---|---|---|
| A | `**Head:**`, a blank line | 1 |
| B | `**Head:**` | **0** |
| C | two lines of text | **0** |
| D | `## Head` | 1 |
| E | `- item` | **0** |
| F | `- item`, a blank line | 1 |
| G | nothing | 1 |

The review assumed the panel's own preprocessing changes nothing here. It doesn't: with the fix taken out, case B through the panel's `setMarkdown` makes no table.

## The fix

`tablesApart` (`mathtypeset.cpp`) runs first in `setMarkdown`, before `htmlBalanced` and `htmlBlocksMarked`. It puts a blank line before a table's header row when the line above it is text: a paragraph's line or a list item's text. Everywhere else the text is left as it was. The checks the review asked for:

- **A header row** is a line with a pipe whose next line is a delimiter row: dashes, a colon at either end, pipes. The delimiter row must have as many cells as the header, so a row with fewer is not a table and gets no blank line, as in GFM. Cells are counted GitHub's way: a pipe splits cells unless it is escaped, so `a \| b | c` has two cells.
- **Pipes in a code span** count as cells, as GitHub counts them. `` `a|b` | c `` over a delimiter of two cells is no table on GitHub either, and is left alone.
- **Not the line above.** A heading, a fence, a list item's start, an HTML block's start or a rule is not text a header row can continue.
- **Left alone:**
  - fenced code, backticks and tildes, including a fence after a list marker;
  - an HTML block;
  - a table's own body: a delimiter-like row inside it is a row, on GitHub too.
- **In a quote,** the line put in is the quote's marks (`>`, or `> >` nested), so the table stays in the quote.
- **In a list item,** a blank line keeps the indented table in the item.
- **One linear pass.** The panel draws a reply again every 40 ms while it streams.

The Markdown documents' preview reads through the same `setMarkdown`, so it gets the fix too, as GitHub shows such files.

## Tests

`test_claude_code aTableRightAfterTextIsATable` (new) checks seventeen cases, counting the tables `setMarkdown` makes:
- the review's A to G, each now one table;
- a quote's text above, and a nested quote;
- a list item with the table indented in it;
- two labelled tables;
- in a backtick fence, and in a tilde fence in a list item: none;
- a delimiter of fewer cells: none;
- a code span's pipe: none;
- an escaped pipe: one;
- a setext heading: none;
- an HTML block: none.

The test also checks:
- **The text put in, exactly.** A blank line; in a quote, its marks. Text that needs nothing (a table alone, one with a blank line above, a heading above, a fence, a short delimiter, no pipes) comes back unchanged.
- **The label stays apart.** "Archived:" stays its own paragraph and the table holds the cells.
- **The cost.** 2,000 labelled tables in 4 ms, 8,000 in 16 ms; the test asks for under eight times, so it holds on CI's sanitized build too.

## Breaks and runs

- **Break:** with `tablesApart` taken out of `setMarkdown`, case B fails (no table).
- **The `test_markdown_doc` fuzz** ran on the sanitized build for three seeds of 60 s each, with no report. Its documents go through `tablesApart`.
- **The rest** ran with the hunt's fixes (`2026-10-18-claude-rhi-hunt-fixes.md`): the Release suite 128 of 128, and test_claude_code and test_markdown_doc under the sanitizers.
