# Bug hunt: the access tools, after a crash in the Claude Code panel

*1 October 2026 - Qucs-S 26.1.5, `782b901` (the DMG of that commit).*

Qucs-S crashed in the user's hands minutes after the full-control round
(`docs/feature_gaps/2026-10-01-claude-full-control.md`). The report, saved as
`qucs_crash.txt`, shows a SIGSEGV at address 0x30 in the main thread:
`QucsControl::subjectOf` + 24308, called from `Session::handleAssistant`, while the
panel read a message of Claude's. The raw crash report isn't kept here.

This hunt found the cause and then swept the tools of the access rounds for the same
kind of fault. It ran in the foreground on macOS arm64 with ngspice 46. Each
`qucs-s --mcp-server` had its own workspace, settings, HOME and trash in a scratch
folder. Methods:

1. **Symbolication.** The installed binary has the build tree's UUID
   (`E1E8AA8A-1F39-39E1-A11F-2D604F3700CC`). Its disassembly at the crash's address
   (`lldb`, `disassemble`) gives the branch of `subjectOf`.
2. **The host's label functions for every tool**, through a new test,
   `theHostReadsAnyCallUnharmed`. The panel calls `subjectOf`, `irreversible`,
   `askedEachTime`, `actionOf` and `forDocument` on a call's arguments as they come,
   before any check. The test calls each for all 87 tools:
   - with no arguments;
   - with each argument its schema has;
   - with each argument of six wrong types;
   - pinned to a document, too.
3. **An ASan/UBSan build** (Debug, `-fsanitize=address,undefined`, as CI's `linux-asan`
   job). `test_qucs_control` (178 functions) and `test_claude_code` (42) ran whole on it.
4. **An argument fuzzer** of the access tools on the ASan app (`h_accessfuzz.py`, beside
   this report). It sends random, mostly wrong arguments to 20 tools:
   - `get_ui`, `set_ui`, `context_menu`;
   - `console` (reading only);
   - `get_settings`, `set_settings`;
   - `get_text`, `edit_text`, `goto_line`;
   - `rename_file`, `trash_file` and `undo` with `files`;
   - `wait_for`, `simulate` (in the background and not), `simulation_status`,
     `stop_simulation`;
   - `send_input`, `read_help`, `get_state`, `list_documents`.

   It ran with seeds 11-13, before the fix (3,979 calls), and 21-22, after it (4,355
   calls).

Nothing runs a command: `console` never gets `input` or `interrupt`, `send_input`'s keys
are a short harmless list, and `simulate` never gets `allow_commands`.

*Status:* both findings fixed in `bb513bd`, each with a test, and each test checked by
breaking its fix: 5 breaks, each caught.

| | severity | area | finding |
|---|---|---|---|
| F1 | high | crash | A `set_settings` call takes the window down: its subject read `values` through an iterator of an object already gone |
| C1 | low | the tools | `edit_text`'s and `wait_for`'s `revision`, and `send_input`'s points, are made whole numbers unchecked: undefined for -1 or 1e308, and on arm64 a `revision` of -1 passes for 0 |

## F. A crash

### F1. A `set_settings` call takes the window down

The panel shows each of Claude's calls as a line: what it does and on what. For a tool
of Qucs-S that is `QucsControl::subjectOf(tool, arguments)`, called in
`Session::handleAssistant` as soon as Claude's message names the call, before it is
run or asked about. `set_settings`' branch read:

```cpp
for (auto it = a.value(QLatin1String("values")).toObject().constBegin();
     it != a.value(QLatin1String("values")).toObject().constEnd(); ++it)
```

Each `a.value(...).toObject()` is a temporary, destroyed at the end of its expression.
`it` points into the first, which is already gone, and is compared with the end of a
second. The key and value were then read through freed memory.
`QJsonValueConstRef::objectKey` read a null pointer's field, at 0x30. The disassembly
at the crash's address shows it: `QJsonObject::value("values")`, `toObject()`, `size()`,
`~QJsonObject()`, then `objectKey` on what it had just destroyed.

It came in `019aa65` (the settings tools). The tests call tools directly
(`callNow`), and the panel's labels are made only in the panel, so nothing exercised
it. The release build crashed on every such call. The new test crashes the same way on
the old code, every run: SIGSEGV at 0x30.

*Fix:* iterate one object kept in a variable. Nowhere else in the source takes an
iterator of a temporary (a search for `).toObject().constBegin()` and the like across
`qucs/`).

## C. The tools

### C1. Numbers made whole unchecked

UBSan, under the fuzzer: `qucscontrol_text.cpp:118:36: runtime error: -1 is outside the
range of representable values of type 'unsigned long long'`. It was in each of the
three runs before the fix (UBSan reports a place once a process).

`edit_text` made its `revision` an unsigned number as given. -1 or 1e308 so made is
undefined. On arm64 -1 came out as 0, so an edit with `revision` -1 was made to a new
tab, whose revision is 0: the break test showed it ("changed": 1). `wait_for`'s
`revision` was made the same way, and `send_input`'s `click` and `drag_to` were rounded
into an int (`qRound(1e308)`). The fuzzer didn't happen to reach those two, which are
the same fault.

*Fix:* a `revision` is a whole number from 0, and a point is finite and within
10,000,000 of 0. Anything else is refused, and said so. The older conversions of a
number to a whole one in `qucscontrol*.cpp` (the pane, `move`'s `dx` and `dy`, `undo`'s
`steps`, a painting's choice) were checked first already.

## What was found right

- **ASan/UBSan, the tests:** `test_qucs_control` and `test_claude_code` pass whole, with
  no report. That covers every tool of the access rounds as its tests drive it: the
  windows' parts and right-click menus, the text tabs, the consoles, the settings,
  waiting, background runs, raw input, help, undo of moves, the panel's permissions.
- **The fuzzer:** 8,334 calls, no server that died, and no report but C1. That includes
  dialogs left open by a double click or a menu choice (closed by the fuzzer), runs
  stopped half-way, moves undone over and over, and settings set to junk.
- **The host's label functions:** for every tool and every input above, no crash, and
  each tool that changes something has an action to name.
- Nothing was written outside the scratch folder. `~/QucsWorkspace` and
  `~/Library/Caches/qucs-s` were not written. The system's Trash changed at 11:53, after
  the crash and before the first run of this hunt (11:54), and no run of the hunt wrote
  to it: each had a trash of its own (`QUCS_TRASH_DIR`).

## Tests and breaks

- `theHostReadsAnyCallUnharmed`: the five label functions for every tool, as above;
  `set_settings`' subject names each value. Breaks: the old loop (the test crashes, as
  the window did); the values left out of the subject.
- `numbersOutOfReachAreRefused`: `edit_text` and `wait_for` with a `revision` of -1, 1e308
  and 1.5; `send_input` with a point at 1e308 or -1e9. Breaks: each of the three checks
  taken away.

Full suite 80/80, e2e 77/77, 87 tools checked.

## How to run it again

```
cmake -S qucs-s-26.1.1 -B <build-asan> -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
ASAN_OPTIONS=detect_leaks=0 cmake --build <build-asan> --target qucs-s test_qucs_control test_claude_code
python3 h_accessfuzz.py <build-asan>/qucs/qucs-s.app/Contents/MacOS/qucs-s 21 200 <scratch folder> <repository>
grep -h "runtime error\|AddressSanitizer:" <scratch folder>/server.err | sort | uniq -c
```
