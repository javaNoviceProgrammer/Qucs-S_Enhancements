# Bug hunt: library search paths and project libraries, ZIP archives, settings, the week's features

*4 October 2026 - Qucs-S 26.1.5, `8df9a9f` (the Release and ASan/UBSan apps as built for
`ce1f490`; `8df9a9f` changed a test only). 17:30 to 18:01 and 18:17 to 18:32: 45 minutes,
the session interrupted between.*

This hunt covers what changed since the hunt of 1 October:
- library search paths and the Libraries panel (`6e92914`), Claude's library tools
  (`01ffb32`), a library's Verilog-A linked into the project that uses it and compiled
  there (`109a7d2`, `15c7514`, `1dc30d4`), and a part taking the first library of its name
  that has it (`ce1f490`);
- binary datasets (`1d41990`, `c0d6a32`);
- NgOpt's new optimizers (`6f57f50`);
- Maximize Document (`0432f0e`), Copy Circuit as Image (`00ec2e1`, `935b184`);
- Filter by name with regular expressions (`95885e5`), New Zip… (`299cd70`) and the ZIP tab
  it opens in;
- Simulator Settings > Netlist's `ngspice_mathfunc.inc` box (`05dc583`).

It ran in the foreground on macOS arm64 with ngspice 46 and OpenVAF-reloaded. Each
`qucs-s --mcp-server` had its own workspace, settings, HOME, trash and cache in a scratch
folder. Methods:

1. **Probes through the tools**, p1 to p39, files beside this report (`p*.py`, with
   `mcp.py`, the client, and `libsetup.py`, a search path library with Verilog-A linked
   into a project). Most findings come from these.
2. **Throwaway tests in `test_zip_doc`** for the ZIP tab, which no tool reaches: five
   functions, each built and run once and then removed (`zip_probes.cpp`).
3. **The ASan/UBSan build**, for:
   - 120 mutants of a binary dataset through `reload_data`, `get_dataset`, measures and a
     screenshot (`p14`);
   - `fuzz-sch.py`, 150 mutants of 24 schematics placing library parts;
   - the menu-action monkey, 91 actions in four states (`p23`) and in a project with a
     linked library source (`p34`);
   - 34 damaged project library records through the sync (`p39`).
4. **The end-to-end scenarios** with every dataset binary (`QUCS_E2E_DATASET_LIMIT_MB=0`):
   90/90.
5. **Reading** the code behind each feature, where a probe could not reach it.

## Summary

| | Finding | Since |
|---|---|---|
| **A1** | A project's library record can make the sync delete any file of the project (fixed in `403ff80`) | `15c7514` |
| **A2** | `export_netlist` onto a linked library source overwrites the library's own file, outside the project (fixed in `403ff80`) | `15c7514` |
| **A3** | ZIP names without the UTF-8 flag are garbled, and saving makes it permanent | before (the ZIP tab) |
| **A4** | A ZIP entry repeated under one name: the later one is never extracted; deleting one deletes both | before (the ZIP tab) |
| **A5** | An archive of 65,535 entries or more is written so it cannot be read back, or loses entries | before; `299cd70` |
| B1 | macOS: `ngspice_mathfunc.inc` is not in the package, so `limexp`, `step` and `stp` never work | before; `05dc583`'s box |
| B2 | A library source that is a link to a link: its includes are not followed, and a changed include is not rebuilt | `1dc30d4` |
| B3 | `edit_component` changing a library part's Lib or Comp cuts its nets without a word | before |
| B4 | `create_library`'s descriptions are written raw: one can add components to the library | `01ffb32` |
| B5 | Two search paths' libraries of one name: the second's part cannot be placed or described by name | `ce1f490` |
| B6 | `trigger_action` with no action, or an empty one, runs File > New | before |
| B7 | `set_settings`'s whole numbers: some silently ignored, one cut to 0 | before |
| N1-N13 | Minor: messages, costs, names, settings answers | various |

No crash, hang or sanitizer report anywhere.

## A. Data lost or changed

### A1. A project's library record can make the sync delete any file of the project

`Libraries/<library>/.qucs-library.json` lists the files Qucs-S made there. When the
library is no longer used, the sync removes them with `QFile::remove` (not to the Trash).
`readRecord` (`projectlibraries.cpp:205`) refuses an item path that is absolute or starts
with `..`, but not one that climbs out later: `./../../victim.txt` passes.

With such a record in `Libraries/Evil/` (`p1b`), `open_project` deleted `PROJECT/victim.txt`,
`victim.osdi` beside it, and `keep/notes.txt`. The migration of an older layout reads
records in any top-level folder too: `OldLib/.qucs-library.json` listing
`./../docs/thesis.tex` deleted `docs/thesis.tex` when the project opened, with no
`Libraries/` at all (`p1c`). `x/../../../victim.txt` is safe only while `x` does not exist.

A record comes with a project: a ZIP, a clone, a shared folder. Opening it is enough.

*Fix:* resolve each item against its folder and keep it only when the cleaned path is
inside it (`QDir::cleanPath`, then a prefix check against the folder plus `/`); remove only
what is a link to, or a copy of, the record's `original`.

**Fixed in `403ff80`.** A record's file is taken away only when it is in the record's folder as the
folders really are (`pathIn` in `projectlibraries.cpp`):
- a path that leads out by `..` (`./../../victim.txt`, `x/../../../keep/notes.txt`) does
  not count;
- nor does one through a folder in it that is a link elsewhere (`out/far.txt`, with `out`
  a link out of the project).

A copy, or a copied include, is taken away only while it is still the copy Qucs-S made:
- The record now keeps each copy's SHA-256, written with the record.
- A record from before has no sums. Its copy goes only when it has its original's bytes,
  and the original must be another file than the copy.

So a file of the user's that a record names stays, edited or not.

The same check guards what the sync writes:
- `Libraries/` itself a link: nothing is read, written or taken away through it, and the
  report gives it as a conflict.
- A library's folder in it that is a link: nothing is written through it.
- A folder on a source's or an include's way that is a link elsewhere: nothing is written
  or removed there.

Tests (`test_project_libraries`):
- `aRecordTakesAwayNothingOutsideItsFolder`: the hunt's paths in `Libraries/` and in a
  folder of before; the copies kept and taken away; a copy's sum in its record.
- `nothingIsWrittenThroughAFolderLeadingElsewhere`: `Libraries/` a link, a library's folder
  a link, an include's folder a link, and a source in a library's subfolder whose folder in
  the project is a link, the record claiming the user's file there, for links and copies.

### A2. `export_netlist` onto a linked library source writes through the link

`export_netlist {save_as: "Libraries/VaRes/vres.va", replace: true}` (`p2b`) followed the
link and replaced the team library's `vres.va`, outside the project, with a netlist. Every
project using that library then compiles a netlist as Verilog-A. `save_document` to the
same path is refused ("could not be saved").

*Fix:* the tools that write a file the user names refuse a path inside `Libraries/` (or
any symbolic link), as `save_document` does, or replace the link rather than writing
through it.

**Fixed in `403ff80`.** One check, `projectlibraries::notToWrite(path)`, says when a file is a library's
Verilog-A a project keeps:
- a link, which would write the library's own file;
- or a copy, which is renewed, so what is written is lost.

It tells the file by the file itself, so it works however the path is spelled: through
another link to the project, in another case, `./`, or a link to nothing (where writing
would make the file where the library was).

Who checks:
- Every tool write goes through `QucsControl::aboutToWrite`. It now answers with that
  refusal, and each of its callers stops on it: `export_netlist`, `save_document`,
  `export_image` (and a pdf_tex's PDF), `copy_document` and its results, `import_netlist`'s
  subcircuits, `import_data`, the dataset to the trash, and a data display's renamed
  traces.
- In the window, every save dialog asks again while the file chosen is one
  (`misc::saveFileName`). Save As checks the name with its suffix added. The Export
  dialog and Import Data check their typed names.
- `TextDoc::save` already refused such a file.

The answer: "…/Libraries/WriteLib/good.va is the Verilog-A of the library WriteLib, linked
into the project from …: written, the library's own file would change, which every
project using it shares. Write to another file - the library's Verilog-A is changed in the
library."

Tests:
- `test_project_libraries` `aKeptFileIsNotToWrite`: a link, the same through another
  spelling, a link to nothing, a copy; the library's own file, a new file and the record
  not refused; the dialogs' message.
- `test_qucs_control` `aLibrarysLinkIsNotWrittenThrough`: `export_netlist` (SPICE and CDL)
  and `save_document as` refused; the library's file unchanged; a file elsewhere written.
- `test_graphics_export` `theDialogsDiagramAndItsFolder`: the Export dialog onto a kept
  file and onto one as a pdf_tex's PDF.

Each part of both fixes was broken on purpose: 21 breaks, 20 caught. The one not caught
is `pathIn`'s guard for its folder vanishing while it looks, which only a race reaches;
without it, that case loops forever.

Tests that at first missed a break:
- The stale copy was a different size from its original, so the size test hid the byte
  comparison. It is now the same size.
- `save_document as` for a text document was also refused by `TextDoc::save`'s own,
  similar message. The test now asks for the new message.

Checks that could never make a difference were left out: `..` and drive checks in
`pathIn` (the system resolves them; its result is always under the folder), a file's own
type test before a copy's sum (`removeItem` decides), and a shortcut for new files in
`notToWrite`.

`export_data` cannot reach a link: it adds its format's suffix to `good.va`. Full suite
88/88; under AddressSanitizer 88/88 (`test_claude_code` timed out waiting for a turn
under load and passed alone), with no report; the end-to-end scenarios 90/90.

### A3. ZIP names without the UTF-8 flag

`zip::list` (`zipfile.cpp`) decodes a name without the UTF-8 flag as Latin-1. That is
neither the ZIP specification's CP437 nor the UTF-8 that macOS's Archive Utility and
`ditto -c -k` write without setting the flag. An archive made in the Finder with
`Résumé.txt` and `日本/a.txt` (a throwaway test, `huntNames`):
- shows `RÃ©sumÃ©.txt` and `æ\u0097¥æ\u009C¬/a.txt`;
- extracts under those names.

Then, after any change and a save (`huntSaveNoFlag`: a folder added), the archive's names
are written back as those strings, with the UTF-8 flag set. Every tool shows
`RÃ©sumÃ©.txt` from then on: the archive's names are changed for good.

*Fix:* without the flag, take the name as UTF-8 when it is valid UTF-8 (as Info-ZIP and
7-Zip do), else CP437; and write a name's original bytes back unchanged when it was not
renamed.

### A4. A ZIP entry repeated under one name

Appending an updated file to an archive (Python's `ZipFile(mode='a')`, some `zip`
tools) leaves two entries of one name; unzip and Python's `ZipFile.read` take the later.
The ZIP tab lists both rows, but each action finds an entry by name and takes the first
(`huntDuplicateNames`: `a.txt` "old" then "new"):
- `contents("a.txt")` and opening either row give "old";
- Extract writes "old" for both: the later entry is never extracted;
- Delete of one row removes both.

*Fix:* refuse such an archive or mark the duplicate; or act on entries by index, not by
name, and extract the later one as other tools do.

### A5. Archives of 65,535 entries or more

`zip::write` puts the entry count in 16 bits and offsets in 32, with no check
(`huntManyEntries`):

| Entries written | Read back |
|---|---|
| 65,534 | 65,534 |
| 65,535 | "A ZIP64 archive (not read)." |
| 70,000 | 4,464, no error |

`ZipDoc::save` ignores the re-read's failure. At 65,535 the tab says it saved and the file
then cannot be opened. At 70,000 the tab shows the 4,464 entries it read back, marked clean.
Offsets past 4 GB would wrap the same way (not run).

*Fix:* refuse to save more than 65,534 entries or past 4 GB (or write ZIP64), and treat a
failed re-read as a failed save.

## B. Bugs

### B1. macOS: `ngspice_mathfunc.inc` is never found

`Ngspice::findMathFuncInc` looks for
`BinDir/../share/qucs-s/xspice_cmlib/include/ngspice_mathfunc.inc`, which is
`Contents/MacOS/share/qucs-s/...` in the app. `scripts/package-macos.sh` copies examples,
library, symbols, lang and spicelibrary into that tree, but not `xspice_cmlib`, which only
`make install` installs (`extsimkernels/xspice/CMakeLists.txt`).

With the box "Include ngspice_mathfunc.inc (limexp, step, stp)" on, as it is by default
(`p35`, and on the installed 26.1.5 app, `p35b`):
- the netlist has no `.INCLUDE`;
- a B source using `limexp()` fails: "no such function 'limexp'";
- Check Schematic says nothing;
- every ngspice run logs "[Warning!] … file not found!".

The box does nothing on macOS.

*Fix:* copy `xspice/include` into `share/qucs-s/xspice_cmlib/include` in the package, and
add it to the script's list of files it checks for.

### B2. A library source that is a link to a link

When a library's `vres.va` is itself a link into a store (git-annex, Nix, stow) and
`` `include``s a file of the store (`p13`), it is linked in, compiled and simulated
(v(out) = 2/3 V). Then the include changes (r = 2k to 1k): the model is not rebuilt (the
`.osdi`'s time is the same) and v(out) stays 2/3 V instead of 0.5. `sourceIncludes`
(`osdiselection.cpp`) follows `symLinkTarget()` one link deep and looks for the includes
beside the second link. OpenVAF is given the canonical file and finds them. Create Library
takes a linked source's includes the same way (`dialogs/librarydialog.cpp:493`).

*Fix:* `canonicalFilePath()` in both places.

### B3. A library part's Lib or Comp changed cuts its nets

`edit_component` on a wired library part (`p19`):
- `{Lib: "NoSuchLibrary"}` is "done", with no error or note. U1 is left with 0 pins, R1's
  net to U1.1 is broken, and `get_netlist` then fails ("Cannot load library component").
- `{Lib: Ideal, Comp: VSum}` (3 pins for 5) is also "done", and R1.2 and U1.1 become two
  nets, unannounced.

Changing back rejoins them, and undo works. The promise that "a change that cannot keep
every net is not made" covers turns and moves only.

*Fix:* refuse a library that cannot be found or a part it lacks; and say which pins lost
their nets, or refuse, as for turning.

### B4. `create_library`'s descriptions are written raw

A description containing `\n  </Description>\n</Component>\n<Component Fake>…` is written
into `NAME.lib` as it is (`p20`). The library gains a component `Fake` and is read wrong:
`list_libraries` and the answer's `parts` give `DescLib_vres` and `m1` (names from its
SPICE section), not `vres`. The dialog's description field goes through the same code.
`<Model>` or `<10 dB>` alone in a description is harmless (`p20b`).

*Fix:* refuse, or escape, a description holding `</Description>` or a line starting
`<Component` / `</Component>`.

### B5. Two search paths' libraries of one name

With two search paths A and B, each holding a `mylib.lib` with a part `opamp` (`p12`):
- `find_library_component` lists both;
- each `place` says `{Lib: "mylib"}`, so B's placed that way is A's part (the first
  that has it);
- `describe_part {library: "mylib"}` can only reach A's.

`list_libraries {library: <B's file>}` places B's by its path, which is right.

*Fix:* when a library's name is not unique, give `place` its path; let `describe_part`
take a file.

### B6. `trigger_action` with no action runs File > New

`trigger_action` with no `action`, `null`, `""` or `" "` opens a new document each time
(`p9b`). An unknown action ("zzz") answers "There is no action zzz", but with `isError`
false (N4).

*Fix:* refuse a missing or blank action, and mark the unknown one an error.

### B7. `set_settings`'s whole numbers

- **Results/Binary above** given 1e12, 2^31 or 3.7: `"changed": []`, no `not done`. Nothing
  happens and nothing is said. -1, "abc", null and true are refused with the range.
- **Appearance/Cut long file names after** given 3.7: set to 0 (it was 50), neither 3 nor
  refused; 1e12 and -1e12: nothing, and nothing said.
- **Content panel refresh interval** refuses all three with its range, which is right.

The checks differ by key (`p25`, `p25b`).

*Fix:* one check for every integer key: a whole number within the range, else `not done`
with the range.

## N. Minor

- **N1.** `list_libraries` shows a search path folder that cannot be read (mode 0) as an
  empty section, with no reason. `set_settings` keeps `ok`, `ok/` and `ok/../ok` as three
  entries, which the panel shows as one section (`p4b`).
- **N2.** Cost with 3000 search paths:
  - A 200-part schematic whose library is in the last path opens in 1.65 s, against 0.07 s
    with it in the first (`p5`).
  - A check of 100 distinct installed parts takes 0.20 s, against 0.00 s with no paths
    (`p27`). It runs after edits, for the status bar.

  Each part's library is looked up path by path, several times per part, with nothing kept
  for the load or the check.
- **N3.** `create_library destination=project` accepts the names of the project's own
  folders (`p6`):
  - `Scratch`: models go into the temporary files' folder;
  - `Libraries`: models go into the managed folder, beside its links.

  `clean_scratch` leaves them.
- **N4.** `trigger_action` of an unknown action: `isError` false (see B6).
- **N5.** `close_document` with `path` `""` or `" "` closes the document in front, while
  other tools refuse an empty name (`p10`).
- **N6.** In a read-only project folder, `save_document` says "Library Verilog-A not
  linked, a file of the project's in the way: Libraries/VaRes/vres.va". No file is in the
  way: `Libraries/` cannot be made (`p16`).
- **N7.** A subcircuit file `amp>x.sch` becomes `<Component amp>x>`, read as `amp`. It loads
  only because `<Component amp>` is a prefix, and an `amp.sch` beside it would collide
  (`p21b`).
- **N8.** `LibComp::hasComponent`'s cache is keyed by time and size. A library rewritten
  within the second with the same size (HFS+, FAT, SMB) keeps its old part names.
  `create_library replace` does not tell it (reading).
- **N9.** The Content panel's Duplicate on a linked source writes
  `Libraries/VaRes/vres_copy1.va`: a second module `vres` in the managed folder. Delete
  removes the link only (right), and its confirmation box has no title (`p29`).
- **N10.** `import_library` of a models folder (`p30`):
  - copies a link to a file outside the folder as that file's contents (a private file
    can end up in `user_lib`);
  - skips a dangling link without a word;
  - answers with the header's name (`mylib`) as `library` while the file and the parts'
    Lib are `VLib`.
- **N11.** NgOpt (`p33`):
  - Polish "yes" with a local method is written `-method nm -polish`, though the dialog
    offers Polish for global methods only;
  - Polish "maybe" is taken as no, and Check Schematic says nothing.
- **N12.** Two console options set true in one `set_settings` (`p36b`):
  - It is accepted, and the later in key order wins: QJsonObject sorts keys, so "in the
    legacy window" beats "in a separate window", whatever order they were sent in.
  - `changed` reports the intermediate state. One option is listed going false to false;
    the other is listed as "was false" when it was already true. The dock's true to false
    is not listed.
  - Putting the `was` values back, as the note suggests, is refused.
- **N13.** ZIP entries `./a.txt`, `a//b.txt` and `a/./c.txt` (and `""`, `.`) are not
  extracted, each "a name that would leave the folder". The first three stay inside
  (unzip writes them). The tree shows rows `""` and `.`. Safe, but the reason is wrong
  (`huntOddNames`).

## What was found right

- **ZIP safety:** `safeName` refuses `../`, absolute and drive paths, and no symbolic link
  is written. A file `a` beside a folder `a/` extracts the file and says
  "a/x.txt: Not a directory". Rename onto an existing name is refused.
- **Binary datasets:**
  - counts bounded by division;
  - 120 damaged files under ASan, no crash or report;
  - text and binary runs give the same `get_dataset`, measures and `export_data`;
  - Save as Text identical to the text run (`p8`, `p14`, `p28`);
  - the e2e scenarios all binary: 90/90.
  The optimizer, Import/Export and dataset readers each handle the binary form.
- **Library search paths:**
  - a file as a search path refused;
  - relative paths, `~`, non-strings and `""` refused, `[]` clears (`p26`);
  - a folder named `mylib.lib`, or a link to itself of that name, skipped (`p37`);
  - library file names with quotes, spaces, accents, `;`, `<>` or a leading dash placed,
    saved and reopened (`p17`);
  - a case-different name resolved to the record's real name (`p7`);
  - the 23 shipped examples placing library parts check clean (`p22`).
- **Project libraries:**
  - an open schematic outside the project keeps no link in it (`p11`);
  - one simulated from outside compiles beside the source (`p11b`);
  - the Content panel notes library sections by the record, and opens a linked `.va`
    read-only;
  - an include cycle ends in 0.1 s with OpenVAF's error, and once fixed it compiles
    (`p38`);
  - 34 damaged records: no crash, no project file lost, the team library unchanged
    (`p39`);
  - 91 menu actions with a linked source open: the original unchanged, the link kept
    (`p34`).
- **NgOpt:**
  - constraints normalised, min > max refused;
  - a bad method, Starts or Seed taken by `edit_component` but said by Check Schematic
    and the netlist;
  - the optimize line for cmaes, `-starts`, `-polish` and `-constrain -min` as described.
- **Maximize Document:** closing the pane's only document restores; the hidden pane's
  document brought to front restores; a move that would empty a pane is refused (`p3`,
  `p32`).
- **Filter by name:** an invalid `(` taken as text, `[` matches none, case ignored, `a|b`,
  and `(a+)+$` in 0.03 s (`p31`).
- **Copy Circuit as Image** on an empty schematic, a diagram alone, a block alone: done, no
  crash (`p9`).
- **`set_settings` yes/no keys:** `"yes"`, `"true"`, 1, 0, null, `[]` and `{}` refused;
  an option set false alone refused (`p36`).
- **The menu-action monkey:** 91 actions with none, a schematic, a text document and a ZIP
  open, on ASan: nothing slow, no report (`p23`).

## How to run it again

```
# a probe: an isolated --mcp-server under $HUNT_RUNS (QUCS chooses the app, the ASan one for p14/p23/p34/p39)
HUNT_RUNS=/tmp/hunt python3 p1b_record_dot.py
QUCS=<repo>/build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s HUNT_RUNS=/tmp/hunt python3 p39_record_fuzz.py
# the probes using the review's op-amp library (p4, p5, p12, p17, p18, p30)
MYLIB=<path to mylib.lib> HUNT_RUNS=/tmp/hunt python3 p12_two_of_a_name.py
```

The ZIP findings: paste a function of `zip_probes.cpp` into `TestZipDoc`'s private slots
in `qucs/tests/test_zip_doc.cpp` (the `noflag.zip` and `fileandfolder.zip` of
`huntNames` are made by the Python lines in its comment), build `test_zip_doc` and run
it with the function's name.

A2: in a project with a search path library's Verilog-A linked
(`libsetup.setup`), `export_netlist` with `save_as` set to the link's path and `replace`
true, then compare the library's file.

The probes' notes, as taken: `notes.md`, beside this report.
