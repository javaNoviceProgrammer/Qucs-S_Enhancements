# notes (17:30 start)
- F1 (p1, p1b): record item path "./../../victim.txt" (kind copy) passes readRecord's filter (only a leading ".." or an absolute path
  is refused); open_project's sync, the library unused, deletes PROJECT/victim.txt and victim.osdi, and keep/notes.txt.
  "x/../../../victim.txt" is safe only while x does not exist. projectlibraries.cpp readRecord + takeAway/removeItem/removeModel.
- (zip: safeName refuses ../, absolute, drive; no symlinks written: right.) (binary reader: counts bounded by division, graph checks: right.)
- F2 (p2b): export_netlist {save_as: Libraries/VaRes/vres.va, replace: true} writes through the link: the team library's
  vres.va (outside the project) becomes a netlist. save_document as the link: refused ("could not be saved").
- (maximize: closing the pane's only document restores, by design; new_document reuses an empty untitled: right.)
- (NgOpt constraints: numbers normalised, min>max refused: right.)
- N1 (p4b): list_libraries shows an unreadable (chmod 0) search path folder as an empty section, no reason; set_settings keeps
  "ok", "ok/", "ok/../ok" as three entries (the panel shows one section).
- (a file as a search path: the whole set_settings refused, said: right.)
- N2 (p5): 3000 search paths, the library in the last: a 200-part schematic opens in 1.65 s (0.07 s with it in the first) -
  each part's libraryFile() walks every path, several times a part (Symbol, Model, Description...), nothing cached per load.
- N3 (p6, p6b): create_library destination project accepts the names of the project's own folders: "Scratch" (models into
  PROJECT/Scratch/, the temporary files' folder) and "Libraries" (models into the managed Libraries/, beside its links; the
  Content panel's library sections are Libraries/<x>). clean_scratch (per schematic) leaves them; no project-wide clean found.
- (p7 case: vares finds VaRes.lib, one folder, the record's real name: right.) (fuzz-sch 150 mutants of 24 library-part
  schematics, ASan, -n: clean.) (p8: text and binary datasets, sweep x AC + tran: get_dataset, measures, export_data the same.)
- (Content panel: library notes by the record (entryOf), a user's folder under Libraries/ not marked: right; linked .va
  opened: setReadOnly(true): right.)
- F3 (p9b): trigger_action with no 'action', null, "" or " " runs File > New (a document opened each time), not refused.
  N4: trigger_action of an unknown action ("zzz"): "There is no action zzz" but isError false.
- (Copy Circuit as Image on an empty schematic, a diagram alone, a block alone: "done", no crash.)
- N5 (p10): close_document with path "" or " " closes the document in front (an empty path taken as none given), where
  other tools refuse an empty name. (trash_file of ".", "..", the workspace or a folder holding it: refused: right.)
- (p11: an open schematic outside the project keeps no link in it; p11b: one outside simulated: no link, compiled beside
  the library's source (writable): right.) (trash_file/rename_file outside the workspace: allowed, recoverable: by design.)
- F4 (reading zipfile.cpp write(), zipdoc.cpp archive()/save()): the archive written has no limit on its total or its
  entries (each entry <= 128 MB only): put16(parts.size()) wraps at 65,536 (70,000 entries -> 4,464 in the end record;
  65,535 -> read back as "A ZIP64 archive (not read)", and save() re-reads the bytes: the tab emptied?); offsets quint32
  wrap past 4 GB (a corrupt archive); one past 1 GB (MaxArchiveFile) is written but not opened again here. Not run.
- F5 (p12): two search paths A, B, each mylib.lib with a part opamp: find_library_component lists both, each 'place'
  {Lib: "mylib"} by name - B's placed so is A's part (the first that has it). list_libraries {library: B's file} places
  B's by its path: right. describe_part {library: mylib} can only reach A's (a name, no path or file).
- F6 (p13): the library's vres.va itself a link into a store folder (git-annex, Nix, stow); `include "vres_params.vams"
  of the store. Linked in, compiled, simulated: v(out) = 2/3 V (r = 2k): right. The include changed (r = 1k): not
  rebuilt (osdi mtime the same), v(out) still 2/3 V, not 0.5. osdiselection.cpp sourceIncludes(): symLinkTarget() one
  link deep - includes looked for beside team/VaRes/, where the second link is; OpenVAF is given the canonical file
  (compileArguments), which found them. (One link: rebuilt - the review's re-test.)
  F6 also (reading): dialogs/librarydialog.cpp:493 Create Library takes a linked source's includes beside symLinkTarget()'s folder, one link deep.
- (p14: 120 mutants of a binary dataset - index numbers, words, lines, footer, NaN/Inf values, truncation, shifted values, the first line, r/c - through reload_data, get_dataset, measures, screenshot, ASan: no crash, no report.)
- N6 (p16): the project folder read-only: save_document says "Library Verilog-A not linked, a file of the project's in the way: Libraries/VaRes/vres.va" - no file is in the way; Libraries/ cannot be made.
- (p17: library files named quo"te, two words, Bibliothèque, semi;colon, brack<et>, lead-dash: placed, saved, reopened, 5 pins each: right; a quote saved as '' and read back.)
- F7 (p19): edit_component on a wired library part, {Lib: "NoSuchLibrary"}: done, no error, no note - U1 has 0 pins, R1's
  net to U1.1 broken, get_netlist then fails ("Cannot load library component"); {Lib: Ideal, Comp: VSum} (3 pins for 5):
  done, R1.2 and U1.1 now two nets, no word. (Back to uA741: the nets rejoin; undo works.) The "a change that cannot keep
  every net is not made" promise is worded for turning and moving; a Lib/Comp change says nothing of what it cut.
- (e2e, every dataset binary (QUCS_E2E_DATASET_LIMIT_MB=0), current build: 90/90.)
- F8 (p20): create_library 'descriptions' are written into NAME.lib as they are: one holding "\n  </Description>\n
  </Component>\n<Component Fake>..." gives the file a component Fake, and the library is read wrong - list_libraries and the
  answer's 'parts' give DescLib_vres and m1 (its SPICE section's names), not vres. (The dialog's description field is the
  same code: LibraryDialog writes Descriptions as they are.) A description with "<Model>" or "<10 dB>" alone: right for
  ngspice (p20b).
- N7 (p21b): a subcircuit file amp>x.sch becomes <Component amp>x> - the part read as "amp" (loads by accident: "
<Component amp>" is a prefix); amp.sch beside it would collide. qu"ote, two words, lt<gt: right.
- (p22: check_schematic over the 23 shipped examples placing library parts: no library-part error, no new warning.)
- (p23: 91 menu actions x 4 states (none, schematic, text, ZIP), ASan: no crash, none slow, no report.)
- F9 (p25, p25b): set_settings and whole numbers: "Results/Binary above" given 1e12, 2^31 or 3.7: "changed": [], no
  "not done" - silently nothing (-1, "abc", null, true: refused with the range). "Appearance/Cut long file names after"
  given 3.7: set to 0 (was 50) - neither 3 nor refused; 1e12 and -1e12: silently nothing. "Content panel refresh
  interval": all three refused with its range (right). The checks differ by key.
- N2 also (p27): 3000 search paths, 100 distinct installed parts: check_schematic 0.20 s (0.00 s with none) - the check
  lists every library of each part's name (properAbsFileNamesIn walks every path, no early stop), and runs after edits
  (status bar); open 0.39 s (0.08). (p26: the search path list: relative, ~, non-strings, "" refused; [] clears: right.)
- N8 (reading): LibComp::hasComponent's cache is keyed by mtime and size: a library rewritten within the second with the
  same size (a coarse disk: HFS+, FAT, SMB) keeps its old component names - the case projectlibraries' record cache
  forgets on its own writes; create_library replace does not tell hasComponent.
- (p28: a binary dataset written out by the Content panel's Save as Text = the text run's file, 3080 chars, identical.)
- N9 (p29): the Content panel's Duplicate on a linked source writes Libraries/VaRes/vres_copy1.va (the content): a second source of module vres in the project, in the managed folder. Delete removes the link only; the original intact (right); its confirmation box has no title.
  F1 also (p1c): through the migration of a top-level folder: OLD_prj/OldLib/.qucs-library.json listing ./../docs/thesis.tex deleted docs/thesis.tex when the project opened - no Libraries/ needed, any top-level folder with a record.
- N10 (p30): import_library copies a models folder's link to a file outside it as that file's contents (a private file
  pulled into user_lib/VLib/notes.txt); a link leading nowhere is skipped without a word; the answer's "library" is the
  header's name (mylib) while the file and the parts' Lib are VLib.
- (p31: Content filter by name: invalid ( taken as text, [ matches none, case ignored, amp|notes, (a+)+$ 0.03 s: right.)
- (p32, p32b: Maximize with two panes: the hidden pane's document brought to front restores; a move refused when it would empty a pane: right.)
- N11 (p33): NgOpt Polish "yes" with a local method (nm) is written: "-method nm -polish" - Polish is "a global method's best
  point finished by a local one" and the dialog offers it for global methods only; Polish "maybe" is taken as no without a
  word (check_schematic silent). Method nosuch, Starts 0 / 2.5, Seed -1.5, Size abc: edit_component takes them, but
  check_schematic says each and the netlist echoes the error (right). The optimize line for cmaes, -starts, -polish,
  -constrain -min: as described (right).
- (p34: the 91 actions in a project with a linked library source, its tab open and the schematic, ASan: no crash, no report; the original unchanged, the link kept.)
- F10 (p35, p35b): macOS: ngspice_mathfunc.inc is never found - "Include ngspice_mathfunc.inc (limexp, step, stp)" on
  (the default) adds no .INCLUDE and a B source with limexp() fails "no such function 'limexp'" (build tree and the
  installed 26.1.5 app alike); check_schematic silent. findMathFuncInc looks in BinDir/../share/qucs-s/xspice_cmlib/
  include/ (= Contents/MacOS/share/qucs-s/xspice_cmlib/...); scripts/package-macos.sh copies examples, library, symbols,
  lang, spicelibrary into that share tree but not xspice_cmlib (extsimkernels/xspice/CMakeLists.txt installs it on
  `make install` only). The setting's box is then a no-op on macOS; every ngspice run also logs "[Warning!] ... not found".
- (p36: a yes/no key given "yes", "true", 1, 0, 1.0, null, [], {}: each refused, "it takes true or false": right; an option
  set false alone: refused, "an option is left by choosing another": right.)
- N12 (p36, p36b): two console options set true in one set_settings: accepted, no "not done"; the later in key order wins
  (QJsonObject sorts keys: "in the legacy window" over "in a separate window", whatever order sent). The answer's
  'changed' gives the intermediate state: "in a separate window" was false now false (no change, listed), the legacy
  window was false now true when it was already true (second call), the dock's true -> false not listed. Putting the
  'was' values back (as the note says) is refused: both false.
- (p37: a search path folder named mylib.lib, a link to itself named mylib.lib, the real one in a later path: the odd ones skipped, the real one listed, described, placed: right.)
- (p38: a library .va's includes including each other: the run ends in 0.1 s with OpenVAF's error per include; fixed, it compiles: right.)
- F11 (reading zipfile.cpp list(), zipdoc.cpp contents()/extract()/remove()/rebuild()): an archive holding two entries
  of one name - what appending an updated file gives (Python ZipFile(mode='a').writestr, some zip -u/append tools) -
  is listed as two rows, but every action finds an entry by its name and takes the first: opening either row shows the
  first's contents; Extract writes the first's bytes for both (contents(n) per entry), so the later entry - the one
  unzip and Python's ZipFile.read() take - is never extracted (with overwrite: the first written twice); Delete of one
  row removes both. list() neither refuses nor marks the duplicate. Run (a throwaway test in test_zip_doc, removed): rows [a.txt, a.txt], contents "old", extract wrote "old" twice, Delete left none; Python reads "new".
- F12 (throwaway test in test_zip_doc, removed): ZIP names without the UTF-8 flag are decoded as Latin-1 - neither the
  spec's CP437 nor the UTF-8 that macOS (Archive Utility, ditto -c -k) writes without the flag: "Résumé.txt" shows and
  extracts as "RÃ©sumÃ©.txt", "日本/a.txt" as "æ\u0097¥æ\u009C¬/a.txt" (zipfile.cpp list(): fromLatin1).
- (same test: a file "a" and a folder "a/": both rows, Extract writes the file and says "a/x.txt: Not a directory";
  Rename onto an existing name refused, "The archive has c.txt already": right.)
  F12 also: saved after any change (a folder added, writeTo), the garbled names are written back as UTF-8 with the flag
  set (0x800): "RÃ©sumÃ©.txt" in every tool from then on - the archive's names changed for good.
  F4 run (throwaway test, removed): zip::write of 65,534 entries reads back 65,534; of 65,535: written (5.7 MB),
  read back "A ZIP64 archive (not read)."; of 70,000: written, read back 4,464 entries with no error - 65,536 lost.
  F4 also (reading ZipDoc::save): the re-read's failure is ignored - 65,535: the tab says saved, the file then cannot be opened; 70,000: the tab re-reads 4,464 entries and is marked clean.
- N13 (throwaway test, removed): ZIP entries "./a.txt", "a//b.txt", "a/./c.txt" (and "" and ".") are not extracted,
  each "a name that would leave the folder" - the first three stay inside (unzip writes a.txt, a/b.txt, a/c.txt); the
  tree shows rows "" and "."; openEntry("./a.txt"): "not a file that can be opened". Safe, but the reason is wrong.
  ("d\e.txt" -> d/e.txt, " g.txt", "h.txt ", "con.txt", "f" beside "f/": extracted as named: right.)
- (p39: 34 damaged .qucs-library.json records - empty, null, wrong types, 5000 items, 5000-char paths, '.', '', the record itself, NUL bytes, truncated, 15 random byte flips - through close/open_project and the check, ASan: no crash, no report, no project file lost, the team library unchanged: right.)
