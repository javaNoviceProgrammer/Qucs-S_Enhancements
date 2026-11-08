# Bug hunt: the layout tab's crash at closing

*7 November 2026 - Qucs-S 27.0.7 (`f3267f7`), the user's crash at 21:44; reproduced and fixed on
`0e08010` (the Release build, and the ASan/UBSan build where said). Everything found is fixed in the
commit that adds this report, each fix with its test and broken in turn.*

The user's request: "qucs crashed on me. Investigate in a new session:
'qucs-reviews/Crashes/qucs_crash_Nov-7-2026.txt'".

## Summary

| | Finding | Since | Fixed |
|---|---|---|---|
| **F1** | Closing a layout's tab while its layer list's menu was up killed Qucs-S: "pointer being freed was not allocated" (the user's crash); the canvas's menu the same | `b1aa6cc` (6 October, the viewer's first commit) | the menus shown with `popup()`, freed when they close |
| **F2** | The same in the picture viewer, the PDF viewer, the presentation viewer and the multiphysics view: a `QMenu` on the stack, shown with `exec()` | each viewer's first commit | the same |
| **F3** | The XML tree's menu and the git History's two menus: freed with their tab or window inside `exec()`, then `deleteLater()` called on the freed menu (a crash as the deferred delete is delivered) | each one's first commit | the same |

Not the metal router (`f3267f7`) or the pen markers (`871750f`): the layer menu was a month old, and
the crash needs only the menu up when the tab closes.

## The report, symbolicated

The report's `qucs-s` is `/Applications/qucs-s.app` 27.0.7, binary UUID `ed838473-…`, the build of
`bin/macos/apple-silicon/qucs-s-27.0.7-f3267f78-macos-apple-silicon.dmg`. Its unnamed frames (`atos`
names none: they are lambdas and static functions) found with `nm -n` and `otool -tV` on that
binary, loaded at `0x100f60000`:

```
Thread 0 Crashed (SIGABRT)
 2  libsystem_c        abort
 3  qucs-s             qucs_s::crash's signal handler (crashhandler.cpp, a static function)
 5  libsystem_platform _sigtramp
 7  libsystem_c        abort
10  libsystem_malloc   ___BUG_IN_CLIENT_OF_LIBMALLOC_POINTER_BEING_FREED_WAS_NOT_ALLOCATED
11  QtCore             QObjectPrivate::deleteChildren()
12  QtWidgets          QWidget::~QWidget()
13  qucs-s             non-virtual thunk to LayoutDoc::~LayoutDoc()
14  qucs-s             QucsApp::closeFile(int)                    qucs.cpp:3654  delete Doc
15  qucs-s             QucsApp::slotFileClose(int)
17  QtWidgets          QTabWidget::tabCloseRequested
19  QtWidgets          QTabBarPrivate::closeTab()                 the tab's close button
23  QtWidgets          QAbstractButton::mouseReleaseEvent
 …                     (an event loop)
52  QtWidgets          QMenuPrivate::exec
53  QtWidgets          QMenu::exec(QPoint const&, QAction*)
54  qucs-s             LayoutDoc::buildUi()'s lambda for the layer list's
                       customContextMenuRequested                 layoutdoc.cpp:2018 menu.exec(...)
56  QtWidgets          QWidget::event                             (Qt::CustomContextMenu)
58  QtWidgets          QAbstractItemView::viewportEvent
59  QtWidgets          QTreeView::viewportEvent
64  QtWidgets          QWidgetWindow::handleContextMenuEvent      a right click on the layer list
 …
94  qucs-s             main
```

Frame 54 is the function at `0x1001a0xxx` whose strings are "Show All", "Hide All", "Show Only This",
"Colour…", "Load Layer Properties…" and "Colours of the Palette", with `QTreeWidget::itemAt`,
`QMenu::QMenu(QWidget*)` and `QMenu::exec` among its calls: the layer list's menu. The other threads
are Qt's file-info gatherers and an idle thread pool, nothing of the layout's.

## F1-F3: a menu freed by the tab it belongs to

The layer list's menu was

```cpp
connect(a_layers, &QTreeWidget::customContextMenuRequested, this, [this](QPoint at) {
    QMenu menu(this);              // on this lambda's stack, a child of the LayoutDoc
    menu.addAction(...);
    menu.exec(a_layers->viewport()->mapToGlobal(at));
});
```

`exec()` runs an event loop of its own inside the lambda. On macOS a click outside a menu closes it
and goes on to what it lands on. The user's click on the tab's close button went on in that loop:
`closeFile` deleted the `LayoutDoc`, `~QWidget` deleted its children, and one of them was the menu,
on the stack of the lambda still waiting in `exec()`. libmalloc was asked to free a stack address and
aborted. The same happens when Claude's `close_document` arrives while the menu is up: the tools
refuse to run while a modal dialog waits, but a menu is not modal.

Where the menu is on the heap and freed with `deleteLater()` after `exec()` (the XML tree, the
History), the tab or window takes it along in the loop, and `deleteLater()` then runs on freed
memory: the deferred delete it posts is delivered to a dead object later (ASan: SEGV in
`QApplicationPrivate::notify_helper` from `sendPostedEvents`).

The same pattern, and the owner that can go:

| where | menu | its owner goes when |
|---|---|---|
| layout: the layer list, the canvas | stack | the tab closes |
| picture viewer, PDF viewer, presentation viewer | stack | the tab closes |
| multiphysics view (inside the 2D and 3D models' tabs) | stack | the tab closes |
| XML tree | heap, `deleteLater()` after `exec()` | the tab closes |
| git History: commits, files | heap, `deleteLater()` after `exec()` | its window or dock is closed |

Hunt 2026-10-23 A6 was the same crash with a dialog in place of a menu: there, `closeAllFiles` defers
freeing a document that is inside its own dialog (`QucsDoc::InADialog`). A menu needs no deferring.

## The fix

`qucs/popupmenu.h`: `qucs_s::popUp(menu, at)` sets `Qt::WA_DeleteOnClose` and calls `popup()`. The
menu is made with `new QMenu(this)`. The slot returns at once, so nothing waits under it. The menu is
an ordinary child of its tab: freed with the tab if the tab goes first, otherwise freed when it closes
(an entry chosen, Escape, a click elsewhere). The eight menus above use it. None of them read what
`exec()` returned: each entry's work is in its own slot. `QucsApp::closeFile` is unchanged.

Left as they are:
- the multiphysics, 3D and RF 2.5D panels' tree menus, and the File Browser's and the Claude dock's
  menus: their owners are docks, which a tab closing does not free;
- the Python dock's two menus, which read `exec()`'s answer: a dock too.

## How it was proved

- `test_layout_doc` `aTabClosedWithItsMenuUpTakesTheMenuWithIt`: a layout open, the Route panel shown,
  a pen stroke drawn, then the layer list's menu (and then the canvas's) asked for. Once the menu is
  up, the tab's close button is clicked, or `close_document` is called, or Close All (as quitting
  closes every document: `closeAllFiles` deletes a document as `closeFile` does). The tab and its menu must be
  gone and no menu left up. Then "Hide All" is clicked in the menu (every layer unticked, the menu
  freed) and the menu is put away with Escape (freed).
- The same check, in `tests/menu_owner_close.h`, for each of the other menus:
  `test_image_doc`, `test_pdf_viewer`, `test_pptx_doc`, `test_xml_doc` (each
  `aTabClosedWithItsMenuUpTakesTheMenuWithIt`), `test_git_integration`
  `theHistoryClosedWithItsMenuUpTakesTheMenuWithIt` and `test_multiphysics_view`
  `theViewFreedWithItsMenuUpTakesTheMenuWithIt`.
- Before the fix, the layout's test aborted in Release with the user's message ("pointer being freed
  was not allocated" at a stack address) and the user's frames 10-15. Under ASan it reported
  `attempting free on address which was not malloc()-ed` in `QObjectPrivate::deleteChildren` under
  `~LayoutDoc` under `closeFile`, the address "located in stack of thread T0" in the frame of
  `LayoutDoc::buildUi()::$_34`, at `menu.exec(...)`.
- [breaks.py](2026-11-07-layout-close-crash/breaks.py) puts each of the eight menus back as it was,
  builds, and runs its test offscreen, in ctest's environment. On the ASan/UBSan build, 8 of 8
  were caught: the six stack menus as `attempting free on address which was not malloc()-ed`, the two
  heap ones as the SEGV above.
- After the fix, through ctest (offscreen), the seven programs pass whole in Release. Under
  ASan/UBSan, `test_layout_doc`, `test_image_doc`, `test_pdf_viewer`, `test_pptx_doc`, `test_xml_doc`
  and `test_multiphysics_view` pass whole. `test_git_integration` passes whole too, but not every
  time: in about half its runs a `git` the test itself starts dies of signal 9 (`QProcess::Crashed`,
  exit 9), in `aCommitWithAPathOutsideCommitsNothing`, `claudesGitTools` or
  `pullOfBranchesThatDiverged`. It does so with the new function's body emptied as well, and before
  this change (`119aec3`), and each of those functions passes alone. It is the suite's own, not this
  fix's, and it is not looked into here. The four lints are green (158 tools).

Not verified: Qt 6.10.3 (CI's), Windows and Linux, a person's hand on the real window (the tests click
the close button with `QAbstractButton::click`, not a pointer through a native menu), a native file
dialog's loop. The router panel's own dialogs (Technology, Nets, Commit) and the layout's Load Layer
Properties do not hold `InADialog`, and `closeFile` does not ask `inADialog()` (only `closeAllFiles`
does). Being modal, they are safe from the user's clicks and from the tools' calls, but not from
anything else that closes a tab while one is up.
