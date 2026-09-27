# Feature gaps: Content categories of your own, and a ZIP archive editor

*27 September 2026 — Qucs-S 26.1.3.*

Two requests:

1. The Content panel's categories were fixed: Datasets, Data Displays, ...,
   Text, Others, Scratch, each with patterns the user could change
   (Application Settings → Contents). The user wanted **categories of their
   own**, each with its own file patterns, set up in the same tab.
2. Eclipse has a zip file viewer and editor plugin. The user wanted **the
   same for ZIP files in Qucs-S**.

## Categories of your own

| Asked | Now |
|---|---|
| Define new categories | *Application Settings → Contents* has a table, *Your categories*: a name and patterns per row. *Add Category* adds one ("New Category", "New Category 2", ..., its name being edited), *Remove* takes the one selected out, *Move Up* and *Move Down* put them in order. |
| Give them file extension filters | The patterns are those of the built-in categories: `*.s2p`, `.s2p` or `s2p` for an extension, names with wildcards (`touchstone*`), separated by commas or spaces. They are tidied when applied (`s2p .s4p` becomes `*.s2p, *.s4p`). A row without a name is dropped. |
| Where they go | After Text and before Others, in the table's order. The tab shows them there: the built-in categories down to Text, then the table, then Others and Scratch. A file is listed under the first category from the top that matches it, as before, so *Others* still takes whatever no category took. To have a built-in category's files under your own, take the pattern out of the built-in one (both are in the tab). |
| Kept | In the settings (`ContentUserCategories`, an array of names and patterns). They are read at start, and *File → Export Settings* and *Import Settings* carry them like any other setting. *Default Values* in the dialog clears them. |
| In the panel | They are top-level rows like the built-in ones, with the same menus, tree or flat listing, expanded state and automatic refresh. The panel is rebuilt as soon as the dialog applies, like it is after any change of patterns. |

Inside, a user category is `ProjectView::UserCategory + n` (100, 101, ...).
`ProjectView::categories()` gives the panel's order, and `rowOf()` gives a
category's row. `categoryOf()` reads the category a top-level row was built
for (the row keeps it in `CategoryRole`), so it stays right when rows move.
With no user categories, the rows are the `Category` values themselves, as
before.

## A ZIP archive editor

Eclipse's zip editor (the plugin by Uwe Voigt, and the like) shows an
archive as a table or tree of entries with their sizes and packing. It
opens an entry in an editor and puts it back when that editor saves, and
adds, deletes and extracts entries. Qucs-S now opens a `.zip` in a tab of
its own (`zipdoc.h`), from anywhere a file opens: *File → Open*, the Content
panel (where `.zip` falls under Others), the File Browser, a drop, a link.

| Eclipse's zip editor | Qucs-S |
|---|---|
| Entries in a tree or a flat list | A tree of folders (folders first, then files, by name) or a list of every entry by its path: the *Tree* / *List* button. The folders at the top are open when the archive opens. |
| Columns: name, size, packed size, ratio, date, method, CRC | Name, Size, Packed, Ratio (space saved), Modified, Method (Stored, Deflated, "method n", encrypted, or "new or changed"), CRC-32. A folder row shows the sizes of what is in it. An entry's comment is its tooltip. |
| Open an entry in an editor; saved, it goes back into the archive | *Open*, a double-click or Return opens a file in Qucs-S. The file is a copy in a folder of its own that is watched: when its tab saves it, the file is in the archive again, as a step to undo, and the archive is marked changed until it is saved. The status bar says so. Opening it again shows the same copy. |
| Add files and folders; drop them in | *Add Files…* and *Add Folder…* add into the folder selected (or the selected file's folder, or the top). So do files and folders dropped on a row from anywhere, and files copied in a file manager and pasted (*Edit → Paste*). A folder comes with everything in it (links not followed, times and Unix modes kept). A name the archive has already is asked about: *Replace*, *Skip Them* or *Cancel*. |
| New folder, rename, delete | *New Folder* (in the folder selected), *Rename…* (F2: a file, or a folder with all in it) and *Delete* (the Delete key). |
| Extract, and drag entries out | *Extract…* writes the entries selected (all of them when none is) into a folder you choose, and *Extract All…* writes every one. Files there already are asked about: *Overwrite*, or leave them. Times are kept. Entries dragged out of the tab are extracted to a temporary folder and dropped as files. |
| Save | *File → Save* writes the archive (to a temporary name, then renamed over it). Files not changed, and files renamed, are copied as they were packed, byte for byte. New and changed files are deflated, or stored when that is not smaller. The archive's comment, the entries' comments, times and Unix modes are kept. *Save As* writes a copy. |
| — | Undo and redo for every change (*Edit → Undo*, *Redo*). *Edit → Find* goes to a filter by name. Zoom in and out scale the list. The status line counts files and folders, sizes and packing, and says when there are changes to save. |

What it does not do:

- ZIP64 archives (4 GB and more, or more than 65,535 entries) are refused.
- Entries packed in ways other than stored and deflated (bzip2, LZMA, ...)
  are listed and kept as they are when saving, but not opened or extracted.
- Encrypted entries are listed (Method: encrypted), kept as they were packed
  when saving, and not opened.
- TAR, 7z and RAR are not read.

### Safety

- **Nothing in an archive is run.** Only a file Qucs-S opens itself (a
  schematic, a text or a netlist, a spreadsheet, Markdown, a PDF, another
  archive) opens in a tab. Anything else (a `.exe`, a `.command`, an `.app`)
  says to extract it, and is never handed to the system. This follows the
  links rule of bug hunt B2.
- **Nothing is extracted outside the folder chosen.** An entry named
  `../x`, `/x`, `C:/x` or `a/../../x` is refused ("zip slip"), and the reason
  is given.
- **Limits** (bug hunt C2): an archive larger than 1 GB is not opened.
  Nothing is inflated when the archive is opened, only when a file is opened
  or extracted, and each file is inflated no further than its declared size
  and at most 128 MB. Checksums are checked.
- Every change is a step to undo, and nothing reaches the file until it is
  saved. Closing with changes asks, as any document does.

### The ZIP layer

`zipfile.h` can now also:

- **list** an archive's directory without inflating anything
  (`zip::list`, into `Item`: name, method, flags, checksum, sizes, time,
  comment, attributes, where the data is);
- **extract** one file (`zip::extract`);
- **write** from `Part`s, each either data to pack or an item's packed bytes
  to copy as they are (`zip::write(parts, comment)`);
- read the archive's comment (`zip::comment`).

`zip::read` (spreadsheets) is built on these, with its limits unchanged.

## Tests

- `test_content_categories` `categoriesOfYourOwn`: categories are added,
  named, reordered and applied in the dialog, and a nameless one is dropped.
  The test checks where they are listed and which files they take (a `.txt`
  stays under Text), `categoryOf`, what is saved and read back, and what
  happens when they are removed.
- `test_zip_doc`, in ten parts:
  - the ZIP layer: listing, extracting, raw copies that keep times, modes
    and comments;
  - the tab's tree, columns, list and filter;
  - adding with each way of handling a name taken, new folders, renaming
    and deleting, undo, saving (unchanged files byte for byte), Save As;
  - extracting, with zip-slip names refused and existing files left;
  - opening a copy and taking back its saved changes, a program refused,
    an encrypted entry kept;
  - closing with changes not saved (a crash, found by the tests and fixed);
  - 1,500 damaged archives, listed, read and opened without a crash;
  - files dropped from outside.
