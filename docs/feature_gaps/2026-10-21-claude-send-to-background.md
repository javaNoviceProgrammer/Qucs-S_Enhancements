# Send to Background: Stop's menu in the Claude panel

The user (2026-10-21): *"add a right click menu to the "Stop" button of the Claude chat GUI that has a
menu item called "Send to Background" which then sends the current pending task that Claude is waiting
on to the background so that I can continue sending chat promts. Looks like currently, Claude waiting
on something will block sending more prompts."*

**Was.** While a turn was under way, the composer's button was Stop: a prompt could not be sent until
Claude was done. A long command or a long Qucs-S tool (a solve, a build) held the conversation for its
whole length.

**What Claude Code allows** (version 2.1.285, checked against the program itself over stream-json):
- *Its own commands and agents.* The control request `background_tasks` with a tool use's id moves it
  to the background, as its terminal's Ctrl+B does: the tool's result says so at once, and the turn
  goes on. Asked before the command has become a task (about 3 s after it starts), it answers
  `backgrounded: false`. When a command in the background ends, the program says so
  (`task_notification`) and goes on with a turn of its own, unasked.
- *An "sdk" MCP server's tools*, Qucs-S's own: the request answers `backgrounded: false`. The program
  waits for the answer, whatever is asked; a prompt sent meanwhile waited until the tool was done.

**Now.** A right click on Stop shows Send to Background and Stop. Send to Background is on while Claude
waits on a command, an agent or a Qucs-S tool (`Session::canSendToBackground`); otherwise it is off. The
button's tool tip says so.

`Session::sendToBackground` does this:
- *A command or an agent* is asked to go (`background_tasks` with its tool use). Refused because it is
  not a task yet, it is asked again when the program says it is one (`task_started`). Refused once it
  is a task, the refusal is said in a note. A refusal does not restart the program: refused mode or
  model changes do, and they share the control responses.
- *A Qucs-S tool* is answered at once by the dock. The answer says that it goes on in Qucs-S, that its
  result will come in a message, and that Claude should not call it again meanwhile. Claude Code says
  which tool use a call answers (`_meta` `claudecode/toolUseId`), so the dock keeps each call by it
  until it is answered (`Session::a_hostCalls`). The job itself goes on as it was. When its answer
  comes, it is not written to the call, which is answered already. It is a message to Claude instead:
  - "(Qucs-S: fem_solve, which the user sent to the background, has finished. Its result:)", the
    result's text, and its images;
  - sent as a prompt when no turn is under way, and read in the turn when one is.

  The panel notes it. If Claude Code has ended meanwhile, the panel says the result could not be given.

The turn then goes on without the tool, and usually ends at once, so the composer is free.

**The program's own turns.** A turn the program begins by itself (a command in the background has ended,
a Monitor event) was shown, but the session was not busy: Stop did nothing, and a prompt was sent into
it. Now any reply or stream while no turn is under way begins a turn (`Session::beginTurn`, shared with
`send`). It is as busy as the user's own: Stop stops it, and it ends with its result. The end of a task
in the background is said in a note ("“make all”, sent to the background, has finished.").

**Not done: sending while Claude works.** Claude Code takes a prompt in the middle of a turn: it is read
at the next step, and both are answered in the same turn. The composer could therefore send while busy,
with Stop beside it. That changes what Enter does during a turn, and how a turn's prompts are kept and
edited, so it was left for the user to decide.

**Tests** (`test_claude_code`, fake programs):
- `aCommandGoesToTheBackground`:
  - nothing to send before a turn;
  - asked before the command is a task, the program refuses, then says it is one: asked again, it is
    sent (two requests, both with the tool use's id);
  - the turn ends;
  - a second later the command ends: said once, and the program's own turn is busy (states: Ready,
    Thinking busy, Ready);
  - a command the program will not send: said, and the program is not restarted (one start).
- `aHostsToolGoesToTheBackground`:
  - the host's "change" call waits; sent to the background, it is answered at once with the
    background's text (one answer to the call in all), and the turn ends;
  - the host's late answer is a user message ("(Qucs-S: change, which the user sent to the
    background, has finished. Its result:)" and the text), a turn, and `backgroundFinished`;
  - a call not sent to the background is answered as before.
- `stopsMenuSendsToTheBackground`:
  - no menu while the button is Send;
  - while a command runs, the menu is "Send to Background", "Stop";
  - triggered, the turn ends, the button is Send again, and the notes and Claude's later reply follow;
  - while Claude only writes, Send to Background is off, and Stop stops.

**Against Claude Code itself** (a probe, not kept as a test):
- Haiku ran `perl -e 'sleep 25'` in the foreground. Sent to the background 2.8 s in, it went:
  "Command was manually backgrounded by user". Its end was said. (The run did not log whether the
  first ask or the second one sent it.)
- A slow Qucs-S tool sent to the background was answered at once, and the turn ended. Its result,
  given once that turn had ended, began a turn in which Claude replied with it.
- Haiku, its command sent to the background, chose to wait for it in the same turn (it watched the
  output file). The turn ended with the command, not at once. Which a model does is its own choice;
  Stop still stops it.

**Breaks.** Thirteen breaks, all caught:
- a command not asked to go;
- not asked again once it is a task;
- a refusal before its task taken as final;
- a refusal not said;
- the program's own turn not busy;
- a task's end in the background not said;
- a Qucs-S call not answered at once;
- its late answer written to the call;
- its late answer not given to Claude;
- no tool use kept for a Qucs-S call;
- no menu on Stop;
- the menu's item always on;
- the menu shown while the button is Send. At first this one was not caught: the button is disabled
  while the composer is empty, so the test's right click never reached it. The test now right-clicks
  with text in the composer.

The new tests pass under ASan, as do the earlier tests of the host's tools over the stream and of Stop.
