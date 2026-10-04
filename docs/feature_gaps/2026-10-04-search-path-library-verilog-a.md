# Feature gaps: Verilog-A of search-path libraries in projects

*4 October 2026 - Qucs-S 26.1.5.*

The review (`qucs-s-search-path-library-verilog-a-review.md`) tested a library found
through the library search paths whose part wraps a Verilog-A device: the `.va`
linked into the project, compiled there, loaded, the library folder never written.
It found the design sound and listed five open issues. Each was reproduced on the
build of 15c7514 first, with OpenVAF-reloaded and ngspice 46, through Claude's tools
(a library of two parts, `sub_a` with `good.va` - which `` `include``s
`good_params.vams` from its own folder - and `sub_b` with `better.va`, made into a
search path folder and placed in another project):

| Issue | Found |
|---|---|
| 1. The link is absolute | Yes: `TwoLib/good.va -> /…/teamlibs/TwoLib/good.va`. |
| 2. The folder is the project's top level, where a project library's folder is | Yes: `PROJECT/TwoLib/`; a project with its own `TwoLib.lib` got `TwoLib_2/`. |
| 3. An `` `include `` beside the original | Worked with OpenVAF-reloaded (v(out) = 0.4 V, 1k against 2k ∥ 1k): it resolves includes from the file a link leads to. Another OpenVAF need not. |
| 4. Clean-up | All right: one of two parts taken away took its own link and model only; undone, its link came back at the next simulation; taken away and not saved, or closed without saving, the link stayed (the saved schematic uses it). |
| 5. The library by its path | Claude's `describe_part` placed by name, `create_library` by path, the *Libraries* panel by path for `user_lib` and the search paths. A part whose stored path is not there already found its library by name. |

## What was done

| Review | Now |
|---|---|
| **1. Portability.** Regenerate from the record and the search paths; a dangling link means relink; copy on Windows when a symlink cannot be made. | The link is **relative** (`Libraries/VaLib/good.va -> ../../../libs/VaLib/good.va`), taken from where the link really is, so it holds when the project and the library move together - a repository of both cloned, another user's home with the same layout - before Qucs-S looks again. A link that leads nowhere was already made again (when the project opens, a schematic is saved, before a simulation) to where the library is found, by its name; one whose library is not found here keeps what it has. **Where no link can be made** - a disk without symbolic links (exFAT, some network shares), and Windows, where a link needs Developer Mode and Qt's `QFile::link` makes a shortcut - the source is copied, with the files it includes, copied again when the original changes; while links still cannot be made the copy is left as it is (no new copy, no new compile, each time), and a link takes its place once one can be. Windows keeps copies: a real symbolic link there needs the Windows API with Developer Mode, which cannot be tried here. |
| **2. Collision with project libraries.** A dedicated location, `PROJECT/.qucs/libs/<library>/` or `PROJECT/Scratch/libs/<library>/`, keyed by library path. | **`PROJECT/Libraries/<library>/`**: apart from the project's own libraries (`NAME.lib` and `NAME/`), and visible - the *Content* panel lists the link with the project's Verilog-A and the user can see the file, which was the point (a dot folder is hidden from both; *Scratch* is for temporary files, emptied by *Clean Scratch*, and for a folder that is not a `_prj` project it is outside the project). Two libraries of one name: `Libraries/VaLib` and `Libraries/VaLib_2`, each folder's record saying which library it is (its tool tip too). `Libraries/` is marked as Qucs-S's when Qucs-S makes it (`.qucs-libraries`) and taken away when it holds nothing else; a `Libraries/` folder of the user's is never. The folders the earlier build made at the top level are moved: what they held taken away, made again in `Libraries/`. |
| **3. Multi-file Verilog-A.** Compile the canonical path, or link every include. | **OpenVAF is given the file the link leads to and told where to write** (`openvaf /…/libs/VaLib/good.va -o /…/PROJECT/Libraries/VaLib/good.osdi`): its includes are found beside the original whatever an OpenVAF does with links. Everywhere a `.va` is compiled: before a simulation, Claude's `build_verilog_a`, *Build All* and *Compile* in the *Content* panel, *Build* on an open `.va` (`osdi::compileArguments`). The staleness check followed the link's includes already. Tested with OpenVAF-reloaded end to end: a library `.va` including a file of its folder, linked, compiled and simulated. |
| **4. Clean-up edge cases.** Several schematics, undo, delete without saving, what else goes, two parts. | All were right (above); each is a test now: a part kept while another schematic - closed - uses it; taken away in the tab and not saved, kept; saved, taken away; undone, back at the next simulation (the tab's part counts); closed without saving, kept; one of two parts of a library with their own sources takes only its own link and model; the last one takes the model, the record, the folder and `Libraries/`. |
| **5. Absolute library path in the schematic.** Store the library by name, resolve it through the search paths, fall back to the stored path. | **A part names its library by its name when that name finds this very library** - no installed library, the project's, `user_lib`'s or an earlier search path's of the same name is found first -, **else by its path** (`LibComp::referenceTo`): the *Libraries* panel's parts of `user_lib` and the search paths (the installed and the project's were by name already), Claude's `create_library`, `list_libraries` and `import_library` (`describe_part` and `find_library_component` placed by name already, and find a library in the order a name is resolved). A part placed by a path that is not there - an older schematic, another computer - already found its library by name in the search paths. Storing the name alone always would let an installed library of the same name take the part. |

## Tests

- `test_project_libraries` (24): the folders in `Libraries/`, apart from the project's
  own library of the name; `Libraries/` taken away when empty, the user's kept; the
  link relative, still leading to the file when the tree of project and library is
  moved; a copy where no link can be made (`setLinkMaker`), kept while no link can be,
  a link in its place once one can; the top-level folders of before moved, one of a
  library not found kept; two parts with their own sources; a part placed by a path
  of another computer linked; parts named by name when that finds them (the panel's of
  a search path and of `user_lib`, a dotted name by its path); what OpenVAF is given;
  a simulation giving OpenVAF the link's file; and the earlier ones, moved.
- `test_qucs_control`: taken away and not saved, kept; saved, taken away; undone, back;
  closed without saving, kept; `create_library`'s part placed by name.
- `test_build_all_va`: *Build All* and *Build* of a linked source.
- `test_library_paths`: the panel's search-path part placed by its name.
- `scripts/mcp-e2e-scenarios.py` s12, with OpenVAF-reloaded and ngspice: a library whose
  `.va` includes a file of its folder, made into a read-only search path folder; its
  part placed by name, saved - a relative link in `Libraries/` -, compiled beside the
  link with the include found, simulated (v(out) = 2/3 V), taken away with
  `Libraries/`.

Each part of the change broken on purpose: 27 of 28 breaks caught; the 28th - that
`Libraries/` is taken away only when it bears Qucs-S's mark - could never fail, the
test beside it requiring the mark to be all that is left, and was removed.

Full suite 88/88, under AddressSanitizer 88/88, the end-to-end scenarios 88/88.

## Not done

- A real symbolic link on Windows with Developer Mode: copies there, as before.
