# Feature gaps: Copy and Paste in the File Browser

*12 October 2026 — Qucs-S 26.1.8.*

Asked for in conversation: the File Browser's right-click menu had no *Copy* and *Paste*
for files. Files could be copied only by dragging them with the copy key. ⌘C in the
browser went to the window's *Edit → Copy*, which copies the schematic's selection.

## Now

- **Copy:** an entry's menu has *Copy*, or *Copy 3 Items* when several are chosen and
  the one right-clicked is among them. It puts them on the clipboard as files (URLs),
  with their paths as text for programs that take text. The Finder or the Explorer
  pastes them too.
- **Paste:**
  - On a folder, *Paste into “folder”* pastes into it. On a file, *Paste* pastes beside
    it. The empty area's menu pastes into the folder shown.
  - It is enabled only when the clipboard holds files that are still there, whether
    copied here or in the Finder.
  - It copies them as a drop with the copy key does: a name that is taken is asked about
    (*Replace*, *Keep Both*, *Skip*, *Stop*), and a copy into its own folder is
    `name copy`. A folder can't be pasted into itself.
- **Keys:** ⌘C and ⌘V (Ctrl+C and Ctrl+V) in a view of the browser do the same, ahead
  of the window's shortcuts for them, as the views already take their arrow keys.
  - Copy takes the entries chosen.
  - Paste goes, in the Tree, into the folder in focus among those chosen (or the folder
    of the file in focus). In the other views, it goes into the folder shown.
  - The menus show the keys only where the key does the same thing.
- **Edit → Copy and Paste:** while a view of the browser has the focus, they are the
  browser's, as the Finder's Edit menu copies files. This holds even where the menu bar
  takes its shortcuts first. In the browser's filter or path field, they stay the text
  field's.

## Test

In `test_file_browser_drop`:

- **`copiedAndPasted`**:
  - The clipboard holds the files' URLs and their paths as text.
  - Paste into another folder copies a file, and a folder with what is in it; the
    originals stay.
  - Pasting there again asks about the names taken (Keep Both: `one 2.sch`, `a 2`).
    Pasting into their own folder gives `one copy.sch` and `a copy`.
  - The menus:
    - A file has *Copy*, which takes it, and *Paste*.
    - A folder has *Paste into “deep”*, which copies into it.
    - The folder shown has *Paste* and no *Copy*.
    - With text on the clipboard, or a copied file since removed, Paste is disabled and
      pastes nothing.
    - Two entries chosen give *Copy 2 Items*.
- **`copiedAndPastedByKeys`**:
  - In a window whose own actions hold ⌘C and ⌘V (as *Edit → Copy* and *Paste* do), the
    keys in the List view copy the file chosen. Paste goes into the folder shown even
    with a folder chosen.
  - In the Tree, Paste goes into the folder chosen, then into the chosen file's folder.
  - The window's actions never fire. With the focus in the filter, the keys are not the
    browser's.

In `test_file_browser`, **`theApplicationHasIt`**: in the application, with the focus in
the browser, *Edit → Copy* puts the file chosen on the clipboard. *Edit → Paste* pastes
it as `a copy.txt`, and the Paste action is left unchecked (it is the schematic's paste
mode).

## Breaks

Each break was built and the tests run against it. All of them failed:

| Break | Fails at |
|---|---|
| Copied as text only | The clipboard's files, in all three tests |
| The keys not taken from the window | The window's action fired, nothing copied |
| The Tree pastes into the folder shown | Not in the folder chosen |
| Paste always enabled | Enabled with text on the clipboard |
| Files since removed still on the clipboard | Paste offered for a file gone |
| The filter field's focus taken as the browser's | `hasEditFocus` in the filter |
| *Edit → Copy* not the browser's | Nothing on the clipboard |
| *Edit → Paste* not the browser's | No `a copy.txt` |
