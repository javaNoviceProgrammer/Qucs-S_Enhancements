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

#include <QDockWidget>
#include <QFileInfo>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>

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
  w.sideTab = TabView != nullptr ? TabView->currentIndex() : -1;
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
  if (TabView != nullptr && workspace.sideTab >= 0 && workspace.sideTab < TabView->count() && QucsSettings.RestorePanels)
    TabView->setCurrentIndex(workspace.sideTab);

  if (!said.isEmpty())
    statusBar()->showMessage(tr("The workspace as it was: %1 (Application Settings > Workspace).").arg(said.join(QStringLiteral(", "))),
                             10000);
  return skipped;
}
