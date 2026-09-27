# Bug hunt: the new features - paintings, File Browser, spreadsheets, Markdown/PDF, projects, the Claude Code dock

*26 September 2026 - Qucs-S 26.1.3, `60ec10d`. One hour, 19:48-20:48.*

Aimed at what was added or changed lately (the new paintings, File Browser drag and drop, the
"Any folder is a project" setting, the Claude Code dock and its git bar, spreadsheets, Markdown and
PDF documents), macOS arm64, Qt 6.11.2, with:

1. **Probes**: a QtTest file built against the application's objects (`qucs-core`), run on the
   Release build and on the ASan/UBSan Debug build, each test printing what it found
   (`2026-09-26-new-features/probe_hunt.cpp`); crafted `.xlsx` files made by the scripts beside it.
2. **Schematic mutation**: 210 mutated schematics (components, wires, diagrams, graphs, markers and
   all 13 new paintings) printed (`-p`) and netlisted (`-n`) by the ASan/UBSan build. This used a
   quick throwaway mutator. Afterwards the project's fuzzer, `scripts/ci/fuzz-sch.py`, with
   `--extra docs/bug_hunts/2026-09-26-new-features` (so `seed_paintings.sch` is among its 252
   seeds), seed 926, 40 mutants: 80 runs, no failure.
3. **The GUI monkey** (`qucs/tests/test_gui_monkey`, ASan/UBSan): 10 new seeds x 1,200 steps and
   one of 600.
4. **Reading the code** of the new features; entries found that way say so.

*Status:* F1 (the CI timeout) fixed in `7c8dd74`; everything else below is open. Most urgent: A1 (a PDF deleted by Save As),
A2 (a folder sent to the Trash by Replace), A3 (permanent deletion of any folder), A4 and A5 (text
corrupted / a document emptied by Undo), B1 and B2 (programs run from an untrusted project), C1
and C2 (tiny files that exhaust memory), C3 (a big folder as project freezes the app), A8
(workbooks Excel calls damaged), B3 (a one-conversation permission that becomes the default),
F1 (CI red on main and on the tag).

| | severity | area | finding |
|---|---|---|---|
| A1 | high | PDF viewer | PDF Save As onto the same file under another spelling deletes the PDF |
| A2 | high | File Browser drag and drop | File Browser "Replace" can trash the very folder the moved item lives in (and its siblings) |
| A3 | medium-high | Projects | With "Any folder is a project", Delete Project permanently erases any workspace folder - the question does not even name it |
| A4 | high | Text editor | Edit > Comment/Uncomment corrupts the text (duplicated fragments, an extra line) |
| A5 | medium-high | Text editor, Markdown | Undo right after opening a text document empties it (the load is an undoable edit) |
| A6 | medium | Text editor | The text editor corrupts every non-UTF-8 character of a file when it is saved |
| A7 | medium | Spreadsheets | CSV not in UTF-8 - "Ω" typed into it is saved as "?"; UTF-16 files are read as garbage |
| A8 | medium-high | Spreadsheets | Editing the first cell of a shared formula leaves the workbook damaged for Excel |
| A9 | medium | File Browser | Renaming a file or folder in the File Browser leaves open documents on the old path |
| A10 | low-medium | File Browser | File Browser Rename: a case-only rename is refused, and a name with "../" moves the file |
| B1 | medium-high | Claude dock git bar | The Claude dock's git bar runs programs named by the project's own .git/config |
| B2 | medium | Markdown, PDF viewer | A link in a Markdown preview or a PDF launches programs without asking |
| B3 | medium | Claude Code dock | "Allow All Edits" in one conversation silently becomes the default for every conversation, after restarts too |
| C1 | high | Spreadsheets | A 1.3 KB .xlsx with one cell at row 2,147,483,647 takes 25 GB and a minute to open |
| C2 | medium-high | Spreadsheets | A small .xlsx can exhaust memory - the ZIP reader inflates without any limit |
| C3 | high (with any-folder projects) | Content panel | A large folder opened as a project freezes Qucs-S - the Content panel walks the whole tree on the GUI thread every 3 s |
| C4 | medium | Paintings | A large Formula painting, zoomed in, allocates a 4.6 GB image |
| C5 | low-medium | Diagrams | A marker's (or trace's) precision from a file is not clamped - one label can become gigabytes |
| D1 | medium | Paintings | Callout/TextBox pointer tip is not clamped on load - signed overflow when turned |
| D2 | low-medium | Spreadsheets | Signed integer overflow (UB) parsing column letters and `<col min>` in .xlsx |
| D3 | low-medium | Spreadsheets | A date-formatted cell holding 1e300 or nan is converted to qint64 unchecked (UB) |
| D4 | low | Paintings | Waveform cycles and Dimension scale accept NaN / inf from a file, and save them back |
| E1 | medium | Claude Code dock | A spreadsheet (or PDF) open in Qucs-S that Claude changes is not reloaded - "could not be loaded again" |
| E2 | low-medium | Projects | With "Any folder is a project", opening a folder silently creates a `Scratch` folder in it |
| E3 | low-medium | PDF viewer | The PDF viewer stops following its file for good once the file is deleted and written again later |
| E4 | low-medium | Claude Code dock | /resume lists none of Claude Code's sessions when the folder's path goes through a symlink |
| E5 | low-medium | Spreadsheets | Saving a CSV whose name contains "%1" as .xlsx writes a broken workbook |
| E6 | low | Markdown, Claude dock | `$...$` inside an indented code block (or `<code>`) is typeset as math |
| F1 | medium | CI | CI - test_file_browser_drop hangs (timeout) on the Linux ASan job; main and the tag are red |

---

## A. Data lost or corrupted

### A1. PDF Save As onto the same file under another spelling deletes the PDF

**Severity:** high (the document is permanently deleted - `QFile::remove`, not the Trash)
**Area:** PDF viewer (new) - `pdfdoc.cpp` `PdfDoc::save()` (lines ~1150-1170)

#### Reproduce (probe `pdfSaveAsSameFileOtherSpelling`, macOS)
1. Open `report.pdf`.
2. File > Save As, and give `Report.pdf` in the same folder (on macOS/Windows the same file) - or the
   same file through another path: a symlinked workspace, `/tmp` vs `/private/tmp`, a Dropbox link.
   Confirm the dialog's "replace?".

#### Observed
    QCRITICAL: .../report.pdf could not be copied to .../Report.pdf.
    size before 16072 save() returned -1 report.pdf exists: false Report.pdf exists: false QList()
`save()` decides "same file" by comparing path strings
(`QFileInfo(a_source).absoluteFilePath() == a_DocName`), which differ in case/spelling; it then
`QFile::remove(a_DocName)` - which *is* the source file - and `QFile::copy(a_source, a_DocName)` fails
because the source is gone. The folder is left empty; the PDF is not in the Trash.

#### Expected
- Compare files, not strings (canonical paths, or `QFileInfo::operator==` / inode) and do nothing
  when they are the same file.
- Never delete the target before the copy has succeeded: copy to a temporary name beside it and
  rename over (QSaveFile-like), so a failed copy leaves the old file.

Probe: `2026-09-26-new-features/probe_hunt.cpp` -> `pdfSaveAsSameFileOtherSpelling`.

#### The same remove-then-copy pattern elsewhere
- `qucscontrol.cpp:3182-3185` (Claude's `simulate` with `keep_as`): `kept = <name> + ".dat.ngspice"`
  beside the schematic, then `QFile::remove(kept); QFile::copy(dataset, kept)`. With `keep_as` equal
  to the schematic's own base name (`amp` for amp.sch - or `Amp` on macOS/Windows), `kept` *is* the
  dataset just written: it is deleted, the copy fails ("not kept: ... could not be written"), and the
  simulation's results are gone (a re-run brings them back).
- `dialogs/librarydialog.cpp:385-386`: `QFile::remove(target); QFile::copy(source, target)` - the
  same shape; a failed copy leaves nothing.

### A2. File Browser "Replace" can trash the very folder the moved item lives in (and its siblings)

**Severity:** high (a whole folder goes to the Trash; the move then fails)
**Area:** File Browser drag and drop (new) - `filebrowser.cpp`, `FileBrowser::transfer` / `FileBrowser::refusal`

#### Reproduce
    W/
      x/
        other.txt      "also precious"
        x/
          data.txt     "precious"

Drag `W/x/x` (the inner folder) onto `W` (the folder shown, or its row/crumb).
The question "W has an item named x already. Replace it with the one being moved?" comes up;
answer **Replace**.

#### Observed
- `QFile::moveToTrash("W/x")` trashes the *outer* x - which holds the inner x being moved and
  `other.txt`.
- Then `moveEntry("W/x/x", "W/x")` fails (the source is gone), and a second box says
  "These could not be moved into W: x".
- Result: `W/x` does not exist any more; `data.txt` and `other.txt` are only in the Trash.

#### Expected
Refused up front (Finder: "can't be replaced because it contains the item being moved"), or
at least not offered: `refusal()` checks "a folder into itself" but not "the destination is an
ancestor of the source". A check like `isInside(source, dest)` before trashing `dest` fixes it.

Probe: `2026-09-26-new-features/probe_hunt.cpp` -> `fileBrowserReplaceAncestor`.

### A3. With "Any folder is a project", Delete Project permanently erases any workspace folder - the question does not even name it

**Severity:** medium-high (unrecoverable data loss, made reachable for every folder by the new setting)
**Area:** Projects - `qucs.cpp` `QucsApp::deleteProject` (lines 2002-2011) / `recurRemove` (1945-1953)
**Found by:** code inspection

#### Reproduce
1. Settings > Locations > Projects: turn on "Any folder is a project".
2. A workspace that holds more than Qucs projects - common when the workspace is the home folder,
   Documents, or a folder of work: the Projects panel now lists *every* non-hidden folder
   (Desktop, Documents, Library, a git checkout, ...).
3. Select one, Project > Delete Project (or the button), answer Yes.

#### Observed
- The only question is "This will destroy all the project files permanently ! Continue ?" - it does
  not say *which* folder, how many files, or that it is not a `_prj` folder Qucs-S made.
- `recurRemove` -> `QDir::removeRecursively()`: everything in the folder is deleted for good, not
  moved to the Trash (the File Browser, by contrast, trashes).
- Before the setting only `NAME_prj` folders could be deleted this way; now any folder can.

#### Expected
Move the folder to the Trash (`QFile::moveToTrash`) instead of deleting it; name the folder (and
its size / file count) in the question; for folders that are not `NAME_prj` (i.e. projects only by
the setting), at least a second, explicit confirmation - or offer "Remove from workspace" only.

### A4. Edit > Comment/Uncomment corrupts the text (duplicated fragments, an extra line)

**Severity:** high (silent corruption of netlists/sources with an everyday command)
**Area:** Text editor - `textdoc.cpp` `TextDoc::commentSelected` (lines 524-579)
(the code predates the enhancements - probably also upstream - but the release notes advertise it)

#### Reproduce (probe `textCommentPartialSelection`, a SPICE file)
    R1 a b 1k
    C1 b 0 1n
    L1 b c 1u
1. Select from after "R1 " to the middle of the second line; Comment/Uncomment.
2. Select lines 1-2 the usual way - from the start of line 1 down to the start of line 3; Comment/Uncomment.

#### Observed
    1 ->  R1 *R1 a b 1k
          *C1 b 0 1nb 0 1n
          L1 b c 1u

    2 ->  *R1 a b 1k
          *C1 b 0 1n
          *L1 b c 1uL1 b c 1u
- The lines are rebuilt whole, but `insertPlainText` replaces only the *selected characters* (the
  local cursor extended to the lines is never applied with `setTextCursor`), so the part of the
  first line before the selection and the part of the last line after it appear twice.
- A selection ending at column 0 of a line counts that line too: it is commented and duplicated.
- Lines are looked up with `findBlockByLineNumber` (laid-out lines, which differ from blocks when
  a long line wraps) while the range comes from `blockNumber()`.
- With no comment mark (JSON, Markdown, XML, ...) the selection is still replaced by whole lines.

#### Expected
Extend the widget's own cursor to whole blocks (not counting a final block the selection only
touches at column 0), use `findBlockByNumber`, edit in one undo step, and do nothing for languages
without a line comment.

### A5. Undo right after opening a text document empties it (the load is an undoable edit)

**Severity:** medium-high (one habitual Cmd+Z wipes the document; a Save then writes an empty file)
**Area:** Text editor (and Markdown, a TextDoc) - `textdoc.cpp` `TextDoc::load` (line ~408) / `reload` (430)

#### Reproduce (probe `textUndoAfterOpenAndLatin1`)
Open any text document (netlist, Verilog-A, .md, ...) and press Edit > Undo (Cmd/Ctrl+Z) once.

#### Observed
    after opening: undo steps 4 undo available true
    after one Undo: text length 0 modified true
`load()` fills the editor with `insertPlainText(stream.readAll())`, which QTextDocument records as an
edit: Undo removes the whole file's text, and the document is marked modified - Save (or the save
before a simulation) writes the empty document over the file. The same after `reload()` (a file
Claude changed, "loaded again with Claude's changes"): Undo takes the document to empty, not to the
version before Claude's change.

#### Expected
Load with `setPlainText()` (or call `document()->clearUndoRedoStacks()` after inserting), so the
undo history starts at the file as loaded; `setModified(false)` already marks it clean.

### A6. The text editor corrupts every non-UTF-8 character of a file when it is saved

**Severity:** medium (vendor model libraries, old netlists and scripts lose their °, µ, ©, Ω)
**Area:** Text editor - `textdoc.cpp` `TextDoc::load` / `TextDoc::save` (QTextStream, UTF-8 only)

#### Reproduce (probe `textUndoAfterOpenAndLatin1`)
A SPICE library in Latin-1/Windows-1252 (as many vendor `.lib` files are):
`* (c) 2019 Vendor, T=25°C, 10 µA`. Open it, change anything (or nothing) and save.

#### Observed
    Latin-1 file saved back: "... 32 35 ef bf bd 43 2c 20 31 30 20 ef bf bd 41 ..."
`QTextStream` reads as UTF-8: each invalid byte becomes U+FFFD (shown as �), and the save writes
U+FFFD back as `EF BF BD` - the original characters are gone for good, without a warning. (The
spreadsheet reader, by contrast, falls back to Latin-1 - see A7 for its own problems.)

#### Expected
Detect the encoding on load (UTF-8 valid? BOM? else the local 8-bit code page), remember it and
save in it - or at least warn when a file with invalid UTF-8 is about to be saved.

### A7. CSV not in UTF-8 - "Ω" typed into it is saved as "?"; UTF-16 files are read as garbage

**Severity:** medium (silent data loss on save; unreadable Excel "Unicode Text" exports)
**Area:** Spreadsheets (new) - `spreadsheet.cpp` `readCsv` (lines 680-685) / `writeCsv` (line ~760)

#### Reproduce (probe `csvLatin1Edit`)
1. A CSV saved by a Windows program in its ANSI code page (`10 kµ`, byte 0xB5) - Qucs-S reads it as
   Latin-1 (`latin1 = true`).
2. Type `4.7 kΩ` into a cell (Ω is everywhere in a circuit simulator's data) and save.
3. A tab-separated file in UTF-16 LE with a BOM (what Excel writes for "Unicode Text").

#### Observed
    read as latin1: true "10 kµ"
    typed "4.7 kΩ" saved and read back: "4.7 k?"
    UTF-16 read: latin1 true cells "ÿþp\u0000a\u0000r\u0000t\u0000" "\u0000v\u0000a\u0000l\u0000u\u0000e\u0000"
- `writeCsv` does `text.toLatin1()` for a Latin-1 book: every character outside Latin-1 becomes `?`
  with no warning - the saved file has lost data.
- A UTF-16 file (BOM FF FE / FE FF, or NUL bytes) is not recognised: it falls back to Latin-1, the
  cells are full of NULs and `ÿþ`, and the delimiter detection fails.
- (Also: Windows-1252 bytes 0x80-0x9F - €, smart quotes - show as invisible C1 control characters.)

#### Expected
- Detect UTF-16/UTF-32 BOMs; treat "not UTF-8" as the local 8-bit code page (Windows-1252) rather
  than ISO-8859-1.
- On save, when the text can no longer be encoded in the file's encoding, ask (or switch the file
  to UTF-8 with a BOM) instead of writing `?`.

### A8. Editing the first cell of a shared formula leaves the workbook damaged for Excel

**Severity:** medium-high (Excel: "We found a problem with some content..."; formulas of other cells lost)
**Area:** Spreadsheets (new) - `spreadsheet.cpp` `cellXml` (line ~356) / `readSheet` (formula read at line ~303)

#### Reproduce (probe `xlsxSharedFormulaMasterEdited`)
Excel stores a formula filled down B1:B3 as a *shared* formula:

    B1: <f t="shared" ref="B1:B3" si="0">A1*2</f>
    B2: <f t="shared" si="0"/>
    B3: <f t="shared" si="0"/>

Open it in Qucs-S, change B1 (to `=A1*3`, or any value), save.

#### Observed
    <c r="B1"><f>A1*3</f></c>
    <c r="B2"><f t="shared" si="0"/><v>4</v></c>
    <c r="B3"><f t="shared" si="0"/><v>6</v></c>
The master is rewritten without `t="shared" ref si`, so B2 and B3 refer to a shared formula
`si="0"` that no longer exists. Excel repairs the file on opening (with its warning) and B2:B3 lose
their formulas. The same holds when B1 is cleared.
Also: in Qucs-S the dependents show no formula at all (`B2 formula ""`) - their edit text is the
value `4`, not `=A2*2`.

#### Expected
When a shared-formula master is changed, write the dependents again with their own formulas
(translated from the master's, or at least as plain values plus a recalculation), or move the
master to the next cell of the range; show dependents' formulas. Same care for `t="array"`
formulas and cells covered by a data table.

Probe files: `2026-09-26-new-features/make_xlsx.py` (the sheet XML above).

### A9. Renaming a file or folder in the File Browser leaves open documents on the old path

**Severity:** medium (the next Save writes the old name again: two copies, edits in the "wrong" one)
**Area:** File Browser (new) - `filebrowser.cpp` rename (in place through `QFileSystemModel` with
`setReadOnly(false)`, line 1019, and the Rename dialog, line 1903); `qucs.cpp:704`

#### Reproduce
1. Open `proj/amp.sch` in a tab.
2. In the File Browser rename `amp.sch` to `amp2.sch` (or rename the folder `proj`).
3. Edit the schematic and save.

#### Observed
- Drag-and-drop moves emit `FileBrowser::moved(from, to)`, which `QucsApp::documentsMoved` uses to
  retarget open documents and the Claude conversations pinned to them (`qucs.cpp:704`).
- Renames emit nothing: the probe's spy saw `moved() emitted: 0` after two successful renames, and
  `QFileSystemModel::fileRenamed` (the in-place path) is not connected anywhere.
- The tab keeps `proj/amp.sch`: saving recreates it beside `amp2.sch`; a Claude conversation pinned
  to it says "not open"; for a renamed folder every open document inside it is affected.

#### Expected
Renames go through the same `moved(from, to)` signal (connect `QFileSystemModel::fileRenamed`, and
emit it from the dialog path).

Probe: `2026-09-26-new-features/probe_hunt.cpp` -> `fileBrowserRenameDialog`.

#### Related: Move to Trash
`FileBrowser::moveToTrash` (filebrowser.cpp ~1907) trashes a file or folder with open documents
without telling anyone either: the tabs stay open on paths that no longer exist, and the next Save
silently recreates the trashed file (or the whole folder path) in place.

### A10. File Browser Rename: a case-only rename is refused, and a name with "../" moves the file

**Severity:** low-medium
**Area:** File Browser (new) - `filebrowser.cpp` `FileBrowser::rename` (lines 1896-1904; the dialog
path, used in Recent Documents and for entries not in the view shown)

#### Reproduce (probe `fileBrowserRenameDialog`, macOS)
1. Rename `proj/amp.sch` to `Amp.sch`.
2. Rename `proj/filter.sch` to `../escaped.sch`.

#### Observed
    case only: ("“amp.sch” could not be renamed to “Amp.sch”.")   files: amp.sch, filter.sch
    with ../:  ()   proj: (amp.sch)   root: (escaped.sch)
- `QFileInfo::exists(target)` is true on a case-insensitive file system (macOS, Windows) when only
  the case changes, so the rename is refused although `rename()` would work.
- The new name is joined with `info.dir().filePath(name)` unchecked: "../x", "sub/x" or an absolute
  path move the file to another folder (no clash question, no trash, nothing like a move's checks).

#### Expected
- Allow a rename whose target is the same file (compare canonical paths / inode), as the Finder does.
- Refuse names containing '/' (and '\\' on Windows), "." and "..".

## B. Security: programs run from an untrusted project, permissions that outlive a conversation

### B1. The Claude dock's git bar runs programs named by the project's own .git/config

**Severity:** medium-high (code execution from opening an untrusted project folder)
**Area:** Git status bar (new) - `gitstatus.cpp` `run()` / `status()` / `diff()`

#### Reproduce
A project folder that is a git repository whose `.git/config` has

    [core]
        fsmonitor = /path/in/the/project/hook.sh

(any command). Open it as the project in Qucs-S with the Claude Code dock's git bar on (the
default) - or anything that calls `qucs_s::git::status()`.

#### Observed
`git diff --numstat ...` (and `ls-files`, and `diff()` when Show Changes opens) consult
`core.fsmonitor` and execute it. The probe's hook wrote its marker file both on `status()` and on
`diff()`: **"fsmonitor hook ran: true"**. No click is needed: the bar refreshes when the project is
opened. A project downloaded as a zip (with its `.git`) is owned by the user, so git's
`safe.directory` protection does not apply.

Other repo-configurable programs on these code paths: `diff.<driver>.textconv` (numstat/diff of a
file with a textconv attribute), `core.fsmonitor`, `filter.<x>.clean` (index refresh of a
modified file with a filter attribute).

#### Expected
Read-only git calls from Qucs-S never run repo-configured programs, e.g. run git with
`-c core.fsmonitor=false -c core.untrackedCache=false --no-textconv` (and `--no-ext-diff`, which is
already passed), or only look at repositories the user has trusted.

Probe: `2026-09-26-new-features/probe_hunt.cpp` -> `gitStatusRunsRepoConfiguredPrograms`.

### B2. A link in a Markdown preview or a PDF launches programs without asking

**Severity:** medium (one click on a disguised link runs a program shipped with a project)
**Area:** Markdown documents (new) - `markdowndoc.cpp` `MarkdownDoc::followLink` (lines 388-403);
PDF viewer (new) - `pdfdoc.cpp:1071` (`PageView::urlActivated` -> `QDesktopServices::openUrl(url)`)

#### Reproduce
A project (downloaded, cloned) with

    README.md:      [Open the schematic](evil.command)
    evil.command:   #!/bin/sh ... (or setup.bat / tool.exe / Something.app on Windows / macOS)

Open README.md in Qucs-S (it opens in Preview mode by default) and click "Open the schematic".

#### Observed (probe `markdownLinkToProgram`, with a harmless URL handler registered)
    handed to the system: QList(QUrl("file:///.../evil.command"), QUrl("x-custom://whatever"))
- Any existing file whose suffix is not one Qucs-S opens goes to `QDesktopServices::openUrl`:
  on macOS a `.command` runs in Terminal and an `.app` launches; on Windows `.exe`, `.bat`, `.cmd`,
  `.lnk`, `.js`, `.vbs` run.
- Any URL scheme other than file goes to the system too (custom URL handlers of installed apps,
  `smb:`, `x-apple.systempreferences:`, ...).
- The PDF viewer does the same for every link annotation of a PDF (`file:` links included).

The Claude Code dock already does this safely: only http/https/mailto go outside, and local files
open inside Qucs-S (`claudecodepanel.cpp:1639-1644`).

#### Expected
Follow the dock's rule: http(s)/mailto outside; files Qucs-S opens itself; anything else -
executables, other schemes - only after a confirmation that names the target (or never), and
"Show in Finder/Explorer" instead of opening for unknown files.

Probe: `2026-09-26-new-features/probe_hunt.cpp` -> `markdownLinkToProgram`.

### B3. "Allow All Edits" in one conversation silently becomes the default for every conversation, after restarts too

**Severity:** medium (a permission meant for one conversation persists everywhere; the dock says the opposite)
**Area:** Claude Code dock - `claudecodepanel.cpp:503-506` (permissionModeChanged handler) and `:536`
**Found by:** code inspection (no test covers it)

#### Reproduce
1. Permissions: Ask Before Acting (the default).
2. In a conversation, Claude wants to edit a file; answer the card with **Allow All Edits**.
   The dock notes: "Claude may change files without asking for the rest of this conversation."
3. Open a new conversation (+), or restart Qucs-S.

#### Observed
The handler of `Session::permissionModeChanged` does
`QucsSettingsFile().setValue(kMode, mode)` - `ClaudeCode/permissionMode = acceptEdits` - and every
new panel starts from that setting (`a_session->setPermissionMode(settings.value(kMode)...)`, line
536). So the new conversation, the other tabs created later, and every conversation after a restart
edit files without asking. The ⋯ > Permissions menu shows "Accept Edits" checked there, but nothing
told the user their default changed.

#### Expected
Allow All Edits changes only that conversation's session (as the note says); the saved default
changes only when the user picks a mode in the Permissions menu.

## C. Memory and time: tiny files and big folders that exhaust the machine

### C1. A 1.3 KB .xlsx with one cell at row 2,147,483,647 takes 25 GB and a minute to open

**Severity:** high (denial of service from a tiny file; out of memory on most machines)
**Area:** Spreadsheets (new) - `spreadsheet.cpp` `readSheet` (lines ~277-281) and `Sheet::cell`

#### Reproduce
    python3 2026-09-26-new-features/make_xlsx.py row-huge.xlsx \
      '<sheetData><row r="2147483647"><c r="A2147483647"><v>1</v></c></row></sheetData>'
Open `row-huge.xlsx` (1,366 bytes) in Qucs-S.

#### Observed (probe `xlsxZipBomb` with PROBE_BOMB=row-huge.xlsx, ASan build)
    file 1366 bytes; read true "" in 61383 ms; peak RSS 25148 MB
`rowIndex = r - 1` is taken as is and `sheet.rows.resize(rowIndex + 1)` allocates 2^31 rows. The
same happens through `Sheet::cell(row, ...)`. On a machine without 25+ GB free the app is killed;
otherwise the GUI thread is frozen for a minute and the table then has 2^31 rows.

#### Expected
Rows beyond Excel's limit (1,048,576) refused; and a sheet stored sparsely (or only up to the last
row that has content) so that one far-away cell does not allocate every row before it.
Even `r="1048576"` - legal, and common in files where a style was applied to a whole column -
allocates a million empty rows.

Probe files: `2026-09-26-new-features/make_xlsx.py`.

#### And in the table
`SheetModel::refresh()` (sheetdoc.cpp:212) sizes the view as `s.rowCount() + kMoreRows`: with the
2^31 rows above that addition overflows `int` (undefined behaviour) before the view is even drawn.
The view also always offers `kMoreRows`/`kMoreColumns` beyond the data with no cap, so typing at
the edge can go past Excel's 1,048,576 rows / 16,384 columns (XFD) and write a workbook Excel
rejects.

### C2. A small .xlsx can exhaust memory - the ZIP reader inflates without any limit

**Severity:** medium-high (a 200 KB file -> 1.9 GB of memory; a few MB -> out of memory / crash)
**Area:** Spreadsheets (new) - `zipfile.cpp` `zip::inflate` / `zip::read`, used by `sheet::readXlsx`

#### Reproduce
    python3 2026-09-26-new-features/make_bomb.py bomb200.xlsx 200     # 205,194 bytes; its sheet is 200 MB of spaces
Open `bomb200.xlsx` in Qucs-S (File Browser, Content panel, File > Open, a drop).

#### Observed (release build, probe `xlsxZipBomb`)
    file 205194 bytes; read true "" in 2274 ms; peak RSS 1899 MB
- `inflate()` appends to `out` with no cap: not the entry's declared uncompressed size (known from
  the central directory before inflating - it is only compared *after* inflating), not an absolute
  limit. DEFLATE reaches ~1000:1, so every MB of file is ~1 GB of output, and the XML/QString
  copies after it multiply that by ~9.
- Back-references are copied one byte at a time (`out.append(out.at(from + k))`), so the GUI thread
  is also frozen for seconds per hundred MB.
- A 2 MB file would ask for ~19 GB: the app is killed or the machine swaps.

#### Expected
- Stop inflating once the output exceeds the entry's declared `size` (and fail), and refuse
  entries/workbooks above a sane limit (e.g. a few hundred MB total).
- Read on a worker thread, or at least not block the GUI thread for large files.

Probes: `2026-09-26-new-features/make_bomb.py`, `2026-09-26-new-features/probe_hunt.cpp` -> `xlsxZipBomb`.

### C3. A large folder opened as a project freezes Qucs-S - the Content panel walks the whole tree on the GUI thread every 3 s

**Severity:** high once "Any folder is a project" is on (the app becomes unusable); low before it
**Area:** Content panel - `projectView.cpp` `refresh()` / `listingSignature()` / `refreshIfChanged()`
(poll every `ContentRefreshSeconds` = 3, `ContentAutoRefresh` = true by default);
`misc.cpp` `collectProjectFiles` (unbounded recursion)

#### Reproduce (probe `contentPollOfLargeFolder`, release build)
Turn on "Any folder is a project" (new) and open a big folder as the project - a git checkout with
build output, a folder of downloads, the home folder.

#### Observed
    this repository (with its build trees):  8748 files;   one walk took   437 ms
    ~/git:                                    454318 files; one walk took 33393 ms
- `listingSignature()` walks every file of every subfolder (and stats each for its date) on the
  GUI thread, every 3 seconds, to see whether anything changed; `refresh()` walks it again and
  opens every `.sch` (`Schematic::testFile`).
- With a big tree the window freezes for the walk (33 s here), then 3 s later it starts again: the
  app never becomes responsive. The Content panel would also list hundreds of thousands of rows.

Before the setting only `NAME_prj` folders could be projects - small in practice - so this was
latent.

#### Expected
Walk in a worker thread (or use a `QFileSystemWatcher`/`QFileSystemModel` per expanded folder), cap
depth/count (and say so in the panel), skip heavy folders (`.git`, `node_modules`, build trees,
`Scratch`), and don't poll the whole tree on a timer.

Probe: `2026-09-26-new-features/probe_hunt.cpp` -> `contentPollOfLargeFolder` (PROBE_BIGDIR=<folder>).

### C4. A large Formula painting, zoomed in, allocates a 4.6 GB image

**Severity:** medium (out of memory / a frozen window from a legal painting and an ordinary zoom)
**Area:** Paintings (new) - `paintings/formulapainting.cpp` `paint()` (lines 103-117) / `image()`

#### Reproduce (probe `formulaBigZoomed`)
Place a Formula of size 400 (the dialog's maximum) holding a transfer function such as
`H(s) = \frac{\omega_0^2}{s^2 + \frac{\omega_0}{Q}s + \omega_0^2} = \sum_{k=0}^{N} a_k s^k`,
and zoom in to 4x on a Retina screen (or 8x on a normal one).

#### Observed
    size in units QSizeF(9393, 2004) image QSize(75139, 16030) 4594 MB in 463 ms; peak RSS 4763 MB
`paint()` typesets the whole formula at `ratio = clamp(ceil(zoom * dpr * 2) / 2, 1, 8)` pixels a
unit and caches it: 9393 x 2004 units x 8^2 = 1.2 G pixels = 4.6 GB, however little of the formula
is on screen. On an 8-16 GB machine the app is killed or swaps; at size 100 it is still ~290 MB per
formula.

#### Expected
Cap the cached image (e.g. a few thousand pixels a side / ~64 MB) and draw the typeset result as
vectors (a `QPicture`/`QPainterPath`, which mathtypeset could provide) or tile it for the exposed
part only; lower the ratio as the formula gets bigger.

Probe: `2026-09-26-new-features/probe_hunt.cpp` -> `formulaBigZoomed`.

#### Related: in a subcircuit's symbol
`symbolPrimitives()` turns the formula into a `qucs::Image` typeset at a fixed ratio of 4
(`formulapainting.cpp:245`): for the formula above that is 37570 x 8015 px = 1.15 GB - and it is
done again for every instance of the subcircuit when its symbol is read (`Component::analyseLine`
creates a new painting each time), so five instances hold five such images.

### C5. A marker's (or trace's) precision from a file is not clamped - one label can become gigabytes

**Severity:** low-medium (from a file; hang / out of memory when the diagram draws)
**Area:** Diagrams - `diagrams/marker.cpp:583` (`Precision = n.toInt(&ok)`), `diagrams/graph.cpp:160`;
used by `qucs_s::numberformat::format(v, notation, Precision)` (new notations) in `marker.cpp:249`

#### Reproduce
In a schematic or data display with simulated data, a marker line such as

    <Mkr 1.41295e+07 170 -180 999999999 0 0>

in a diagram whose number notation is Decimal (or Engineering/Scientific/Power, all of which
honour the decimals).

#### Observed (probe `markerHugePrecision`)
    precision 10000000 : 'g' 8 chars ... ; numberformat Decimal 10000009 chars in 58 ms
`QString::number(v, 'g', p)` ignores a silly precision, but the notation formatter (`fixed()` ->
`QString::number(v, 'f', decimals)`) builds a string with that many decimals: 999,999,999 is a
~2 GB QString for one marker label, then laid out and drawn.

The GUI never gives such a value - the marker dialog allows 0..12 (`QIntValidator(0, 12)`) and
Claude's tools clamp (1..12 for markers, 0..16 for traces) - only the file loaders do not.

#### Expected
Clamp `Precision` on load exactly as the dialog and the tools do (and in `numberformat::format`
itself, as a last guard).

Probe: `2026-09-26-new-features/probe_hunt.cpp` -> `markerHugePrecision`.

## D. Undefined behaviour from a file

### D1. Callout/TextBox pointer tip is not clamped on load - signed overflow when turned

**Severity:** medium (undefined behaviour from a file; an oversized bounding box)
**Area:** Paintings (new) - `paintings/shapes.cpp`, `TextBoxPainting::loadExtra`

#### Reproduce
A schematic painting line with a callout whose tip is far out:

    <TextBox 0 0 100 60 #000000 1 1 #ffffc0 1 1 0 0 2 6 6 #000000 10 0 1 1 1 2147483647 2147483647 ~hi>

Load it, then rotate it (Ctrl+R) or mirror it.

#### Observed
- `boundingRect()` is `QRect(0,0 2147483648x2147483648 (oversized))`.
- UBSan: `geometry/one_point.h:32: signed integer overflow: 50 + 2147483617 cannot be represented in type 'int'`
  from `ShapePainting::rotate` -> `TextBoxPainting::mapExtraPoints` (shapes.cpp:541).
- The saved line afterwards holds wrapped-around values (`-2147483629 2147483627`).

#### Expected
The tip is clamped like every other coordinate (`misc::clampCoordinate`, as the box, the
Dimension points and the Formula position are), so turning, mirroring and bounds stay in range.

#### Where
`m_tip = QPoint(toInt(f, 9, 0, &ok), toInt(f, 10, 0, &ok));` - no `misc::clampCoordinate`.

Probe: `2026-09-26-new-features/probe_hunt.cpp` -> `textBoxHugeTip`.

### D2. Signed integer overflow (UB) parsing column letters and <col min> in .xlsx

**Severity:** low-medium (undefined behaviour from a file; UBSan reports)
**Area:** Spreadsheets (new) - `spreadsheet.cpp` `columnOf` (line 623) and `readSheet` (line 271)

#### Reproduce
    python3 2026-09-26-new-features/make_xlsx.py col-letters.xlsx '<sheetData><row r="1"><c r="AAAAAAAAAAAA1"><v>1</v></c></row></sheetData>'
    python3 2026-09-26-new-features/make_xlsx.py col-min.xlsx '<cols><col min="2147483647" max="2147483647" width="10"/></cols><sheetData><row r="1"><c r="A1"><v>1</v></c></row></sheetData>'

#### Observed (UBSan)
    spreadsheet.cpp:623:25: runtime error: signed integer overflow: 321272407 * 26 cannot be represented in type 'int'
        in qucs_s::sheet::columnOf  <- readSheet:291 <- readXlsx:808
    spreadsheet.cpp:271:81: runtime error: signed integer overflow: 2147483647 + 1024 cannot be represented in type 'int'
        in readSheet (the <col> element)
- `columnOf` keeps multiplying for every letter and checks `column > 16384` only after the loop.
- `std::min(max, from + 1024)` overflows for a large `min`; a negative `min` is also accepted.

#### Expected
Stop (return -1) as soon as the column exceeds 16384 inside the loop; clamp `min`/`max` to
1..16384 before the arithmetic.

### D3. A date-formatted cell holding 1e300 or nan is converted to qint64 unchecked (UB)

**Severity:** low-medium (undefined behaviour from a file; a garbage date at best)
**Area:** Spreadsheets (new) - `spreadsheet.cpp` `dateOf` (line 967), reached from `readSheet` -> `dateText`

#### Reproduce
    python3 2026-09-26-new-features/make_xlsx_dates.py dates.xlsx
(A1..A3 styled with the built-in date format 14, holding `1e300`, `nan`, `-1e300`.) Open it.

#### Observed (UBSan)
    spreadsheet.cpp:967:38: runtime error: 1e+300 is outside the range of representable values of type 'long long'
        in qucs_s::sheet::dateOf <- dateText:977 <- readSheet:335 <- readXlsx:808
`QDate::addDays(qint64(std::floor(serial)))`: a float-to-integer conversion out of range (or of
NaN) is undefined behaviour; `QString::toDouble` gladly gives inf/nan/1e300.

#### Expected
Serials outside Excel's date range (0 .. 2958465, i.e. up to 9999-12-31) - or not finite - are
shown as the number, not converted to a date. The same guard belongs in the editor's date parsing
(`serialOf` / setting a date typed into a date-styled cell).

### D4. Waveform cycles and Dimension scale accept NaN / inf from a file, and save them back

**Severity:** low
**Area:** Paintings (new) - `WaveformPainting::loadExtra` (shapes.cpp), `DimensionPainting::load` (dimensionpainting.cpp)

#### Reproduce
    <Waveform 0 0 100 60 #000080 2 1 #c0c0c0 1 0 0 0 0 nan 25 0>
    <Dimension 0 0 100 0 20 #000000 1 1 0 10 nan 2 ~mm ~>
    <Dimension 0 0 100 0 20 #000000 1 1 0 10 1e308 2 ~mm ~>

#### Observed
- `QString::toDouble` accepts "nan"/"inf"; `std::clamp(NaN, 0.05, 100.0)` returns NaN (every
  comparison is false), so the waveform's cycle count is NaN: nothing is drawn, and the file is
  saved back with `nan`.
- The Dimension's scale is not checked at all: its label reads `nan mm`, `inf mm`, `-inf mm`
  (1e308 overflows to `inf mm`), and a scale of 0 gives `0.00 mm`.

#### Expected
A non-finite value is refused (load() false or a default), as the dialog's own ranges would
allow; `std::isfinite` before clamping.

Probe: `2026-09-26-new-features/probe_hunt.cpp` -> `waveformNaNCycles`, `dimensionOddScale`.

## E. Wrong behaviour

### E1. A spreadsheet (or PDF) open in Qucs-S that Claude changes is not reloaded - "could not be loaded again"

**Severity:** medium (stale data shown; the next Save overwrites Claude's changes silently)
**Area:** Claude Code dock -> `qucs.cpp` `QucsApp::reloadChangedFiles` (lines 3000-3044)
**Found by:** code inspection

#### Reproduce
1. Open `results.csv` (or `.xlsx`) in Qucs-S.
2. Ask Claude to add a column to results.csv (it edits the file with its Write/Edit tools).

#### Observed
`reloadChangedFiles` reloads only `Schematic` and `TextDoc` (MarkdownDoc included). `SheetDoc` is
a `QFrame, QucsDoc`, not a TextDoc, so `loaded` stays false and the dock says
**"results.csv could not be loaded again."** The table keeps the old contents with no "changed on
disk" marker; editing a cell and saving writes the old data back over Claude's version.
A PDF Claude regenerates (`PdfDoc`, also not a TextDoc) gets the same false note, although its
own file watcher does reload it.

Related: when the open document *has* unsaved changes, the dock only notes that it was not loaded
again; the next Save then silently replaces Claude's changes - no "changed on disk, overwrite?"
question.

#### Expected
A `reload()` for SheetDoc (keeping the sheet/cell in front), PdfDoc treated as reloaded (or
skipped), and on Save of a document whose file changed since it was loaded, ask before
overwriting.

### E2. With "Any folder is a project", opening a folder silently creates a `Scratch` folder in it

**Severity:** low-medium (unexpected writes into user folders; the workspace can list itself)
**Area:** Projects setting (new) - `qucs.cpp` `QucsApp::openProject` -> `useProjectScratch(true)`
**Found by:** code inspection

#### Reproduce
1. Settings > Locations > Projects: turn on "Any folder is a project".
2. Project > Open Project (or drop a folder on the window) and pick an ordinary folder -
   a git checkout, your home folder, or the Qucs workspace folder itself.

#### Observed
`openProject` accepts any non-hidden folder not named user_lib, then
`useProjectScratch(true)` does `QDir().mkpath(<folder>/Scratch)` straight away, before anything
is simulated:
- a git repository gets an untracked `Scratch/` directory (and later netlists, logs, raw files);
- the home folder gets `~/Scratch`;
- opening the workspace root itself as a project is not refused: it gets `Scratch/`, and since
  every workspace folder is a project with the setting on, `Scratch` then appears in the
  Projects list as a project.

Before the setting existed, only `NAME_prj` folders (made for Qucs) could be opened, so creating
`Scratch` was harmless.

#### Expected
- Create `Scratch` only when something is written to it (first simulation/netlist), or keep
  the scratch files in the cache directory for folders that are not `_prj` projects.
- Refuse the workspace root (and probably the home folder) as a project.

#### How easily the workspace itself becomes "the project"
Project > Open Project (`qucs.cpp:1878`) opens the folder picker *in the workspace folder*; pressing
Open without selecting a subfolder picks the workspace itself (the macOS picker's usual behaviour).
With the setting on, `openProject()` accepts it (its name is not hidden or user_lib): the whole
workspace becomes the project - Content panel over every project at once (see C3), `Scratch/`
created in the workspace and listed as a project.

### E3. The PDF viewer stops following its file for good once the file is deleted and written again later

**Severity:** low-medium (a stale report on screen, with no sign that it is stale)
**Area:** PDF viewer (new) - `pdfdoc.cpp` lines 846-856 (watcher and reload timer)

#### Reproduce (probe `pdfRewrittenAfterDelete`)
1. Open `report.pdf` (1 page).
2. Rewrite it in place with 2 pages -> the viewer shows 2 pages (works).
3. Delete it; 1.5 s later write it again with 3 pages (a script that removes the old report
   first and takes a moment to make the new one - `rm report.pdf && python make_report.py`).
4. Write it again with 4 pages.

#### Observed
    pages at first: 1
    after writing in place: 2
    after delete, 1.5 s, write: 2
    and written once more: 2
On deletion `fileChanged` fires; the file does not exist, so it is not watched again; 400 ms later
the reload timer finds no file and does nothing. From then on nothing watches the path, and every
later version is ignored until the tab is closed and reopened.

#### Expected
Keep following the path: watch the folder (`QFileSystemWatcher::directoryChanged`) while the file is
missing, and re-add the file (and reload) when it appears; or poll the path now and then while it
is missing. Show that the file is gone meanwhile.

Probe: `2026-09-26-new-features/probe_hunt.cpp` -> `pdfRewrittenAfterDelete`.

### E4. /resume lists none of Claude Code's sessions when the folder's path goes through a symlink

**Severity:** low-medium
**Area:** Claude Code dock - `claudehistory.cpp` `claudeSessions` (lines ~240-255)

#### Reproduce (probe `claudeSessionsThroughSymlink`)
A conversation folder spelled through a symbolic link: anything under `/tmp` or `/var` on macOS
(really `/private/tmp`, `/private/var`), a workspace that is a link (to a cloud-synced folder, an
external disk), a project linked into the workspace with Link Project (new: any folder).
Talk to Claude there, then `/resume` (or ⋯ > Resume a Conversation…).

#### Observed
    spelled ".../T/probe_hunt-okDmFv/work"  real "/private/var/folders/.../T/probe_hunt-okDmFv/work"
    sessions found by the path as spelled: 0 - by the real path: 1
Claude Code files a session under the name of its *working directory as the OS reports it*
(`getcwd()`, symlinks resolved): `~/.claude/projects/-private-var-folders-...` - the user's own
`~/.claude/projects` holds such names. `claudeSessions` builds the name from
`QFileInfo(folder).absoluteFilePath()`, which keeps the link, so it looks in `-var-folders-...`.

#### Expected
Build the name from `QFileInfo(folder).canonicalFilePath()` (falling back to the absolute path),
and perhaps look under both spellings.

Probe: `2026-09-26-new-features/probe_hunt.cpp` -> `claudeSessionsThroughSymlink`.

### E5. Saving a CSV whose name contains "%1" as .xlsx writes a broken workbook

**Severity:** low-medium (an unreadable file; rare names)
**Area:** Spreadsheets (new) - `spreadsheet.cpp` `freshXlsx` (line ~516): chained `QString::arg`

#### Reproduce (probe `csvWithPercentNameToXlsx`)
Open `duty50%1k.csv`, Save As `out.xlsx`.

#### Observed
    <sheets><sheet name="duty501k" sheetId="%2" r:id="rId%2"/></sheets>
    read back: false "The workbook has no worksheet."
`QStringLiteral("<sheet name=\"%1\" sheetId=\"%2\" r:id=\"rId%2\"/>").arg(escaped(name)).arg(k + 1)`:
the first `arg` puts the name - with its `%1` - into the string, and the second `arg` replaces the
lowest placeholder left, which is now the `%1` inside the name. The sheet id and the relationship
id stay literally `%2`: Excel reports the file as damaged, and Qucs-S itself cannot open it again.
(A name containing `%2` gets its `%2` replaced as well: `gain%2` -> `gain1`.)

#### Expected
One multi-argument call - `.arg(escaped(name), QString::number(k + 1))` - which substitutes in one
pass; worth a grep for other chained `.arg()` calls whose earlier arguments are user text.

Probe: `2026-09-26-new-features/probe_hunt.cpp` -> `csvWithPercentNameToXlsx`.

### E6. `$...$` inside an indented code block (or `<code>`) is typeset as math

**Severity:** low (a garbled code sample in a Markdown document or a Claude reply)
**Area:** Markdown/Claude dock math - `mathtypeset.cpp` `qucs_s::math::findMath` (line 2348)

#### Reproduce (probe `mathInIndentedCode`)
    Run it:

        export PATH=$HOME/bin:$PATH
        cp $SRC/a.sch $DST/

    and <code>$A-$B</code>

#### Observed
    typeset as math: "HOME/bin:"
    typeset as math: "A-"
Fenced code blocks and backtick spans are skipped, but a CommonMark *indented* code block (4
spaces or a tab after a blank line) and HTML `<code>`/`<pre>` are not: `$HOME/bin:$` becomes an
italic formula in the middle of the shell command, and the dollars disappear.

#### Expected
Skip indented code blocks (a line indented 4+ spaces after a blank line or another such line,
outside a list) and `<code>`/`<pre>` elements, as GitHub does.

## F. CI

### F1. CI - test_file_browser_drop hangs (timeout) on the Linux ASan job; main and the tag are red

**Fixed in `7c8dd74`.** Cause: the test set `XDG_DATA_HOME` to a folder it never created. Qt's
Linux `moveToTrash()` (`qfilesystemengine_unix.cpp`, `openHomeTrashLocation` -> `getTrashDir`)
makes `$XDG_DATA_HOME/Trash` with `mkdirat()`, one level only: with the parent missing it fails
(ENOENT, then EBADF from `fstat(-1)`) and, not being EXDEV, Qt tries no other trash. Replace then
reported "These could not be moved" in a sixth box - the log shows six boxes shown - that the
test's `answering()` never answered, having stopped after the clash question. The test now makes
the folder, and `answering()` closes and fails on any box it did not expect. The product's
behaviour was right: where no trash can be made, Replace says so.

**Severity:** medium (CI red on main and on v26.1.3 since the drag-and-drop commit; the release itself built)
**Area:** File Browser drag and drop - `tests/test_file_browser_drop.cpp` (added in dba62e7) and
possibly `FileBrowser::transfer` on Linux
**Found by:** the CI runs of 60ec10d (run 36280475773 on main, 36280476621 on the tag)

#### Observed
    Linux · Debug + ASan/UBSan · smoke tests: failure
    99% tests passed, 1 tests failed out of 69
    61 - test_file_browser_drop (Timeout)
macOS (release, unit tests) and Windows passed. The earlier runs that included this test
(70bc114) were cancelled before finishing, so this is the first completed Linux run with it; the
last green main (ca1dedf) predates the test.

#### Prime suspect
The Replace step of `aNameTakenAlready` (test_file_browser_drop.cpp:240-245) is inside
`#ifdef Q_OS_LINUX`: it has never run on macOS, where the test was developed and passes - only on
the Linux runner, which is exactly where the test hangs.

#### Not yet diagnosed - likely cause (unverified)
A modal box nobody answers blocks the test until CTest's 300 s timeout. Candidates:
- "Replace" uses `QFile::moveToTrash`; on a Linux runner a temporary folder on another filesystem
  (/tmp) may have no usable trash, so the item is reported in a second box ("could not be moved"),
  which the test's `answering()` helper - stopped after the clash box - never answers.
- The Linux modifier mapping (Shift = move, Ctrl = copy) differs from macOS (Alt = copy,
  Cmd = move); a drop the test expects to be a move may be a copy (or the reverse), bringing up a
  clash question the test does not expect.
Running the test on Linux with QT_QPA_PLATFORM=offscreen and a stack dump at the timeout (or
QTest's per-function timeout) will say which box it is. If it is the trash, the product also has a
real problem there: Replace fails outright where no trash exists, instead of offering to delete.

#### Release
The Release workflow succeeded: v26.1.3 was republished from 60ec10d with all 9 assets
(2026-09-27T00:23:00Z).

## G. Minor notes

- Markdown: links to a duplicate heading (GitHub's `#setup-1`) do not scroll - `anchorOf` has no
  duplicate numbering.
- A Dimension whose two points are far apart with a large offset reaches beyond the ±8M model plane
  clamp (bounds to -13.6M in a probe).
- A 0x0 RoundRect (a click without a drag) cannot be picked with a click (the other shapes can);
  a zero-length Dimension moves when turned (it turns about its label, not its points).
- Chained `.arg()` with user text first also garbles three messages (`schematic_file.cpp:2199`,
  `dialogs/librarydialog.cpp:623`, `qucscontrol.cpp:2142`) when a name contains `%1`/`%3`.
- Reading a CSV costs ~29x its size in memory (20 MB -> 586 MB peak), on the GUI thread.
- With "Any folder is a project", folders `amp` and `amp_prj` both show as the project "amp"
  (New Project "amp" happily makes the second); `deleteProject` compares names, so deleting one
  while the other is open is refused as "an open project".
- `openProject` adds the path to Recent Projects *before* checking that it may be a project, so a
  refused folder stays in the recent list.
- A link in a Claude reply (or File > Open, a File Browser double-click) to a file of a type Qucs-S
  does not know opens it as text whatever its size - a 500 MB binary ngspice `.raw` goes into a
  QPlainTextEdit (`QucsApp::gotoPage`, qucs.cpp ~2278).
- Deleting rows that run past the end of a CSV and undoing it adds empty rows (the undo copy is
  padded to the count asked for).

## H. Checked and found right

- Schematic loader and netlister: 210 mutated schematics (components, wires, paintings - all 13
  new ones -, diagrams, graphs, markers) printed and netlisted by the ASan/UBSan build: no crash,
  hang or sanitizer report.
- GUI monkey (ASan/UBSan): 10 new seeds x 1200 steps + 1 x 600 - clean.
- TeX typesetter: nesting 20,000 deep and unbalanced input - fast, no crash.
- All eight new painting types near the ±8M clamp turned/mirrored about far centres, with 0x0
  boxes, and inside a subcircuit's symbol with turned and mirrored instances (ASan/UBSan): clean.
- The live model/mode switch (60ec10d), the tuner's invalidation on Claude's edits, nested
  `batch` refusal, the read-only tool list, linked-project deletion, Import Project into itself,
  File Browser moves through symlinks into themselves, `.xlsm` saving, sheet names over 31
  characters, bounded reads of Claude Code session files, net renames in expressions.

## Reproducing

`2026-09-26-new-features/probe_hunt.cpp` holds every probe (a test function each; they print what
they find rather than assert). To run them:

```bash
cp docs/bug_hunts/2026-09-26-new-features/probe_hunt.cpp qucs-s-26.1.1/qucs/tests/
echo 'qucs_add_test(probe_hunt)' >> qucs-s-26.1.1/qucs/tests/CMakeLists.txt
cmake build && ninja -C build probe_hunt
QT_QPA_PLATFORM=offscreen build/qucs/tests/probe_hunt textCommentPartialSelection
```

(and `build-asan` for the UB findings). Some take a file or a folder from the environment:

- `make_bomb.py <out.xlsx> <MB>` - C2; `PROBE_BOMB=<file> ... xlsxZipBomb`
- `make_xlsx.py <out.xlsx> '<sheet XML>'` - C1, D2 (read with `PROBE_BOMB=`), A8
  (`PROBE_SHARED=<file> ... xlsxSharedFormulaMasterEdited`); the sheet XML of each is in its entry
- `make_xlsx_dates.py <out.xlsx>` - D3; `PROBE_BOMB=<file> ... xlsxZipBomb`
- `PROBE_BIGDIR=<folder> ... contentPollOfLargeFolder` - C3
- `seed_paintings.sch` - a schematic with every new painting, a seed for `scripts/ci/fuzz-sch.py --extra`
