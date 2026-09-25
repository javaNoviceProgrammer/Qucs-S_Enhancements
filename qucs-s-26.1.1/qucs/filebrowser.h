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
    enum Glyph { Page, Text, Schematic, Symbol, Plot, Table, Code, Image, Archive, Folder, Project };
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
 * a folder, moves it to the trash. The folder, the view and the options
 * are kept for the next start.
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

    bool showHidden() const { return a_showHidden; }
    void setShowHidden(bool on);
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
    void moveToTrash(const QString& path);
    QAbstractItemView* viewFor(View view) const;
    /// Whether \a path is in what the view shows (under the folder shown).
    bool inView(const QString& path) const;

    QString a_location;
    QString a_home;
    QString a_project;
    QStringList a_recentFiles;
    std::function<QString()> a_document;
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
};

#endif // QUCS_FILEBROWSER_H
