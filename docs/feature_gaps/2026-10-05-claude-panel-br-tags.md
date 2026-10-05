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

## The re-check of `f35dccc`

The reviewer re-checked `f35dccc` in the installed app and found the void elements
fixed: the table draws with its multi-line cells, every bullet and paragraph keeps its
text, and code and an escaped `\<br>` show as written. Two things were left.

| Re-check | Now |
|---|---|
| **Still open (HIGH): any unclosed tag, not only void elements.** `a<p>b`, `a<li>item`, `a<span>x`, `a<u>x`, `a<b>unclosed bold`, `vector<int> x`, `a <T> b`, `a<x>b` each lose the rest. In the viewer, "1. The dataset is <name>.dat.ngspice beside the schematic." showed "1. The dataset is", an empty "2.", and nothing after. Placeholders such as `<name>`, `<sch>` and `QList<Span>` are common in Claude's replies. | **Fixed: `htmlBalanced()`** replaces `voidElementsClosed()`, on the same walk past code. Measured on Qt 6.11 alone first: Qt counts open tags across the whole document and drops the text after any tag never closed, and after a closing tag with nothing open (`a</b>b`). Mismatched names (`<b>x</i>`), tags across lines, cells or paragraphs, comments and autolinks lose nothing. So, outside code:<ul><li>a void element written open is closed, as before;</li><li>a tag that is no HTML element (`<name>`, `<sch>`, `<T>`, `<int>`, `<x>`, `<name/>`) is text: its `<` is escaped (`\<`), so it shows as written;</li><li>an element not closed in its paragraph (`<b>` with no `</b>`, a lone `<p>`, the `<Span>` of `QList<Span>`), a closing tag that closes nothing (`</b>`, `</br>`), and one left open within another, are text too;</li><li>an element closed in its paragraph stays HTML: `<b>x</b>`, `<kbd>F9</kbd>`, `<sup>`, `<details>` over several lines, `<b><i>x</i></b>`.</li></ul>Paragraphs are apart by blank lines. A `<b>` closed only in a later paragraph is text, rather than merging the two paragraphs as Qt does. Autolinks and comments are not tags to it. |
| **Small: an `<hr>` in a paragraph is drawn under the text that follows it.** Qt puts that text into the rule's block. | **Between the parts now.** `rulesApart()` moves a rule block's text into a block of its own after the rule, in the panel, its text copy and the viewer. In a list item the rule's block is already outside the list; in a table's cell the cell gets one line more. The text copy writes the rule, then the text, which made its own branch for this unnecessary: it was removed. |
| **Until then, Claude should put any `<...>` in backticks.** | Not needed: written as text, a placeholder shows as written. |

### Tests

- **`test_claude_code`**, `aRepliesHtmlIsReadWhole` (was `htmlLineBreaksKeepTheRestOfAReply`):
  - Each case of the re-check's table becomes text, as do an unknown pair (`<T> b
    </T>`), a stray `</b>` and `</br>`, a mismatch, one left open inside another, a
    `<b>` closed only in the next paragraph, `<brx>`, `<name/>`, and a placeholder in a
    list item's indented line.
  - Kept as written: closed elements (across a line, nested, upper case, with
    attributes), `<details>` over lines, autolinks, comments open and closed, `x < y`,
    `a <3 b`, code of every kind, and escapes.
  - Every case of the re-check, rendered and followed by a paragraph: that paragraph
    is kept.
  - The panel: the review's reply with a `QList<Span>` in a cell, a list item with
    `<name>.dat.ngspice in Scratch/<sch>/`, a list item with an `<hr>`, and a lone
    `<b>` - all shown. The rule's block is empty and sits between "A rule" and "then
    text".
  - The Text export: the same text, the rule between its parts.
- **`test_markdown_doc`**, `anUnclosedTagKeepsTheRest` (was
  `anOpenVoidTagKeepsTheRest`): the viewer case of the re-check (`<name>.dat.ngspice`
  in a numbered list, then "still here?"), `vector<int>`, a lone `<b>`, and "THE END"
  are all rendered; the rule sits between its paragraph's parts.

Each part was broken on purpose: 18 breaks, all caught. The first run had 20, and
four were not caught:
- **Autolinks and comments** were passed over at first. They made no difference: a
  tag's name must be followed by a space, `/` or `>`, which a link's `:`, an address's
  `@` and a comment's `!` are not. That code was removed.
- **The text copy's split** was not caught, because its old branch did the same
  thing. The branch was removed, and the split is now caught.
- **The viewer's split** had no test: one was added.
- **The list exception** guarded nothing, as above, and was removed.

Full suite 88/88. Under AddressSanitizer 88/88, with no report:
`tuneHoldsAndCompares` once found the simulator not starting while four tests ran at
once, and passed alone. The end-to-end scenarios: 97 checks, none failed.

## The re-check of `9e587d3`

The reviewer re-checked `9e587d3` in the Markdown viewer, and on `htmlBalanced()`
itself compiled into a small program. The placeholders (`<name>.dat`, `QList<Span>`,
`array<array<int,2>,3>`, `Use <Ctrl>+<S>`), stray closing tags, closed elements,
autolinks, code and the `<hr>` were right. Four items were left.

| Re-check | Now |
|---|---|
| **1. A comment with anything tag-like in it blanks the rest (MEDIUM)**: `<!-- a comment with <b> inside -->`, `text <!-- c <b> --> more`, a comment over several lines; plain Qt does the same. READMEs comment out badges (`<!-- <img …> -->`), and the viewer opens any `.md`. | **Comments are taken out** before Qt reads the Markdown - they draw nothing. One never closed is text (`&lt;!--`); at a line's start it had made the rest of the document one comment. |
| **2. A tag opened in one list item and closed in the next moves text between them (LOW).** | **Each block on its own.** A tag is now matched in its own block: a paragraph, a list item, a heading, a quote's line, a table's cell. An element closed only in another block is taken out with its closing tag, its text kept: "item one opened", "item two closed". |
| **3. The same across table cells (LOW).** | The same: "col x" and "y col". |
| **4. `<details>` over blank lines shows its tags as text (cosmetic).** | **Taken out, the text kept**, by the same rule: `<summary>` and the body are drawn, and the tags no longer show. |

### What a random test found beyond the four

A property test (in `aRepliesHtmlIsReadWhole`) builds random text from tags, comments,
backticks, fences, quotes, list marks, headings, pipes and escapes. Each text must keep
a paragraph that follows it. On the first try 341 of 3,000 lost it. The failures,
shrunk to their smallest form, came from seven things the function did not yet know
about Markdown:
- **HTML blocks** (CommonMark): a line that starts with `<pre>`, a comment, a block
  element's tag (`<hr>`, `<p>`, `<details>`), or a whole tag on its own after a blank
  line, starts raw HTML. In it a backslash is no escape, there is no code, and Qt counts
  every `<` - `x < y` and `<x` too. Text is now written `&lt;`, which reads as text in
  Markdown and in HTML alike, and a lone `<` in such a block is text too. A tag escaped
  can end a line's being an HTML block, so the function runs again until nothing
  changes (each run only removes tags, so it stops).
- **`<code>` and `<pre>`** keep their tags HTML to Qt. They were walked past as code,
  as GitHub shows them for the math. For HTML they are walked through now (`outsideCode`
  takes which reading it is).
- **Quotes and list items**: an HTML block or a fence may start after their marks, and
  ends with the quote or the item.
- **Inline code** ends in its paragraph. A paragraph ends at a blank line, a heading's
  line, or a line that starts another block (a fence, a quote, a list item, an HTML
  block). It closes on a run of backticks exactly as long.
- **A backtick fence**'s line holds no other backtick.
- **A backslash at a line's end** is a line break, and no longer hides the next line's
  start.
- **A comment that starts in an HTML block and ends past it** made the walk read its
  end twice. Two edits overlapped, and the text came out garbled (`<hr/>F IT`). The
  walk now goes on from the comment's end.

On 200,000 random texts, plain Qt loses the closing paragraph for 151,692. With
`htmlBalanced()` it loses it for 8. Those mix quote or list marks with fences or
indented code in ways no reply writes, for example `>     <pre>` followed by `</pre>`.
The property test runs 3,000 such texts, all kept, and each shrunk case above is a
check of its own.

### Tests

- **`test_claude_code`**, `aRepliesHtmlIsReadWhole`:
  - each of the re-check's forms: the comments, the list items and cells, `<details>`
    over blank lines, a heading's line and a quote's;
  - the shrunk cases, each followed by a paragraph that must be kept;
  - a comment past an HTML block's end, taken out once;
  - a fence inside the paragraph that inline code is looked for in, left as code;
  - `<code>` and `<pre>` with tags in them;
  - the property test.
- **`test_markdown_doc`**, `anUnclosedTagKeepsTheRest`: a README's commented-out badge
  `<!-- <img src="badge.svg"> -->`, then "THE END", rendered.

Each part was broken on purpose. This re-check's 30 breaks were all caught; one was
caught only after a test was added for it: a fence ending inline code's paragraph,
whose break garbled code without losing text. Of the previous round's breaks, the 13
that still apply were caught again.

Full suite 88/88; under AddressSanitizer 88/88, with no report. The end-to-end
scenarios: 97 checks, none failed.
