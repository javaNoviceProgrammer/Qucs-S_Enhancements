# Feature gaps: Verilog-A of search-path libraries, round 2

*4 October 2026 - Qucs-S 26.1.5.*

The review (`qucs-s-search-path-library-verilog-a-review.md`) re-tested 1dc30d4 in a
fresh project against two search-path libraries, `mylib` (the op-amp) and `inclib` (a
`.va` that `` `include``s a file of its folder). It found the five issues of
[round 1](2026-10-04-search-path-library-verilog-a.md) fixed:
- relative links, relinked when the libraries moved;
- `Libraries/<library>/`, and the earlier `mylib/` migrated;
- the include found;
- the clean-up;
- parts named by their library's name.

One new issue needed a fix: **a library of the same name shadows the part**. Here
`Lib="mylib"` is the search path's op-amp, and `create_library name=mylib
destination=project` makes the project a `mylib.lib` without it. Reopened, the
op-amp loses its pins and the run fails. The sync takes `Libraries/mylib/` away,
since the part is unresolved and its library looked unused. The check says the
part is "not ... in a folder of the library search paths", which is wrong.

## Reproduced

On the build of 1dc30d4, headless, with copies of the review's library in a search path
folder and its test bench `tb_opamp.sch` in a project:
- With only the search path's `mylib`, the netlist has `mylib_opamp` and `XOPA1`.
- With the project's own `mylib.lib` (one part, `rcfilt`) beside it, the netlist
  fails ("Could not go throughAllComps"): no op-amp.
- In the tests on the same code, the sync takes the link away and the check gives
  the old message.

The cause is the one the review found. `misc::properAbsFileName` returns the first
`NAME.lib` there is, and looks beside the schematic, in the project and in
`user_lib` before the search paths. It never checks that the file has the part.
`LibComp::referenceTo` checks only when the part is placed.

## What was done

| Review | Now |
|---|---|
| **Resolve a name to the first candidate that contains the part.** | **A part's library is the first library of its name that has the part** (`LibComp::libraryFileOf`). `misc::properAbsFileNamesIn` lists the libraries of a name in the order a name is looked for. `LibComp::hasComponent` reads a library's parts once while the file is unchanged (decoded as the loader decodes it, a byte order mark left out). The file at the part's path comes first when it has the part: a path the part names, or the installed library of a name. This holds wherever a part finds its library: loading, the netlist, the check, and the project's sync, for schematics read as text and for open ones. With the review's library and test bench the netlist is now identical to the one without the project's `mylib.lib`. |
| **When a name has more than one candidate, record which one was used and prefer it.** | **Of two libraries that have the part, the part keeps the one the project linked its Verilog-A from.** The record in `Libraries/<library>/` already says where that was (`projectlibraries::linkedFolders`). So a library of the name made later, earlier in the order, does not take the part. The records looked at are the open project's, and those of the schematic's folder, for a project netlisted with none open (the command line). A record is read again when it changes, including a rewrite within the second on a disk whose file times are coarse (HFS+, FAT). A part named by its path keeps that library while the library has the part. |
| **`create_library` and `import_library` could warn when the name is used.** | **They say so.** `create_library`, and `import_library` for a Qucs-S library, answer `also_named` with the other libraries of the name, and a `warning` saying what that means. A SPICE library's parts name its file, so it gets no warning. *Create Library*'s messages, in the dialog and in `create_library`'s `messages`, end with a note. |
| **The error should name what it found.** | **The check says which libraries of the name there are**: "the libraries mylib there are - …/mylib.lib, ~/Desktop/mylib.lib - have no part opamp", or for one, "the library mylib there is, X, has no part opamp". When a library has the part but it can't be read from: "it is in X, which it could not be read from: a library of a later Qucs-S, or damaged". When no library is found the message is as before. A new **warning** covers a part named by its library's name that two libraries of the name have: "X1: more than one library mylib has a part opamp: A is used (found first), not B - a path in the part's Lib names the one meant". When the project's record decided, it says "(the project's Verilog-A of it is linked from there)" instead. |

Both side effects the review listed are gone:
- *Its Verilog-A is removed*: a part that no library of its name has is now
  unresolved, like one whose library is not found, so its folder in `Libraries/`
  stays. This also holds when the part is named by the path of a library that
  lacks it.
- *The message is wrong*: fixed, as above.

Claude's instructions say how a part finds its library. `create_library`'s and
`import_library`'s descriptions mention `also_named`.

## Tests

- **`test_project_libraries`** (27), two new tests:
  - A library of the name without the part (the project's own `VaLib.lib`) does
    not take it. The part keeps its pins and its library, the link and model stay,
    and the check is silent.
  - None has the part: the check names the libraries, one or more; the link stays,
    also when the part is named by a path to one without it.
  - A library of a later Qucs-S's that has the part: said.
  - Not found: as before.
  - A part named by a path whose library lacks it takes the library of its name
    that has it.
  - A library saved with a byte order mark has its part and loads it. A file of the
    name that is no Qucs library does not count, even with a line like the part's.
  - The project's own library gaining the part: the linked one keeps it; with the
    record gone, the first does. Two parts of one name in one schematic are each
    found in their own library.
  - Two libraries with the part, another search path's put first: the one linked
    from keeps it, in a project schematic, one in a subfolder, and an open one; the
    check warns which.
  - Named by its path: that library, no warning. A project without a record takes
    the first, and the warning says "found first".
  - The record is read again when another program changes it, and when Qucs-S
    rewrites it within the second.
  - *Create Library* with another library's name ends with the note; with a name
    of its own, no note.
- **`test_qucs_control`**: `create_library` and `import_library` with a name another
  library has: `also_named` and `warning`; none for a name of its own or a SPICE
  library.
- **`scripts/mcp-e2e-scenarios.py` s12**, with OpenVAF-reloaded and ngspice: after
  the search path library's part is linked and simulated, `create_library` makes
  the project a `VaRes` without the part and its answer names the search path's.
  The schematic, closed and reopened, keeps the part (two pins), the link and the
  model, and simulates (v(out) = 2/3 V).

Each part of the change was broken on purpose: 30 breaks, all caught. The tests at
first missed two:
- The UTF-8 byte order mark could not tell the loader's decoding from
  `QString::fromUtf8`, which drops that mark too. The test library is now UTF-16,
  which only the loader's decoding reads.
- The sync keeping a library named by a path without the part was hidden by
  another schematic that named it by name. It is now tested alone.

Code that could never make a difference was removed:
- a flag in the lookup to go on past an absolute path (each caller's choice already
  decided);
- the mutexes of the two caches (no thread uses them);
- a check for a missing `Libraries/`;
- a guard for a part with fewer than two properties;
- falling back to the first library there is for a part that no library has (the
  part isn't loaded either way).

Full suite 88/88, under AddressSanitizer 88/88, the end-to-end scenarios 90/90.

## Cost

A schematic of 1000 parts of a search path's library (`--mcp-server`):
- **Opening it**: 0.172 s, against 0.164 s on 1dc30d4.
- **The check**: 0.044 s (0.046 s). The check looks up each part's libraries once
  per run; without that it took 0.070 s.

## Not done

- A library without Verilog-A has no record in the project. Of two libraries of one
  name that both have a part, the part takes the first; the check warns which.
