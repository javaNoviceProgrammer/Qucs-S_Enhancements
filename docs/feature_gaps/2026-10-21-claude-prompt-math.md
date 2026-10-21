# Math typeset in the user's own prompts

The user (2026-10-21): *"if I type a math equation in the latex format, it actually shows the rendered
equation when I send the message? Currently, if I type $\Delta x$ in the chat and send it, it will keep
the literal text instead of rendering it as an equation."*

**Was.** The Claude panel typeset the math in Claude's replies (`renderMarkdown`, `mathtypeset.h`). The
user's own prompts were set in their bubble line by line, as typed, so `$\Delta x$` stayed as text.

**Now.** A prompt's math is typeset in its bubble as a reply's is:
- inline `$...$` and `\(...\)`;
- display `$$...$$` and `\[...\]`, centred on a line of its own.

The same reader finds it (`findMath`), so the same rules hold: math in a code span stays as typed, and so
does money ("it costs $5 or $10"). The rest of the prompt is still set as typed, not read as Markdown. Each
formula's TeX is its tool tip.

Not changed:
- the prompt Claude is sent;
- the text put back by Edit;
- the Markdown and text exports, which keep the TeX (a Markdown export shown in the viewer typesets it).

The step that typesets the marks is shared by the two (`ClaudeCodePanel::placeMath`).

**Test.** `test_claude_code aPromptsMathIsTypeset`: a prompt with inline and display math, a code span and
money.
- Its two formulas are typeset; the reply's one is too.
- The code span and the money are shown as typed.
- The display formula is centred.
- The Markdown export keeps the TeX.

Taking the step out of the prompt's bubble fails the test (the formulas: one, not three). Under the
sanitizers it passes.
