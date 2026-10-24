# A Workspaces panel, and the project that is open already

The user, 24 October: "Let's add a new panel right above "Projects" tab and call it "Workspaces" that
keeps a history of all the workspaces that has been opened by qucs. It should have features such as
showing which workspace is currently active similar to how "Projects" panel shows which project is active
and open. Also, looks like if a project is open and I try to open it again, it will closes all the open
tabs and tries to do a fresh open. This behavior doesn't make sense. So add a check such that if a
project is already open, re-opennign it should not do anything and it should jump to the Contents panel."
They also asked whether a VS Code-style remote workspace over SSH is possible; that assessment is at the
end, and nothing of it is built.

## The panel

The left dock's first page now, above Projects: the workspaces this copy of Qucs-S has opened, newest
first, up to twenty. The one open now carries the same green dot the Projects panel gives the open
project - the delegate is the very same class, which moved to `projectstatedelegate.h` so both panels can
draw it - and the others a grey one. A row's tooltip gives the folder's full path and says which it is.

- **New** chooses or creates a folder and makes it the workspace; **Open** is Project > Switch Workspace;
  **Forget** takes the chosen row out of the list and leaves the folder alone.
- A double-click switches. Switching closes the documents the way the menu's Switch Workspace does -
  asking about unsaved ones - and each workspace keeps its own session, so the tabs of the workspace you
  come back to come back with it.
- A right-click gives Open, Open in Finder (Explorer, the File Manager) and Forget, each enabled only
  where it makes sense.
- A workspace whose folder has gone is greyed, says so in its tooltip, and is not opened; Forget is how
  it leaves the list. The workspace open now cannot be forgotten - it would come back at the next
  refresh, so the panel says so instead of appearing to do nothing.
- Two workspaces of the same folder name in different parents show the parent beside the name.
- The history lives in the settings as `RecentWorkspaces`, beside `RecentProjects`, and is state rather
  than a preference (`settingsio.cpp`), so exporting settings does not carry one machine's folders to
  another.
- The workspace a run starts in is folded in at startup, so the first start lists the workspace in use
  even though nothing was switched - a workspace given on the command line too, which is set before the
  window is built.

## The project that is open already

`QucsApp::openProject()` had no notion of the project it already had: it closed every document
(`closeAllFiles()`), opened a fresh untitled tab, re-pointed the work directory, the Content tree, the
file browser and Claude's project folder, and jumped to Content. So a double-click on the open project in
the panel - or "Open with Qucs-S" on its folder - threw away the work in front for nothing.

It now compares the folder with the one open (the canonical path, so a link to it counts) and, when a
project is open and it is that one, shows the Content page, says "%1 is the project open already." and
returns. Another project still closes what is open, as before; the check is tight enough that this is the
only case it changes.

## The dock's pages by name

The panel had to go in **above** Projects, and the pages were jumped to by bare number in five places
(`TabView->setCurrentIndex(0|1|2)`) with a sixth number kept in each saved session. Inserting a page
would have sent every one of those jumps to the wrong panel, and a session written before the change
would have opened the wrong page the next time - the kind of silent slip the panel itself is meant to
avoid.

So the pages have a name and an identity now: `QucsApp::SidePage` with `sidePage()`, `showSidePage()`,
`sidePageName()`, `currentSidePageName()` and `showSidePageNamed()`, each page registered as it is added
(`a_sidePages`), and the five numbers replaced. A session keeps `"side page": "content"` instead of a
number, and a session written before this reads its old number through the order the dock had then -
projects, content, components, libraries, file browser, multiphysics - so an upgrade opens the page the
user left, not its neighbour.

## Tested

| Test | Suite | What it holds to |
|---|---|---|
| `theProjectOpenIsNotOpenedAgain` | `test_project_folders` | Two documents open, the project opened again by the panel: the same tabs, the same one in front, the dock on Content - and another project still closing them |
| `theWorkspacesPanelKeepsItsHistory` | " | Three workspaces switched: the list newest first, the green dot on the one open and grey on the rest, the tooltips, a folder that has gone greyed and not opened, Forget taking a row out while the open one stays, the folder left on disk, and the list written to the settings |
| `theSidePageIsKeptByName` | `test_workspace_session` | A page kept by name, an old session's number read through the old order, a number that meant no page saying nothing, every page named, no two names alike, and the window answering to those names |

## Broken on purpose

`docs/feature_gaps/2026-10-24-workspaces-panel/breaks.py` holds twelve: the already-open check removed,
then loosened to any open project (which would stop another project closing its tabs), then left in place
but not showing Content; the workspace not remembered; the history oldest first; no dot for the one open;
a folder that has gone passed over; the open workspace forgotten; the list not written; the page kept as
a number again; an old number read as the new order; and two pages answering to one name.

Two of them did nothing the first time round, and both were worth knowing:

- **The remembering in `setWorkspace()` was dead code.** `refreshWorkspaces()`, called right below it,
  folds the workspace in itself - that is also how the workspace a run starts in gets into the history -
  so taking the call out changed nothing at all. It is gone, and the break now points at the one place
  that does the work.
- **The open workspace could not really be forgotten anyway.** The row comes back at the next refresh, so
  all the guard does is say so instead of letting the list flicker. The test asks for those words now,
  which is what the guard is actually for.

## What the suites caught

Two things the three new tests did not, both worth having:

- **A run with a workspace of its own was writing it into the user's settings.** `--workspace DIR` (how a
  client of `--mcp-server` starts Qucs-S) exists so that a run keeps its files out of the user's own
  setup, and `test_mcp_server`'s `theWorkspaceIsTheOneGiven` asserts the settings file never names it.
  Remembering every workspace broke that at once. `rememberWorkspace()` now keeps the list in memory - so
  the panel of such a run still shows where it is - and writes nothing when `workspaceOfRun` is set.
  `theWorkspacesPanelKeepsItsHistory` holds to that now too.
- **Two assertions in `test_workspace_projects` read the Projects page by number** ("the Projects tab" was
  index 0) and failed the moment a page went in above it - the very slip the naming was meant to end. They
  ask `currentSidePageName()` for the page now, "wherever it sits".

## Asked about: a remote workspace over SSH

Nothing here is built; this is what the question is worth, in four parts, because "remote workspace" is
four features wearing one name. VS Code splits into a thin UI and a server on the host that does file
I/O, terminals and extensions. Qucs-S is one process in which the GUI, the document model, the simulators
(`QProcess` ngspice/Xyce), the FEM solver (in-process), the file browser (`QFileSystemModel`), the watcher
(`QFileSystemWatcher`) and Claude's tools all reach the disk through `QDir`/`QFile` - `qucsWorkspaceDir`
alone appears 141 times.

1. **A mounted workspace works today, with no code.** SSHFS, NFS or SMB, then point the workspace at the
   mount. The limits are real: `QFileSystemWatcher` gets no events over SSHFS, so a document following
   its file falls back to the 1500 ms re-scan; many small reads over latency make the project scan and the
   library load sluggish; and simulation still runs on this machine, so the server's cores go unused.
2. **Remote simulation is the contained feature with the payoff**: stage the netlist to the host, run
   ngspice or Xyce there, bring the dataset back. A transport, staging, and the simulator launch behind an
   interface - weeks, not months.
3. **A full remote workspace is an architectural project, but the groundwork exists.** `qucs-s
   --mcp-server` is already a headless Qucs-S driven by JSON-RPC over stdio, which is how VS Code's client
   talks to its server. Run it on the host and a remote Qucs-S can already open, edit, simulate and export
   under Claude. What is missing is the GUI as a client of that protocol: the document model, the
   painting, the browser and the watcher would have to stop touching `QDir`/`QFile`. Its own design round.
4. **A devcontainer today** means Qucs-S inside the container with its UI forwarded (X11, waypipe, VNC):
   no code, mediocre interactivity.

Worth doing whichever route is taken: **telling Claude it is remote.** The workspace record would carry a
host and how it is reached, every tool answer naming a path would say which machine it is on, and the
guards would treat a remote path as outside the local workspace until the user allows that host. With a
mounted workspace today the tools work and say nothing of it, which is the same silence the last three
bug-hunt rounds were about.

## Not done

- **No filter on the panel.** Twenty rows at most, so the Projects panel's name filter would be furniture.
- **Nothing reaches into a workspace that is not there.** A greyed row cannot be opened; it is not
  mounted for you, and no attempt is made to guess where it went.
- **The history is this machine's.** It is state, not a preference, so a settings export leaves it behind
  on purpose.
