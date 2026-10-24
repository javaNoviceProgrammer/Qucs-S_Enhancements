/*
 * qucs_workspace.cpp - the workspace kept from one run to the next: the
 *                      project, the documents in their panes, the panels
 *                      and toolbars (Application Settings > Workspace)
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qucs.h"
#include "main.h"
#include "octave_window.h"
#include "qucsdoc.h"
#include "workspace.h"
#include "workspacesession.h"

#include "projectstatedelegate.h"
#include "settings.h"

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QIcon>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <map>

namespace session = qucs_s::session;

namespace {

bool &kept()
{
  static bool on = false;
  return on;
}

// A file as it is to be compared: its real path when it is there.
QString realPath(const QString &file)
{
  const QString real = QFileInfo(file).canonicalFilePath();
  return real.isEmpty() ? QDir::cleanPath(file) : real;
}

} // namespace

void QucsApp::setWorkspaceKept(bool on) { kept() = on; }

bool QucsApp::workspaceKept() { return kept(); }


// ---- the Workspaces panel ----------------------------------------------
//
// The workspaces opened before, newest first, the one open now with a dot
// as the Projects panel marks the open project. Switching from here is
// Project > Switch Workspace with the folder already chosen, so it closes
// the documents the same way (asking about unsaved ones) and the workspace
// that is left keeps its own session.

QString QucsApp::sidePageName(SidePage page)
{
  switch (page) {
  case SidePage::Workspaces: return QStringLiteral("workspaces");
  case SidePage::Projects: return QStringLiteral("projects");
  case SidePage::Content: return QStringLiteral("content");
  case SidePage::Components: return QStringLiteral("components");
  case SidePage::Libraries: return QStringLiteral("libraries");
  case SidePage::FileBrowser: return QStringLiteral("file browser");
  case SidePage::Multiphysics: return QStringLiteral("multiphysics");
  case SidePage::Rf25: return QStringLiteral("rf 2.5d");
  }
  return {};
}

int QucsApp::sidePage(SidePage page) const
{
  QWidget *widget = a_sidePages.value(int(page), nullptr);
  if (widget == nullptr || TabView == nullptr) return -1;
  return TabView->indexOf(widget);
}

bool QucsApp::showSidePage(SidePage page)
{
  const int index = sidePage(page);
  if (index < 0) return false;
  TabView->setCurrentIndex(index);
  return true;
}

QString QucsApp::currentSidePageName() const
{
  if (TabView == nullptr) return {};
  for (auto it = a_sidePages.constBegin(); it != a_sidePages.constEnd(); ++it)
    if (it.value() != nullptr && TabView->indexOf(it.value()) == TabView->currentIndex())
      return sidePageName(SidePage(it.key()));
  return {};
}

bool QucsApp::showSidePageNamed(const QString &name)
{
  if (name.isEmpty()) return false;
  for (auto it = a_sidePages.constBegin(); it != a_sidePages.constEnd(); ++it)
    if (sidePageName(SidePage(it.key())) == name) return showSidePage(SidePage(it.key()));
  return false;
}

void QucsApp::initWorkspacesPanel()
{
  QWidget *group = new QWidget();
  group->setObjectName(QStringLiteral("workspacesPage"));
  QVBoxLayout *layout = new QVBoxLayout();

  QWidget *butts = new QWidget();
  QPushButton *newOne = new QPushButton(tr("New"));
  newOne->setObjectName(QStringLiteral("workspaceNew"));
  newOne->setToolTip(tr("Makes a folder the workspace, creating it if it is not there"));
  QPushButton *open = new QPushButton(tr("Open"));
  open->setObjectName(QStringLiteral("workspaceOpen"));
  open->setToolTip(tr("Chooses another folder as the workspace"));
  QPushButton *forget = new QPushButton(tr("Forget"));
  forget->setObjectName(QStringLiteral("workspaceForget"));
  forget->setToolTip(tr("Takes the chosen workspace out of this list; the folder itself is left alone"));
  QHBoxLayout *buttsLayout = new QHBoxLayout();
  buttsLayout->addWidget(newOne);
  buttsLayout->addWidget(open);
  buttsLayout->addWidget(forget);
  butts->setLayout(buttsLayout);

#if __APPLE__
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  buttsLayout->setContentsMargins(5, 0, 5, 0);
  buttsLayout->setSpacing(5);
#endif

  layout->addWidget(butts);
  Workspaces = new QListWidget();
  Workspaces->setObjectName(QStringLiteral("workspacesList"));
  Workspaces->setItemDelegate(new ProjectStateDelegate(Workspaces));   // the open workspace's dot
  Workspaces->setContextMenuPolicy(Qt::CustomContextMenu);
  layout->addWidget(Workspaces);
  group->setLayout(layout);

  TabView->addTab(group, tr("Workspaces"));
  a_sidePages.insert(int(SidePage::Workspaces), group);
  TabView->setTabToolTip(TabView->indexOf(group), tr("the workspaces opened before, and the one open now"));

  connect(Workspaces, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
    const QString dir = workspaceOfRow(item);
    if (dir.isEmpty()) return;
    if (QDir(dir).canonicalPath() == QucsSettings.qucsWorkspaceDir.canonicalPath()) {
      // Nothing to switch to, so it shows what the workspace holds - its
      // projects - as opening the project already open shows its contents.
      showSidePage(SidePage::Projects);
      statusBar()->showMessage(tr("%1 is the workspace already.").arg(QDir::toNativeSeparators(dir)), 5000);
      return;
    }
    if (!QFileInfo(dir).isDir()) {
      statusBar()->showMessage(tr("%1 is not there now: Forget takes it out of the list.")
                                   .arg(QDir::toNativeSeparators(dir)), 8000);
      return;
    }
    if (switchWorkspace(dir)) statusBar()->showMessage(tr("Workspace: %1").arg(QDir::toNativeSeparators(dir)), 5000);
  });
  connect(Workspaces, &QListWidget::customContextMenuRequested, this, &QucsApp::slotWorkspacesContextMenu);
  connect(open, &QPushButton::clicked, this, &QucsApp::slotSwitchWorkspace);
  connect(newOne, &QPushButton::clicked, this, [this] {
    // (The dialog makes a folder: one chosen that is not there yet is made
    // by setWorkspace, as a workspace given on the command line is.)
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Choose or Create the Workspace Folder"), QucsSettings.qucsWorkspaceDir.absolutePath(),
        QFileDialog::ShowDirsOnly);
    if (dir.isEmpty()) return;
    if (QDir(dir).canonicalPath() == QucsSettings.qucsWorkspaceDir.canonicalPath()) {
      statusBar()->showMessage(tr("%1 is the workspace already.").arg(QDir::toNativeSeparators(dir)), 5000);
      return;
    }
    if (switchWorkspace(dir)) statusBar()->showMessage(tr("Workspace: %1").arg(QDir::toNativeSeparators(dir)), 5000);
  });
  connect(forget, &QPushButton::clicked, this, [this] {
    const QString dir = workspaceOfRow(Workspaces->currentItem());
    if (dir.isEmpty()) {
      statusBar()->showMessage(tr("Choose a workspace of the list to forget."), 5000);
      return;
    }
    if (QDir(dir).canonicalPath() == QucsSettings.qucsWorkspaceDir.canonicalPath()) {
      statusBar()->showMessage(tr("The workspace open now stays in the list."), 5000);
      return;
    }
    QucsSettings.RecentWorkspaces.removeAll(dir);
    rememberWorkspace(QString());   // (writes the list as it is now)
    refreshWorkspaces();
    statusBar()->showMessage(tr("%1 is out of the list; the folder itself is untouched.")
                                 .arg(QDir::toNativeSeparators(dir)), 5000);
  });
}

QString QucsApp::workspaceOfRow(const QListWidgetItem *item) const
{
  return item == nullptr ? QString() : item->data(Qt::UserRole).toString();
}

void QucsApp::rememberWorkspace(const QString &dir)
{
  if (!dir.isEmpty()) {
    const QString path = QDir(dir).absolutePath();
    // The same folder reached by another spelling (a link, a trailing "/")
    // is one row, the newest spelling of it.
    const QString real = QFileInfo(path).canonicalFilePath();
    for (const QString &had : QStringList(QucsSettings.RecentWorkspaces))
      if (had == path || (!real.isEmpty() && QFileInfo(had).canonicalFilePath() == real))
        QucsSettings.RecentWorkspaces.removeAll(had);
    QucsSettings.RecentWorkspaces.prepend(path);
    while (QucsSettings.RecentWorkspaces.size() > MaxRecentWorkspaces) QucsSettings.RecentWorkspaces.removeLast();
  }
  // A run with a workspace of its own (--workspace, as a client of
  // --mcp-server starts it) keeps nothing in the user's settings: its
  // folder is that run's, and writing it here put it in the user's own
  // history (test_mcp_server's theWorkspaceIsTheOneGiven). The panel still
  // shows it - the list in memory has it - but the file is left alone.
  if (!QucsSettings.workspaceOfRun.isEmpty()) return;
  QSettings *settings = new QucsSettingsFile;
  settings->setValue(QStringLiteral("RecentWorkspaces"), QucsSettings.RecentWorkspaces.join(QLatin1Char('*')));
  delete settings;
}

void QucsApp::refreshWorkspaces()
{
  if (Workspaces == nullptr) return;
  const QString now = QucsSettings.qucsWorkspaceDir.absolutePath();
  const QString nowReal = QFileInfo(now).canonicalFilePath();
  // The one open now is in the list whether or not the settings had it
  // (the first start, or a workspace given on the command line).
  if (!now.isEmpty() && !QucsSettings.RecentWorkspaces.contains(now)) rememberWorkspace(now);
  // Two workspaces of one name in different folders: the parent says which
  // is which.
  QHash<QString, int> names;
  for (const QString &dir : std::as_const(QucsSettings.RecentWorkspaces)) ++names[QDir(dir).dirName()];
  const QString chosen = workspaceOfRow(Workspaces->currentItem());
  Workspaces->clear();
  for (const QString &dir : std::as_const(QucsSettings.RecentWorkspaces)) {
    const QString name = QDir(dir).dirName();
    QDir parent(dir);
    const bool ambiguous = names.value(name) > 1 && parent.cdUp();
    auto *item = new QListWidgetItem(ambiguous ? tr("%1 (%2)").arg(name, QDir::toNativeSeparators(parent.absolutePath()))
                                               : name);
    item->setData(Qt::UserRole, dir);
    const bool open = !nowReal.isEmpty() ? QFileInfo(dir).canonicalFilePath() == nowReal : dir == now;
    item->setData(QucsFileSystemModel::ProjectStateRole,
                  int(open ? QucsFileSystemModel::OpenProject : QucsFileSystemModel::ClosedProject));
    const bool there = QFileInfo(dir).isDir();
    item->setData(QucsFileSystemModel::RowStateRole,
                  !there ? QStringLiteral("missing") : open ? QStringLiteral("open") : QStringLiteral("closed"));
    // Its icon, as the Projects panel gives a project one: the folder of the
    // platform's own look - open for the workspace in use, closed for the
    // others - and greyed where the folder is not there now.
    const QStyle *look = Workspaces->style() != nullptr ? Workspaces->style() : QApplication::style();
    const QIcon folder = look->standardIcon(open ? QStyle::SP_DirOpenIcon : QStyle::SP_DirClosedIcon, nullptr, Workspaces);
    if (there) {
        item->setIcon(folder);
    } else {
        const int side = 16 * int(Workspaces->devicePixelRatioF() > 1.5 ? 2 : 1);
        QIcon greyed;
        greyed.addPixmap(folder.pixmap(QSize(side, side), QIcon::Disabled));
        item->setIcon(greyed);
    }
    QStringList said{QDir::toNativeSeparators(dir)};
    said << (open ? tr("The workspace open now") : tr("A workspace, not open"));
    if (!there) {
      said << tr("It is not there now (moved, deleted, or on a drive not mounted).\n"
                 "Forget takes it out of the list.");
      item->setForeground(QApplication::palette().color(QPalette::Disabled, QPalette::Text));
    }
    item->setToolTip(said.join(QLatin1Char('\n')));
    Workspaces->addItem(item);
    if (dir == chosen) Workspaces->setCurrentItem(item);
  }
}

void QucsApp::slotWorkspacesContextMenu(const QPoint &point)
{
  QListWidgetItem *item = Workspaces->itemAt(point);
  const QString dir = workspaceOfRow(item);
  if (dir.isEmpty()) return;
  Workspaces->setCurrentItem(item);
  const bool open = QDir(dir).canonicalPath() == QucsSettings.qucsWorkspaceDir.canonicalPath();
  QMenu menu(this);
  QAction *openIt = menu.addAction(tr("Open"));
  openIt->setEnabled(!open && QFileInfo(dir).isDir());
  QAction *reveal = menu.addAction(
#if defined(Q_OS_MACOS)
      tr("Open in Finder")
#elif defined(Q_OS_WIN)
      tr("Open in Explorer")
#else
      tr("Open in the File Manager")
#endif
  );
  reveal->setEnabled(QFileInfo(dir).isDir());
  QAction *forget = menu.addAction(tr("Forget"));
  forget->setEnabled(!open);
  const QAction *chosen = menu.exec(Workspaces->viewport()->mapToGlobal(point));
  if (chosen == openIt) {
    if (switchWorkspace(dir)) statusBar()->showMessage(tr("Workspace: %1").arg(QDir::toNativeSeparators(dir)), 5000);
  } else if (chosen == reveal) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(dir).absolutePath()));
  } else if (chosen == forget) {
    QucsSettings.RecentWorkspaces.removeAll(dir);
    rememberWorkspace(QString());
    refreshWorkspaces();
    statusBar()->showMessage(tr("%1 is out of the list; the folder itself is untouched.")
                                 .arg(QDir::toNativeSeparators(dir)), 5000);
  }
}

// ---- the workspace kept from one run to the next ------------------------

session::Workspace QucsApp::currentWorkspace() const
{
  session::Workspace w;
  if (!ProjName.isEmpty()) w.project = QucsSettings.QucsWorkDir.absolutePath();
  const QList<ContextMenuTabWidget *> all = panes();
  for (ContextMenuTabWidget *pane : all) {
    session::Pane p;
    const QPoint cell = paneCell(pane);
    p.row = std::max(0, cell.y());
    p.column = std::max(0, cell.x());
    for (int i = 0; i < pane->count(); ++i)
      if (QucsDoc *doc = docIn(pane->widget(i)); doc != nullptr && !doc->getDocName().isEmpty())
        p.documents << doc->getDocName();
    if (QucsDoc *doc = docIn(pane->currentWidget()); doc != nullptr && !doc->getDocName().isEmpty())
      p.current = doc->getDocName();
    w.panes << p;
  }
  w.activePane = std::max<qsizetype>(0, all.indexOf(DocumentTab));
  // The document maximized, the other panes are hidden: their sizes from
  // before - unless the panes are others now (Close all but this takes the
  // hidden ones away).
  QList<int> columnCounts;
  if (a_paneArea != nullptr)
    for (int r = 0; r < a_paneArea->count(); ++r)
      if (auto *row = qobject_cast<QSplitter *>(a_paneArea->widget(r))) columnCounts << row->count();
  QList<int> countsBefore;
  for (const QList<int> &columns : a_columnSizesBeforeMaximized) countsBefore << columns.size();
  if (isDocumentMaximized() && columnCounts == countsBefore) {
    w.rowSizes = a_rowSizesBeforeMaximized;
    w.columnSizes = a_columnSizesBeforeMaximized;
  } else if (a_paneArea != nullptr) {
    w.rowSizes = a_paneArea->sizes();
    for (int r = 0; r < a_paneArea->count(); ++r)
      if (auto *row = qobject_cast<QSplitter *>(a_paneArea->widget(r))) w.columnSizes << row->sizes();
  }
  w.sidePage = currentSidePageName();
  w.saved = QDateTime::currentDateTime();
  return w;
}

void QucsApp::saveWorkspace()
{
  // (What the tests and the MCP server open is no one's workspace; with
  // the option off, nothing is kept.)
  if (!kept() || !QucsSettings.RestoreWorkspace) return;
  // The document maximized, its panels are kept as they were before.
  session::save(currentWorkspace(), isDocumentMaximized() ? a_layoutBeforeMaximized
                                                          : saveState(session::kWindowStateVersion));
}

int QucsApp::openWorkspaceDocuments(const session::Workspace &workspace, const QSet<QString> &skip,
                                    QStringList *skipped, QStringList *gone)
{
  // The panes that have a document left to open, in their grid; a pane
  // whose files are all gone is not made.
  struct Kept {
    session::Pane pane;
    int index;   // in workspace.panes
  };
  std::map<int, std::map<int, Kept>> grid;   // row -> column -> pane
  for (int i = 0; i < workspace.panes.size(); ++i) {
    session::Pane pane = workspace.panes.at(i);
    QStringList files;
    for (const QString &file : std::as_const(pane.documents)) {
      if (!QFileInfo(file).isFile()) {
        if (gone != nullptr) *gone << file;
      } else if (skip.contains(realPath(file))) {
        if (skipped != nullptr) *skipped << file;
      } else {
        files << file;
      }
    }
    pane.documents = files;
    if (!files.isEmpty() && !grid[pane.row].count(pane.column)) grid[pane.row][pane.column] = {pane, i};
  }
  for (auto it = grid.begin(); it != grid.end();)
    it = it->second.empty() ? grid.erase(it) : std::next(it);
  if (grid.empty()) return 0;

  // The grid: from the one pane there is, a row below when there were
  // two, then a pane to the right in each row that had two.
  if (grid.size() > 1 && canSplitDown()) {
    setActivePane(panes().constFirst());
    slotSplitPaneDown();
  }
  int r = 0;
  for (const auto &[row, columns] : grid) {
    if (columns.size() > 1)
      for (ContextMenuTabWidget *pane : panes())
        if (paneCell(pane) == QPoint(0, r)) {
          setActivePane(pane);
          if (canSplitRight()) slotSplitPaneRight();
          break;
        }
    ++r;
  }

  // Each pane's documents, in their order, and the one in front.
  const QList<ContextMenuTabWidget *> made = panes();
  QList<Kept> order;
  for (const auto &[row, columns] : grid)
    for (const auto &[column, pane] : columns) order << pane;
  int opened = 0;
  ContextMenuTabWidget *active = nullptr;
  for (int k = 0; k < order.size() && k < made.size(); ++k) {
    ContextMenuTabWidget *pane = made.at(k);
    setActivePane(pane);
    QWidget *current = nullptr;
    for (const QString &file : std::as_const(order.at(k).pane.documents)) {
      // (No question at the start: the data names were asked about when
      // the document was first opened.)
      if (!gotoPage(file, false, false)) continue;
      ++opened;
      if (realPath(file) == realPath(order.at(k).pane.current)) current = pane->currentWidget();
    }
    if (current != nullptr) pane->setCurrentWidget(current);
    if (order.at(k).index == workspace.activePane) active = pane;
  }

  // The panes' sizes, as they were (a row's two panes, the rows).
  if (a_paneArea != nullptr) {
    if (workspace.rowSizes.size() == a_paneArea->count() && workspace.rowSizes.size() > 1)
      a_paneArea->setSizes(workspace.rowSizes);
    r = 0;
    for (const auto &[row, columns] : grid) {
      auto *line = r < a_paneArea->count() ? qobject_cast<QSplitter *>(a_paneArea->widget(r)) : nullptr;
      if (line != nullptr && row < workspace.columnSizes.size() && workspace.columnSizes.at(row).size() == line->count()
          && line->count() > 1)
        line->setSizes(workspace.columnSizes.at(row));
      ++r;
    }
  }
  setActivePane(active != nullptr ? active : made.constFirst());
  return opened;
}

QStringList QucsApp::restoreWorkspace(bool projectGiven, const QSet<QString> &skip, bool askFirst)
{
  setDocumentMaximized(false);   // (the panels and panes are restored as kept)
  QStringList skipped;
  if (!QucsSettings.RestoreWorkspace) return skipped;
  const session::Workspace workspace = session::saved();
  QStringList said;
  // After a run that did not end cleanly, asked: what it opens may be what
  // brought it down, and would at every start.
  bool open = true;
  if (askFirst && (QucsSettings.RestoreProject || QucsSettings.RestoreDocuments) && !workspace.isEmpty() && !projectGiven)
    open = QMessageBox::question(this, tr("Workspace"),
                                 tr("Qucs-S did not exit cleanly the last time it ran. Open the workspace again as it "
                                    "was - %1?").arg(workspace.summary()),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::Yes;

  // The panels and toolbars: which are shown, where, and their sizes.
  if (QucsSettings.RestorePanels) {
    const QByteArray state = session::savedWindowState();
    if (!state.isEmpty() && restoreState(state, session::kWindowStateVersion)) {
      // (The Octave dock's program starts when the View menu shows it: a
      // dock shown by the state starts it here.)
      if (octDock != nullptr && octDock->isVisible()) slotViewOctaveDock(true);
      said << tr("the panels");
    }
  }

  // The project, when it is still there (and none is named to open).
  if (open && QucsSettings.RestoreProject && !projectGiven && !workspace.project.isEmpty()) {
    // (By its name, a folder is one - but it must be there, or openProject()
    // says so in a box no one waits for at the start.)
    if (QFileInfo(workspace.project).isDir() && qucs_s::workspace::isProjectFolder(workspace.project)) {
      openProject(workspace.project);
      if (!ProjName.isEmpty()) said << tr("project %1").arg(ProjName);
    } else {
      said << tr("not project %1 (it is gone)").arg(QDir(workspace.project).dirName());
    }
  }

  // The documents, in their panes (not with a project named: opening it
  // closes them again).
  QStringList gone;
  if (open && QucsSettings.RestoreDocuments && !projectGiven) {
    const int opened = openWorkspaceDocuments(workspace, skip, &skipped, &gone);
    if (opened > 0) said << (opened == 1 ? tr("1 document") : tr("%1 documents").arg(opened));
    if (!gone.isEmpty())
      said << (gone.size() == 1 ? tr("not %1 (no longer there)").arg(QFileInfo(gone.first()).fileName())
                                : tr("not %1 documents no longer there").arg(gone.size()));
  }
  if (QucsSettings.RestorePanels) showSidePageNamed(workspace.sidePage);

  if (!said.isEmpty())
    statusBar()->showMessage(tr("The workspace as it was: %1 (Application Settings > Workspace).").arg(said.join(QStringLiteral(", "))),
                             10000);
  return skipped;
}
