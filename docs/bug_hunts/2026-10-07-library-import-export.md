# Bug hunt: library import and export

*7 October 2026 - Qucs-S 26.1.6, `1306eaf` (the Release and ASan/UBSan apps built for it).
07:45 to 08:22: 37 minutes.*

This hunt covers getting a library out of a project and into another:
- **Export**: Project > Create Library (`dialogs/librarydialog.cpp`) and Claude's
  `create_library`, which drives the same code. Covered: the subcircuits' names, their
  hierarchy, the SPICE files and Verilog-A they bring, replacing a library, and the dialog's
  own pages.
- **Import**: Claude's `import_library` (`qucscontrol_libraries.cpp`), and the Qucs library
  output of Tools > Convert Data File (`qucsconv_rf -of qucslib`).
- **Reading a library**: how a placed part reads it (`components/libcomp.cpp`): a library
  from another computer, with Windows line ends, from a newer Qucs-S, renamed, without its
  folder.

It ran in the foreground on macOS arm64 with ngspice 46, OpenVAF-reloaded and the
Qucsator built beside the app. Each `qucs-s --mcp-server` had its own workspace, settings,
HOME, trash and cache in a scratch folder. Nothing was written to the real cache, workspace
or Trash (checked with `find -newer` against a marker). Methods:

1. **Probes through the tools**, p1 to p18, files beside this report:
   - `mcp.py`: the client, now with Qucsator and OpenVAF set in each server's settings;
   - `lib.py`: a divider subcircuit, and a bench that places a part, drives its pin 1 with
     1 V and reads its pin 2.

   Every library part was simulated, not only listed. A part counts as right when its
   v(out) is the subcircuit's own.
2. **Throwaway tests in `test_library_paths`** for the dialog's pages, which no tool reaches.
   Three functions were built and run once, then removed (`dialog_probes.cpp`).
3. **The ASan/UBSan build**, for 800 damaged library files (two seeds, 400 each). Each was
   run through `import_library`, `list_libraries`, `describe_part`, `add_component`,
   `get_netlist` and `check_schematic` (`p13`, seeds: installed libraries and `seeds/`).
4. **Reading** the code where a probe could not reach, or where running it would not be
   safe (E1).

## Summary

| | Finding | Since |
|---|---|---|
| **A1** | The dialog's Rewrite, when the new library cannot be made, deletes the old one for good | before (upstream) |
| **A2** | `create_library replace` that fails leaves no library; the answer does not say the old one is in the trash | `01ffb32` |
| **A3** | Library parts keep their inner subcircuits' bare names: two of one name share one definition, and the results are wrong without an error | before (upstream) |
| **A4** | Two SPICE files of one name, in two folders, become one file in the library's folder | before (upstream) |
| B1 | A part made of a hierarchical subcircuit never simulates under ngspice, Xyce or SPICE OPUS: its first SPICE line is lost | before (upstream) |
| B2 | Replacing a library breaks its parts that place another of its parts | `01ffb32`; the dialog before |
| B3 | A subcircuit in a project folder gives a part that never simulates | `7f11356` |
| B4 | Subcircuits whose names differ after a dot become parts of one name | before (upstream) |
| B5 | `.inc` and `.mod` files are copied into the library but never included | before (upstream) |
| B6 | Under Qucsator, a part whose subcircuit places a library part does not simulate | before (upstream) |
| B7 | A renamed library file: none of its parts simulates | before (upstream) |
| B8 | `import_library` of a SPICE library leaves the files it includes behind | `01ffb32` |
| B9 | `import_library` of a Qucs-S library without its folder: nothing said; every run fails | `01ffb32` |
| B10 | A library from a newer Qucs-S imports, lists and describes, but no part can be placed, and the reason is never given | `01ffb32`; before |
| B11 | The dialog: Rewrite? No, another name, Next - every subcircuit is in the library twice | before (upstream) |
| B12 | The dialog makes the library of the saved files; unsaved changes are left out without a word | before (upstream) |
| B13 | Convert Data File's Qucs library of a SPICE file of subcircuits is empty, "Successfully converted" | before (upstream) |
| **E1** | Commands in a library part's SPICE text, or in a file it attaches, are not found: `simulate` would not refuse them | `8f88220` (the check) |
| N1-N7 | Minor: name resolution, the converter's names, nested copies, leftovers, dialog boxes, `get_ui`'s rows, deactivated parts | various |

No crash, hang or sanitizer report anywhere (p13: 800 damaged libraries).

All 18 findings and the 7 minor notes are fixed in `638903e` (each says how, below; *The fixes,
checked* says how they were tested).

Most findings come from upstream code of Create Library that was never exercised this way.
B1 alone means that a hierarchical subcircuit cannot be a library part under any SPICE
simulator. A3 is hidden behind B1 and becomes a wrong result once B1 is fixed, so they are
best fixed together.

## A. Data lost, or results wrong without an error

### A1. The dialog's Rewrite, when the new library cannot be made, deletes the old one

Project > Create Library with the name of a library that is there asks "A library with this
name already exists! Rewrite?". `slotSave` then opens the old file for writing, which
empties it. When a subcircuit cannot be made into the library, it ends with
`LibFile.remove()` ("Error creating library.").

`huntRewriteThatFails` (`dialog_probes.cpp`): `Kept.lib` of 532 bytes, rewritten with a
subcircuit whose own subcircuit file is missing:
- the messages end "Error: Cannot create netlist for "outer.sch". Error creating library.";
- `Kept.lib` is gone, and the trash is empty.

The library and every project's parts that placed it are lost. A subcircuit that cannot be
made is common: a file moved, a library part's library gone, a Verilog-A source that cannot
be copied (p14).

*Fix:* write the new library beside the old one (`NAME.lib.new`, or a QSaveFile) and rename
it over only when it is made; or move the old one to the trash first, as `create_library`
does, and bring it back on failure.

**Fixed in `638903e`.** A library is made in a folder of its own beside where it goes,
`.NAME.qucs-new/` (its `NAME.lib` and its folder of files), and put in place only when it
is whole: then what was there - the library and its folder - goes to the trash ("The library
it replaced is in the trash."). One that cannot be made is removed with its folder, and the
messages end "The library ... there is as it was." Where there is no trash (a share), the
dialog's Rewrite writes over it and says so.

### A2. `create_library replace` that fails leaves no library

`create_library` moves the old `NAME.lib` (and its folder) to the trash before it writes, so
nothing is lost. But when the new one cannot be made (`p5`, the same missing subcircuit):
- `user_lib` has no `Amps.lib`;
- the answer is `{"error": "the library was not made (its messages say why)", ...}` and does
  not say the old one is in the trash.

Every schematic placing `Amps:div` now fails to netlist until someone thinks to look in the
trash.

*Fix:* bring the old library back from the trash when the new one is not made (or write
beside it, as for A1); at least say where it went.

**Fixed in `638903e`.** `create_library` makes it as the dialog does (A1): the old library stays
until the new one is in place, and the error says "the one there is as it was". With no trash
to move it to, `replace` is not done (it promises the trash), and says so.

### A3. Inner subcircuits keep their bare names

A library part's SPICE model carries the subcircuits its subcircuit places, under their own
names: `.SUBCKT inner P1 P2`, not `.SUBCKT LA_inner`. Two libraries whose parts were made of
different `inner.sch` (or a library part and the user's own `inner.sch`) put two
`.SUBCKT inner` into one netlist.

`p6c`, with B1 worked around (a newline put after `<Spice>`):
- `LA:outer` (an inner 1k/1k: 0.5) and `LB:outer` (an inner 1k/3k: 0.75) in one circuit give
  0.5 and **0.5**;
- with the user's own `inner.sch` (3k/1k: 0.25) placed too: 0.5, 0.5 and **0.5**;
- ngspice keeps the first definition. `simulate` succeeds, and its only sign is a warning,
  "redefinition of .subckt inner, ignored".

Qucsator refuses the same circuit ("found 3 definitions of `Def:inner'"), which is at least
said.

Today B1 stops every such part under SPICE first. Fixing B1 alone would turn those failures
into wrong numbers.

*Fix:* prefix each inner subcircuit's name with the library's and the part's (as the part
itself is, `LA_outer`), in both the `<Spice>` and the `<Model>` sections and in the calls to
it.

**Fixed in `638903e`**, where a part is used, so libraries made before are mended too:
`LibComp::scopedSpice()` and `scopedQucsModel()` rename a part's subcircuits as its model is
netlisted. Its own is `LIB_part` (the one of that name, else of the name the library was made
under, else the last); every other one `LIB_part__name`, with the calls to it (`.SUBCKT`,
`.ENDS`, the subcircuit word of an X line before its parameters, over continuation lines,
before a `;` or `$` comment; `Type="..."` of a Qucs `Sub:` line). A part's includes
(`ModelIncludes`) are read for each part, renamed for it. `p6c`: 0.5, 0.75 and 0.25 under
ngspice, and under Qucsator. The installed libraries share inner names too (`ig` in three PWM
controllers); every installed part was run again under ngspice (below).

### A4. Two SPICE files of one name become one

`intoFile()` copies each SPICE library a subcircuit uses into the library's folder under its
file name, with no check that another file already took that name. `copyIntoLibrary()`,
used for Verilog-A, has such a check.

`p2`: `a.sch` uses `models/dev.lib` (DEVA) and `b.sch` uses `other/dev.lib` (DEVB):
- the folder gets one `dev.lib`, the second (DEVB), and both parts attach it;
- `Sp:a` then fails: "unknown subckt: ... deva";
- the library was "Successfully created".

When both files define a subcircuit of the same name, as two versions of a vendor's
`models.lib` do, the first part simulates the other file's model, and nothing says so.

*Fix:* copy SPICE files through `copyIntoLibrary()` (the same "would both be ... in the
library" error), or keep their folders below the library's folder.

**Fixed in `638903e`.** `copySpiceFile()` takes a SPICE file in under its own name, else in a
folder named as the one it is in (`other/dev.lib`, numbered when that is taken too), with the
files it includes kept in their places beside it (`sub/x.inc`); one included from outside its
folder is a warning, as for Verilog-A. A `.lst` of two subcircuits of one name in two folders
is the "would both be" error.

## B. Parts that never simulate, and other bugs

### B1. A part made of a hierarchical subcircuit loses its first SPICE line

`AbstractSpiceKernel::createSubNetlist(stream, true)` writes the subcircuits placed inside
first, and only then the newline that is meant to follow the tag. In the library,
`<Spice>` is therefore followed on the same line by the first `.SUBCKT`:

```
  <Spice>.SUBCKT inner P1 P2
R1 P1 P2  1K tc1=0.0 tc2=0.0
...
```

`LibComp::loadSectionOf` takes a section from the line after its tag, so `.SUBCKT inner`
is dropped. ngspice stops: "Mismatch of .subckt ... .ends statements! ... fatal error"
(`p6b` gives the netlist).

`p6`: every part made of a subcircuit that places another fails under ngspice:
- `outer` (one level), `outer3` (two levels), `outer2` (with a parameter);
- all of them simulate right under Qucsator.

A flat subcircuit is not hit: its section starts with the newline.

*Fix:* write the newline before the inner subcircuits (`if (lib) stream << "\n";` before
`prepareSpiceNetlist`). Also make `loadSectionOf` keep text that follows the tag on its line,
since libraries made until now have it. Fix A3 with it.

**Fixed in `638903e`**, both: the newline is written first, and a section is read from its
tag's own line when something follows the tag there (the 106 installed sections that do hold a
`* Qucs ...` comment there). `p6`: every hierarchical part simulates under ngspice.

### B2. Replacing a library breaks its parts that place another of its parts

`p3b`:
- `Keep:div` is a library part, and `wrap.sch` places `Keep:div`.
- `create_library Keep replace` moves the old `Keep.lib` to the trash first, then reads
  `wrap.sch`, whose `Keep:div` cannot be found any more.
- The new `wrap` gets an empty `<Spice>` section; its call is netlisted `XX1 gnd Keep_div`
  with no pins, and fails ("unknown subckt").
- It still said "Successfully created library", with one line among the messages:
  "WARNING: Skipping library component X1".

The dialog's Rewrite empties the file before reading the subcircuits, with the same result.
Made under another name (`Other`), the same `wrap` simulates.

*Fix:* read the subcircuits before the old library goes (write beside, rename over, as for
A1). Refuse, or say plainly, when a part's model is left empty.

**Fixed in `638903e`** by A1's staging: the old library is there while the new one is made.
`p3b`: `Keep:wrap` gives 0.75 after the replace.

### B3. A subcircuit in a project folder gives a part that never simulates

Since `7f11356` the Content panel shows folders, and `exportSchematic()` lists their
subcircuits as `sub/deep.sch`. Create Library and `create_library` take them, but:
- the part is named `sub/deep` (`<Component sub/deep>`);
- its SPICE subcircuit is named `deep`: the document's name `Odd_sub/deep.sch` cut to the
  file name, without the library's prefix;
- the part is netlisted as `Odd_sub_deep`;
- `p1`: "unknown subckt: ... odd_sub_deep".

*Fix:* name the part and its subcircuit from the file's base name (`deep`, `Odd_deep`), and
refuse two of one base name.

**Fixed in `638903e`** (with B4): a part is its file's name without `.sch`
(`LibraryDialog::partName()`), its subcircuit the library's and the file's name
(`Odd_deep`). Two subcircuits that would be one part, or one SPICE subcircuit in any case
(`a_b.sch` and `a.b.sch`), are refused before anything is written (`nameClash()`).

### B4. Subcircuits whose names differ after a dot become one part's name

The component is named `SelectedNames[i].section('.', 0, 0)`. `div.sch` and `div.v2.sch`
both become `<Component div>` (`p1`):
- `list_libraries` and the answer list two parts `div`;
- the second can never be placed: the first of the name is found;
- its SPICE model `Odd_div_v2` is in the file, unused.

*Fix:* take `completeBaseName()` (without `.sch`) and make it a proper name, as the SPICE
side does; refuse two parts of one name.

**Fixed in `638903e`** with B3: `div.v2.sch` is the part `div.v2`, its subcircuit `Odd_div_v2`.

### B5. `.inc` and `.mod` files are copied but never included

A subcircuit's SPICE files come from its SPICE library parts (SpLib) and its `.INCLUDE`
parts. All of them are copied into the library's folder and listed in `<SpiceAttach>`. A
placed part includes only those ending in `.cir`, `.ckt`, `.lib` or `.sp`
(`LibComp::getSpiceLibrary`).

`p2`:
- `Sp:c` (a SpLib of `vendor.inc`) fails: "unknown subckt ... devc";
- `Sp:e` (an `.INCLUDE` part of `vendor.mod`) fails the same way;
- each subcircuit simulates right on its own.

The `.INCLUDE` part's default file is a `.inc`.

*Fix:* include every attached file that is not Verilog-A (`.va`, `.osdi`), or every file
whose suffix the SPICE parts take.

**Fixed in `638903e`:** every attached file but Verilog-A (`.va`, `.vams`, `.vh`, `.osdi`) is
included (`LibComp::getSpiceLibrary()`).

### B6. Under Qucsator, a part whose subcircuit places a library part

`createLibNetlist` skips library parts while a library is made ("WARNING: Skipping library
component X1"). The part's `<Model>` is then `Sub:X1 P1 P2 Type="Keep_div"` with no
definition of `Keep_div`.

`p3c`, under Qucsator:
- `Keep:div` gives 0.75;
- `wrap.sch` placed as a subcircuit gives 0.75;
- `Other:wrap` fails: "no such subcircuit `Keep_div' found as referred in `Sub:X1'".

Under ngspice the `<Spice>` section has the definition, and it simulates.

*Fix:* write the library part's model into the `<Model>` section as the `<Spice>` section
has it (prefixed, A3), or `ModelIncludes` its library.

**Fixed in `638903e`:** while a library is made, a library part a subcircuit places has its Qucs
model written to a `.lst` of its own (`Keep_div.lst`), which the new library takes in as a
subcircuit's (`Schematic::setLibraryScratch()`); the warning is gone. `p3c`: `Other:wrap`
gives 0.75 under Qucsator. Create Library also takes in such a part's SPICE files
(`LibComp::getSpiceLibraryFiles()`).

### B7. A renamed library file

A part is netlisted by its library's file name (`LibComp::createType()`: `AmpsTeam_div`).
The SPICE and Qucs models inside the file are named after the name the library was made
under (`.SUBCKT Amps_div`, `.Def:Amps_div`).

`p18`: `Amps.lib` renamed `AmpsTeam.lib` and imported:
- ngspice: "unknown subckt ... ampsteam_div";
- Qucsator: "no such subcircuit `AmpsTeam_div'".

A rename is what `also_named` advises ("a name of its own avoids it") when an imported
library's name is taken. `import_library`'s answer then gives `"library": "Amps"` (the
header's) while the parts' `Lib` is `AmpsTeam` (N10 of 4 October).

*Fix:* netlist a part by the name in the library's header (`<Qucs Library ... "Amps">`)
when it has its component, or have `import_library` take a new name and rewrite the header,
the model names and the folder together.

**Fixed in `638903e`** by A3's renaming, which finds the part's own subcircuit by the name in
the header (the one it was made under) and calls it as the netlist does. `p18`: 0.75 under
ngspice and Qucsator. `import_library` takes `name` (the file and its folder under that name;
the header kept, said as `made as`) and its answer's `library` is now the name its parts are
placed by.

### B8. `import_library` of a SPICE library leaves its includes behind

`import_library` copies the `.lib` and a folder `NAME/` beside it, nothing else. Vendor
SPICE libraries often include other files beside them.

`p4`: `vend.lib` with `.include "sub/inner.inc"` and `.lib "corners.lib" TT`:
- placed from where it is, its parts give 0.5 and 0.75;
- after the import, both fail: "Could not find include file sub/inner.inc ... fatal error
  in ngspice";
- the import said "It is in the Libraries panel" and nothing else.

A missing include is fatal to the whole run, not only to that part.

*Fix:* follow the library's `.include` and `.lib` lines (relative ones) and copy those files
too, keeping their places. Or refuse, naming the files it needs. Or tell the user to add the
folder as a library search path.

**Fixed in `638903e`:** the files a SPICE library includes by a relative path are brought along
in their places (`sub/inner.inc`, `corners.lib`) and listed in `written`; one there already
with the same bytes is used as it is, another is refused unless `replace`. One included by a
path out of its folder (`../shared.inc`) is refused, nothing brought, with the library search
path as the way to use it where it is. `p4`: both parts give 0.5 and 0.75 after the import.

### B9. `import_library` of a Qucs-S library without its folder

`p17`: a library whose part attaches `dev.lib`, sent as the `.lib` alone:
- `import_library` writes the `.lib` and says nothing;
- `describe_part` gives the part's pins; `check_schematic` finds nothing wrong;
- the run fails: "Could not find include file .../user_lib/Sent/dev.lib ... fatal error in
  ngspice".

*Fix:* `import_library` reads the parts' `<SpiceAttach>` and `<ModelIncludes>` and says which
files are not there (or refuses). Check Schematic says so for a placed part.

**Fixed in `638903e`:** `import_library` gives `missing` and a warning, `describe_part` gives
`missing`, and Check Schematic an error for a placed part ("X1: its library Sent names dev.lib
in its folder ..., which is not there"), so `simulate` stops before the run. The files are
read once while the library is unchanged; the SPICE model's are checked for a SPICE
simulator, the Qucs model's for Qucsator.

### B10. A library from a newer Qucs-S

`LibComp::loadSectionOf` refuses a library whose header gives a later version than this one
(-3), unless Ignore Future Version is set. Create Library writes the version that made it,
so a colleague's library from 26.1.7 is refused by 26.1.6.

`p9`: a library whose header says 26.2.0:
- `import_library`: done, with its part listed;
- `describe_part`: an answer, with `"pins": []`;
- `add_component`: "There is no part div in a library Theirs here (Lib and Comp name them)".

Nothing says the version is the reason. The format has not changed between those versions.

*Fix:* say it, wherever such a library is read: "made by Qucs-S 26.2.0, newer than this one
(Application Settings: ignore it)". Better, compare only the format's version, or refuse
only a major version above.

**Fixed in `638903e`** by saying it: `LibComp::newerVersionReason()` ("it was made by Qucs-S
26.2.0, newer than this one (26.1.6), and is not read: Application Settings > Load documents
from future versions reads it") in `import_library` (`made by` and the warning),
`list_libraries` (`not read`), `describe_part` and `add_component` (refused with it), the
Libraries panel (greyed, in its tooltip) and Check Schematic. The rule itself is the one a
schematic of a newer Qucs-S meets, and was left so.

### B11. The dialog: Rewrite? No, then another name: every part twice

`slotCreateNext` appends the ticked subcircuits to `SelectedNames` and `Descriptions` each
time it runs, before it asks "Rewrite?". The answer No returns, keeping them.

`huntRewriteNoThenRename`: name `Taken` (there), No, name `Fresh`, Next:
- `Fresh.lib` has 2 components for one subcircuit ticked, two `<Component amp>`;
- with descriptions on, the description pages also go through each subcircuit twice.

The same happens after the "Cannot create the folder" warning.

*Fix:* clear `SelectedNames` and `Descriptions` at the start of `slotCreateNext`.

**Fixed in `638903e`** so.

### B12. The dialog makes the library of the saved files

`slotSave` loads each subcircuit from its file. A subcircuit open with unsaved changes goes
in as it was last saved. Neither `slotCreateLib` nor the dialog saves it or says so.
`create_library` refuses in that case ("save_document it first").

(Reading: `qucs_actions.cpp` `slotCreateLib`, `librarydialog.cpp` `slotSave`.)

*Fix:* ask to save the open subcircuits with changes (as Simulate does), or list them and
refuse.

**Fixed in `638903e`:** Next asks "amp.sch has changes that are not saved, and the library is
made of the saved file" with *Save and Go On* and *Cancel*.

### B13. Convert Data File's Qucs library of SPICE subcircuits is empty

Tools > Convert Data File offers "Qucs library" as the output of a SPICE netlist
(`qucsconv_rf -if spice -of qucslib`). The converter's `qucslib_producer` writes only
`.model` cards (diodes, transistors, JFETs, MOSFETs) and ignores `.subckt`.

`p8`: a file of two subcircuits gives `<Qucs Library 1.0.7 "Vend">` and nothing else:
- the converter exits 0, so the dialog says "Successfully converted file!";
- imported, the library has no parts.

The dialog's input filter has no `*.lib`. A vendor `.lib` given anyway is taken as a Qucs
dataset, which hides the "Qucs library" output; the input format must be set to SPICE by
hand.

*Fix:* say in the dialog that only `.model` cards are converted, and that a SPICE library of
subcircuits is used as it is (SpLib, or `import_library`). Make the converter fail, with that
message, when it writes no component.

**Fixed in `638903e`:** the converter counts what it would write before it opens the output; with
nothing, it fails with that message (shown in the dialog's messages), and an output file chosen
to be overwritten is left as it was. The dialog's input filter offers `.lib`, `.mod` and
`.inc`, and takes them as SPICE.

## E. Security

### E1. Commands in a library part's SPICE text are not found

`erc::commandsOf` looks for programs a run would start in the schematic's own parts only:
- System command parts (CMD);
- the ngspice text of custom simulations, NutmegEq and `.spiceinit` parts.

`simulate` refuses those unless `allow_commands`, and Check Schematic warns of them. A
library part's `<Spice>` section is copied into the netlist as it is, and its
`<SpiceAttach>` files are `.INCLUDE`d. Neither is looked at.

`p12`, detection only: a colleague's library holds a `.control` block in its `<Spice>`
section, and another in an attached file. The block's line is the bare keyword `shell`, with
no command. The schematic has no analysis, and nothing was simulated:
- `import_library` brought it in, and `check_schematic` warned of nothing but the missing
  analysis;
- `get_netlist` shows the part's `.control` / `shell` / `.endc` lines in the netlist, and
  `.INCLUDE` of the attached file.

ngspice reads the `.control` lines of the whole deck, included files too. With an analysis,
`simulate` would run such a library's commands with no refusal. Neither was run here.

`import_library`'s purpose is a library "from another computer, a colleague's", which is
exactly the file one has not read. The same applies to SPICE library files (SpLib) and
`.INCLUDE` parts, whose files are not scanned either.

*Fix:*
- Scan what the netlist will hold: each library part's `<Spice>` section and attached SPICE
  files, SpLib files and `.INCLUDE` files, for `.control` blocks with `shell`, `system` or
  `!`.
- Report them as `commandsRun` does, so `simulate` and `tune` refuse them unless asked.
- `import_library` could say so when the library holds one.

**Fixed in `638903e`.** The check is the one the setting turns on (*Check commands*, off by
default since 30 September; with it off nothing is looked for, as before). With it on,
`erc::commandsRun()` now also reads, for each part in the circuit, the SPICE text it brings
in: a library part's `<Spice>` model and the SPICE files it attaches; a SPICE library part's,
an `.INCLUDE`'s and a `.LIB`'s file; and the files these include (relative or not, a few
hundred at most). It looks inside subcircuits too, and through their library parts, each
subcircuit once. A program started in a `.control` block (shell, system, `!`) is said as
"X1 brings SPICE text into the netlist whose .control block runs a command in a shell: shell
(in the library Theirs)", so `simulate` and `tune` refuse it unless `allow_commands`. What it
reads is kept while each file is unchanged. Tested by detection only (`p12` again, and
`commandsInLibrariesAreFound`): the lines are bare keywords, the schematics have no analysis,
and nothing was simulated.

## N. Minor

- **N1.** `create_library`'s `subcircuits` by bare name takes the last that matches.
  `["div"]` with `div.sch`, `sub/div.sch` and `zz/div.sch` made the part of `zz/div.sch`,
  not the project's top `div.sch`, and said nothing of the others (`p15`). **Fixed in
  `638903e`:** a path that matches (`div`, `div.sch`, `sub/div`) is taken; a name alone that
  two subcircuits have is refused, naming both.
- **N2.** The converter drops a first letter that is the SPICE prefix of the device's kind,
  whether or not it is meant as one. `.model MYNMOS NMOS` becomes the part `YNMOS`, and
  `D1N4148` becomes `1N4148` (which is the intent); a `DMOD` would become `MOD` (`p8`).
  **Fixed in `638903e`:** the letter goes only before a digit (`1N4148`, `2N3904`); `MYNMOS`
  stays.
- **N3.** `import_library` into a search path folder that is the library's own folder of
  models (`team/Vendor/` for `team/Vendor.lib`) copies the folder into itself. The copy
  picks up its own new files, three levels deep (`Vendor/Vendor/Vendor/Vendor.lib`); then it
  ends (`p4b`). **Fixed in `638903e`:** refused ("a library is not brought into itself"), and
  a folder is listed whole before it is copied.
- **N4.** A library that could not be made leaves the files already copied into its folder
  (`user_lib/Em0/vres.va`, `p14`). It is not listed, but the next library of that name finds
  them. **Fixed in `638903e`** by A1's staging folder, removed with a library not made. (A folder
  of the name without its library - a `.lib` taken away by hand - is not taken away: the
  library's files go into it, as before.)
- **N5.** The dialog:
  - While a library is made, a missing subcircuit shows two modal "Cannot load document"
    boxes before the dialog's own message (`huntRewriteThatFails`).
  - `slotCreateLib` makes `new LibraryDialog(this)` each time and never deletes it.
  - It lists what the Content panel shows, without refreshing it first (`create_library`
    does).

  **Fixed in `638903e`:** a load's errors go among the messages (`misc::ErrorCapture`); the
  dialog lives for its one use; the Content panel is refreshed first.
- **N6.** `get_ui` gives a tree's first 200 rows and does not say that there are more. The
  Libraries panel, with the system libraries open, ends in the middle of them (`p16`).
  **Fixed in `638903e`:** `rows not shown` (a tree, a table, a view) and `items not shown` (a
  list, a box) say how many more.
- **N7.** A deactivated SPICE library or `.INCLUDE` part still has its file copied into the
  library and attached. `getSpiceLibraryFiles()` and `collectSpiceLibraryFilesIn()` do not
  check `isActive` (reading). **Fixed in `638903e`:** a part left out of the circuit has none of
  its files taken in.

## The fixes, checked

Where: `components/libcomp.cpp` (a part's model read and named: A3, B1, B5, B7, B9, B10),
`dialogs/librarydialog.cpp` (the library made: A1, A2, A4, B2-B4, B11, B12, N4, N5, N7),
`extsimkernels/abstractspicekernel.cpp` (B1, N7), `schematic_file.cpp` (B6),
`qucscontrol_libraries.cpp` (`create_library`, `import_library`, `list_libraries`: A2, B7-B10,
N1, N3), `qucscontrol.cpp` and `qucscontrol_design.cpp` (`add_component`, `describe_part`,
`get_ui`: B9, B10, N6), `erc.cpp` (E1, B9, B10), `qucs.cpp` (the panel: B10),
`qucs_actions.cpp` (N5), `dialogs/importdialog.cpp` and the converter (B13, N2).

Tests:
- `test_library_export`, new: the parts' names, hierarchical parts and a library made before,
  two libraries' inner subcircuits and the circuit's own (ngspice and Qucsator netlists), the
  renaming word by word, a renamed library, the SPICE files, a library part inside, the
  replace (by `create()`, with no trash, by the dialog's Rewrite), a folder of the name, the
  dialog's Next and its unsaved subcircuit, the commands, Convert Data File and the converter.
  Netlists are written, not simulated: no simulator is needed (CI's macOS has none).
- `test_qucs_control` `librariesComeInWhole`: `import_library`'s includes, refusals, missing
  files, newer library and `name`; `describe_part`, `add_component`, `list_libraries`, the
  panel and Check Schematic on them; `create_library`'s names and a failed replace; `get_ui`'s
  rows not shown.
- `test_project_libraries`: the newer library's message, and a damaged one's.

Each fix was broken on purpose and the tests run: 47 breaks, all caught. Four were caught only
once a test was added for them: a part's includes once per netlist rather than per part (a
Qucsator netlist of two parts sharing an inner subcircuit), a deactivated SPICE library part
inside a subcircuit, `get_ui` on a tree of more than 200 rows, and a command line outside a
`.control` block or in a deactivated part.

Then:
- the full suite 91/91; under AddressSanitizer and UBSan 91/91, no report (a first parallel
  run had `test_build_all_va` and `test_claude_git` time out under load; both pass alone and in
  a second full run);
- the end-to-end scenarios 97/97;
- the hunt's probes again: every part of p1, p2, p3b, p4, p6, p6c and p18 gives its
  subcircuit's value, under Qucsator too (p3c, p6, p6c); p5, p9, p12 (with the check on), p15
  and p17 say what they should;
- 800 more damaged libraries on the ASan app, with the command check on (p13): no report;
- every installed library part under ngspice, as the nightly runs it
  (`scripts/ci/test-library-parts.py` against `ngspice-tested.json`): 4049 of 4235 parts pass
  and 1796 of 1808 benches, as with the app of `1306eaf`. Its first run found a part whose
  call has a comment after it (`555_XSPICE`: `X1 6 5 22 comparator5 ; the reset comparator`),
  which the renaming had taken a word of; fixed and tested before the commit. A part of
  VDMOS_IR, another each run, now and then has no pins with 8 jobs - with the app of
  `1306eaf` too: not of these changes.

## What was found right

- **Names:** a subcircuit named with a space (`my div`), an accent (`Résumé`) or a leading
  digit (`2stage`) is placed and simulated from its library (`p1`).
- **Pins:**
  - ports numbered in another order than they are drawn give the same circuit as the
    subcircuit placed directly;
  - the ground pin (`ground_pin`) on or off simulates the same (`p7`).
- **Parameters:**
  - a part made of a subcircuit with a parameter takes its default, or a value set on the
    placed part (r2 = 3k: 0.75), under ngspice and Qucsator;
  - a wrapper passing a parameter down works under Qucsator (`p6`).
- **Hierarchy under Qucsator:** one and two levels down simulate right (`p6`).
- **Verilog-A:**
  - a library with its Verilog-A and an include embedded was carried to a second workspace
    (another computer), imported, linked into the project, compiled and simulated: 0.75
    (`p10`);
  - a source that cannot be read fails the library, with digital models or without (`p14`).
- **Windows line ends:** a Qucs-S library with CRLF lines imports, describes and simulates
  (`p4`).
- **The converter's `.model` cards:** imported and placed as the Diode, BJT and MOSFET they
  are, with their values in the netlist (`p8`).
- **Descriptions:** text with `<in>`, `<out>` and `<b>` survives the dialog's Next and
  Previous and is written as typed (`huntDescriptionThroughPrevious`).
- **Damaged libraries:** 800 mutants of 8 installed and 3 generated libraries on ASan/UBSan
  (cut, lines deleted, duplicated or swapped, tags inserted, quotes and brackets removed,
  bytes changed, 100,000-character lines, huge and negative numbers): no crash, hang or
  report; 2 of them refused by the import (`p13`).
- **Replace by `create_library`** moves the old library to the trash before writing (only
  the failure of A2 is not said).

## How to run it again

```
cd docs/bug_hunts/2026-10-07-library-import-export
export HUNT_RUNS=/tmp/hunt          # each server's workspace, settings, HOME, trash and cache
python3 p6_hierarchy_params.py      # B1: hierarchical parts under ngspice and Qucsator
python3 p6c_inner_names.py          # A3 (B1 worked around in the files)
python3 p2_spice_files.py           # A4, B5
python3 p1_subcircuit_names.py      # B3, B4
python3 p5_replace_fails.py         # A2
python3 p3b_library_part_inside.py  # B2
N=400 SEED=7 QUCS=<repo>/build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s python3 p13_library_fuzz.py
```

The probes need the Release app built (`build/`), and its Qucsator for the Qucsator runs.
`mcp.py` sets Qucsator, and OpenVAF (`openvaf-r` on the PATH), in each server's settings.

The dialog findings (A1, B11): paste a function of `dialog_probes.cpp` into
`TestLibraryPaths`'s private slots in `qucs/tests/test_library_paths.cpp`. Add `QPlainTextEdit`,
`QPushButton` and `QTextEdit` to its includes. Build `test_library_paths` and run it with the
function's name, with `QUCS_TRASH_DIR` set.

E1 is to be tested by detection only: a library part whose SPICE text holds the bare
keyword, in a schematic with no analysis, read with `check_schematic` and `get_netlist`
(`p12`). Nothing that would run a command was simulated.
