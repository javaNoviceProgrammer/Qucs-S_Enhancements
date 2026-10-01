# Feature gaps: the full-control tools, tested live

*1 October 2026 - Qucs-S 26.1.5.*

Claude tested the full-control round's tools live
(`qucs-s-full-control-test-report.md`). It used the window of the crash fix (`019aa65`,
`782b901`, `bb513bd`), restarted at 12:34. Every tool worked, and it left:
- one crash, already fixed;
- five small bugs;
- one smaller doubt;
- one suggestion;
- two calls its approval check blocked, for a check by hand.

Each finding was reproduced first through `qucs-s --mcp-server`, with its own
workspace, settings, trash and HOME, and a stand-in simulator that takes its time.
Each change has a test, and each was broken on purpose to see its test fail.

## The crash

| Report | Now |
|---|---|
| Qucs-S crashed (SIGSEGV at 0x30) as the panel labelled a `get_settings` call: `subjectOf` iterated over an object already gone. The report suggests a test of `subjectOf` for every tool, under ASan. | Fixed in `bb513bd`, with that test: `theHostReadsAnyCallUnharmed` calls `subjectOf`, `irreversible`, `askedEachTime`, `actionOf` and `forDocument` for every tool. It passes them no arguments, each argument of the schema, and each argument of six wrong types. It ran whole under ASan/UBSan, with an argument fuzzer of 20 tools. See `docs/bug_hunts/2026-10-01-access-tools.md`. |

## The bugs

| Report | Now |
|---|---|
| **1. A run's outcome comes back as a JSON *string*.** `simulation_status`, `wait_for` and `stop_simulation` gave a summary in one text, and the outcome's JSON in a second text that read as a string. | One answer, as every tool gives it: the outcome's JSON with a `summary` field ("Simulation 2 stopped; its outcome, as simulate gives it, is this answer."). An outcome that is no JSON (an error said in words) has the summary before its text, in one text. |
| **2. A stopped run is reported as a crash:** `"stopped": true`, with "The simulator crashed or did not start." and exit code -1. | A stopped run has no crash error, whoever stopped it: `stop_simulation`, a background run's timeout, or the user's Simulation > Stop Simulation while Claude waits. It says "Stopped before it ended: its dataset is the one from before the run, or what the simulator wrote of it up to then." A run Claude follows also says by what (`"stopped by": "stop_simulation"`, "its timeout of 5 s"), Qucsator's too. |
| **3. The console's output is noisy:** Python's banner, and the typed line twice ("print(...)" then ">>> print(...)"). | Its answer is what came after the line typed. The echo is the terminal's, and again after the prompt from the program's own line editor (Python's, on its first line); it is dropped, with all that came before it, the banner too. A result the same as the line typed stays: two echoes at most are dropped. The cleaning is one function, `consoleReply`, tested on the transcripts a pseudo-terminal gives. Found on the way: see below. |
| **4. `context_menu` on a row the filter hides says only "No menu came up".** | It says so, and how: "gray_two.cir is hidden by the Content panel's filter "opamp": clear it (set_ui on dock:Content, its "Filter by name" field set to ""), then set it back after - it is the user's." The Projects panel drops what its filter hides from its list, so there it adds "- or there is no such project". A file that is not there at all is told as before. |
| **5. A Claude button in the status bar.** `get_ui statusbar` listed "Claude · mcp__qucs__get_ui 13 s". It is the Claude chip, which says what Claude does and shows and hides the Claude Code panel. | The user's alone, as the panel is. The chip is marked `qucsUsersOnly`, and Claude's tools pass over what is so marked: `get_ui` does not list it (its button or its texts), `set_ui` finds no such control, and `send_input` refuses a click on it ("the Claude chip, which shows and hides it: the user's. Nothing was sent."). |
| **Smaller: the cursor after `goto_line`**, on line 1 in a `get_text` after an Edit > Undo. | Checked with nothing in between: `get_text` gives the line and column `goto_line` went to. The undo had moved it. The test now reads it back. |

## The suggestion

| Report | Now |
|---|---|
| `simulate` of a schematic that is not open is refused ("is not open (open_document opens it)"); "a path-only run would be handy". | A schematic file not open is opened first, as `open_document` opens it (a run is of a document in its tab), and the answer says so: `"opened": "rcl.sch was not open: it is open now, in a tab of its own."`, in the background answer and in the outcome. A path that is no schematic, or no file, is refused as before. |

## The two blocked calls

| Report | Now |
|---|---|
| `send_input {keys: "Ctrl+Q"}`: Qucs-S should refuse a quit shortcut, with nothing sent. | Refused where the key quits. In the window ⌘Q is File > Exit's shortcut, and `send_input` refuses a key that would set off an action Claude does not use, before anything is sent. `rawInputIsSent` tests it with the shortcut bound. Through `--mcp-server` (offscreen) no Quit key is bound at all, so ⌘Q is a plain key there and the server lives on: checked. |
| `context_menu {part: "R1"}` on a schematic. | Checked through `--mcp-server`: Edit Properties, Move Component Text, Set on Grid, Copy, Copy as Image, Paste, Delete, Deactivate/Activate, Rotate, Mirror about X and Y, each with its shortcut. `rightClickMenusAreOpenedAndChosen` tests it. |

## Found on the way

- **Every console answer ended in its prompt** (`"42\n>>> "`), not the first one only. The
  prompt at the end was matched with a space added to the line, and Python's `>>> `
  already ends in one. The prompt is now matched as trimmed.

## Not done

- **The synthesis tools** (section 4 of the wishlist): parked as a proposal by the
  user's choice, `docs/proposals/2026-10-01-synthesis-tools.md`.

## Tests

- **`test_qucs_control`:**
  - `backgroundSimulationsAreFollowed`:
    - the outcome as one JSON with its summary;
    - a stop that is no crash, by `stop_simulation` and by the user's Stop while
      Claude waits;
    - a schematic not open, opened and said;
    - no such file, refused.
  - `consolesAreTypedIntoAskedEachTime`:
    - `consoleReply` on banner and double echo, a result the same as the line, a
      shell's prompt (`$ ` and `bash-3.2$ `), and a run not done;
    - the terminal's and Python's answers exactly `console-42` and `42`.
  - `rightClickMenusAreOpenedAndChosen`: the Content and Projects filters.
  - `theClaudeChipIsTheUsers`: the window shown, so its status row lays the chip out;
    not listed, not set, a click on it refused, and the panel as it was.
  - `textTabsAreReadAndEdited`: the cursor after `goto_line`.

Breaks: 19, each caught. One was caught only after a test was added: the note of a
run stopped by the user. `endSimRun` also notes a stop made through `stop_simulation`,
so removing the run's own note changed nothing until a run stopped another way was
tested.

The changed tests and the access fuzzer also ran on the ASan/UBSan build, with no
report: seed 31, two minutes, 379 calls, no server that died. The full suite passes 81/81 and the e2e scenarios 77/77.
