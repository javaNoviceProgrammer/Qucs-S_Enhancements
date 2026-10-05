# Feature gaps: a `<br>` in a Claude reply blanked the rest of it

*5 October 2026 - Qucs-S 26.1.5.*

## What the review found

The review (`Qucs-S_claude_panel_br_tags_2026-10-05.md`) looked at `dc8ae63` with
Qt 6.11.2 from Homebrew. In a Claude reply, a table's cells used `<br>` to hold
several lines. In the panel, everything after the first `<br>` broke:
- the table's other rows lost cells, and their neighbours ran together;
- every bullet and paragraph after the table lost its text, all but its inline code.

The Markdown was valid GitHub Markdown, and GitHub and Claude Code's terminal show
`<br>` as a line break.

The cause is in Qt. Its Markdown importer (`QTextDocument::setMarkdown`, the GitHub
dialect) takes an HTML void element written without the closing slash for a tag that
never closes. It holds the rest of the document aside as HTML, then drops it.

## Reproduced

The review's case reproduced on `dc8ae63` with a few lines of Qt alone: `setMarkdown`,
then `toPlainText`. Each case was followed by `NEXT paragraph **bold**` and a list with
inline code.

| Markdown | After it |
|---|---|
| `a<br>b`, `a<BR>b`, `a<hr>b`, `a<wbr>b`, `a<img src="x.png">b` | only `a`, and the list's `code`: the rest lost |
| `a<br/>b`, `a<br />b` | everything |
| `a<b>bold</b>b`, `x < y and y > z` | everything |
| a table with `one<br>two` in a cell | `A B one x`, then nothing but `code` |
| the same table with `one<br/>two` | everything, the cell on two lines |

The same Markdown goes to Qt in three places:
- the panel's drawing of a reply (`renderMarkdown`);
- its text form (`plainTextOf`: the Text export, and a reply copied as text);
- the Markdown file viewer (`markdowndoc.cpp`).

## What was done

| Review | Now |
|---|---|
| **Write each HTML void element self-closed before every `setMarkdown`**: `area`, `base`, `br`, `col`, `embed`, `hr`, `img`, `input`, `link`, `meta`, `source`, `track`, `wbr`, in any case. | **Done.** `qucs_s::markdown::voidElementsClosed()` (`mathtypeset.cpp`) writes `<br>` as `<br/>`, `<IMG src="x.png">` as `<IMG src="x.png"/>` and `<hr class=a >` as `<hr class=a />`. One already closed is left as it is. A name that only begins like one (`<brx>`, `<break>`) is not touched, nor are paired tags (`<b>`, `<sup>`, `<kbd>`), nor a `<` that starts no tag. All three `setMarkdown` calls take its result. A `<br>` in a table's cell is a line break in the cell, as on GitHub. |
| **Leave code alone**: inline code spans, fenced and indented code blocks. | **Left alone.** It uses the same walk past code that the math uses: the code that found where code starts and ends in `findMath` is now `outsideCode()`, which both use. So code here means what the math already treats as code: inline code, fenced and indented blocks, `<code>` and `<pre>`, and a character a backslash escapes (`\<br>` stays text). `` `<br>` `` shows as written; a `<br>` in a list item's continued line, which is no code, is closed. |
| **Put it in one helper**, next to the math pass, used by the panel's two and by the Markdown viewer. | **One function**, beside `findMath`, in the `qucs_s::markdown` namespace the viewer already uses. Each caller applies it after the math has been swapped for its marks, just before `setMarkdown`. |
| **Tests** (`test_claude_code`). | See below; one test added to `test_markdown_doc` as well. |
| **Upstream:** report the Markdown to Qt (`QTextMarkdownImporter`). | **Not filed from here.** A report goes out under the user's name; the reproduction above, five lines of Qt, is ready for it. |
| **Until then**, replies should not use raw HTML. | Not needed now: a `<br>` in a reply is drawn as GitHub draws it. |

Found while testing the text form: a reply copied as text, or exported as Text, lost
the text after an inline `<hr>` once the rule was closed. Qt puts that text into the
rule's own block, and `plainTextOf` wrote `----` for a rule block and dropped its text.
It now writes the rule and then the text, in the order they were written. A cell's line
break joins its lines with a space in the copy, as a cell's paragraphs already were, so
each row stays on one line.

## Tests

- **`test_claude_code`**, `htmlLineBreaksKeepTheRestOfAReply`:
  - The function itself: `<br>`, `<BR>`, `<Hr>`, `<wbr>`, an `<img>` with
    attributes, and `<hr class=a >` are closed.
  - Left as they are: `<br/>`, `<br />`, paired tags, `x < y`, `<brx>`, `<break>`;
    inline code, fenced (backticks and tildes) and indented code, `<code>`, `<pre>`;
    and an escaped `\<br>`.
  - A reply like the review's, drawn in the panel: a table with `<br>` and `<BR>` in
    its cells, then bullets with inline code, then a paragraph with `<hr>`, `<img>` and
    `<wbr>`, then `` `<br>` `` as code. All of its text is shown, and the cell's block
    holds two lines.
  - Its Text export: each row on its line with nothing lost, every bullet, the text
    after the rule, and no line-separator character left.
- **`test_markdown_doc`**, `anOpenVoidTagKeepsTheRest`: a Markdown file with `<br>`
  in a cell, `<HR>` and `<img>` rendered whole, and `` `<br>` `` kept as code.
- **The math tests** (`mathIsTypeset`, `codeIsNotMath`, `mathSurvivesAnything`) pass on
  the shared walk, as before.

Each part was broken on purpose: 12 breaks, all caught. The first run had 13, and one
was not caught: skipping spaces before a `/`, which matters only for `<br / >`. That
is no HTML, and closing it again is harmless, so the code was taken out.

Full suite 88/88; under AddressSanitizer 88/88, with no report. The end-to-end
scenarios: 97 checks, none failed.
