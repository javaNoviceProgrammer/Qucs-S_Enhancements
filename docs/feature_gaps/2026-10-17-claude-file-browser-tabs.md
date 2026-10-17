# The Content panel to the File Browser, and no folder in two tabs

The request (2026-10-17): *"Add a new right click menu item for the context menu that is showed when right clicked on an empty area called "Open in File Browser" to the Contents panel so that I can quickly jump to the file browser to see the actual file and folder structure of the project. It should open the project folder in a new tab in the file browser. If the tab was previously opened, it should just jump to that tab and not create a new tab. Similarly, the "open in new tab" menu item in the File browser should not create new tab for a folder if it was previously opened in a tab, it should just jump to the existing open tab."*

## Now

- **The Content panel's empty area: Open in File Browser,** the menu's first item.
  - It shows the open project's folder in a File Browser tab of its own, and brings the File Browser to the front of the left dock (showing the dock if it was hidden).
  - Asked again, it goes back to the tab that shows the folder, wherever the File Browser is meanwhile; no second tab is made.
  - With no project open, it cannot be chosen.
- **The File Browser does not open a folder in a second tab.** If a tab already shows the folder, that tab comes to the front, for:
  - Open in New Tab (a folder's menu);
  - Show in a New Tab (a document's, in the Recent view);
  - Command with the double-click (Ctrl elsewhere).

  For a document, its folder's tab comes to the front with the document chosen in it.
- **A middle click,** which opens a folder in a tab behind, makes no second tab either. The tab it finds stays behind, with a document chosen in it for when it comes to the front.
- **What counts as showing the folder** (`FileBrowser::tabShowing`):
  - a tab whose folder is that folder on the disk, so a path through a link (/tmp, a linked home) is the folder it names;
  - not a tab in the Recent view, which shows the documents opened last, not its folder.
- **The Options menu's New Tab still opens the folder shown in another tab,** as Finder's New Tab does: it is asked for.

## Tests

- **test_file_browser_tabs `aFolderInATabAlreadyIsJumpedTo` (new):**
  - Open in New Tab twice: one tab, brought back;
  - Command with the double-click, the same;
  - a middle click on a folder with a tab: none made, the tab in front kept;
  - on another folder: a tab behind, as before;
  - a document: its folder's tab, the document chosen there, in front and behind;
  - a path through a link: the same tab;
  - a tab in the Recent view: a new tab made;
  - New Tab: a second tab, as asked.
- **test_project_folders `theContentPanelOpensTheProjectInTheFileBrowser` (new):**
  - the item first in the empty area's menu, and not to be chosen without a project;
  - with one, the File Browser in front at the project's folder;
  - elsewhere in the File Browser, the Content panel in front, asked again: back to that tab, none made;
  - closed: not to be chosen.

  On a Mac's display the menu shown is the platform's own, not a popup of Qt's, so the test finds it through the item's menu.

Eleven changes broken on purpose, all caught:

- no tab ever showing a folder;
- Open in New Tab, the middle click, or Command with the double-click making a new tab again (one break each);
- a Recent tab taken as its folder;
- folders compared as written, not as on the disk;
- a document not chosen in the tab found;
- a tab found brought to the front behind too;
- the item open without a project;
- the File Browser not brought to the front;
- the item not in the menu.

## Runs

- **The Release suite (offscreen):** 127 of 127, after the rebuild that followed the breaks.
- **Both tests on the display and offscreen:** pass.
- **Under the sanitizers:** both tests. No report.
- **The lints pass.**

**Not done:** Claude's `context_menu` tool reaches the Content panel's rows, not its empty area.
