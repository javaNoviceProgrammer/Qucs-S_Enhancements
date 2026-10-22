# Bug hunt: the Claude Code panel, fuzzed

*22 October 2026 - Qucs-S 27.0.3, `2c89cec` (the Release build; the ASan/UBSan build where said).
In the foreground: a new fuzzer of the panel, the user's two scrolling reports of the same
afternoon, what reproducing them showed. Unlike the hunts before it, everything found is fixed in
the commit that adds this report, each fix with its test and broken in turn.*

The user's requests:
- "focus on claude chat GUI and run fuzz tests on it to make sure it has no issues and glitches";
- during it, after `2c89cec`: "if I'm up in the chat and claude sends prompts, I see sudden jump
  down and back up in the scroll bar", and "if I'm up reading a prompt but send a new prompt then
  the scroll bar jumps down ... My preference is for scroll bar to say exactly where I am looking at
  the chat history regardless of whether I send a new prompt or claude send me new prompts".

## The fuzzer

`qucs/tests/test_claude_fuzz.cpp` (ctest `test_claude_fuzz`). It drives a real `ClaudeCodePanel`
from both sides.

**The program's side.** It feeds the stream-json messages Claude Code writes, about 8 % of them with a
field replaced or taken out, some cut short, some not the protocol at all:
- `system` messages (init, status, tasks, compaction);
- streamed text and tool input;
- replies with text, tool uses and thinking;
- tool results (text, images, anything);
- permission requests, an MCP server's questions, MCP messages for the dock's own server, cancels;
- turn results with usage and cost;
- rate limits.

The replies' text comes from about 40 kinds of piece: Markdown of every sort, HTML, math (closed
and not), images (there, broken, gone, a folder), runs of up to 20,000 letters without a break, deep
lists and quotes, right-to-left text, NUL.

**The user's side.** It does what a user does:
- renders and resizes;
- clicks every link the transcript has (links to the outside are caught, not opened);
- types prompts and sends them, among them the dock's commands, also through the command list;
- attaches images and removes them;
- edits prompts and cancels the edit;
- presses the permission and question cards' buttons, after filling their fields;
- exports, saves and restores the conversation (also changed in between), and imports sessions of
  Claude Code's own;
- changes the theme, scrolls, stops, sends to the background, starts new conversations.

**After each render it checks:**
- no dialog was left open (one is closed and counted);
- the render took less than 1.5 s (Release);
- the newest plain reply's last paragraph is in the transcript;
- no formula's mark was left unplaced;
- following the end, the view is at the end;
- scrolled up, the view did not move while what was drawn again lay below its top;
- now and then, the transcript as drawn equals the transcript drawn whole, block by block: every
  block's frame, list, block and character formats, and every fragment.

A seed and a trace file replay any step. `QUCS_FUZZ_SEED`, `QUCS_FUZZ_STEPS` (400 by default, CI's
short pass) and `QUCS_FUZZ_SECONDS` set the run; `QUCS_FUZZ_TRACE` names the trace file.

**Runs.**
- Release: 60 seeds of 1500 steps, and two runs of 240 s with big texts (about 71,000 steps each).
- ASan/UBSan: 8 seeds of 1500 steps, and two runs of 300 s (about 26,000 steps each).
- The image fuzz of `test_claude_code` (`QUCS_CHAT_FUZZ`) ran 120 s on ASan.

## Summary

| | Finding | Since | Fixed |
|---|---|---|---|
| **S1** | Scrolled up, a reply coming in: the scroll bar's handle jumps down and back up (the user's report) | `2c89cec` and before | drawn in part; laid out whole in the render |
| **S2** | Scrolled up, a prompt sent: the view goes to the end (the user's report) | `2c89cec` | a prompt leaves the view where it is |
| **P1** | Every render rebuilds the whole transcript and lays it out four times: 87 ms at 10 replies, 256 ms at 40, 720 ms at 120, for each streamed piece (a render every 40 ms) | before | 1 ms at 120 replies |
| **U1** | A turn's result with 1e308 tokens: converted past `qint64`'s range, and two summed wrap round (undefined behaviour, `claudecode.cpp:257` and `:249`; CI's sanitizers stop there). Likewise a rate limit's reset time and a turn's duration; a cost of 1e308 was kept | before; found by the fuzz | bounded |
| **F1** | (In the new render, before it was committed.) A block begun after an image took the image's format as its own, as the image was at that moment: unfitted when drawn whole, fitted when drawn after it | the new render; found by the fuzz | its own format |

Apart from these, the fuzzer found no crash, hang, sanitizer report, dialog left open, unplaced
formula or slow render. The slowest render was 35 ms on ASan. The export dialog that the `/export`
command opens was closed by the fuzzer's watch, as meant.

Five early "findings" were the fuzzer's own; its checks were corrected:
- a mutation had taken out a reply's `type`;
- an import, a restore, a prompt edited and `/new` take replies away;
- near the end, the panel counts the view as following within 16 pixels. When nothing new is drawn,
  it is left there.

## S1, S2: where the reader is

**S1.** A render cleared the transcript and filled it again. Qt lays out a document filled from
empty ("full layout") in steps of about 10 ms after the render. So the view's range was first part
of the document, or a guess: 31,028 px for 33,655 with the old render, and 40,072 for 33,107 on a
fresh one. It came right over the following steps, and the handle moved with each.

Now only what changed is drawn again (P1), and Qt lays that out at once. Where all is still drawn
afresh (an image written again, a conversation brought back), the render finishes the layout itself
and tells the view its size before it sets the bar
(`emit layout->documentSizeChanged(layout->documentSize())`).

**S2.** A prompt sent made the view follow the end again. Now only the reader's scrolling does:
- to the end, the view follows it;
- scrolled up, it stays put whether Claude replies or a prompt is sent.

**The test** `aReaderScrolledUpStaysPut` looks at the bar, its range and the paragraph at the top
at every pass of the event loop while the reader is scrolled up. Meanwhile it brings in 6 replies,
an image, notes, a prompt, and the plot written again. It runs twice: with scroll bars beside the
text, and with bars over it as on macOS (a style with `SH_ScrollBar_Transient`). Beside the text, a
bar shown or hidden makes Qt lay the document out again, which hid the problem. The old code fails
it at the first reply.

`followsTheConversationsEnd` now sends its prompt from the end and stays there.

## P1: drawn in parts

Profiling a 120-reply transcript showed four layouts per render:
- `clear()` hid the scroll bar, and the content brought it back, so QTextEdit laid out again for the
  new width;
- the viewport's resize then fitted the images in an edit of its own, laid out again;
- then the edit's own layout.

The conversation is now drawn in parts, an entry or a row of tools. For each part the panel keeps:
- a key of all it is drawn from: the entry's fields, whether its Edit link shows, the rows and tools
  open, the turn's line as shown, the prompt's images as their files are, and the caption before it;
- where the part starts;
- the block it starts in, as it was before the part.

The colours, fonts and pixel ratio are a key of their own. A render takes out what follows the
unchanged parts and draws from the first part that changed. A render with nothing changed draws
nothing. The images are fitted inside the same edit, so the result is laid out once.

An image shown whose file has changed (a plot written again under its name) has all of it drawn
afresh, the images read again, as before. A streamed piece after 120 replies takes 1 ms (9 ms on
ASan), against 720 ms.

**The test** `aRenderDrawsWhatChangedAlone` checks after each kind of change that the transcript
drawn in part equals the transcript drawn whole, and that drawing started at the right part. The
changes are:
- a note, a reply with lists, a table, code, math and an image;
- tools and their results, rows and tools opened and folded;
- a reply streamed, the turn's end;
- the plot written again (read again: its new size), another theme;
- a reply ending in an image with one streamed after it;
- a prompt;
- a prompt edited while the conversation goes back (the Edit links gone and back);
- a turn's line, right after a prompt's frame, hidden.

It also times the streamed piece and checks that nothing is kept to undo.

## U1, F1

**U1.** All counts read from a message are now whole numbers, 0 up to a bound:
- tokens: 1e15 each, their sums stopping at 1e18, so that a total of four never wraps;
- a reset time: 1e11 s;
- a duration: 1e13 ms;
- a cost outside 0 to 1e9 dollars is taken as the one reported before.

`tokenCount()` (`claudecode.h`) reads the token counts of both the program and a saved
conversation. The test is `numbersPastAnyRangeAreBounded`.

**F1.** `startBlock()` gives a block begun after an image a plain format instead of the image's.
The fuzzer's part-versus-whole check found it at seed 11, step 710. `aRenderDrawsWhatChangedAlone`
covers it.

## Breaks

Each fix was undone in turn, its target built and its tests run:
- 16 on the Release build;
- the three conversions on the ASan/UBSan build, where they abort.

16 are caught, some only after their tests were made sharper:
- the size told at once: by the plot written again while scrolled up;
- the Edit links: by the prompt edited while the conversation goes back;
- the start block put back: by the turn's line hidden after a prompt.

Two are not, by their nature:
- The caption before a part, in its key, cannot change while the earlier parts stay the same, and a
  change of those draws from there anyway. It is kept as cheap insurance.
- Leaving a render with nothing changed only saves time.

Turning undo off was found to be QTextBrowser's own default already. The line was removed, and the
test's check of no undo steps kept.
