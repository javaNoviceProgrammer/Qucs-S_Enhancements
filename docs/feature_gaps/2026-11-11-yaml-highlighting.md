# YAML in the text editor

The request: YAML files (`.yaml`, `.yml`) opened in the text editor as plain text; highlight them like the other
languages.

## What was built

- **YAML is a language of the editor** (`LANG_YAML`, saved as `yaml`), the default one of `.yaml` and `.yml`. It is in
  the status bar's language menu, can be chosen for any other suffix, and has its page on the Source Code Editor tab of
  the settings, with a preview. Its colours are the shared styles, so it follows the light and dark papers like the
  others.
- **What it picks out:** keys, plain and quoted, before `: `; single- and double-quoted strings, the escapes of
  double-quoted ones (`\t`, `\x41`, `µ`, a backslash at the line's end) and the `''` of single-quoted ones; the core
  schema's literals `true`, `false`, `null`, `~` (and their `True`/`TRUE` forms; `on` and `yes` are words, as in YAML
  1.2) and numbers (`-1.5e3`, `0x1F`, `0o17`, `.inf`, `.nan`); anchors, aliases and tags (`&a`, `*a`, `!!str`,
  `!<tag:...>`); the directives `%YAML` and `%TAG` and the document markers `---` and `...`; the indicators (`-`, `?`,
  `:`, `,`, `[ ] { }`, a block scalar's `|` or `>` with its modifiers); comments.
- **A `#` begins a comment** only at the start of a line or after a blank: not in a string, a URL's `#frag` or a key
  like `a#b`. A key is a key whatever its word (`true: x`), and a value is a literal or a number only when all of it is
  one (`1.2.3` is a word).
- **Read by a scanner of its own** (`YamlLine` in `syntax.cpp`), not by the patterns the other languages use: what a
  word is depends on where it stands. It reads a line at a time and carries to the next line, in the block state, what
  is still open there:
  - a **block scalar**: after `key: |`, `- >-` or `--- |`, the lines indented more than the key's (or the dash's)
    column, and the blank ones, are its text - neither keys nor comments - until a line at that column or less, or a
    document marker;
  - a **quoted string** going on over the next line;
  - the **flow collections** open (`[a,` on one line, `b: c]` on the next: `b` is still a key in one).
- **Edit > Comment/Uncomment** puts `#` before YAML lines and takes it away.

## Not done

- A block scalar's explicit indentation (`|2`) is highlighted but not used: its text is what is indented more than its
  parent's column, which is the same for every file that is valid YAML.
- A tagged scalar is highlighted by its look (`!!str 42` shows `42` as a number).
- Plain scalars over several lines are not followed: a continuation line that is all a number shows as one.

## Files

- `qucs/syntax.h`, `qucs/syntax.cpp`: `LANG_YAML`, its definition, styles and sample; `YamlLine`, and
  `SyntaxHighlighter::scanYaml`, which applies it.
- `qucs/textdoc.cpp`: `#` as YAML's comment mark.
- `qucs/tests/test_syntax_highlighting.cpp`: `yamlIsHighlighted` (each case above, and the lines after a block scalar or
  an open quote highlighted again as it is taken away and put back), `aYamlFileOpensHighlighted` (a `.yml` opened in
  the app: its language, its colours, the status bar and its menu); `.yaml` and `.YML` in the suffix test.
- `qucs/tests/test_text_editing.cpp`: Comment/Uncomment on a YAML file.
