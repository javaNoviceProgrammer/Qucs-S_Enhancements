# Feature gaps: the git integration, checked again in depth

*7 November 2026, on top of `3972088`. Apple M2 Ultra, macOS 27.2, APFS, git 2.54.0 (Apple), Qt 6.12.0 from Homebrew.
CI pins Qt 6.10.3 and none of this has run on it (see "What could not be verified").*

The user asked for a focused quality pass on the git integration: a bug hunt that drives it, fuzzing, optimization by
measurement, and a plan for what to add. The plan is at the end. Nothing in it is built.

Everything here is in the working tree on `main`, **not committed**.

## What was checked

The whole feature was read: `gitrepo.*`, `gitstatus.*`, `githistory.*`, `gitgraph.*`, `gitui.*`, `qucs_git.cpp`,
`qucscontrol_git.cpp`, `claudegitbar.*`, and two callers the file names did not show: `schematicdiff.*` (which runs git
itself) and the conflict checks in `qucs.cpp`. `filebrowser.cpp` and `statusbar.cpp` only read the tracker's state and
were read for pointers held across an event loop. None are.

It was driven through a new test that has no window (`test_git_robust`, gitrepo.h and gitstatus.h as the windows and
Claude's tools call them), the fuzzers below, and git itself by hand in throwaway repositories. Every repository was
in a temporary folder or the scratchpad. This checkout was only looked at, as the profiling subject.

**The check of 10 October still holds.** Its 31 test functions pass unchanged in `test_git_integration`, in Release and
under ASan and UBSan: the conflict that keeps the open schematic, `git_resolve`, a failed reload put back, relative
paths found alike, parts keeping their place, `View=` kept, the part-by-part summary.

States driven (`aRepositoryInEveryState`, `whatIsUnderWayIsSaidRightly`): before the first commit, detached HEAD, a
branch with no commit beside others with them, a linked worktree, a shallow clone, a bare repository, a `.git` that is
bare, a `.git` file that leads nowhere, a submodule from inside, a locked index, a corrupt index, a corrupt HEAD,
mid-merge, mid-rebase stopped at a merge, mid-`git am`, no git at all.

## Faults found

All fixed unless the "left" list says otherwise. Each has a regression test; `breaks.py` breaks each fix in turn and
runs the test that should catch it (see "Tests").

### Data lost, or a crash

| # | Fault | Found by | Now |
|---|---|---|---|
| 1 | **A file's name was read as a pattern.** Discard of `amp[1].sch` also threw away the changes in `amp1.sch`. Discard of a file named `:!x` restored **every other file**, since to git that means "all but x". The same in stage, unstage, commit, stop tracking, Resolve Conflict (which resolved `x1.txt` too), and in the diff and history read for a name. | Reading, then driven | A path is that file. Commands that change things pass `:(literal)` per path. Commands that only look use `GIT_LITERAL_PATHSPECS`. Not the variable for all: hooks would inherit it and their own patterns would stop matching, and `git check-ignore` refuses it. A test's hook checks this. |
| 2 | **A crash (SIGSEGV) when a git command could not start**, for example with its folder gone: `Job::start` went on using the process that had just been let go. | Driving | Said as "git could not be started". |
| 3 | **Discard sent a file to the trash instead of restoring it**, when it was out of the index and on the disk, as Stop Tracking, Keep the File leaves it. git lists that file twice, and the restore ran first, then the trash took the restored file. | The model fuzzer, under ASan | Restored as HEAD has it, not trashed. |
| 4 | **A stopped git left `index.lock`**: Cancel in the progress window, a tool's time limit, a job gone with its window. Every git command after that fails until someone deletes the file. A stopped clone left a half-made folder, and the next try said it already exists. | Reading, then driven | Asked to stop first (SIGTERM), so git lets go of its locks and takes the folder back, then killed after 1.5 s. Windows still kills at once. |
| 5 | **An untracked link was read through**: a link to a pipe hung the bar's status for good, a link to `/dev/zero` would fill the memory, and a link to a file outside showed that file's contents in the Changes window. Also a schematic-named link, and `hasConflictMarkers`. | Reading, then driven (pipe, outside file). `/dev/zero` is not run against the old code, for the memory | A link is one line, its target, as git shows it. Only regular files are read. |
| 6 | **Discard said done and did nothing** for a name beginning with `:` (to Qt, a resource path, so `QDir::filePath` dropped the folder, and `relative()` refused it as outside), and for a name in the other spelling of its accents. | The model fuzzer | `fileIn()` joins paths. Names are compared in one spelling on macOS. |
| 7 | **Open Both Versions changed a file's bytes**: read as text and written back, Latin-1 and any NUL came out altered. | Reading, then driven | Written from git's bytes. |
| 8 | **Unstage failed before the first commit** on a file edited since it was staged. | The model fuzzer | Unstaged, the file left alone. |
| 9 | **Committing a staged deletion by name failed** ("did not match any files"), from `commit()` and `git_commit`. | The model fuzzer | Committed. |

### What a repository's own settings could run, when Qucs-S only looks

`readingRunsNoneOfTheRepositorysPrograms` already covered the file system monitor, filters and text conversions. A
project downloaded with its `.git` is the threat the code names. Four more ways through, each confirmed by a program
that writes a mark when run:

| # | Way through | Now |
|---|---|---|
| 10 | **`log.showSignature` with `gpg.program`:** any `git log` of a commit with a signature header ran the repository's program. The same setting in a user's own global configuration put git's lines before each record and spoiled the commit's hash in the History. | `log.showSignature=false` among the guards. |
| 11 | **A filter driver whose name holds `=`:** `-c filter.a=b.clean=` is read as the key `filter.a`, so that driver stayed. | Passed by `GIT_CONFIG_KEY_n`, which takes the key whole. |
| 12 | **A submodule's own filter:** `git status` looks inside a submodule, under its own configuration, which the guards do not list. The code's comment said submodules are not entered. | `--ignore-submodules=dirty` (a behaviour change, below). |
| 13 | **A partial clone's fetch:** a diff of a commit whose file is not here fetches it, which runs `core.sshCommand` or an `ext::` remote. | `GIT_NO_LAZY_FETCH=1`, git 2.45 and later only (behaviour change, and left for older git, below). |

Names from a repository as options: a branch `--force` or a remote `--receive-pack=evil` went straight into `git push`,
and a branch `--web` into `gh pr view`. All are refused now.

### Wrong, without loss

| # | Fault | Now |
|---|---|---|
| 14 | **A commit written by hand could make rows of its own:** the fields were split on 0x1e and 0x1f, which a name or subject may hold. A made-up row's hash and parents reached the graph, the History's HTML and a temp-folder path. | Fields end in NUL, which no commit can hold. A hash that is none drops the row. Branches and stashes alike. |
| 15 | **A folder named `.git` with nothing in it** counted as a repository: git goes on up, so its paths were the parent's and every command acted on the parent. | `topLevel` wants HEAD, `objects` and `refs`, as git does. |
| 16 | **`git am` was shown as a rebase,** and its abort refused. **A rebase stopped at a merge was shown as a merge,** and abort gave up the merge alone, leaving the rebase under way. | `am` of its own; rebase checked first. |
| 17 | **New files with an accent, a quote or a tab in the name were missing from Show Changes:** `ls-files` without `-z` quotes them, and no such file is found. | `-z`. |
| 18 | **A schematic with such a name had no part-by-part summary,** and **a merge commit never had one.** | Read from `changedFiles`. |
| 19 | **A file with a tab in its name showed as binary** in a commit's files. | The path is everything after two tabs. |
| 20 | **`color.ui=always` put escape codes in** a commit's text (Show Its Changes), a stash's text and Claude's `git_log` graph. | `color.ui=false` in every git's environment. |
| 21 | **The whole History was empty on a branch with no commit** where other branches have them. | `--ignore-missing`. |
| 22 | **`.gitignore`:** trailing spaces were lost (`sp ` ignored `sp`). A name with a line break wrote a second line: `x\n*` ignored everything. `../x` was accepted. | Escaped; a line break is refused, with why; outside the repository is refused. On macOS, a name with accents gets both spellings, as git compares either. |
| 23 | **An older read could replace a newer one:** a background read that began before a commit ended after it, and the tracker showed the state from before. | A read begun before the last direct read is dropped. |
| 24 | **A partial clone's fetch URL was empty:** `(fetch) [blob:none]` did not match the pattern. | Matched. |
| 25 | **`git_commit` with `paths: ["."]` failed:** an empty path. | `.`. |
| 26 | **A one-character line in a damaged `.sch`** (`<` alone, or `>` in Properties) cut past the end of a string, undefined in Release and an assert under the sanitized build. | Cut within bounds. |
| 27 | **On macOS with `core.precomposeunicode` off,** or a name holding an emoji (git then does not compose it), a file was not found by the spelling Qt gives. | Lookups in one spelling. |

### Behaviour changes to look at

These follow from the fixes above, and you may want some differently:

1. **Claude's `git_stage` and `git_commit` no longer expand patterns.** `paths: ["*.sch"]` used to work by accident. The
   schema always said files. It now fails with git's "did not match".
2. **In a partial clone, looking does not fetch** (git 2.45 and later). A diff of an older commit whose file is not local
   shows git's "could not fetch" until the file is fetched. To undo it, remove one line in `lookingEnvironment`, but then
   looking at a downloaded partial clone can run what its remote names. I judged that worse, since this is rare in an
   EDA project.
3. **A submodule with modified files inside no longer shows as changed** in the File Browser and the Commit window. New
   commits in it still do. The dock's bar has always worked this way.
4. **Show Changes writes a new file's diff itself.** No `index` line, and a name is shown as it is, not quoted by git. A
   test compares the rest with `git diff --no-index` line for line.
5. **A `.gitignore` line for a name with accents can be two lines** on macOS.
6. **`Status::root` is the folder as found, not its real path,** and `Status::head` is seven characters, not git's
   `--short`.

### Left unfixed

Large, or small enough to wait:

- **The windows wait for git on the GUI thread.** History, Commit, Blame, Changes and the menus are all synchronous, and
  a merge or revert from the History's menu can block for up to 300 s (`merge()`'s limit). Moving them is a real
  refactor, and the tests read their results synchronously. It is item 3 of the plan. See the measurements for what
  remains.
- **`git < 2.45`:** fault 13 stays open, as the variable is ignored and the test skips.
- **A cherry-pick of several commits between steps** (`sequencer/`, no `CHERRY_PICK_HEAD`) shows nothing under way. From
  reading only.
- **Discard of a submodule's entry** moves the index and not the submodule's files. From reading only.
- **A file both staged as deleted and untracked:** `entryOf` returns the untracked one, so the menus do not offer
  Unstage. The Commit window lists both.
- **`&` in a branch name** is read as a menu's shortcut mark; `%2` in a file's name is expanded in a few of `namesOf`'s
  messages. Both cosmetic.
- **Blame** builds one tree item per line, which is slow on a very long file.

## Fuzzing

`tests/test_git_fuzz.cpp` follows `test_claude_fuzz`: `QUCS_FUZZ_SEED`, `QUCS_FUZZ_STEPS` (a short pass in ctest),
`QUCS_FUZZ_SECONDS`, `QUCS_FUZZ_TRACE`, plus `QUCS_FUZZ_ONLY` for one fuzzer. To fuzz them, the readers were split from
the running of git, so they are now public: `readStatus`, `readCommits`, `readBranches`, `readRemotes`, `readStashes`,
`readChangedFiles`, `readBlame`, `readBlobs`, `readNumstat`.

- **`fuzzTheReaders`:** real git's output for a repository of awkward names, captured by a git that keeps what it
  writes, then damaged. Read by every reader, commits laid out as a graph and painted, damaged schematics summed. It
  checks that a hash is a hash, that lanes stay inside the width, that each entry is found by its own path, and that no
  reader takes long.
- **`fuzzTheWindows`:** a git program that answers with such damaged text and random exit codes, under the History, the
  Commit, Blame and Changes windows and the menus.
- **`fuzzARepository`:** real git on hostile file and branch names, under random edits, stages, unstages, discards,
  commits, stashes, branches and `.gitignore` lines, against a model of each file in HEAD, the index and the disk. After
  every step every file on disk must be as the model says, so an operation on one file that touches another fails at
  once. Every few steps `read()`, `status()`, `diff()`, `log()`, `fileAt()` and `blame()` are checked against the model.

| Run (final sources) | Build | Seeds × time | Steps | Crashes, reports, model failures |
|---|---|---|---|---|
| Readers | ASan + UBSan, Debug | 4 × 420 s | 2,358,217 | 0 |
| Readers | Release | 3 × 120 s | 1,390,483 | 0 |
| Repository | ASan + UBSan, Debug | 8 × 420 s | 55,819 steps on 469 repositories | 0 |
| Repository | Release | 6 × 420 s | 63,209 steps on 530 repositories | 0 |
| Windows | ASan + UBSan, Debug | 4 × 420 s | 767 rounds | 0 |
| Windows | Release | 3 × 240 s | 535 rounds | 0 |

**Found by fuzzing, before those clean runs:** faults 3, 6, 8, 9 and 26, plus the `.gitignore` spelling part of 22. The
readers and windows found no crash of their own in Release; 26 needed the sanitized build's asserts. Some first findings
were the model's own mistakes or my run script's (QtTest's 300 s limit aborts a longer function) and are not counted.

## Optimization

**Method.** `test_git_perf` with `QUCS_GIT_BENCH=<repository>`: for each operation, the git programs it starts (counted
by a logging git) and the best time of five. Release, Qt 6.12.0, an idle machine. Before is with the bug fixes in and no
optimization.

**Subjects.** *Large*: `make_big_repo.py`, seeded: 50,000 commits, 20,000 files, 199 branches, 100 tags, 240 files
changed, 200 new, a 5,000-file ignored folder, on the internal SSD. *This*: this repository, 716 commits on `main`, 4,807
files, on the external X9.

**What costs here.** A git program costs 8.5 ms just to exist on this machine (`git --version`). On the large
repository, `status` is 320 ms, of which 240 is the walk for untracked files (79 ms with `--untracked-files=no`).
`--ignored=matching` adds nothing measurable.

| What | Large: programs | Large: ms | This: programs | This: ms |
|---|---:|---:|---:|---:|
| read() | 2 → 1 | 351.2 → 329.0 | 2 → 1 | 34.3 → 24.9 |
| status(): the dock's bar, every 10 s | 14 → 4 | 786.7 → 445.5 | 12 → 5 | 149.3 → 73.7 |
| diff(status, a file) | 2 → 1 | 21.7 → 12.6 | 2 → 1 | 31.1 → 20.5 |
| diff(root, a file): a click in the Commit window | 4 → 2 | 356.1 → 24.2 | 4 → 2 | 55.4 → 21.1 |
| diff(root, staged) | 3 → 1 | 353.2 → 17.0 | 3 → 1 | 43.8 → 10.2 |
| diff(root): all | 204 → 3 | 2676.8 → 376.7 | 12 → 3 | 139.1 → 50.4 |
| diff(root, an untracked file) | 3 → 1 | 344.6 → 14.1 | 3 → 1 | 43.2 → 10.3 |
| schematicChangesOf(root) | 24 → 2 | 571.4 → 346.6 | 2 → 1 | 33.5 → 25.0 |
| schematicChangesIn(HEAD) | 5 → 4 | 54.9 → 44.4 | 3 → 3 | 31.5 → 31.7 |
| History window: opened | 19 → 13 | 914.1 → 477.6 | 17 → 12 | 368.0 → 258.9 |
| History: reload() | 19 → 10 | 916.4 → 479.0 | 17 → 9 | 377.7 → 266.5 |
| History: refresh(), on any change of the repository | 4 → 3 | 450.3 → 106.4 | 4 → 2 | 133.9 → 100.2 |
| History: a commit chosen | 9 → 6 | 111.4 → 72.6 | 9 → 6 | 100.0 → 68.7 |
| History: uncommitted row chosen | 241 → 9 | 3965.0 → 455.4 | 25 → 9 | 346.8 → 105.2 |
| History: its menu | 2 → 0 | 343.6 → 0.0 | 2 → 0 | 34.9 → 0.0 |
| Commit window: opened | 2 → 1 | 342.5 → 335.1 | 2 → 1 | 36.5 → 27.0 |
| Changes window: opened (All changes) | 2 → 1 | 120.6 → 121.7 | 2 → 1 | 39.7 → 34.3 |
| log(root), 200 | 1 → 1 | 18.1 → 17.6 | 1 → 1 | 16.2 → 16.3 |
| history(All), a page of 500 | 1 → 1 | 200.4 → 198.8 | 1 → 1 | 20.2 → 20.5 |
| log(a file), 200, followed | 1 → 1 | 384.9 → 387.5 | 1 → 1 | 44.0 → 43.7 |
| blame(a file) | 1 → 1 | 375.8 → 375.3 | 1 → 1 | 19.4 → 19.2 |
| History: 100 rows drawn | 0 → 0 | 5.6 → 5.7 | 0 → 0 | 9.6 → 9.6 |
| GraphLayout: as many rows | 0 → 0 | 6.8 → 6.5 | 0 → 0 | 0.1 → 0.1 |
| readCommits: what git wrote for up to 50,000 | 0 → 0 | 50.3 → 53.4 | 0 → 0 | 0.8 → 0.8 |

Before → after, best of five. (On "This", the working tree held this work in both runs.)

**Changes kept, each from a row above:**

- `diff(root, a file)` no longer runs a whole status to learn whether the file is untracked: `ls-files` of that path
  answers it. That is the Commit window's click, 15 times faster on the large repository.
- New files are diffed in-process, not by a git each. 200 new files took 200 programs and most of 4 s on the History's
  row of uncommitted changes.
- `read()` finds the git folder from what is on disk instead of asking `rev-parse`.
- `status()` takes branch, upstream, ahead/behind, dirty and the new files from one `git status`, and the default branch
  from one `for-each-ref`, 14 programs down to 4 or 5. Guards are kept 30 s here too, as `read()` already did.
- Schematic sides are read in one `git cat-file --batch`, not one program each, and a commit's files are remembered by
  full hash, as a commit never changes.
- The History uses the tracker's read when it is under a second old, rather than another status. The right-click menu
  takes 5 s old for its words. A direct read also clears a pending read of that repository.

**Looked at and left, as the measurement did not justify a change:** `GraphLayout` (6.8 ms for 50,000 rows), `readCommits`
(50 ms for 50,000), `readStatus` (under 5 ms of 325), painting (100 rows in 5.6 ms) and the tracker's per-file lookup.

**What remains on the GUI thread on the large repository:** opening the History about 470 ms, of which `history()` is
about 200 (see plan item 5), and a status about 320 more when none is recent. The rows for History's `refresh` and `reload`
benefit only when the tracker has just read. After a command or a change notice it has, so those rows are the common
case, not every case. The time of `status` and `log --follow` themselves is git's.

**Timing tests.** None were added. `test_git_perf`'s `theProgramsEachStarts` bounds the *number of git programs* each
operation starts. That count is the same on every machine, so it cannot go red on a slow runner the way `3972088` and
`c6c7eb2` did, and it fails if a saved program comes back (`breaks.py` checks six).

## Tests

- **`test_git_robust`** (new, 29 functions): the faults above.
- **`test_git_fuzz`** (new): a short pass in ctest, 29 s under ASan, within its limit of 300 s at CI's scale.
- **`test_git_perf`** (new): the program counts.
- **`test_git_integration`**: `claudesGitTools` gains Claude's tools on `.` and hostile names.

**Before any fix**, 22 of the then 25 functions of `test_git_robust` failed: one by the crash, one past its time limit on
the hang. Two that passed were correct behaviour (`stashesAreNumbered`, `aRenamesNamesAndLinesAreRead`). The third,
`aSubmodulesOwnProgramsAreNotRun`, passed through a flaw in the test (a file of another size needs no filter). It was
corrected, and its break below is caught.

**52 breaks, 52 caught** (`2026-11-07-git-integration-deep-dive/breaks.py`). Each changes one place, rebuilds, and runs the test
that should catch it. Two escaped their test at first and those tests were tightened: `git_commit` of `.` (the first break
was not the old code) and a `paths` entry beginning with `:` when the workspace holds a file of that name.

Not broken, and why: the one-character `.sch` cut (only an assert tells, under the sanitized build, and the fuzzer found
it there), and the `/dev/zero` and schematic-link reads (a break would fill the memory; the pipe case covers the guard
next to them).

**Run**, each by ctest, in Release and under ASan and UBSan (a sanitized Debug build), with a HOME of their own:

- **The git suites**, passed in both: `test_git_robust`, `test_git_fuzz`, `test_git_perf`, `test_git_integration`,
  `test_claude_git`.
- **The others that reach the changed code**, passed in both: `test_file_browser_drop`, `test_project_folders`,
  `test_mcp_server`, `test_multiphysics_doc`, `test_qucs_control`.
- **`test_python_tools`** failed two functions here, `theCallsSignature` and `whatANameIs`, as it did on an untouched
  export of `3972088`, so this work did not cause it. Found afterwards: `completer.py` took its list of standard modules
  from `sys.stdlib_module_names`, which Python only has from 3.10, so on macOS's own 3.9.6 `math`, `json` and `os` gave no
  signature or help. Fixed in `completer.py`, with a test that plays an older Python on any Python.
- **`test_library_export` was not run.** It needs `qucsconv_rf`, whose build needs `dos2unix`, which is not installed here
  and was outside the approved install. It reaches the changed code only through `misc::moveToTrash`.
- **The full ctest was not run**, only these.
- **The four lints** are green (143 tools checked).

## What could not be verified

- **Linux and Windows:** not built or run. The tests are written for them (the `#ifdef`s), but CI will be their first
  run. Windows skips the three new test files, whose programs are shell scripts, so its stop path and new-file diff have
  no test there.
- **Qt 6.10.3,** CI's version: everything here ran on 6.12.0.
- **git before 2.45,** and other versions than 2.54 generally.
- **File names that are not valid UTF-8:** APFS refuses them. By reading, they turn into U+FFFD and such a file cannot
  be staged by name on Linux.
- **A real remote:** ssh, https, credentials and a passphrase prompt were not driven. An ssh started from a terminal can
  still reach that terminal, as `GIT_TERMINAL_PROMPT` does not cover ssh. Cancel stops it.
- **A window on a real display:** everything ran offscreen.
- **Finder's short PATH:** whether git-lfs and hooks' programs are found when the app is opened from the Finder.
- **A case-sensitive disk.**
- **`/dev/zero` against the old code:** not run, for the memory. The pipe case was, and hung.

## Plan: what to add

A prioritized proposal, nothing built. Sizes are rough: S a day or two, M about a week, L several weeks.

| # | Item | Scope | Files | Risks | Size |
|---|---|---|---|---|---|
| 1 | **Set up a repository for a project** | Create Repository offers a `.gitignore` for what simulation makes (`*.dat.ngspice`, `*.dat.*`, netlists, spice output) and a `.gitattributes` with `*.sch text eol=lf`. The Commit window warns before staging a generated or very large file. | `gitui.cpp`, `gitrepo.cpp` | Guessing wrong about what a user keeps. Offer, do not impose. | S |
| 2 | **Continue, not only Abort** | Continue and Skip for a rebase, merge, cherry-pick, revert, `am`. Today a rebase stopped by a conflict can be given up in the app and not finished. Sequencer state shown. `git_continue` for Claude. | `gitrepo.*`, `gitui.cpp`, `qucs_git.cpp`, `qucscontrol_git.cpp` | Each operation's own rules. Small with tests per state. | S–M |
| 3 | **The windows off the GUI thread** | History, Commit, Blame and Changes read aside and show when ready, with a stale-result guard as the dock's bar has. History's merge, revert, cherry-pick by `Job`. | `githistory.*`, `gitui.*`, `claudegitbar.cpp`, their tests | Tests read results synchronously, and every view needs a "loading" state. Large diff, little new behaviour, but it is the largest felt gain on a big repository. | L |
| 4 | **Visual schematic diff** | Beside the part-by-part text: the two versions drawn, parts added, removed, moved and changed in colour, in the History, the Commit window and Resolve Conflict. | new `gitschematicview.*`, `schematicdiff.*` (structured result, not only text), `githistory.cpp`, `gitui.cpp` | A schematic drawn from a text needs symbols and subcircuits as of that commit. Layout of two sheets. Headless parsing exists since the check of 10 October. | L |
| 5 | **Keep large histories quick** | Offer `git commit-graph write` (or `git maintenance`). Measured on the large repository: `history()` 194 ms to 28 ms, written in 0.2 s. Offer, because it writes in the user's `.git`, and looking here has promised to write nothing. | `gitui.cpp`, `githistory.cpp` | Very low. | S |
| 6 | **Semantic merge of `.sch`** | A merge driver (`qucs-s --git-merge %O %A %B`): parts by name, wires as sets, properties by key, with conflicts listed in a window part by part. Replaces Keep Mine or Take Theirs whole. | new `schematicmerge.*`, `main.cpp`, `gitui.cpp`, `qucscontrol_git.cpp`, a setup step (`.gitattributes` + `git config`) | A wrong merge makes a wrong circuit silently, so it needs a model fuzz and a connectivity check. The driver is per-user config. | L |
| 7 | **Large files and LFS** | Detect git-lfs. Track with LFS on datasets, Touchstone, GDS. Pointer files shown as such. Warn on size. | `gitui.cpp`, `gitrepo.*`, `gitstatus.cpp` | The guards turn a repository's own filter off, the Finder's PATH may not find it, and a partial clone's rules (fault 13) interact. | M |
| 8 | **Partial staging** | Stage or unstage a hunk or line in the Commit window, by `git apply --cached`; by part for a schematic later. | `gitui.*`, `gitrepo.*` | Patch building on odd files. Schematic part-level needs item 4's structure. | M |
| 9 | **Credentials** | A Qt dialog through `GIT_ASKPASS`/`SSH_ASKPASS`, so https and a passphrase can be asked rather than fail with "terminal prompts disabled". | `gitrepo.cpp`, new helper | Secrets in a process's hands: nothing logged, nothing kept. | M |
| 10 | **Per-part history and blame** | Who last changed R1, and its value over time; `git_log` by part. | `schematicdiff.*`, `githistory.*` | Cost: walking blobs. Needs the batch reader kept open. | M |
| 11 | **Circuit-level compare across commits** | Netlists of two versions diffed (`-n` headless exists): "electrically the same" against a move. Later, two versions' simulation results on one graph. | `schematicdiff.*`, `extsimkernels` | Subcircuits and libraries as of each commit. | M–L |
| 12 | **Submodules** | List, init, update; project libraries kept as one. | `gitrepo.*`, `gitui.cpp` | Mostly UI. Their own config is what fault 12 guards against. | S–M |

**Order I would take:** 1, 2 and 5 first, as they are cheap and independent. Then 3, since every later window builds on
it. Then 4, the feature most specific to this tool, then 6, which needs 4's structure and much more testing. 7 to 12 as
users ask.
