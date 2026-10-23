# The file browser: Refresh, folders imported and linked, and renaming

The user, 23 October: "I'd like to add the following new features to the file browser" - a Refresh in
both menus and on the bar; Import Folder; Link Folder and Unlink Folder, with what a linked folder holds
known to the panels and to Claude; and renaming that chooses the name without its suffix.

| Asked for | Now |
|---|---|
| 1. A **Refresh** in the right-click menu and in the ⋯ menu, and a button left of the view button, each forcing the browser out of a stale state | `FileBrowser::refresh()` gives `QFileSystemModel` its root again, which reads every folder anew (the model otherwise reads a folder once and then follows it by its watcher), asks the git tracker for the folder again, fills the Recent list again, and puts the view back as it stood: the folder shown, the tree's open folders, what was chosen, the scroll. It is on the bar as a button (left of the view button, right of the stretch), first in the ⋯ menu, and last in the right-click menu - both on an entry and on the folder shown |
| 2. **Import Folder**: choose one or more folders, copied into the folder shown | "Import Folder…" in the ⋯ menu and in the folder's right-click menu. The folders are chosen in Qt's own dialog (the native one offers no choice of several), then copied by `transfer()` - the same call a drop uses, so a name already there is asked about (replace, keep both, skip, stop), and the browser is refreshed and the last one chosen |
| 3. **Link Folder** and **Unlink Folder**, with the contents of a linked folder shown and known | "Link Folder…" makes a symbolic link in the folder shown for each folder chosen (a free name each; a folder that holds the folder shown is refused, since the link would lead round into itself). "Unlink Folder…" takes a link away and leaves what it led to: on a linked folder's own menu it is that link, in the ⋯ menu it asks which of the links here. What a linked folder holds is shown by the browser (it is a folder to the model), and - the part that was missing - is now part of the project for the Content panel and for Claude: `misc::projectFiles()` no longer passes `QDir::NoSymLinks`, so a linked folder is walked, and each real folder is walked once, so a link that leads back does not go round for ever |
| 4. **Renaming** chooses the name, not the suffix | In place (the item's own editor) and in the dialog: "amp.sch" opens with "amp" chosen, so typing replaces the name and leaves ".sch". A name that is all suffix (".spiceinit"), a name with no dot, and a folder's name are chosen whole, as the Finder does |

## What each change is

**Refresh** (`filebrowser.cpp`). `QFileSystemModel` has no refresh of its own, so the model is given
`setRootPath("")` and then its root again. What the view showed is taken from `currentState()` - the
same record a tab keeps - and put back as `loadTab()` does, with the folders that are not read yet left
to `a_expandPending`. A `refreshed()` signal goes with it. The glyph is a circle with an arrowhead, drawn
like the bar's others.

**Import Folder** and **Link Folder** share `chooseFolders()`: a `QFileDialog` in Qt's own look, with
`ShowDirsOnly`, whose list and tree are set to `ExtendedSelection` so that several folders can be
chosen. Import copies them with `transfer(..., Qt::CopyAction)`. Link makes each link with
`QFile::link`, into a free name, and refuses a folder that holds the folder shown.

**Unlink Folder** takes the link away with `QFile::remove`, which unlinks it: what it led to is not
touched (the test checks the target's files are still there afterwards). It asks first. The ⋯ menu's
entry is enabled only while a link is there, and offers the links of the folder shown by name.

**A linked folder's files** (`misc.cpp`). `collectProjectFiles` now walks symbolic links to folders,
keeping the real path of each folder it has walked (`QSet<QString>`, seeded with the project's own real
path) so each is walked once and a link that leads back ends the walk. The names it returns are
relative to the project, through the link ("shared/brought.sch"), so everything that reads them - the
Content panel, `list_documents`, the ERC's search for `.va` and `.osdi` files, the symbol and
Verilog-A sweeps - sees them as the project's own. The files guard is already right for them: it counts
a path as inside the workspace if either the name or what it leads to is inside, so a file named
through a link in the workspace is inside it.

Probed through the real server on a project with `shared -> .../outside/shared`:
`list_documents` gives `local.sch`, `shared/linked.sch` and `shared/sub/note.md`, where before it gave
`local.sch` alone; the Content panel's tree holds the linked folder too.

**Renaming** (`filebrowser.cpp`). `GitDelegate::setEditorData` - the delegate already on every file
view - chooses the name up to its last dot once the editor is shown (a zero timer, since the view
selects all of it as it opens, and only if it did select all). The dialog for an entry out of sight is
now a `QInputDialog` of its own: its line is given the same selection, and a small event filter
(`NameChosen`) sets it again when the line is shown, since showing the dialog gives the line the focus
and that chooses all of it. (The dialog first had the zero timer too, and the break run showed it did
nothing: the dialog's own showing undid it.) A folder is told from a file by the model's
`FilePathRole`.

## Tests

`test_file_browser` gains four functions, and one is extended:

| test | what it holds to |
|---|---|
| `renamingChoosesTheNameNotTheSuffix` | in place and in the dialog: "amp" of "amp.sch"; "data.tar" of "data.tar.gz"; "filter" of "filter.sch"; the whole of ".spiceinit", of "plain", of "notes" and of the folders "folder.old" and "inner.d" |
| `refreshReadsTheFoldersAgain` | with the model's watcher turned off - the state Refresh is for - a file and a folder written are shown only once Refresh has read them; the folder shown, the choice and the scroll kept |
| `aLinkedFolderIsShownAndUnlinked` | a link is shown, listed by `linkedFoldersHere()`, entered (its files and subfolder shown), then unlinked - the link gone, the target's files still there |
| `aLinkedFoldersFilesBelongToTheProject` | `misc::projectFiles` holds exactly `own.sch`, `shared/brought.sch` and `shared/sub/deeper.txt`; with a link from the shared folder back to the project nothing is named twice and nothing comes from under `back/` |
| `entriesAreMadeAndTheirMenusOffered` | Refresh, Import Folder and Link Folder in the menus; Unlink Folder only where a link is there, on the folder's menu and on the link's own |

`test_file_browser` (20), `test_project_folders`, `test_workspace_projects`, `test_content_categories`,
`test_project_libraries`, `test_qucs_control`, `test_mcp_server`, `test_claude_code`,
`test_git_integration`, `test_library_export` and `test_loader` pass.

## Not done

- **Import Folder and Link Folder in the Projects panel.** They are the file browser's, as asked. "Open
  as Project" already links a folder into the workspace for the Projects panel.
- **A link's own look.** A linked folder is drawn as a folder, with the system's own link mark where the
  platform draws one; nothing of Qucs-S's own says "linked" beside it except the Unlink entry in its
  menu and the target in that entry's status tip.
- **Watching what a link leads to.** The browser follows the folders it shows; a change inside a linked
  folder that the platform does not report for the link's path needs a Refresh, which is what Refresh is
  for.
