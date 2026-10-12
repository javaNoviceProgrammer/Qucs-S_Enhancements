# Feature gaps: the File Browser's view of the disk

*12 October 2026 — Qucs-S 26.1.8.*

Reported in conversation: the File Browser "doesn't seem to automatically refresh file
changes on the disk and might show a stale view". There is no review file for it.

## What was found

The browser is built on Qt's `QFileSystemModel`, which watches the folders it has read
(FSEvents on macOS). A browser on its own, in a test, shows every change within about
half a second: a file made, renamed, changed in size or removed; a folder made; a
change in a folder open in the Tree. It does so after going away and back, under
another tab, from the folder above, and for a folder named through a link (`/var` is
`/private/var` on macOS).

In the application it did not. Entering a folder made a moment before (a new project, a
folder made by a simulation or by Claude, one made in the Finder) left the browser
showing "/", the top of the file system. Nothing in the folder appeared after that,
though the model went on following it. The cause is a race in `QFileSystemModel`. A
listing of the folder above, gathered before the new folder was made, arrives after the
browser has entered it. The model then takes the folder out (and adds it again). The
views' root index went with it and was never set again. The same happens to a folder
removed and made again, for example by a checkout.

The trace, on `f02c77f`:

- 0 ms: `setLocation(inapp)`; the root is valid.
- 3 ms: the model removes the row of `inapp` under the workspace.
- 100 ms: the root is invalid, and "/" is shown from then on.

A new file never appeared. A removal was seen by the model but never shown.

A second gap: a folder deleted while it was shown, with no folder above it watched, stayed
as an empty folder that no longer existed.

## Now

| Case | Before | Now |
|---|---|---|
| The folder shown taken out of the model while it is still there (the race, or removed and made again at once) | "/" shown, never updated | Its root set again once the model is done with its change; its entries and their changes shown |
| The folder shown removed for good | Left showing "/", or an empty folder that does not exist | The nearest folder above it still there, as a tab brought back already found it. It is left once it has been gone half a second, so a folder removed and made again at once (a script, a checkout) is kept. |

The views' root is looked at after each removal, reset or layout change of the browser's
model. When it was lost and the folder is there, it is set again. When the folder is
gone, it is checked every 100 ms and left after 500 ms. A tab brought back to a folder
that is gone uses the same "nearest folder above it" code as before.

## Test

In `test_file_browser`:

- **`changesOnTheDiskAreShown`** (the folder named as it is, and through a link): each
  of the changes above is shown within 5 s, including after going away and back, under
  another tab, and from the folder above.
- **`theFolderShownTakenAwayAndBack`**:
  - In the List, Tree and Details views, the folder shown is made again at the moment
    the model takes it away. It is still the location, its root is valid and its own,
    and a new file in it is shown.
  - Removed, then made again 200 ms after the model saw it go: it is kept.
  - Removed for good, with the folder above watched or not: the folder above is shown,
    and its root is valid.
- **`theApplicationHasIt`**: in the application, a folder made and entered at once
  shows its file, then a new file, then that file's removal.

They pass in Release (three runs in a row) and under ASan/UBSan (two).

On CI's Linux a file's new size was never shown. Qt's watcher there (inotify) watches a
folder for entries made, renamed and removed, not for changes inside a file, so the
Details view's size and date of a file changed by another program stay as they were
until the folder is read again. The test checks a file's new size on macOS and Windows,
whose watchers report it, and not on Linux. The README says so.

## Breaks

Each break was built and the tests run against it. All of them failed:

| Break | Fails at |
|---|---|
| The root not looked at after the model changes | The root invalid after the folder is made again |
| A folder gone not left while its root is valid | The location still the folder removed |
| The root set again, the folder gone | The location still the folder removed |
| No half second of grace | The folder made again 200 ms later was left for the one above |
