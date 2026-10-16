# Auto-compact on in the Claude Code chat's conversations

The request (2026-10-16): "can you set the "autocompact" default to true in the claude chat
sessions?"

## What it was

Claude Code compacts a conversation when its context fills, unless its `autoCompactEnabled`
setting is false or `DISABLE_COMPACT` / `DISABLE_AUTO_COMPACT` is set. This was read from 2.1.285:
one function decides it for every session, headless ones included, and it defaults to true. The
dock told the program nothing about it, so each conversation followed the user's own Claude Code
settings: a `/config` change in a terminal, or a settings file, turned it off in the dock too.
Nothing showed which it was.

## What it is now

- **Always told.** Each program the dock starts is given `--settings {"autoCompactEnabled": true}`,
  or false. These are flag settings, which come above the user's and the project's own.
- **A choice under ⋯.** The **Auto-Compact** item is checked unless it was set off. It is kept in
  the Qucs-S settings (`ClaudeCode/autoCompact`), and its tip says what it does.
- **At once.** A change goes to a running conversation as a control request
  (`apply_flag_settings`), not by starting the program again. A version that refuses the request
  is started again with the setting once its turn is over, as for a model.
- **Every tab.** Another tab's conversation takes the setting when its menu shows, and a new tab
  takes it at its start, as does Edit > Settings' reload.

## Checked

test_claude_code:
- theCommandLineCarriesTheOptions: `--settings {"autoCompactEnabled":true}` by default, and
  false when set off.
- theModelAndModeChangeAsTheProgramRuns: the program is started with it on. Set off and on
  again, it is told each time (`apply_flag_settings`), not started again.
- autoCompactIsOnUnlessSetOff (new): the item is checked at first. Turned off, it is kept, and
  the session is off. Another tab picks it up when its menu shows, and a new panel at its start;
  reloadSettings applies it. A session set off before it starts is started with false.

Seven changes broken on purpose, all caught: no `--settings`, off by default, not told while
running, the menu off by default, not loaded at the start, not reloaded, not passed to the
start. test_claude_code 52 of 52.
