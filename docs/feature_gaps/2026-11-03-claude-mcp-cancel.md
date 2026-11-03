# The MCP tools: a cancelled call stops, and a call looks at the disk once

The user (2026-11-03), after the dock's fuzz: *"focus on heavily optimizing and enhancing the MCP
implementation so that it becomes very fast and efficient"* - the same words as on 22 October, so this
round starts where [that one](2026-10-22-claude-mcp-speed.md) stopped. I measured first, as then. The
round made two changes and left several things as they are:
- **M1.** A call the client cancels now stops what it waits on. Claude Code sends the cancellation
  the moment its turn is stopped; Qucs-S ignored it, and a solve went on for minutes.
- **M2.** A call looks at the open files on disk once, where it looked twice: 42 % of a cheap call
  with 40 documents open.
- **Considered, not done:** the server was already fast, and the answers' sizes and the tools in
  every turn were measured and found not worth changing.

## Measured

**The dock's sessions since 23 October** (12 sessions with tool use, 914 calls of Qucs-S's tools).
Aggregates only; the script is in this session's scratchpad (`mcpstats/stats.py`, `searches.py`,
`interrupted.py`).

| how the call was allowed | calls | median | p75 | p90 |
|---|---|---|---|---|
| by a rule (the tools that look, and the undoable ones in auto mode) | 774 | 0.14 s | 1.69 s | 32.9 s |
| by Claude Code's classifier | 140 | 1.47 s | 11.9 s | 73.1 s |

- **The simple tools answer end to end in about 20 ms** (`fem_describe`, `get_state`, `get_ui`
  0.02 s; `fem_edit` 0.04 s; `screenshot` 0.11 s). The long ones are long because they compute:
  `fem_solve` 33 s median, 337 s at p90; `rf25_solve` 12 s and 251 s; `fem_converge` 20 s and 442 s.
- **The answers' sizes:** `fem_describe` 20 % of all result text, `batch` 20 %, `read_pdf` 15 %,
  `run_script` 5 %, `get_state` 4.5 % (2.9 KB median). The model already narrows `fem_describe` itself
  (groups, `max_chars`).
- **Searches:** 51 tool searches in 12 sessions. The model's tools were searched in about 20 of 24
  sessions that had a model, the four diagram tools in 18, the RF 2.5D tools in both RF 2.5D sessions.
- **Interrupted calls:** two `fem_solve` calls were interrupted after waiting 455 s and 1,442 s.
  Their solves ran on after the turn stopped, as nothing told Qucs-S.

**Claude Code 2.1.309, probed** (a fake "sdk" server under the real CLI, Haiku; this session's
scratchpad, `cancelprobe/`):
- **A `tools/call` carries a progress token:** `_meta: {claudecode/toolUseId, progressToken}`, the
  token being the request's id.
- **An interrupted turn cancels its calls:** 1 ms after the interrupt, `notifications/cancelled
  {requestId, reason: "AbortError: remote-cancel"}` comes to the server.
- **Progress goes nowhere the dock sees:** `notifications/progress` sent to the CLI is accepted, and
  nothing of it reaches its stream-json output.

**The server** (`qucs-s --mcp-server`, Release, a scratch HOME; `mcpbench/bench.py`):

| | 40 schematics open, before | after |
|---|---|---|
| `describe_tool` (no document read) | 0.864 ms | 0.590-0.703 ms |
| `check_schematic` | 2.354 ms | 1.927 ms |
| `get_netlist` | 2.156 ms | 1.631 ms |
| `get_schematic` | 3.074 ms | 2.588 ms |
| 300 `get_state` pipelined | 626 ms | 569 ms |

With nothing open: `ping` 0.02 ms, `get_state` 0.13-0.17 ms. `tools/list` lists 139 tools in 287 KB;
the 25 in every turn are 41.6 KB.

## M1: a cancelled call stops, and is not answered

**Before.** `notifications/cancelled` was answered as any notification and nothing else was done. The
call ran on - a solve, a script, a batch of solves - and its answer was written to a request no one
waited for. A call held on the dock's own question (auto mode, a file written over) kept its card up.

**Now:**
- **The server** (`mcpserver.{h,cpp}`) keeps the `tools/call` requests under way by id. On a
  cancellation of one of them it tells the host (`ToolHost::cancelCall(caller, tool)`). When the call
  ends, it hands its carrier an empty object: nothing is sent, as MCP has it, and the carrier knows
  the request is over. stdio writes nothing for it but still counts it closed, so the server still
  ends with its input. The dock writes no control response.
- **The dock** (`claudecode.cpp`): a call held on the dock's own question that the program cancels
  takes the question away (`permissionWithdrawn`) and is never made. A host call's bookkeeping (for
  Send to Background) goes with it.
- **The control** (`QucsControl::cancelCall`):
  - A call still waiting its turn behind one that runs alone is taken out.
  - A call under way is stopped where it waits. A tool that waits says so (`stoppable()`, under the
    call that `callToolFor()` began - `running()`) and that it is over (`stopped()`).
  - What stops what:

    | tool | stopped by |
    |---|---|
    | `fem_solve` | its job cancelled, as `fem_stop` cancels it; or not begun, if it waited behind another's |
    | `fem_build` | its build cancelled |
    | `fem_converge` | no mesh after the one solving, and that solve stopped |
    | `rf25_solve` | its solve stopped, or not begun |
    | `simulate` (not in the background) | the run stopped, as Stop Simulation stops it |
    | `wait_for` | the wait ended |
    | `run_script` | the script interrupted, and the call of it under way stopped |
    | `batch` | its call under way stopped, and none after it |

  A batch's later calls and a convergence's later solves begin from the event loop, after the call
  that holds them returned. They run under that call's identity (`callToolWithin()`), so its
  cancellation reaches them.

Stopping is what the dock's Stop means; Send to Background is the way to let a call go on, and a call
sent there is answered already, so it is never cancelled.

**Tests:**
- **`aCancelledCallStopsAndIsNotAnswered`** (test_mcp_server, the real control). It covers:
  - `wait_for` of 60 s: over at once;
  - cancellations of calls not under way: nothing happens;
  - a transient of 200,000 steps: stopped 52 ms after the cancellation, the model free;
  - a script waiting on the first of two solves: both stopped, the second never begun;
  - a batch whose solve began after its first call: stopped, none after it;
  - a call queued behind the batch: taken out at once while the batch runs.
- **`theProgramServesOverStdio`**: a cancelled call writes no line, a ping after it is answered, and
  the server exits with its input in seconds, not the wait's minute.
- **`aCancelledCallIsNeitherAnsweredNorAsked`** (test_claude_code): a host call under way is told,
  and its answer is not written; a held call's question is withdrawn and the call never made.
- **The dock's fuzz** cancels calls of a moment ago, and its host stops some of them.

## M2: a call looks at the disk once

Every call reloads the open documents whose files changed on disk. This is the per-call reload of
`2c0de7e`: a Bash edit of Claude's just before a call must be read before the call reads the document.
`callToolFor()` looked, then `callTool()` looked again, with nothing between. Sampling a cheap tool
with 40 schematics open put 42 % of the call in those two looks. `callTool()` now skips its look when
`callToolFor()` has just made it (`a_lookedOnDisk`); called by itself, it still looks. The cheap call
went from 0.731 to 0.590 ms (the table above).

`QFileInfo::stat()`, to read each file's attributes in one go, was tried and made it slower (1.86 ms:
it reads every attribute macOS has). It was not kept.

## Considered, not done

- **Progress notifications.** Claude Code takes them and shows nothing of them in its stream-json,
  so the dock would gain nothing. A terminal's Claude Code over `qucs-s --mcp-server` might show
  them; that was not checked.
- **`get_state` smaller.** Its guards' explanation and each document's false flags are about 400
  characters a session (1.6 calls of it each), not worth a change of its form.
- **More tools in every turn.** A tier costs every request of every session; one search costs a
  turn, once.
  - The model's tools are 20.9 KB, and the four diagram tools with `edit_trace` and `reload_data`
    34 KB. The sessions that searched for them began before a model was open, so a tier by what is
    open would not have spared those searches.
  - The RF 2.5D tools (12.4 KB) were searched in two sessions.
- **Shorter schemas for the tools in every turn** (41.6 KB, of which `get_dataset` 5.4 KB and
  `add_component` 4.9 KB). A field's description cut is a wrong call more, and a wrong call is a turn.
- **The server's speed otherwise.** Startup took 0.84-1.5 s, once a session. The rest is a
  millisecond or two; the model's turn is seconds.

## Breaks

[breaks.py](../bug_hunts/2026-11-03-claude-dock-fuzz/breaks.py) undoes each piece of M1, and the
fixes of [the dock's fuzz](../bug_hunts/2026-11-03-claude-dock-fuzz.md), one at a time; each is built
and its tests run. The cancellation's thirteen:
- the server's: the notification taken as any; the cancelled call answered; the host not told;
- stdio writing an empty answer, the dock writing one;
- a held call's question kept;
- a call waiting its turn not taken out;
- `callToolFor()` not naming the call;
- `fem_solve`'s job not cancelled;
- a batch going on after its call, a batch's later call not its own;
- `wait_for` not stopped, `run_script` not interrupted.

All are caught. Four of them only once their tests were made sharper:
- **Answering an empty answer.** The stdio test reads its output to the end. A cancelled wait's empty
  answer comes after the ping's, at the wait's next look.
- **The dock's empty answer.** The dock's test cancels a call with no tool use's id too; one with an
  id is answered empty one level up, in `callHost()`.
- **The script and the batch.** A solve begun while a stopped one ends fails at once ("the work it
  waited behind was stopped"). So the batch goes on past failures (`keep_going`), and the script
  catches each call's. Their third solve would then solve the whole transient, unless they are
  stopped.
- **A script's time.** A script runs within its call: its answer is there before the test waits for
  it. Each part's time after the cancellation is now bounded (20 s; within a second here).

M2 changes no answer and has no break.

Not under test: the stops of `rf25_solve`, `simulate` and `fem_converge`. A long RF 2.5D solve, a
simulator, and a convergence of the long transient are not in these suites. Each stop uses what its
tool's own timeout or Stop already uses.
