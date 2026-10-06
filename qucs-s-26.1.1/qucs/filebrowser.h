/*
 * filebrowser.h - the File Browser panel: the file system in the views a
 *                 file manager has, with icons for the files Qucs-S knows
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef QUCS_FILEBROWSER_H
#define QUCS_FILEBROWSER_H

#include <QAbstractFileIconProvider>
#include <QColor>
#include <QStringList>
#include <QPointer>
#include <QWidget>

#include <functional>

class QAbstractItemView;
class QAction;
class QActionGroup;
class QColumnView;
class QFileInfo;
class QFileSystemModel;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListView;
class QMenu;
class QModelIndex;
class QStackedWidget;
class QStandardItemModel;
class QTimer;
class QToolButton;
class QTreeView;

namespace qucs_s::files {

class SortProxy;

/// What a file is, as Qucs-S sees it: what it is called ("Qucs schematic"),
/// the tag on its icon ("SCH"), the icon's colour and what is drawn on it.
struct Kind {
    enum Glyph { Page, Text, Schematic, Symbol, Plot, Table, Code, Image, Archive, Layout, Folder, Project };
    QString name;
    QString tag;
    QColor colour;
    Glyph glyph = Page;
    /// A file Qucs-S makes or reads itself: a schematic, a dataset, a
    /// netlist, an HDL or Verilog-A source, S-parameters, a script.
    bool qucs = false;
};
Kind kindOf(const QFileInfo& info);

/// The icons of the File Browser: files by kind - a page with its tag on a
/// band of its colour and, large enough, a drawing of what it holds - and
/// folders, a Qucs-S project's with a badge. Drawn at any size, for a light
/// or a dark theme; the names of the kinds are the views' "Kind".
class IconProvider : public QAbstractFileIconProvider
{
public:
    QIcon icon(IconType type) const override;
    QIcon icon(const QFileInfo& info) const override;
    QString type(const QFileInfo& info) const override;
    /// The icon of \a kind.
    static QIcon iconFor(const Kind& kind);
};

/// A line that filters by name, the File Browser's and the Content panel's:
/// "Filter by name", a magnifier before it, a button that clears it.
QLineEdit* nameFilterEdit(QWidget* parent);

} // namespace qucs_s::files

/*!
 * The File Browser: the file system, in the view the user chooses -
 *   Tree     folders that open in place, from the folder shown;
 *   List     the folder's entries, a folder entered with a double-click;
 *   Icons    the same as large icons;
 *   Details  the same with size, kind and date, sorted by any of them;
 *   Columns  a column for each folder on the way, as Finder's;
 *   Recent   the documents opened last.
 * Folders come first, names in natural order (R2 before R10). At the top,
 * back, forward, up and places (the workspace, the open project, home,
 * the examples, the volumes), the view and a menu; under them the path as
 * buttons (a click goes there; a click beside them or Ctrl+L types one),
 * and a filter. A double-click on a file opens it (openRequested: Qucs-S
 * opens it as the Content panel does); its context menu opens it with the
 * system, shows it in the file manager, copies its path, renames it, makes
 * a folder, moves it to the trash. Entries are dragged: onto a folder (a
 * row, the folder shown, a button of the path) they move there - copied
 * with Option (Ctrl elsewhere), or to another disk; onto the document area
 * they open; from the Finder or the Explorer they are copied in. A folder
 * held under a drag opens. The folder, the view and the options are kept
 * for the next start.
 */
class FileBrowser : public QWidget
{
    Q_OBJECT

public:
    enum class View { Tree, List, Icons, Details, Columns, Recent };

    explicit FileBrowser(QWidget* parent = nullptr);
    ~FileBrowser() override;

    /// The folder shown.
    QString location() const { return a_location; }
    /// Shows \a path (a folder; a file's folder, with it selected), a step
    /// back remembered.
    void setLocation(const QString& path);
    View view() const { return a_view; }
    void setView(View view);

    /// Where Home goes - the workspace - and the folder shown when there is
    /// none to show yet.
    void setHomePath(const QString& path);
    QString homePath() const { return a_home; }
    /// The open project's folder, a place (empty: none open).
    void setProjectPath(const QString& path);
    /// The documents opened last, the latest first (the Recent view).
    void setRecentFiles(const QStringList& files);
    /// What names the document in front (for "Show the Document in Front").
    void setDocumentProvider(std::function<QString()> provider);
    /// An open document: its file, and whether it has unsaved changes.
    struct OpenDocument {
        QString path;
        bool modified = false;
    };
    /// What documents are open: Move to Trash closes the ones in what it
    /// trashes (trashed()), and refuses while one has unsaved changes.
    void setOpenDocumentsProvider(std::function<QList<OpenDocument>()> provider);

    bool showHidden() const { return a_showHidden; }
    void setShowHidden(bool on);
    /// The folders' kinds (and icons) looked at again: which are projects
    /// changed (QucsSettings.AnyFolderIsProject).
    void refreshKinds();
    /// Only the files Qucs-S makes or reads (folders stay).
    bool qucsFilesOnly() const { return a_qucsOnly; }
    void setQucsFilesOnly(bool on);
    /// Only the entries whose names hold \a text (in the Tree, files only).
    void setFilterText(const QString& text);

    bool canGoBack() const { return !a_back.isEmpty(); }
    bool canGoForward() const { return !a_forward.isEmpty(); }
    bool canGoUp() const;

    /// What a double-click on \a path does: a folder is entered (in the Tree
    /// and Columns views it opens in place), a file opened.
    void activate(const QString& path);
    /// Makes a folder in \a parent ("New Folder", "New Folder 2", ...) and
    /// returns its path; empty when it cannot.
    QString createFolder(const QString& parent);
    /// The entry selected in the view, or empty.
    QString selectedPath() const;
    void selectPath(const QString& path);

    /// Moves (\a action Qt::MoveAction) or copies \a sources into the
    /// folder \a target, as a drop does: a name the folder has already is
    /// asked about (replace it - the one there goes to the trash -, keep
    /// both, skip); an entry copied into its own folder becomes "name
    /// copy". Returns where they went. Open documents moved follow (moved()).
    QStringList transfer(const QStringList& sources, const QString& target, Qt::DropAction action);
    /// Renames \a path to \a name in its folder, as File > Rename does. A
    /// name, not a path: "/" (and "\\" on Windows), "." and ".." are
    /// refused; another case of the name it has is a rename (the same file
    /// on macOS and Windows). Open documents follow (moved()). Returns why
    /// it could not, or empty.
    QString renameEntry(const QString& path, const QString& name);
    /// Moves \a path to the trash, once asked; the open documents in it
    /// close (trashed()). Refused while one of them has unsaved changes.
    void moveToTrash(const QString& path);
    /// Why \a sources cannot go into \a target (a folder into itself, the
    /// workspace or the open project moved, all there already), or empty.
    QString refusal(const QStringList& sources, const QString& target, Qt::DropAction action) const;
    /// What a drop does: copied with the copy key (Option on macOS, Ctrl
    /// elsewhere), moved with the move key (Command, Shift); else moved
    /// when dragged within the browser on one disk, copied otherwise.
    static Qt::DropAction dropAction(bool fromBrowser, Qt::KeyboardModifiers modifiers, const QStringList& sources,
                                     const QString& target);
    /// The folder a drop at \a pos of \a view's viewport goes into: the
    /// folder under it, else the folder \a view shows (a file's folder in
    /// the tree); \a area is what to light up.
    QString dropTarget(QAbstractItemView* view, const QPoint& pos, QRect* area = nullptr) const;

    // For the tests.
    QAbstractItemView* currentView() const;
    QFileSystemModel* fileModel() const { return a_model; }
    /// The names the view shows under the folder shown, in order (the
    /// Recent view: the files' names).
    QStringList shownNames() const;
    /// The path of an index of the current view.
    QString pathOf(const QModelIndex& index) const;
    /// The index of \a path in the current view (the file system's views).
    QModelIndex indexOf(const QString& path) const;
    QList<QToolButton*> crumbs() const { return a_crumbButtons; }
    QWidget* dropHighlight() const { return a_dropHighlight; }
    QLineEdit* filterEdit() const { return a_filter; }
    QLabel* statusLabel() const { return a_status; }
    QMenu* contextMenuFor(const QString& path);

public slots:
    void back();
    void forward();
    void up();
    void home();
    /// Types a path in place of the path's buttons (Ctrl+Shift+G).
    void editPath();

signals:
    /// A file was double-clicked (or opened from its menu).
    void openRequested(const QString& path);
    /// "New Zip…": a new, empty archive asked for, to be saved in \a folder.
    void newArchiveRequested(const QString& folder);
    /// Entries moved or renamed: \a from[i] is \a to[i] now (a folder with
    /// all in it).
    void moved(const QStringList& from, const QStringList& to);
    /// \a path went to the trash with the open \a documents in it (their
    /// names as the provider gave them; none has unsaved changes).
    void trashed(const QString& path, const QStringList& documents);
    void locationChanged(const QString& path);

protected:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void buildToolbar();
    void buildViews();
    void go(const QString& path, bool remember);
    void applyRoot();
    void rebuildCrumbs();
    void layoutCrumbs();
    void updateButtons();
    void updateStatus();
    void refilter();
    void restyle();
    void save() const;
    void fillPlaces(QMenu* menu);
    void fillRecent();
    void onActivated(const QModelIndex& index);
    void fillPreview(const QModelIndex& index);
    /// The Details view's name column: what the others leave of the width.
    void fitDetails();
    void showContextMenu(QAbstractItemView* view, const QPoint& pos);
    void rename(const QString& path);
    QAbstractItemView* viewFor(View view) const;
    /// A drag over \a watched (a view's viewport, a button of the path):
    /// lit up and accepted where it can drop; dropped.
    bool dragEvent(QWidget* watched, QEvent* event);
    void showDropHighlight(QWidget* on, const QRect& area);
    void endDrag();
    /// The folder held under a drag opens.
    void springOpen();
    /// Whether \a path is in what the view shows (under the folder shown).
    bool inView(const QString& path) const;

    QString a_location;
    QString a_home;
    QString a_project;
    QStringList a_recentFiles;
    std::function<QString()> a_document;
    std::function<QList<OpenDocument>()> a_openDocuments;
    QStringList a_back;
    QStringList a_forward;
    View a_view = View::List;
    View a_fileView = View::List;   // the last view of the file system
    bool a_loaded = false;          // the settings read (then kept)
    bool a_columnsPending = false;  // the Columns view given a folder still loading
    bool a_showHidden = false;
    bool a_qucsOnly = false;
    QString a_selected;   // kept across the views

    qucs_s::files::IconProvider* a_icons = nullptr;
    QFileSystemModel* a_model = nullptr;
    qucs_s::files::SortProxy* a_proxy = nullptr;
    QStandardItemModel* a_recentModel = nullptr;

    // The top.
    QToolButton* a_backButton = nullptr;
    QToolButton* a_forwardButton = nullptr;
    QToolButton* a_upButton = nullptr;
    QToolButton* a_homeButton = nullptr;
    QToolButton* a_placesButton = nullptr;
    QToolButton* a_viewButton = nullptr;
    QToolButton* a_menuButton = nullptr;
    QMenu* a_viewMenu = nullptr;
    QActionGroup* a_viewActions = nullptr;
    QAction* a_hiddenAction = nullptr;
    QAction* a_qucsAction = nullptr;
    QAction* a_documentAction = nullptr;
    // The path.
    QStackedWidget* a_pathStack = nullptr;
    QWidget* a_crumbBar = nullptr;
    QHBoxLayout* a_crumbLayout = nullptr;
    QToolButton* a_crumbMore = nullptr;
    QList<QToolButton*> a_crumbButtons;
    QList<QLabel*> a_crumbSeparators;
    QLineEdit* a_pathEdit = nullptr;
    QLineEdit* a_filter = nullptr;
    // The views.
    QStackedWidget* a_stack = nullptr;
    QTreeView* a_tree = nullptr;
    QListView* a_list = nullptr;
    QListView* a_grid = nullptr;
    QTreeView* a_details = nullptr;
    QColumnView* a_columns = nullptr;
    QListView* a_recent = nullptr;
    QWidget* a_preview = nullptr;          // the Columns view's, for a file
    QLabel* a_previewIcon = nullptr;
    QLabel* a_previewName = nullptr;
    QLabel* a_previewFacts = nullptr;
    QLabel* a_status = nullptr;
    QTimer* a_statusTimer = nullptr;
    // A drag over it.
    QPointer<QWidget> a_dropHighlight;     // the folder it would drop into, lit up
    QTimer* a_springTimer = nullptr;       // a folder held under the drag opens
    QString a_springPath;
    QPointer<QWidget> a_springWidget;
    QTimer* a_scrollTimer = nullptr;       // near an edge, the view scrolls
    QPointer<QWidget> a_dragViewport;
};

#endif // QUCS_FILEBROWSER_H
