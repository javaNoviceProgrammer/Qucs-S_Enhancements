# Icons and states for the panels' rows, and the check of 24 October

Three things, in one round: the user asked for an icon on each row of the Workspaces panel and for a
double-click on the workspace already open to show its projects, and the check
`~/Desktop/qucs-reviews/2026-10-24-claude-workspaces-ci-check.md` - which found `e1a7233` and `5c11931`
sound in the built app - left two nits, both about what a reader of the window is told.

## What the user asked for

- **An icon on every workspace row**, as the Projects panel gives a project one. The platform's own
  folder: open for the workspace in use, closed for the others, and greyed where the folder is not there
  now. The green dot still says which is open; the icon gives the row the weight a project's row has.
- **A double-click on the workspace already open shows its projects.** It said "X is the workspace
  already" and stayed where it was. It now shows the Projects page as well - the same move that opening
  the already-open project makes towards Content, so both "it is open already" cases end somewhere
  useful rather than nowhere.

## The check's two nits

| | What it was | What it is now |
|---|---|---|
| 1 | `get_ui dock:Workspaces` and `dock:Projects` named the rows but not which is the open one or which is missing - the dot and the greying are drawn only, so a reader of the window could not see what a viewer sees | Every row carries its state in a word. A plain list answers `"state": {"BenchWork": "open", "Thesis": "closed"}`; a view's row answers `"state": "open"`, `"closed"` or `"missing"` |
| 2 | `open_project` on the project already open answered as for a fresh open ("relative paths are taken from it now"), and **refused** when a document had unsaved changes although nothing would be closed | "X is the project open already: nothing was closed, and the Content panel shows what it holds." The unsaved-changes refusal is skipped for that case, since the documents are not touched; another project is still refused |

The second nit had more to it than the check could see: the unsaved-changes guard was written for an
open that closes every document, and `e1a7233` made one case of `open_project` close nothing. A caller
with an unsaved document asking for the project it already had was turned away for a reason that no
longer applied.

## One role, two panels, one reader

The state the panels draw now has a name: `QucsFileSystemModel::RowStateRole`, holding "open", "closed"
or "missing". The Projects model answers it from the state it already computes (a dangling link is
"missing"), the Workspaces panel sets it on each item beside the dot's role, and `describeControls()`
reads it for a plain list and for a view - so `get_ui` says it for both panels without either of them
knowing about the other. A row that is neither a project nor a workspace (a plain folder, "..") carries
no state, and the key is left out rather than sent empty.

## Tested

| Test | Suite | What it holds to |
|---|---|---|
| `theWorkspacesPanelKeepsItsHistory` | `test_project_folders` | Every row has an icon, and a double-click on the open workspace lands on Projects without switching anything |
| `theOpenProjectHasAGreenDot` | " | The word beside the dot: "open" for the project open (by its folder and through its link), "closed" for the others, nothing for a folder that is no project, and "missing" once the link's project is removed |
| `theOpenRowIsSaidAndTheOpenProjectIsNotOpenedAgain` | `test_qucs_control` | `get_ui` giving the state of each Projects row and each Workspaces row, the open workspace and the one whose folder has gone, and `open_project` on the open project - its words, nothing closed, the Content page, with an unsaved document in front - while another project is still refused |

## Broken on purpose

`docs/feature_gaps/2026-10-24-workspaces-icons-and-states/breaks.py` holds nine: no icon; the
double-click not showing Projects; the model answering nothing for the role; a dangling link called
"closed"; the workspace list setting no state; a gone folder called "closed"; `get_ui` dropping a list's
states; dropping a view's; and `open_project` answering as for a fresh open.

The fourth was not caught on the first pass: nothing asserted "missing" for a **project**, because the
dot test's linked project is a link that works. The test now removes what it leads to and asks for the
word, which is also the case a user meets when a drive is not mounted.

## Not done

- **The same date and the same size** still hide a replacement, as `5c11931` says in its own write-up and
  the check confirms. The check suggests a content hash on a tie; that is the reload rule's own round to
  make, not this one, and it is a deliberate trade there (the tie is the common "nothing changed" case).
- **`get_ui` says the state, not the tooltip.** A reader wanting the sentence ("Linked from X, which is
  not there now") still has to ask the panel for it another way.
- **The icons are the platform's.** Qucs-S does not draw its own folder, so a workspace row looks like a
  folder of the system's look rather than something of this program.
