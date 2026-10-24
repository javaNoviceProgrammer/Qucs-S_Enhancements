# The third re-check: a call that reads the file, and where a region's edge falls

A re-check of `fe1c00a` (`~/Desktop/qucs-reviews/2026-10-23-claude-mcp-wishlist-recheck3.md`) found all
three items of the second round right - the region drawn by hand, the compounding fallout, the model tab
that follows its file - and left two nits. One is a real race and is fixed; the other is how the new
region rule is meant to work, and is written down here so it is known.

| # | Nit | Now |
|---|---|---|
| **1** | The reload races a call made right after the write: a `run_script` started straight after a `sed` still read `hmax` 0.1, and the next call, seconds later, read 0.2 | A call of its own reads any file changed on disk **as it begins**, instead of waiting for the watcher |
| **2** | A region a point too low loses the line it was meant for: `[100, 600, …]` starts at Eq. (2) where `[117, 600, …]` began a line earlier | Correct by the rule, and said here. No change |

## 1, the file read when the call begins

The watcher is deliberately slow. A file is often written in pieces - a `sed -i`, an editor, a git
checkout - so `QFileSystemWatcher`'s signal starts a 400 ms timer and only what is still there when it
fires is read again. That is right for a person at the keyboard and wrong for a tool call, which arrives
in the same breath as the write that preceded it: the call is answered from what the tab still holds,
and the reload lands a moment later, in time for the *next* call. That is what the re-check saw, and the
likeliest reading of the stale run in the second re-check too.

A probe through the real server shows it plainly. With `coax.qfem` written over an open `m.qfem` and no
pause at all, both `fem_describe` calls that follow gave the old model's objects; with a few seconds in
between, the first already gave the new one. The same probe now gives the new model to a `run_script`
sent straight after the write - the re-check's own case - and that script's answer carries the note
("m.qfem was changed by its file, changed on disk ... and loaded again - now revision 2, it was 1").

So the reading no longer waits for the watcher. At the start of a tool call, every open document whose
file is newer than when the tab read it is read again - the same decision the watcher makes, in the same
code: `reloadOneIfChanged()` is now the one place that holds it, called by the watcher's own loop and by
`QucsControl::readAnyFileChangedOnDisk()`. Nothing about *what* is reloaded changed, only when:

- a tab with **unsaved changes** still keeps them, and the call that found the file changed says so in
  its own answer ("it has unsaved changes in Qucs-S, so the window shows those, not the file") rather
  than in the next one - the reload now runs before the call's notes are gathered;
- a file written by Qucs-S itself, or loaded since, is left alone, by the same comparison of times;
- a modal dialog holds what it edits, so this waits for it exactly as the watcher does;
- a PDF, which follows its own file, and a document with no file are not touched. The kinds are now
  named once, in `QucsApp::followsItsFile()`;
- a file that could not be read, or that is in conflict with the tab, is remembered as it stood
  (`a_notReloaded`), so a call does not try it again until the file changes once more, and the note
  about unsaved changes is said once a minute at most however many calls come.

It is done only for a call **of its own**: not inside a batch, a script, a tune or a preview, and not
while a call that runs alone is under way (`a_callDepth`, `a_alone`, `a_spinning` are all zero). A
reload in the middle of those would pull the ground from under the call it is part of - a script holding
a handle to a component, say.

The cost is one `stat` per open document per top-level call.

## 2, where a region's edge falls (nothing to change)

From the last round, a region reads the runs of text it **meets**. A line whose box tops out just above
the region's upper edge is met by a region drawn from 117 and missed by one drawn from 600 - so
`[100, 600, 495, 660]` starts at Eq. (2) and `[117, 600, …]` starts one line earlier. Both are right by
the rule; a rule that caught a line the region only grazes would instead hand back text the region does
not show. Draw the region from a little above the first line you want, and the answer's
`"of the region"` always says which of the three ways it took.

## Tests

`test_mcp_wishlist` gains a function and has 19.

| test | what it holds to |
|---|---|
| `aCallRightAfterAWriteSeesTheFile` | a model written over with no pause at all is read by the very next call (the event loop has not run, so the watcher cannot have); and a schematic tab with unsaved changes keeps its component, with the call that found the file changed carrying the note |

## Broken on purpose

`breaks.py` gains 4 (T1-T4) and holds 89, all caught: the reading left out of a call, the reading put
after the call's notes are gathered, unsaved changes written over, and a model's file not followed.

## Not done

- **A write *during* a call.** The file is read when the call begins; one written while a long call runs
  is read by the next call, or by the watcher. Reading again at the end would be the wrong way round for
  a call that writes the file itself.
- **Nested calls.** A script that shells out to `sed` and reads the file in its next `qucs.call` still
  waits for the watcher. The guard is deliberate; a script wanting the file can open it again.
