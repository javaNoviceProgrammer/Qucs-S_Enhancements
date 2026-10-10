# Feature gaps: the git integration's first live check

*10 October 2026 — Qucs-S 26.1.7.*

The check (`qucs-s_git_integration_check_2026-10-09.md`, written at
`a470c62`) drove Claude's `git_*` tools through a scratch repository with a
schematic open in a tab. It found a high-severity bug, a medium one, noise
in schematic diffs, and a minor filter that was ignored. Each item was
reproduced first, through `qucs-s --mcp-server` or the window under test,
with its own HOME, settings, trash and cache and repositories in a scratch
folder (not `~/QucsWorkspace/_git_smoke`). Reproducing showed item 1 to be
worse than reported: a schematic whose first part is the one in conflict was
left with no parts, wires or diagrams at all, and still marked as saved.
All of it is done here, on top of `126c16a`, with four more problems found
on the way.

## 1. A merge conflict broke the open schematic

| Review | Now |
|---|---|
| A merge in conflict over an open `.sch`: Qucs-S read the file with its conflict markers and asked twice "Unknown component! <<<<<< ... load anyway?". After No, the tab had 11 of 12 parts, no wires, diagrams, paintings or labels, and `unsaved changes: false`. A Save would have written that over the merge. | A schematic, data display or symbol whose file holds git's conflict marks is **not read again**. The tab keeps the version it had, whole, and asks nothing. The status bar says why and how to go on (Git > Resolve Conflict), and Claude's next tool answer says the same. The status chip is red, as before, and its tool tip says the tab shows the version from before the merge. |
| Reload once per file change; the second dialog suggests two reloads. | A file state that was not read (in conflict, or failing to read) is tried, and said, **once**. Another notice of the same file state, from the window's watch or from a tool, is let go. The file is read again once it changes. |
| A load that fails, or is turned down, should never leave a partly parsed document that looks clean and can be saved. | **Any** schematic reload that fails now puts back what the tab held. That covers a refused unknown part, a file half written and a wrong file type. The parser was split so that a document can be read from its own text; `load()` keeps that text and reads it back on failure. A tab that was editing its symbol is left in symbol mode. The tab's last-save date is kept, so a Save asks before writing over the file. |
| Keep the last good version in the tab, mark it read-only or "in conflict", and say so in the chip and the File Browser. | Kept, and said in the status bar, the chip's tool tip and Claude's answers. The File Browser already shows the file red with `!`. A Save asks, now in the conflict's own words ("the other side's changes are gone from it; Take Theirs brings them back"). It also asks when the file's date does not show the change. Claude's `save_document` refuses, unless given `replace`, which counts as irreversible and so is asked about each time. A Qucsator run's question ("modified by another program: reload?") now reloads through the same check. |
| Possibly: resolve a `.sch` conflict by choosing ours, theirs or opening both, since Qucs-S cannot show a schematic with markers. | **Resolve Conflict**, a submenu of the File Browser's Git menu for the file, of the status chip's menu and of the Git menu: *Keep Mine*, *Take Theirs*, *Open Both Versions* and *Mark Resolved (Stage)*. The last is enabled once no marks are left. Keep Mine and Take Theirs ask first, then put that side in the file whole and stage it; a side that deleted the file deletes it. Open Both Versions writes `name (mine).sch` and `name (theirs).sch` beside the file (so subcircuits and datasets are found from them) and opens them without the dataset-name question. **Opening** a schematic that holds marks no longer reads it as one: the window offers *Open Both Versions* or *Open as Text*, and Claude's `open_document` says to use `git_resolve`. Claude has a new tool, **`git_resolve`**: `show` (the default) gives mine, theirs and base, and for a schematic what each side changed part by part (section 3); `take_ours` and `take_theirs` are asked about each time; `open` writes and opens the two versions. The merge's answer, `git_status`, and the answers of a revert, cherry-pick or stash pop in conflict list the files `kept in their tabs` and how to go on. |

Found on the way:

- **A merge resolved as mine could not be committed.** Taking ours leaves
  nothing staged, and both `git_commit` and the Commit window refused ("Nothing
  is staged", "nothing to commit"), so the merge could not be finished. A merge
  under way is now committed with nothing staged, and the Commit window's
  header says a merge is under way.
- **A stash brought back in conflict was an error.** It is now said as a merge
  conflict is: the conflicts, that the stash is kept, and how to resolve.
- **Claude's git tools did not reload open documents after a switch, pull,
  stash, reset or abort.** They passed an empty list, so the documents were only
  read later by the window's watch, after the answer. Each tool now reads them
  before it answers and says which were `loaded again`, `kept in their tabs`,
  `could not be read again` or not loaded (unsaved changes). A merge that
  succeeds reloads them too.

- **`git_log` given the repository's folder lost merges.** The packaged
  app's check found this. A merge resolved with theirs whole has theirs as
  its tree. `git log -- .` simplifies history and dropped that merge, so the
  history read as if it had never happened. The top folder is now the whole
  repository, with no path given to git.

Other kinds of document, which the review had not covered: symbols and data
displays are schematics and are kept the same way. A text file or a Python
script is read again with the marks in sight, as an editor shows them, and
the status bar says the marks are in it. Saved and staged, it is resolved.

## 2. `git_ignore` looked for relative paths in a different place

| Review | Now |
|---|---|
| `git_ignore path:"_git_smoke/x.dat.ngspice"` looked only in the open project and failed; the other tools fell back to the workspace. Resolve paths the same way in every git tool. | Every git tool takes a relative `path` or `paths` alike. It tries the open project's folder, the workspace, the folder of the document in front and the File Browser's, and takes the first where the path exists. A path that does not exist yet (a dataset to ignore before a run writes it) goes to the first of those inside a repository. Given a repository as `path`, `paths` are relative to it. The schemas say so. |

Found on the way: **staging a file in `.gitignore` failed with "a.txt"**.
That was the last line of git's message, without the reason. It now says
"in .gitignore, so not staged: a.txt (an ignored file is staged only once git
follows it)".

## 3. Noise in `.sch` diffs

| Review | Now |
|---|---|
| Editing a property moved the part to the end of `<Components>`: blame credits the whole line to the edit, and two branches that touch any part conflict at the list's end. | A part changed keeps its place: a property, a move, a turn or a mirror (Claude's `edit_component`), a replacement (`replace_component`) or the window's property dialog. The parts were taken out of the list and put back last; a guard now puts each back where it was. Drags, the window's rotate and mirror, and the `move` tool kept their place already. A probe across the editing tools gives the same order after each. |
| `View=` (scroll and zoom) changed on every save. | A schematic opens fitted to the window, and that resets the symbol's view too, so the file's `View=` restored nothing. It is now **written back as the file had it**. A new file gets the view of its first save, kept from then on. A property edit in a file this version saved changes that one line. |
| The version header changes once per update; acceptable. | Unchanged. It says which version wrote the file, which a reader needs; it changes once per update, not per save. The part-by-part summary below names it only when something else changed. |
| Possibly: a merge driver for `.sch`, or a semantic diff in the history window. | **What changed, part by part**: parts added, removed, moved, turned, mirrored, (de)activated, their type or properties changed (by the type's names, an equation's by its variable); wires and labels; diagrams, paintings, the symbol; settings. `View=` is left out. It comes before git's lines in Claude's `git_diff` and `git_show`, the Show Changes window, the Commit window's diff of a schematic, the History window's details, and `git_resolve show` (each side against where both started). No merge driver was added: git runs one only once each user configures it, and with parts keeping their place and `View=` stable, two branches that touch different parts no longer conflict. |

## 4. Minor

| Review | Now |
|---|---|
| `get_schematic` with format text and `components: ["I1"]` returned the whole file. | Format `text` with `components` or `region` returns those parts' lines alone, as a `<Components>` section that `set_schematic` or `replace` takes back, and says which names were not found. Format `json` is filtered the same way. Without a filter, both forms return the whole file, as before. |

## Tests

- **`test_git_integration`** (25 functions, 10 of them new):
  - **`aMergeConflictKeepsTheOpenSchematic`:** the reviewer's case on a shipped
    example. No question; the parts, wires and diagrams as they were; the file
    left as git wrote it; asked once; the chip and the Resolve menus; a Save that
    asks in the conflict's words, also with the file's date set back; Claude's
    save refused; `git_status`; Take Theirs from the menu and the tab read again.
  - **`claudeResolvesAConflict`:** the merge's answer; `git_resolve` show (both
    sides part by part), open (beside it, no question) and take_ours; the merge
    committed with nothing staged.
  - **`otherDocumentsInAConflict`:** a symbol and a data display kept; a text
    file and a Python script read again with the marks; one resolved by hand;
    abort.
  - **`aMergeResolvedAsMineIsCommitted`:** the Commit window finishes a merge
    with nothing staged; a stash pop in conflict.
  - **`aFailedReloadPutsTheDocumentBack`:** an unknown part answered No; the
    document as it was, clean; asked once (a second notice of the same file
    state); a Save asks; read again once readable; a symbol being edited stays
    so.
  - **`aConflictedSchematicIsNotOpenedAsOne`:** the open question's *Open Both
    Versions* and *Open as Text*; `open_document`; `resolve`.
  - **`relativePathsAreFoundAlike`:** a project open elsewhere, the File Browser
    in no repository; `git_ignore`, `git_status`, `git_stage` and `git_diff`
    alike; an ignored file staged.
  - **`partsKeepTheirPlace`:** `edit_component` value, move and turn,
    `replace_component`, the property dialog OK'd; the file's order.
  - **`theViewLineStaysAsTheFileHasIt`:** a saved file's `View=` after a zoom and
    an edit; one line changed; a new file's line kept.
  - **`schematicChangesPartByPart`:** the summary on texts (moves, values by
    name, labels, equations by variable, the view left out); `git_diff`,
    `git_show`, staged and not staged; the History and Commit windows.
- **`test_qucs_control`**: `theTextOfSomeParts`, text and JSON by name and
  by region, not found.

**46 breaks, 46 caught.** Each one changes one line of the new code, rebuilds, and runs the test that should catch it. The breaks covered:

- **Reloading:**
  - the conflict guard;
  - the once-per-file-state memory;
  - the put-back and the symbol mode after it.
- **Saving:**
  - the Save box's words, and its showing for a conflict whose date does not move;
  - the refusal under the tools.
- **Opening and resolving:**
  - opening refused;
  - the conflict marks' last line;
  - `resolve`'s side and its staging;
  - the conflict versions' stages;
  - the Resolve menus and the Git menu's;
  - the chip's tool tip;
  - `keptInConflict` once the marks are gone.
- **Claude's tools:**
  - `git_resolve`'s side, its part-by-part changes and its asking;
  - the merge's and `git_status`'s kept tabs;
  - a merge committed with nothing staged, in the tool and the window;
  - a stash pop in conflict;
  - the paths' workspace and not-yet-made rules;
  - the ignored file's message;
  - the top folder's log taken as the whole repository's.
- **Part order and `View=`:**
  - the place kept in each of the four paths, and the splice;
  - `View=` read, and kept once written.
- **The part-by-part summary:**
  - property names, the view left out, moves, labels, equations;
  - its place in `git_diff`, `git_show`, the History and Commit windows;
  - staged against the index;
  - `get_schematic`'s filter.

Six of the 46 escaped their tests at first, and those tests were tightened:

- a file replaced as git does it;
- a second notice of the same file state;
- the File Browser away from the workspace;
- a file changed after staging;
- the file's date set back;
- `keptInConflict` checked before the tab is read again.

One break did not build and was rewritten.

One change has no test of its own: a Qucsator run's question ("modified by
another program: reload?") now reloads through the same check. A test would
have to start a Qucsator run. The path it calls, `reloadFromDisk`, is the one
the tests above cover.

The full suite passes, 103 of 103. Under ASan and UBSan there is no report;
two tests that run the real ngspice under load (`test_status_bar`'s version
probe, `test_qucs_control`'s two runs) failed in the parallel run and pass
alone. The scenario script passes 97 of 97, its control check driving the
property dialog that now keeps the part's place. `check-tool-arguments.py`
agrees on 119 tools. Every run had its own HOME, settings, trash and cache;
the repositories were in the scratch folder.
