# Proposal: a built-in web browser

*1 October 2026 - Qucs-S 26.1.5, `f6690a8`. A proposal, not built yet: parked until
the engine is chosen.*

The request is for a nice and powerful web browser inside Qucs-S. In Qucs-S it is
for the web that circuit work goes through:

- datasheets and application notes;
- a vendor's SPICE models;
- the ngspice and Qucs-S manuals;
- forums;
- the pull request the git bar links to;
- the links in Claude's replies.

This is the answer to that, and a plan. It is parked on one decision, the web engine,
which sets how large every bundle is and whether Windows has the browser at all. The
facts below were checked on 1 October 2026.

## What there is already

- **One place where links are followed.** `links.cpp` (`qucs_s::links::follow`)
  decides what a click on a link does. A document Qucs-S opens goes to a tab (`Open`).
  `http`, `https` and `mailto` go to the system's browser (`Outside`), and other
  schemes go there once asked. The Markdown preview, the ZIP archive view and the PDF
  viewer follow their links through it.

  Besides it, a few places call `QDesktopServices::openUrl` with a web address
  directly:
  - the Claude Code panel's replies (`claudecodepanel.cpp`, for `http`, `https` and
    `mailto`);
  - the Help menu's online manuals (`qucs_actions.cpp`);
  - the git bar's pull request (`claudegitbar.cpp`).

  A browser in Qucs-S is reached by changing what `Outside` does, and sending those
  few calls through it. The Help menu's tutorials and reports are local files, and
  stay as they are.
- **Documents of other kinds in tabs.** `PdfDoc` (Qt PDF), `SheetDoc`, `ZipDoc` and
  `MarkdownDoc` are tabs beside the schematics. Each is a `QFrame` (or a `TextDoc`) and
  a `QucsDoc`. They split, close, and come back with the workspace like a schematic. A
  web page is one more.
- **A PDF viewer.** Qt PDF is in every bundle. A datasheet downloaded as a PDF opens
  there, where Claude's `read_pdf` reads it.
- **Claude's tool host.** `QucsControl` serves Qucs-S's tools to Claude (the dock, and
  `qucs-s --mcp-server`). Tools for the browser would be more of them.

## The engine: the decision this is parked on

Qt has two ways to show a web page:

- **Qt WebEngine** is Chromium, inside the application. Its widgets have a full C++
  API: `QWebEngineView`, `QWebEnginePage`, `QWebEngineProfile` and
  `QWebEngineDownloadRequest`.
- **Qt WebView** shows the system's own web view:
  - WKWebView (Safari's engine) on macOS;
  - WebView2 (Edge's) on Windows;
  - Qt WebEngine on Linux, which has none of its own.

  Its API is QML only (the `WebView` type), so a widgets program puts it in a
  `QQuickWidget`.

| | Qt WebEngine (Chromium) | Qt WebView (the system's) |
|---|---|---|
| Find in page, zoom, print and Save as PDF | Yes (`findText`, `setZoomFactor`, `printToPdf`) | Through JavaScript at best; no printing |
| Downloads | Yes, with progress and a place to save | No |
| A link that opens a new window | Yes: a new tab (`createWindow`) | No control |
| Developer tools | Yes (`setDevToolsPage`) | No |
| Profiles, cookies, a private tab | Yes | The system's cookies |
| Size on macOS | `QtWebEngineCore`: 275 MB of code and 68 MB of resources (Homebrew Qt 6.11.2, arm64). About 99 MB compressed (`gzip -6`), so the disk image goes from about 53 MB to about 160 MB with Qt Quick, Qml, WebChannel and Positioning, which it needs | 172 KB, and the macOS backend (`libqtwebview_darwin.dylib`) |
| macOS | CI's Qt (install-qt-action, Qt 6.10.3) has it as a module | Yes (WKWebView) |
| Linux x86_64 and arm64 | The Qt online repository has `qtwebengine` 6.10.3 for both (`linux_arm64` checked) | Renders with Qt WebEngine anyway |
| Windows x64 and arm64 | **Not with our build.** The Windows bundles are built with MSYS2 (`ucrt64`, `clangarm64`), and MSYS2 has no `qt6-webengine`: Chromium is built with MSVC. The Qt online repository's `qtwebengine` 6.10.3 for `windows_x86` is for MSVC builds only | MSYS2 has `qt6-webview` 6.11.2 for `ucrt64` and `clangarm64`. Its file list shows the QML plugin; whether the WebView2 backend is in it is not yet verified |

That gives three ways to go:

- **A. Chromium on macOS and Linux.**
  - A CMake option, `QUCS_WEB_BROWSER`, is on when Qt WebEngine is found.
  - The macOS and Linux bundles have the browser.
  - The Windows bundles build as now, and their links go to the system's browser as
    today, until the Windows build moves to MSVC.
- **B. Chromium everywhere.** First the Windows build moves from MSYS2 and MinGW to
  MSVC. That move covers:
  - `qucs-s` and the tool programs;
  - ADMS with its flex and bison;
  - the bundle script's `windeployqt` and `ldd` checks.

  It is a project of its own, and Windows builds may break while it is done.
- **C. The system's engines.** The bundles barely grow, but the browser has none of
  the left column's tools. On Linux it is Chromium all the same.

**The recommendation is A, with B as a later step.** It is the only one that gives a
powerful browser without first rebuilding the Windows toolchain. The code is the same
for B: only the Windows build changes.

## The browser (on Chromium)

**A tab, `WebDoc`,** a `QFrame` and a `QucsDoc` as `PdfDoc` is. A page sits with the
schematics, in a split beside one, and comes back with the workspace.

**Its bar:**
- back, forward, and reload (or stop while loading);
- the address bar: a URL, or words searched with the search engine the settings
  name. It shows the site's padlock, and completes from history and bookmarks if
  those are chosen;
- zoom;
- a menu.

**What it does:**
- **Tabs.** A link that opens a new window opens a new tab (`createWindow`), and so do
  Ctrl+click and a middle click.
- **Find in page** (Ctrl+F), with the text editor's find bar.
- **Zoom** (Ctrl+plus, Ctrl+minus, Ctrl+0), kept for each site.
- **Print, and Save as PDF.** View Source.
- **Downloads.** A bar with each download's progress, Stop, and Show in Folder. Files
  go to the downloads folder; with the extra below, to the project. A PDF opens in
  Qucs-S's PDF viewer.
- **Permissions.** Camera, location and notifications are asked for in a bar, and
  refused unless allowed.
- **A profile of its own.** Cookies and the cache are kept in Qucs-S's cache directory.
  A Private Tab uses a profile that keeps nothing.
- **Web links in Qucs-S open in it.** Application Settings has "Open web links in
  Qucs-S's browser", on by default where the browser is built. Shift+click, or the
  setting off, sends a link to the system's browser.

**What it does not do:**
- A page runs in Chromium's sandboxed renderer (`QtWebEngineProcess`).
- A local page cannot read other local files (`LocalContentCanAccessFileUrls` off).
- No Qucs-S object is offered to a page's JavaScript (no `QWebChannel`).

## Extras, to choose

1. **Claude can use it.** Three tools:
   - `browse` opens a URL in a tab. The permission prompt names the site, as any
     change Claude asks to make.
   - `read_page` gives the text of the page in front, or of a tab, with its links.
   - `list_tabs` lists the tabs open.

   They read only: no clicks, no typing into forms. Claude Code's WebFetch already
   reads public pages. These add the page the user is looking at: one behind a sign-in,
   a vendor's part selector, the forum thread open now. A page's text can carry
   instructions aimed at Claude, so the answer marks it as the page's text, and the
   server's instructions say to treat it as data.
2. **Downloads into the project.** Save to Project puts a download in the open project,
   where the Content panel lists it under its category:
   - a SPICE model (`.lib`, `.cir`, `.mod`, `.sub`);
   - a Touchstone file;
   - a datasheet.
3. **Bookmarks and history.** Kept in the settings directory, and in Export Settings.
   The address bar completes from both, and the history can be cleared.
4. **Developer tools.** Chromium's inspector in a pane beside the page (F12).

## Questions to decide

1. **The engine:** A, B or C above.
2. **The extras:** any of the four.
3. **Where web links open:** in Qucs-S's browser by default, or only when asked.
4. **The search engine** the address bar uses by default.
5. **A start page:** the recent pages and bookmarks, or the Qucs-S manual.

## The work

1. **CMake.** `find_package(Qt6 OPTIONAL_COMPONENTS WebEngineWidgets)`, and
   `QUCS_WEB_BROWSER` defined when it is found. Without it nothing changes.
2. **`WebDoc`:**
   - its bar;
   - tabs from new windows;
   - find, zoom, print;
   - downloads;
   - permissions;
   - the profiles.
3. **Links.** `links.cpp`'s `Outside` and the direct `openUrl` calls go to a tab, with
   the setting.
4. **The extras** chosen.
5. **Packaging.**
   - **macOS.** `macdeployqt` carries Qt WebEngine: its helper
     (`QtWebEngineProcess.app`), its resources and locales. The ad-hoc signing covers
     the helper.
   - **Linux.** `linuxdeploy-plugin-qt` carries the same into the AppImage. Ubuntu 24.04
     restricts unprivileged user namespaces, which Chromium's sandbox uses. In an
     AppImage the sandbox may have to be turned off (`QTWEBENGINE_DISABLE_SANDBOX`):
     to be checked on 24.04.
6. **CI.** On macOS and Linux, install-qt-action's modules gain `qtwebengine`,
   `qtwebchannel` and `qtpositioning`, and the bundle checks look for the helper.
   Windows is unchanged (A).

## How it would be tested

**`test_web_browser`,** against pages a local server in the test serves (a
`QTcpServer`), never the network:
- a page loads, and back and forward go through the history;
- find in page finds, zoom zooms;
- a `target="_blank"` link opens a tab;
- a download arrives with its progress, and Stop leaves no file;
- the setting sends a link to a tab or to the system;
- tabs come back with the workspace;
- a private tab keeps no cookie.

Qt WebEngine under the offscreen platform the tests use needs software rendering
(`--disable-gpu`): to be checked first.

**Claude's tools,** if chosen: `browse` asks before going to a site, and `read_page`
reads the local page's text and marks it as such.

**Then the usual:** the whole suite, the ASan build, the bundles' size checked, and a
DMG.

## What it costs

- **Download size.**
  - The macOS disk image goes from about 53 MB to about 160 MB.
  - Each Linux AppImage grows by about as much.
  - The Windows bundles are unchanged (A).
- **CI.** Each Qt download is larger. Chromium is not compiled: Qt's builds are used.
- **Memory.** Each page has a renderer process, typically 100 to 300 MB.
