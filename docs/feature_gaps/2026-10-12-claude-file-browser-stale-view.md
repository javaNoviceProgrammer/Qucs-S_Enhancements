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

A second cause, found when CI's macOS runner failed an early version of the test: a
folder removed while it was shown kept its entries. The watcher reports the removal, and
`QFileInfoGatherer` lists the folder again. The folder is gone, so the listing is empty,
and an empty listing is never sent (`if (!allFiles.isEmpty()) emit newListOfFiles(...)`).
Only `directoryLoaded` comes. The model is told nothing, and the browser went on showing
the folder's old files, a folder that no longer existed. A folder that is emptied but
still there does report, since its listing holds `.` and `..`.

Qt's watcher itself is not at fault. A small program watched a folder through
`QFileSystemWatcher` (FSEvents), removed it, made it again and watched it again. Each
step was reported, and the removed path was dropped from its list.

## Now

| Case | Before | Now |
|---|---|---|
| The folder shown taken out of the model while it is still there (the race) | "/" shown, never updated | Its root set again; its entries and their changes shown |
| The folder shown removed | Its old entries shown, or "/", never updated | Left at once for the nearest folder above it still there, as a tab brought back already found it |
| Removed and made again (a script, a checkout) | As above | Left for the folder above, then gone back into once it is there again, if that is within two seconds and the folder above is still the one shown |

The browser looks at its folder each time the model ends a listing. That happens after
every change the watcher reports, including the empty listing of a folder that is gone.
- If the views lost their root and the folder is there, the root is set again.
- If the folder is gone, the browser shows the nearest folder above it.
- For two seconds after that it checks every 100 ms, and goes back into the folder if it
  is there again. Choosing another folder in the meantime ends the wait.

An earlier version waited half a second before leaving a folder found gone, so that one
made again at once would be kept. It failed on CI's macOS runner. The wait could start
from an earlier change of the model's than the test's 200 ms counted from, and in the
meantime the views showed the old entries. Leaving at once and going back is shown in
the right place throughout and does not depend on that timing. Locally the earlier
version passed 24 runs out of 24, eight at a time; only CI's runner showed the failure.

## Test

In `test_file_browser`:

- **`changesOnTheDiskAreShown`** (the folder named as it is, and through a link): each
  of the changes above is shown within 5 s, including after going away and back, under
  another tab, and from the folder above.
- **`theFolderShownTakenAwayAndBack`**:
  - In the List, Tree and Details views, the folder shown is made again at the moment
    the model takes it away. It is the location again, its root is valid and its own,
    and a new file in it is shown.
  - Removed, then made again 200 ms after the model saw it go: the browser is back in
    it, and its file is shown.
  - Removed again, and the workspace chosen meanwhile: the workspace stays when the
    folder is made again.
  - Removed for good, with the folder above watched or not: the folder above is shown,
    and its root is valid. When the folder is not made again, the browser stays there,
    even when the folder is made again after the two seconds.
- **`theApplicationHasIt`**: in the application, a folder made and entered at once
  shows its file, then a new file, then that file's removal.

They pass in Release (three runs in a row, and 16 of 16 eight at a time) and under
ASan/UBSan.

On CI's Linux a file's new size was never shown. Qt's watcher there (inotify) watches a
folder for entries made, renamed and removed, not for changes inside a file, so the
Details view's size and date of a file changed by another program stay as they were
until the folder is read again. The test checks a file's new size on macOS and Windows,
whose watchers report it, and not on Linux. The README says so.

## Breaks

Each break was built and the tests run against it. All of them failed:

| Break | Fails at |
|---|---|
| The folder not looked at when a listing ends | The root invalid after the folder is made again |
| The root not set again, the folder there | The root invalid |
| A folder gone not left | The location still the folder removed |
| Never gone back into | The location the folder above after the folder was made again |
| Gone back into after another folder was chosen | The location the folder made again, not the workspace |
| Gone back into at any time | The location the folder made again after two seconds |
