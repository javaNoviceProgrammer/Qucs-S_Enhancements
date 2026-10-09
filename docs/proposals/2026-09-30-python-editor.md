# Proposal: a Python editor with its own toolbar

*30 September 2026 - Qucs-S 26.1.4, `96013a8`. Built on 8 October 2026, Qucs-S 26.1.7;
the README's "Python editor" says what it does.*

**What was decided.** These are the answers to the questions at the end of this
proposal.

1. **Run:** both, as proposed. Run gives the script a process of its own, with a
   *Python Run* dock and Stop. Run in Shell sits beside it.
2. **Checks beyond the syntax:** ruff or pyflakes only, as proposed, and nothing of
   our own. Ruff is asked first, so that a project's ruff settings apply.
3. **The message at the end of the line:** built as *Messages at Line Ends* on the
   Python toolbar, off by default.

**What changed from the proposal.**

- The toolbar's actions are also in *Simulation → Python*, so they can be used from
  the keyboard and by Claude's `trigger_action`, which runs menu actions only.
- The check runs in an empty folder of its own, not in the script's folder, because
  Python puts that folder first on its path.
- A script reads the end of the file at `input()` instead of waiting for input. The
  Run console takes no typing; Run in Shell is for a script that needs it.

The request is to run Python scripts from inside Qucs-S, with a toolbar like the
Simulate toolbar (the simulator list, Simulate, the simulation console with its
Stop), and to check a script's syntax as it is typed, with the errors and warnings
shown inline in the editor. The request also suggested a separate text editor for
Python scripts. This is the answer to that, and a plan.

## What there is already

- **Highlighting.** `syntax.cpp` knows Python (`LANG_PYTHON`, the `.py` and `.pyw`
  suffixes, its keywords and builtins, its strings and comments), and a `.py` file
  opens highlighted in the text editor (`TextDoc`).
- **An interpreter.** Settings, Locations, "Python Path" (`QucsSettings.PythonExecutable`);
  empty, `python3` (or `python`) on the PATH (`QucsApp::pythonProgram()`).
- **A Python Shell.** View, Python Shell: a dock running that interpreter
  interactively (`ProcessConsole`, `python -i -u`), tabbed with the Terminal dock.
- **A text document with parts of its own.** `MarkdownDoc` is a `TextDoc` with a bar
  at the top and a rendering beside the text; `QucsApp` opens a `.md` file as one by its
  suffix (`isMarkdownFile`). It is the pattern this proposal follows.
- **The Simulate toolbar** (`qucs_init.cpp`): the simulator list, Simulate (F2),
  Tune, the data display, markers and diagram limits. The simulation console dock
  shows a run's output, with a Stop button.

## The recommendation: a Python editor, as a subclass of the text editor

A dedicated editor for Python files, yes; but a `PythonDoc` derived from `TextDoc`, not
an editor written from scratch. The text editor already has what a Python editor must
have, and a new one would have to do it all again, or lack it:

- line numbers, highlighting, search and replace, undo and redo;
- the file's encoding and its line endings (CR LF kept), read and written back;
- reloading when the file changes on disk, and autosave;
- tabs, the split panes, dropping files on it;
- Claude's tools, which read and edit a text document.

That is about 900 lines (`textdoc.cpp`). A copy would drift from the original with each
fix to one of them. A subclass takes all of it and adds only what is Python's. A
`.py` or `.pyw` file opens as a `PythonDoc`, as a `.md` file opens as a `MarkdownDoc`.

## The Python editor

### Checking as it is typed

About half a second after the typing stops, the text is handed to the interpreter the
settings name, through its standard input, and compiled - not run:

- **Syntax errors,** exactly as that Python gives them: the message, the line, the
  column, and where it ends ("'(' was never closed", line 1, column 5). Python 3.14
  compiles a file in about 25 ms, starting included.
- **The compiler's warnings:** `is` with a literal, an escape sequence that is not one
  (`"\d"`), and the others Python gives as `SyntaxWarning`.
- **More, when a checker is installed:** pyflakes or ruff, if the interpreter has one,
  for names not defined, imports not used, variables assigned and never read. Neither
  is installed here. The editor says which checker it uses, in the toolbar's tooltip.

The check is of the interpreter the script will run with, so a feature of a newer
Python is not an error of an older one, and the other way round. A check still
running when the text changes again is stopped, and only the last one is shown. With
no interpreter, the editor says so once, and highlights as now.

### Showing what was found

- **A squiggle** under the place: red for an error, yellow for a warning.
- **An icon in the line numbers' margin,** on the line.
- **The message** when the mouse is over either.
- **At the end of the line,** the message in faint text (as VS Code's Error Lens has
  it) - optional; see the questions below.
- **The Problems tab** (the message dock's, where Check Schematic lists its findings)
  lists them too, and a click goes to the line.

### Writing Python

- A new line after a line ending in `:` is indented one step further; after `return`,
  `pass`, `break`, `continue` or `raise`, one step less.
- Tab inserts four spaces (the style of PEP 8), Shift+Tab takes them away; a file that
  is indented with tabs keeps tabs.
- Comment Selected puts or takes away `#` (it does so already, by the language).

## The toolbar

A Python toolbar, shown when a Python file is in front, as the Simulate toolbar is for
a schematic, and in the Toolbars menu with the others:

- **The interpreter:** a list of those found (`python3` on the PATH, the one in the
  settings, a virtual environment's `.venv/bin/python` beside the script or in the
  project), and Browse.
- **Run** (F2, the key that simulates a schematic, when a Python file is in front: F1 to
  F10 are all taken, F5 by Show Last Messages): the file is saved and run as a process
  of its own, in the script's folder, with its output in a console dock; the run's exit
  code and its time at the end. An error's traceback names a file and a line: a click
  on it goes there.
- **Stop:** the run ended.
- **Run in Shell:** the file run in the Python Shell dock (`exec` of the file), so
  that its variables are there after it, to look at.
- **Check:** the check now, and the Problems tab brought up.

## Questions to decide

1. **Where Run sends the script.** Its own process, with its output in a console and
   a Stop, as a simulation; or in the Python Shell, its variables kept after it. The
   proposal is both, with Run the first button and Run in Shell beside it.
2. **Warnings beyond the syntax.** pyflakes or ruff when one is installed, nothing of
   our own - or a checker of our own for names not defined and imports not used. The
   proposal is theirs alone: a checker of our own would be wrong in the cases theirs
   were written for (a name made by `from x import *`, by `globals()`, in an `exec`),
   and a warning that is wrong is worse than none.
3. **The message at the end of the line**, besides the squiggle and the margin's icon.

## How it would be tested

- `test_python_doc`: a `.py` file opens as a `PythonDoc`, with everything a text
  document does (encoding, CR LF, reload, undo); a syntax error and a warning are found
  at their line and column, drawn, listed in the Problems tab, and gone when fixed; a
  check overtaken by an edit is not shown; no interpreter, no check and one word of it.
- The toolbar: shown for a Python file, hidden for a schematic; Run saves, runs and
  shows the output and the exit code; Stop ends a script that does not; a traceback's
  line is a link to it; Run in Shell leaves the script's variables in the shell.
- The indenting, by keystrokes.
- Then the usual: the whole suite, the ASan build, and a DMG.
