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

## The re-check of `ccc1dd9`

The reviewer re-checked `ccc1dd9` four ways: in the viewer, on `htmlBalanced()`
compiled alone, on all 1,419 Claude replies in the workspace's transcripts, and with a
fuzzer of their own. The four items of the last re-check were fixed. None of the replies
lost text, and none of those that were already fine changed. Three items were left; all
three reproduced on `ccc1dd9` as written.

| Re-check | Now |
|---|---|
| **1. `htmlBalanced()` is quadratic (MEDIUM, performance).** Each `QRegularExpression::match(md, offset)` checks the whole string for valid UTF-16, at every line and tag: 134,000 characters with tags took 1,661 ms, and a 100 kB README froze the viewer for a second. | **Linear.** The text is checked once: a lone surrogate, which is no UTF-16, becomes U+FFFD, as Qt's importer reads it anyway (`validUtf16()`, in `htmlBalanced` and `findMath`). Every match then skips the check (`matchAt()`). Three more costs that grew with the square were found and removed: the edits made one at a time (each `replace` moved the rest), inline code looked for to its paragraph's end from each backtick, and a comment's end looked for the same way. A table check, new this round (below), also needed care: PCRE looks far ahead for the `-` its pattern needs, so the line's first character is checked first. 134,000 characters with tags now take 12.0 ms, as long as Qt's own `setMarkdown` (11.7 ms); 122,000 of prose take 6.7 ms (726 ms before); the reviewer's 268,000 take 23 ms (6,756 ms before). The 1,419 replies render in 67 ms, the slowest in 0.6 ms. |
| **2. A `/>` that ends no tag, in an HTML block, loses the rest (LOW).** `<div>a</div>/>`, `<hr>x/>`, `<div/>/>`; a lone `<hr>`, closed to `<hr/>`, starts such a block itself. | **Written `/&gt;`.** In an HTML block Qt counts every `/>` as a tag closed, so a `/>` there that ends no tag is text. Each of the reviewer's cases keeps the rest. |
| **3. A `<details>`' summary joins the paragraph before it (cosmetic).** "AFTER-ASum line". | **Wider than `<details>`, and fixed for all.** Qt's importer puts any HTML block into the block before it, plain Qt as well: "para", then `<div>x</div>`, read "parax"; a `<h2>` joined the heading before it; a README's centred badges joined its title. `qucs_s::markdown::setMarkdown()` now does what the three callers did, and more. It balances the HTML, then starts each HTML block with a paragraph holding one noncharacter (U+FDD2), which Qt merges into the block before. After `setMarkdown` it takes each mark out. Text after the mark in its block becomes a block of its own, and a block left empty goes. "AFTER-A", "Sum line" and "body" are three blocks; the badges are centred under the title; a `<pre>` keeps its first line. Where Qt already had the HTML in a block of its own - at the start, after a quote, code or a table - it is as it was. A list item's first HTML block stays in its item, as in Qt. A block indented four spaces or more is not marked: md4c may read it as code, where the mark would show. `rulesApart()` became part of `setMarkdown()`. |

### What else the fuzzers found

The reviewer's fuzzer now also puts `<![CDATA[`, `<?x?>` and `<!DOCTYPE html>` in its
fragments. With those, `ccc1dd9` lost the closing paragraph for 699 of 20,000 texts.
These were fixed along the way:
- **Comments' likes.** A processing instruction, a CDATA section and a declaration
  draw nothing in Qt, and a tag in one lost the rest. They are taken out as comments
  are, and they start an HTML block at a line's start, as in CommonMark.
- **Quoted values.** `<img src="a>b">` and `<a title="x/>y">` lost the rest: a tag's
  pattern ended at the first `>`, and Qt counts the `<x` and `/>` inside quotes. A value
  in quotes is part of its tag now, and a `<` or `>` in it is written `&lt;` or `&gt;`.
- **`</br/>`**, counted by Qt as two tags closed, is text.
- **A regression of `ccc1dd9`.** An inline comment over a blank line (`a <!-- b`, blank
  line, `c --> d`) was taken out whole, deleting text that Qt and GitHub both show. A
  comment is now one only within its paragraph or HTML block; otherwise it is text,
  as Qt shows it.
- **Another.** A `<pre>` over blank lines, or with a `|` in it, lost its tags and was
  read as Markdown: blank lines and pipes counted as block boundaries inside an HTML
  block. They no longer do.
- **md4c's reading**, which Qt uses and which differs from CommonMark in places, now
  followed:
  - In a paragraph, a line that starts an HTML block ends the paragraph however far it
    is indented.
  - After a table's dashes, every line is a row until a blank line, a quote, a list
    item, indented code or a rule; a `<div>` line or a fence is a row too.
  - A whole tag alone on its line starts an HTML block after any line that is not a
    paragraph's, a heading's too.
  - A fence closes on a run as long or longer, with nothing after it.
  - Indented code starts after any line that is not a paragraph's: a heading, a fence's
    end, a rule, an empty quote line. Before, `# H` followed by `    <div>x` had its
    `<div>` escaped inside the code.

With the reviewer's full fragments, 32 of 20,000 texts lose the closing paragraph
(699 on `ccc1dd9`). Without the three new ones, 29 (282). The property test, now with
these fragments and through `setMarkdown`, keeps every text of 20,000 on its own seed
and on two others. On two more seeds, 1 and 2 of 20,000 are lost. Every one of those
is indented code inside a list item or a quote (a mark followed by five spaces), which
the walk reads as text; see "Not done".

The repository's 79 Markdown files: plain Qt loses the end of 4, now none. No mark is
left in any text, `htmlBalanced` gives the same text run twice, and Qt prints no
warnings.

### Tests

- **`test_claude_code`**, `aRepliesHtmlIsReadWhole`:
  - each `/>` case of the re-check, `</br/>`, the quoted values, the comments' likes, a
    `<pre>` over blank lines and pipes, a comment over a blank line (text), one in a
    `<pre>` (taken out), a comment never closed in its HTML block (text, nothing after
    it taken);
  - md4c's readings: a table's row, an indented line ending a paragraph, `<?x?>`
    ending one, fences by their runs, indented code after a heading, a fence, a rule, an
    empty quote line and a table;
  - each case of the re-check and each shrunk case, followed by a paragraph that must be
    kept, now through `setMarkdown`;
  - the property test, through `setMarkdown`, with the new fragments; no mark left.
- **`anHtmlBlockIsABlockOfItsOwn`** (new):
  - the `<details>` case and ten more kinds of HTML block, each a block of its own;
  - ten where Qt had it right, as Qt had them;
  - centred badges after a title and at the start, still centred;
  - six texts where md4c reads code, with no mark showing;
  - a reply with `<details>` in the panel, its summary a line of its own;
  - no Qt warning.
- **`aLongReplyIsReadInLinearTime`** (new): three texts - mixed Markdown, one long
  paragraph of code, a comment and an unclosed `<?` on each line, and text with a tag
  to escape every few characters. Four times each takes less than eight times as long
  (about four; sixteen before). A lone surrogate becomes U+FFFD in `htmlBalanced` and in
  `findMath`'s formula.
- **`test_markdown_doc`**, `anUnclosedTagKeepsTheRest`: a `/>` in an HTML block, a tag
  in CDATA and `alt="a>b"`, the text after each rendered; a heading's centred badges
  apart and centred.

Each part was broken on purpose: 49 breaks, 48 caught. The one not caught is the
table check's look at its first character, which saves a constant factor rather than a
square: no test of time can tell it from noise. The first run had 50 breaks and 17 not
caught:
- **Code that made no difference, taken out:**
  - a `/>` written `&gt;` in an escaped `<name/>`: the next round does it;
  - a block boundary after an HTML block: Qt drew the same either way;
  - a list item's or quote's format kept, and the next block's format set, when an empty
    mark's block goes: Qt keeps the next block's format by itself;
  - "no closing run from here" remembered for inline code. A run that found no closer
    has no run of its length after it in its paragraph, so it could never be used.
- **Tests added:** the edits in one pass (the dense-tag text), the HTML block's own
  boundary (a quote's indented code before one), the comments' likes as HTML blocks,
  `<?x` ending a paragraph, fences closed by a longer run or with text after them,
  the indented code rules, and the four-space rule for marks (`<div>` then
  `>      <hr>`).

Full suite 88/88. Under AddressSanitizer 87 of 88 passed together;
`tuneHoldsAndCompares` found the simulator not starting while four tests ran at once,
and passed alone; no sanitizer report. The new linear-time test holds there too
(about 4.1 times for four times the text). The end-to-end scenarios: 97 checks, none
failed.

### Not done

- **Indented code inside a list item or a quote** (`1.` or `>` followed by five
  spaces) is read by the walk as text, and by md4c as code. 1 to 2 random texts in
  20,000 lose their end this way; no reply or file has it.
- **Thousands of unclosed `<!--` or `<pre>` at line starts** each still look to the
  text's end: 40,000 characters of nothing else take about 170 ms. No reply or file has
  more than a few.
- **A mark can show** as `<p></p>` when a fence is indented by a tab inside a list item:
  1 in 20,000 random texts.
- **A list whose first item starts with an HTML block** (`- <div>x</div>`) loses its
  list, and Qt warns "attempted to insert into a list that no longer exists", as before.
  Where a text used to lose its end before reaching such an item, Qt now reaches it
  and warns.
- **An HTML block after a paragraph's line, indented four spaces or more**, is read
  rightly but not marked, so Qt still puts it into the paragraph.

## The re-check of `dbcf2df`

The reviewer checked `dbcf2df` on compiled test programs, then in the window once the
13:08 build was installed. Linear time, the stray `/>`, and HTML blocks as blocks of
their own were all verified. The 1,419 replies lose nothing and change nowhere they
were fine, and no mark is left in any output. The reviewer's fuzzer, reseeded and now
with U+FDD2 in the text, lost 62 and 50 of 20,000 (282 and 699 on `ccc1dd9`). Two
things were left; both reproduced.

| Re-check | Now |
|---|---|
| **Degenerate tables (LOW).** A header line that is only `\|`, or dashes like `-\|`, then HTML: `r`, `\|`, `-\|`, `<hr></` loses the end; so do `>\|`, `\|-`, `<summary></summary><t` and two cases with a tab. md4c reads the lines as a paragraph or a table where `htmlBalanced` read the other. | **The walk starts a table as md4c does.** A table header must be its paragraph's first line: after a line that is no paragraph's, as a list's new item, or as a quote begun. Its dashes must be in the same quote. After `r`, `\|` and `-\|` are a paragraph's lines, so `<hr></` is an HTML block and its `</` is text. After `>\|`, the unquoted `\|-` is the quote's lazy line, so the `<t` is text. The two tab cases kept their end already. |
| **Cosmetic: a `<div>` in a list item is drawn at the left margin.** | **Under its bullet.** Qt indents an item's second paragraph by its list's indent, outside the list. An HTML block inside a list item, or inside a quote, now gets marks of its own at its start and its end. Every block between them is set in as the item's or the quote's paragraphs are: the list's indent, the quote's margin and level. Before, a `<div>` in a quote was drawn outside the quote. A `<pre>`'s lines all move in. What follows the item is as it was. |

Found along the way:
- **A tag over lines** ends within its HTML block or paragraph. `<b`, blank line, `>x</b>`
  had paired a `<b` that md4c reads as text with the `</b>` that Qt counts.
- **A mark read as text.** Where md4c reads code that the walk took for none, a
  mark's paragraph would show as `<p></p>`. Such a mark is taken out with its `<p>`
  and `</p>`. That made the rule against marking blocks indented four spaces
  unnecessary, so it was removed. md4c's indented HTML block after a paragraph is now a
  block of its own as well.
- **An inner block's end mark** goes after the `>` its last line ends with. A block
  whose last line ends inside a tag is marked as before, without indent.
- **The marks' characters in a reply** (noncharacters) are taken out before the
  marks are set.

With all the reviewer's fragments, 14 of 20,000 texts lose the closing paragraph (32
on `dbcf2df`); without the three newer ones, 9 (29). The property test's seeds lose
none, or 1 to 2 of 20,000 on three others; all are indented code inside a list item
or quote. No mark shows. The 1,419 replies and the 79 Markdown files are as before:
nothing lost, nothing changed where Qt was right, no warnings. `setMarkdown` takes
about as long as Qt reading the balanced text (30 against 27 ms for 67,000 characters
with tags), or twice as long where the text is all HTML blocks (140 against 73 ms for
116,000), and it stays linear.

### Tests

- **`aRepliesHtmlIsReadWhole`:** the reviewer's four tables, each kept to its end and
  each written as expected. Also md4c's table starts (a list's new item, a quote begun,
  a paragraph's second line) and a tag over a blank line.
- **`anHtmlBlockIsABlockOfItsOwn`:**
  - an item's `<div>` at its paragraphs' indent, and the text after the list at none;
  - a `<pre>`'s lines in an item;
  - a quote's `<div>` with its margin and level, and one in a quoted item;
  - an item's HTML block last in the text;
  - the marks' characters in a reply, taken out;
  - two more texts where a mark would have shown;
  - a paragraph's indented HTML block, apart.
- **`test_markdown_doc`:** the viewer's list-item `<div>` at indent 1.

Each part was broken on purpose: 65 breaks, 64 caught. The one not caught is still the
table check's look at its first character, a constant factor. The first run had 66
breaks and five not caught:
- two of them only because the code had moved under their text, so the break did not
  apply; their text was corrected, and both were caught;
- the rule against marking blocks indented four spaces made no difference once a mark
  read as text is taken out, and was removed;
- "an end mark only after a `>`" had no test, so one was added (a last line ending in
  a tag);
- the first-character look, as above.

Full suite 88/88. Under AddressSanitizer 87 of 88 passed together: `test_mcp_server`
waited too long for an answer while four tests ran at once, and passed alone. No
sanitizer report. The end-to-end scenarios: 97 checks, none failed.

### Not done

As in the last round:
- indented code inside a list item or a quote is read by the walk as text;
- thousands of unclosed openers are slow;
- a list that starts with an HTML block loses its list.

New: **an HTML block in a list nested in another** (indented four spaces or more)
is not seen as one by the walk. It stays where Qt puts it, in its item's paragraph.

Last round's "a mark can show as `<p></p>`" after a tab-indented fence in a list item
no longer happens: such a mark is taken out as text.

## The re-check of `5181da6`

The reviewer checked `5181da6` in the window, on the build installed a minute after
the commit, and with test programs rebuilt against it. The degenerate tables kept
their end, and a list item's `<div>` sat under its bullet. The 1,419 replies were as
before. On their fuzzer's seeds, losses went from 62 to 16 of 20,000, and with the
newer fragments from 50 to 10. Three things were left; all reproduced.

| Re-check | Now |
|---|---|
| **1. A backtick pair across table rows turns `<…>` in code into `&lt;…>` (LOW-MEDIUM, a regression against plain Qt).** Row 3's stray backtick was paired with row 4's, so the `<name>` md4c reads as code in row 4 was escaped and showed `&lt;name>`. Code across rows lost the end. | **In a table, a row is read on its own, as md4c reads it.** The walk keeps where the table's row ends, its header's too. Inline code ends there, and so does a tag over lines or a comment in the balancer. The console table shows `<name>`, and every case keeps its end. md4c reads a code span or a comment across a cell's `\|` whole, so the row, not the cell, is the limit. |
| **2. Text right after an HTML block, with no blank line, joins it (cosmetic).** "para", `<div>x</div>`, "next line" read "para / xnext line". In a quote or list item that text was drawn at the margin. | **A line of its own.** Qt puts a line after one that ends with a block's closing tag (`</div>`, `</p>`, `</summary>`, `<hr>`) into that block; a browser draws it below. Such a line now starts with a mark, so it is a block of its own: "para / x / next line". Two `<div>`s on two lines are two lines; `<details>`' summary and body are apart. In a quote or list item, the end mark now goes at the block's last line, after text too, so those lines are in the quote or under the bullet. |
| **3. The remaining fuzz losses: tables with a header of only `\|`, followed by `>`, `<hr` or a backtick across rows.** | The backtick ones are item 1. The rest came from four more differences with md4c, now followed:<ul><li>A line with fewer quote marks continues the quote's paragraph lazily, so it starts no table.</li><li>A quote or list mark ends a table. The check had looked past the marks, so it never saw them.</li><li>A tag is one only as CommonMark has it: `<b "x">`, `<b/x>`, `<p "">` and `<a href=x?y=z>` are text to md4c, while the `</b>` after them counted. md4c takes `</d/>` for a tag, which Qt counts twice, so it is text.</li><li>An HTML block in a list item ends with the item. It had run on into the next item, where a separator came after `- ` and the item lost its list.</li></ul> |

With all the reviewer's fragments, 6 of 20,000 texts lose the closing paragraph; 6
without the three newer ones (14 and 9 on `5181da6`). What remains is the known md4c
fence quirk and indented code in containers. On the property test's seeds, nothing
new: 0, or 1 to 2 of 20,000 on three seeds, all of those kinds. The 1,419 replies and
the 79 files are unchanged: nothing lost, nothing changed where Qt was right, no mark
left, no warnings.

### Tests

- **`aRepliesHtmlIsReadWhole`:** the reviewer's table (`<name>` kept as code), code
  and a comment across rows, md4c's tag grammar (`<b "x">`, `</d/>`, valid
  attributes kept), and the next list item's code after an item's HTML block. It also
  has twelve shrunk cases, each kept to its end: the reviewer's tables, `<details/p>`,
  `<p "">`, a lazy quote line, a table ended by a quote, a list item or a rule, and a
  tag across rows.
- **`anHtmlBlockIsABlockOfItsOwn`:**
  - a line after `</div>` apart, two `<div>`s apart, `<details>`' summary and body
    apart;
  - a `- ` line in a top-level HTML block kept whole, and a quote's `> >` line kept;
  - no block of spaces left;
  - the lines after a quote's and an item's HTML block set in;
  - a `<ul>` and a `<div>` inside a paragraph as Qt had them;
  - a quoted line that md4c reads as no tag, in its quote.

Each part was broken on purpose: 81 breaks, 80 caught. The one not caught is the
table check's look at its first character, a constant factor, as before. The first
run had 82 breaks and five not caught, besides that one. Three got tests:
- the strict tag pattern for a lone tag on its line, which keeps `> <b "x">` in its
  quote;
- a list item's HTML block ending with the item, without which code in the next item
  was escaped;
- no split before mere spaces, without which Qt kept a block of spaces.

One was taken out: a space before an end mark that would follow a backslash. Once
blocks end with their list item, it saved nothing, and it left a visible space in 105
of 60,000 random texts.

Full suite 88/88. Under AddressSanitizer 87 of 88 passed together: `test_ngopt`'s
optimizer run did not finish its log in time while four tests ran at once, and passed
alone. No sanitizer report. The end-to-end scenarios: 97 checks, none failed.

### Not done

As before:
- indented code inside a list item or a quote;
- md4c's reading of a fence its quote or item closed, followed by another fence line;
- slow unclosed openers by the thousand;
- a list that starts with an HTML block;
- an HTML block in a nested list.

The separators follow only lines that end with a block's closing tag. Text after
`</div>` on the same line still joins it, as in Qt.

## The re-check of `72afb34`

The reviewer checked `72afb34` in the window, and with test programs rebuilt against
it. Both of last round's items held: the console table showed `<name>` as code, and
text after an HTML block was a line of its own, in the quote and under the number. The
1,419 replies were as before. On their fuzzer's seeds, losses went from 16 to 1 of
20,000, and with the newer fragments from 10 to 3. Their verdict was "done", with four
fuzz losses left that plain Qt loses too, none like a reply. All four reproduced.

| Re-check | Now |
|---|---|
| **1. `1. ```⏎```<n>` and `>```⏎```<r>`: a closing fence with text after it.** | **md4c's reading, followed.** When a quote or a list item ends a fence, md4c still looks at the next line as at the fence's closing. When that fails, it reads what follows the run of backticks as a line of its own: past the run, and past the spaces after it when the run is as long as the fence's. So `<n>` starts an HTML block, and the walk had taken it for a new fence's info string, left raw. The walk now reads such a line the same way: the tag is escaped, and the line shows "```<n>". A run with nothing after it is a blank line, as to md4c. |
| **2. `>```⏎`<p`: a lazy line after a fence in a quote.** | The same reading: past the one backtick, `<p` starts an HTML block, and its `<` is text. |
| **3. `1.     e⏎<b><br>⏎#<b</b>`: indented code in a list item.** | Two things lost it. `#<b` is no heading (a heading's `#`s need a space or the line's end after them), but the balancer cut the paragraph there, so `<b>` and `</b>` were in two blocks and were taken out. With them kept, the stray `<` in `#<b` was inside an element: Qt reads the text in an open element as HTML, so `<b</b>` was a tag to it, and the text went. A `<` that is text inside a kept element is now written `&lt;` (below). |

What else came up on the way, each reproduced against md4c's own output (md4c 0.5.3,
which Qt uses, built into a probe):

- **Indented code in a quote or a list item, read as md4c reads it.** This was in last
  round's "Not done". It is code when it is four columns past the quote marker's space
  or past the item's content, or on an item's first line, five columns past its marker.
  The walk keeps the stack of items a line goes on in:
  - a line that is no paragraph's lazy line closes the items indented past it;
  - a line that opens items nested on one line (`- 1. x`) opens each of them;
  - an item begun empty ends at a blank line;
  - `- ---` and `* * *` are rules, not items.

  Before, `-     x <name>` and a quote's indented `<b>` showed `&lt;name>` and
  `&lt;b>` in their code, where plain Qt showed them right.
- **A fence ends with any of its containers.** It ends at the first line that does not
  go on in each of them: a quote needs its `>`, an item its indent. `- >```⏎><b>` and
  `>> ```⏎> <b>` lost the end, because the fence was taken to go on while the line had
  a `>`.
- **A `<` that is text inside an element.** `<b>a < b</b>` showed "a", and `<b>a \<
  b</b>` too: Qt reads the text in an open element as HTML. Such a `<` is now `&lt;`.
  Inline code and autolinks are left alone, since Qt does not read those as HTML.
- **A link's `<destination>`.** `[l](<a b>)` and `[l]: <a b>` were read as an `<a>` tag
  left open. The tag was escaped and the link was lost; that was already so at
  `72afb34`. md4c reads the link first, so the walk now passes over the destination.
- **Two marks that lost text,** found on two more seeds and already there at
  `72afb34`:
  - An empty block in a table's cell was taken out with the next block, which was in
    the next cell. A selection over two cells takes both cells' text, so now only
    blocks in one cell are joined.
  - An end mark in a `<script>` left open hid the text after it. No end mark goes
    where an HTML block leaves an element open.
- **Time.** Three kinds of text were quadratic, all already so at `72afb34`:
  unclosed `<!--` lines (0.9 s for 112,000 characters), unclosed `<pre>` lines, and
  `<b title="x` with no `>` after it. Now:
  - a closer once found to be missing is not looked for again;
  - PCRE's look-ahead for a far `>` is turned off with `(*NO_START_OPT)`;
  - a `<code>`'s tag is matched no further than the next `>`.

  The 112,000 characters of `<!--` lines take 63 ms.

With all the reviewer's fragments, none of 20,000 texts loses the closing paragraph,
with or without the three newer ones (6 and 6 on `72afb34`). On sixteen seeds of the
property test, none of 20,000 loses it, and no mark changes the text (two did, on
seeds 13 and 99). The 1,419 replies and the 79 files are unchanged: nothing lost,
nothing changed where Qt was right, no mark left, no warnings.

### Tests

- **`codeInAQuoteOrAListIsReadAsMd4cReadsIt`** (new):
  - the reviewer's four, and the nine shapes found on the way, each kept to its end;
  - the balancer's output for each, and for indented code in quotes and items (left as
    code), nested items, `- ---` and `* * *`, a tag over lines at a block's start, a
    `<` in a kept element, autolinks, and links' destinations;
  - what the viewer shows for code in an item and in a quote, a `<` in bold, and a
    link with a `<destination>`;
  - the two cases where a mark lost text: what is shown with the marks equals what Qt
    shows without them.
- **`aLongReplyIsReadInLinearTime`:** seven more units, and `setMarkdown` on a long
  HTML block; each, four times longer, takes less than eight times as long:
  - unclosed comments and `<pre>`s;
  - `<b title="x` with no `>`, and with one `>` at the end;
  - the same tags alone on their lines;
  - `<code x`;
  - `[l](<a `.
- **`aRepliesHtmlIsReadWhole`:** `a <b>⏎<div>x</b></div>`, a paragraph's tag closed in
  the HTML block that ends it.
- **`anHtmlBlockIsABlockOfItsOwn`:** two marks md4c reads as text (an end mark after a
  `</script>`, and a tab after `>`) are taken out.
- **`test_markdown_doc`:** a numbered item's code (`make <target> && ls <dir>`), a `<`
  in bold, a link with a `<destination>`.

Each part was broken on purpose: 112 breaks, 111 caught. The one not caught is the
table check's look at its first character, a constant factor, as before.

The first run of this round's 29 had seven not caught:
- Five got tests: the blank line after a short run (a later line's backticks are not
  its code), an item behind a quote's `>` (not one a line without `>` goes on in), a
  tag over lines at a block's start (md4c's is on one line), and, for the two guards of
  a `<code>`'s tag, `<code x` and `<b title="x` with a `>` at the end.
- One guard was taken out with its break: the look at an HTML block's tags for its
  marks. After balancing, each `<` in an HTML block is a tag with its `>` near. A
  lone tag's `(*NO_START_OPT)`, added after the first run, went too: it is matched
  within its line.
- One break did not build; built, it was caught.

Three earlier breaks had lost their cases, because the walk now reads those cases as
md4c does: the HTML block's own block, a mark read as text, and a rule as no
paragraph. Each has a new case.

Full suite 88/88. Under AddressSanitizer 88/88, with no sanitizer report. The
end-to-end scenarios: 97 checks, none failed.

### Not done

- Two md4c quirks are not followed. After an item's ATX heading (`- # x`), a blank
  line ends the list, and a tab after `>` counts as the marker's space. Where the walk
  marks what md4c reads as code there, `markOut()` takes the mark out.
- A list inside a quote is not tracked: a quote's line uses the quote's own rule.
- As before: a list that starts with an HTML block, and an HTML block in a nested
  list.

## The final check of `206201e`

The reviewer checked `206201e` in the window, and with test programs compiled from the
commit:
- their 46-case regression suite passed;
- the 1,419 replies were as before;
- on their old seeds, no text lost its end.

On new seeds with more fragments, 1.4% to 3.2% of 50,000 texts lost the end, against
75% to 79% in plain Qt. They shrank 120 of those losses, and plain Qt lost all 120 too:

- **112 of them are malformed links:** an empty label (`[]: <v>`) and a stray `](<b>`.
  The walk passed over a `<…>` after them as a link's destination, but to md4c it is
  a tag. Valid links all worked.
- **A line that starts with an unclosed `<script`.**
- **An unclosed `~~~` in a list item** followed by a tag at the margin.
- **A degenerate table** with `<details>` across its rows.

Their verdict was "done". They added that the link one alone was worth doing, and
noted that `build/` was older than the commit. The build was in fact current: the
tests and the app were linked at 18:36:38, three seconds after
`mathtypeset.cpp` was last written (a break run restoring it). The 18:45 commit
changed only a count in this file.

All four reproduced. I wrote a fuzzer with the fragments they listed (tilde fences,
nested quotes and lists, link destinations, `<script>`, attributes, `- - -`, `***`), and
added tabs, `](<` and `]: <v>`. On `206201e` it lost about 2,350 of 50,000 texts per
seed. In 210 of those on one seed, plain Qt kept the end: a closing tag the balancer
made text had evened out Qt's count by luck. Every case was shrunk and checked against
md4c 0.5.3's own reading. Each difference found is now read as md4c reads it:

- **Links.** A `<destination>` is a link's only in a link:
  - its text's `[` comes before it in its paragraph (md4c reads links before a table's
    cells, so the text may cross a `|`), and it isn't around a link of its own;
  - a `)` follows it, or a closed title and then a `)`;
  - an escaped `\]` ends no text.

  `[]: <v>`, `x](<b>`, `[](<n>` and `[](<r>(` now show their `<…>` as written.
- **Definitions.** A definition has a non-empty label (escapes allowed) and a
  destination with its parentheses in pairs. It is its paragraph's first line, or
  follows another definition, and lies within its paragraph; a table's row holds none.
  None of a definition is HTML: `[<b>]: e` hides its `<b>` from Qt, as md4c does.
- **HTML blocks of type 1 and 4.**
  - md4c starts a block at `<pre`, `<script`, `<style` or `<textarea` whatever follows
    (`<prefix>`, `<script</v>`), and ends it at the line of any of the four closers.
  - Any other `<!` (`<!1`, `<!<l`) starts a block that ends at its `>`.
  - One with nothing to close it is written as text, since md4c would take the rest of
    the reply into it.
  - The block marks follow the same rule: a `<p>` mark before `<script/>` had changed
    the block's type.
- **Fences.**
  - A fence ends with its list item.
  - Its closing line is looked for in its own containers: in a top-level fence,
    `>~~~` is code.
  - A tab after a list's marker or a `>` counts as the space.
- **Tables.**
  - Each row is a block of its own for Qt's tag count.
  - The underline may be indented however far.
  - A heading, a fence or an HTML block line is a row; a rule ends the table.
- **Under a paragraph.**
  - A run of `-` or `=` underlines it.
  - A list marker with nothing after it, or a number other than 1, is the paragraph's
    text. md4c lets `1. ` with a space start a list.
  - A lazy line keeps the paragraph's quote going: `>p`, `q`, `>[f]: <a>` is one
    paragraph, with no definition in it.
- **Where a paragraph ends.**
  - It ends at a rule, or at a nested item or fence less than four columns past its
    container's content.
  - A line of its own quote goes on with it, so code spans cross quote lines.
  - A tag over lines does not cross the next line's quote mark.
- **Lists.**
  - A quote, an HTML block or a fence in an item, indented past the item, is found
    there.
  - A line belongs to the deepest item it is indented to, and an item marker indented
    four or more past its container is text.
  - Nested empty items (`- -`) and a quote's `>-` hold no paragraph.
  - An HTML block that breaks into an item's paragraph, however far indented, closes
    the item.
- **Time.** The new looks back to a line's start are bounded. A long line of `[`,
  `](<` or `<script` was quadratic until they were.

Two checks, I found, decided nothing once the balancer's rounds were counted, and were
taken out:
- an unclosed `<!` breaking into a paragraph;
- a table ended by a lone `-`.

Three helpers were made redundant by the list base and were taken out too: a fence's
extra indent skip, a fallback container for an HTML block, and two unused counters.

The fuzzer above, on its three sets of 50,000 inputs: 0, 0 and 2 texts lose the end
(about 2,350 each on `206201e`). On four new sets, one of up to 120 fragments: 0, 1, 1
and 7. That makes 11 of 350,000, with 1 regression against plain Qt in all of them, a
deeply nested shape. The reviewer's old fuzz loses none, nor do sixteen property seeds,
with no mark changing the text. The 1,419 replies and the 79 files are unchanged:
nothing lost, nothing changed where Qt was right, no mark left, no warnings. Time stays
linear.

### Tests

- **`linksAndBlockStartsAreReadAsMd4cReadsThem`** (new):
  - 38 shrunk cases from the final check and the fuzzer, each kept to its end;
  - the balancer's output for 44 more, among them malformed links, definitions,
    openers, fences, tables, underlines, items that may not break in, paragraph ends,
    lazy lines and quotes in items;
  - 23 that must stay as written: valid links, md4c's link across a cell's `|`,
    definitions in a chain, after a heading, in an item;
  - what the viewer shows for links, a malformed definition and an unclosed `<script`.
- **`aRepliesHtmlIsReadWhole`:** the property test's fragments now include `](<`, `[`,
  `]: <v>`, `<script`, `~~~`, a tab, `<!1`, `-` and `***`; none of its 3,000 texts loses
  its end.
- **`aLongReplyIsReadInLinearTime`:** `findMath` alone on tags with one `>` at the
  end.
- **`anHtmlBlockIsABlockOfItsOwn`:** one more mark md4c reads as code, after an item's
  heading, since the walk now reads the earlier two as md4c does.

Each part was broken on purpose: 156 breaks, 154 caught. The two not caught:
- the table check's look at its first character, a constant factor, as before;
- the check that an empty block is taken out only within its table cell. Since a `<pre>`
  line follows md4c's type-1 rule, no known input reaches that case; it stays as a guard.

The first run of this round's 49 breaks had seven not caught, and a later run had nine:
- Most got cases that only the broken reading changes: code against paragraph, a
  definition against a tag.
- Four turned out to decide nothing and were taken out with their code (above).
- One, the stop at a cell's `|`, was wrong: md4c reads `| [a | b ](<c>) |` as one
  link. It was taken out, and the case is a test.

Full suite 88/88. Under AddressSanitizer 88/88, with no sanitizer report. The
end-to-end scenarios: 97 checks, none failed.

### Not done

- md4c ends a list after an item's ATX heading at a blank line (`- # x`). `markOut()`
  takes out the mark that shows there.
- md4c reads a tag over a quote's lines without their marks (`><t` then `>a>` is
  `<t a>`); the walk does not.
- A link whose text holds a code span (`` ```[```](<b>) ``).
- A lazy line under a nested item, with tabs: the fuzzer's last losses and its one
  regression.
- As before: a list inside a quote, a list that starts with an HTML block, an HTML block
  in a nested list.
