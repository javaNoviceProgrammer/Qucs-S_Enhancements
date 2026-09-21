/*
 * ProjectView.cpp - implementation of project model
 *   the model manage the files in project directory
 *
 * Copyright (C) 2014, Yodalee, lc85301@gmail.com
 *
 * This file is part of Qucs
 *
 * Qucs is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Qucs.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "projectView.h"
#include "schematic.h"
#include "misc.h"
#include "main.h"
#include "qucs.h"
#include "simulationconsole.h"

#include <QString>
#include <QStringList>
#include <QDir>
#include <QFileSystemWatcher>
#include <QStandardItemModel>
#include <QTimer>
#include <QDebug>
#include <QDrag>
#include <QFileIconProvider>
#include <QMimeData>
#include <QPainter>
#include <functional>

ProjectView::ProjectView(QWidget *parent)
  : QTreeView(parent)
{
  m_projPath = QString();
  m_projPath = QString();
  m_valid = false;
  m_model = new QStandardItemModel(0, 2, this);
  // Changes in the project's directories come in bursts (a simulation
  // writes several files); one refresh a moment after the last.
  m_watcher = new QFileSystemWatcher(this);
  m_refreshTimer = new QTimer(this);
  m_refreshTimer->setSingleShot(true);
  m_refreshTimer->setInterval(700);
  connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, &ProjectView::scheduleRefresh);
  connect(m_refreshTimer, &QTimer::timeout, this, [this] {
    if (QucsMain != nullptr && QucsMain->simulationConsole() != nullptr
        && QucsMain->simulationConsole()->isRunning()) {
      m_refreshTimer->start();   // not while the simulator is writing
      return;
    }
    refresh();
  });

  this->setModel(m_model);
  refresh();
  this->setEditTriggers(QAbstractItemView::NoEditTriggers);
  this->setSelectionMode(QAbstractItemView::ExtendedSelection); // Allow multiple selection
  // Files can be dragged out (onto the document area, which opens them);
  // nothing can be dropped in.
  this->setDragEnabled(true);
  this->setDragDropMode(QAbstractItemView::DragOnly);
}

ProjectView::~ProjectView()
{
  delete m_model;
}

int ProjectView::categoryOf(const QModelIndex& idx) const
{
  if (!idx.isValid()) return -1;
  QModelIndex top = idx;
  while (top.parent().isValid()) top = top.parent();
  return top.row();
}

QString ProjectView::filePath(const QModelIndex& idx) const
{
  if (!idx.isValid()) return QString();
  return idx.sibling(idx.row(), 0).data(FilePathRole).toString();
}

bool ProjectView::treeView()
{
  return QucsSettings.ContentTreeView;
}

void ProjectView::setTreeView(bool on)
{
  if (QucsSettings.ContentTreeView == on) return;
  QucsSettings.ContentTreeView = on;
  refresh();
}

QList<QUrl> ProjectView::selectedFileUrls() const
{
  QList<QUrl> urls;
  if (!m_valid || selectionModel() == nullptr) return urls;
  const QDir project(m_projPath);
  for (const QModelIndex& idx : selectionModel()->selectedIndexes()) {
    const QString path = filePath(idx);
    if (idx.column() != 0 || path.isEmpty()) continue;   // a category or folder row, or the note
    const QUrl url = QUrl::fromLocalFile(project.absoluteFilePath(path));
    if (!urls.contains(url)) urls.append(url);
  }
  return urls;
}

// The drag carries the files as URLs, like a drag out of a file manager,
// so every drop target that takes files takes them.
void ProjectView::startDrag(Qt::DropActions)
{
  const QList<QUrl> urls = selectedFileUrls();
  if (urls.isEmpty()) return;

  auto* data = new QMimeData;
  data->setUrls(urls);
  auto* drag = new QDrag(this);
  drag->setMimeData(data);

  // The names under the cursor while dragging.
  QStringList names;
  for (const QUrl& url : urls) names.append(QFileInfo(url.toLocalFile()).fileName());
  if (names.size() > 3) names = QStringList(names.mid(0, 3)) << tr("... (%n files)", "", names.size());
  const QFontMetrics fm(font());
  int width = 0;
  for (const QString& n : names) width = std::max(width, fm.horizontalAdvance(n));
  QPixmap pixmap(width + 8, names.size() * fm.height() + 4);
  pixmap.fill(palette().color(QPalette::Highlight));
  QPainter painter(&pixmap);
  painter.setPen(palette().color(QPalette::HighlightedText));
  for (int i = 0; i < names.size(); ++i)
    painter.drawText(4, 2 + i * fm.height() + fm.ascent(), names.at(i));
  painter.end();
  drag->setPixmap(pixmap);

  drag->exec(Qt::CopyAction);
}

void
ProjectView::setProjPath(const QString &path)
{
  // check if path exist
  m_valid = !path.isEmpty() && QDir(path).exists();

  if (m_valid) {
    m_projPath = path; // full path
    m_projName = QDir(m_projPath).dirName(); // only project directory name
    if (m_projName.endsWith("_prj")) {
      m_projName.chop(4);// remove "_prj" from name
    } else { // should not happen
      qWarning() << "ProjectView::setProjPath() : path does not end in '_prj' (" << m_projName << ")";
    }
  }
  refresh();
  // A freshly opened project shows its schematics.
  if (m_valid)
    setExpanded(m_model->index(Schematics, 0), true);
}

QString ProjectView::rowKey(const QModelIndex& idx) const
{
  if (!idx.isValid() || isFile(idx)) return QString();
  QString key;
  for (QModelIndex i = idx; i.parent().isValid(); i = i.parent())
    key.prepend('/' + i.sibling(i.row(), 0).data().toString());
  return QStringLiteral("cat:%1").arg(categoryOf(idx)) + key;
}

void ProjectView::collectExpanded(const QModelIndex& parent, QStringList& keys) const
{
  for (int row = 0; row < m_model->rowCount(parent); ++row) {
    const QModelIndex idx = m_model->index(row, 0, parent);
    if (isFile(idx)) continue;
    if (isExpanded(idx)) keys.append(rowKey(idx));
    collectExpanded(idx, keys);
  }
}

void ProjectView::restoreExpanded(const QModelIndex& parent, const QStringList& keys)
{
  for (int row = 0; row < m_model->rowCount(parent); ++row) {
    const QModelIndex idx = m_model->index(row, 0, parent);
    if (isFile(idx)) continue;
    if (keys.contains(rowKey(idx))) setExpanded(idx, true);
    restoreExpanded(idx, keys);
  }
}

QStandardItem* ProjectView::folderItem(QStandardItem* category, const QString& dir)
{
  QStandardItem* parent = category;
  if (dir.isEmpty()) return parent;
  static const QIcon folderIcon = QFileIconProvider().icon(QFileIconProvider::Folder);
  for (const QString& name : dir.split('/', Qt::SkipEmptyParts)) {
    QStandardItem* found = nullptr;
    for (int row = 0; row < parent->rowCount() && !found; ++row) {
      QStandardItem* child = parent->child(row, 0);
      if (child->data(FilePathRole).toString().isEmpty() && child->text() == name)
        found = child;
    }
    if (!found) {
      appendRow(parent, name, QString());
      found = parent->child(parent->rowCount() - 1, 0);
      found->setIcon(folderIcon);
    }
    parent = found;
  }
  return parent;
}

void ProjectView::appendFile(int category, const QString& path, const QString& note)
{
  QStandardItem* cat = m_model->item(category, 0);
  if (cat == nullptr) return;
  QStandardItem* parent = cat;
  // Named relative to the project, or to the Scratch folder under Scratch.
  QString shown = category == Scratch ? path.section('/', 1) : path;
  if (treeView()) {
    const int slash = shown.lastIndexOf('/');
    if (slash >= 0) {
      parent = folderItem(cat, shown.left(slash));
      shown = shown.mid(slash + 1);
    }
  }
  auto* name = new QStandardItem(shown);
  name->setData(path, FilePathRole);
  // Every row has both cells, the note one empty when there is nothing to
  // say: a row short of a cell in a two-column model leaves the
  // accessibility layer with a table it cannot make sense of (Qt asks for
  // the missing cell, then loses count of the rows, and has crashed on
  // the next expand with an assistive client attached).
  auto* noteItem = new QStandardItem(note);
  noteItem->setFlags(noteItem->flags() & ~Qt::ItemIsDragEnabled);
  parent->appendRow(QList<QStandardItem*>{ name, noteItem });
}

// refresh using projectPath
void
ProjectView::refresh()
{
  // Keep the categories and folders the user has opened.
  QStringList expanded;
  collectExpanded(QModelIndex(), expanded);

  m_model->clear();

  QStringList header;
  header << tr("Content of %1").arg(m_projName) << tr("Note");
  m_model->setHorizontalHeaderLabels(header);

  appendRow(m_model->invisibleRootItem(), tr("Datasets"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("Data Displays"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("Verilog"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("Verilog-A"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("Osdi"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("VHDL"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("Octave"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("Schematics"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("Symbols"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("SPICE"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("Others"), QString(""));
  appendRow(m_model->invisibleRootItem(), tr("Scratch"), QString(""));

  if (m_valid) {
    // put all files into "Content"-ListView: those of the project directory
    // and of any subdirectory, named relative to the project (the files of
    // one directory arrive together, so a folder row's files come first,
    // then its sub-folders)
    const QDir workPath(m_projPath);
    const QString scratchPrefix = QString::fromLatin1(misc::ScratchFolder) + QLatin1Char('/');
    for (const QString& fileName : misc::projectFiles(workPath)) {
      const QFileInfo info(workPath.filePath(fileName));
      const QString extName = info.suffix().toLower();
      const QString fullExtName = info.completeSuffix().toLower();

      if(fileName.startsWith(scratchPrefix)) {
        appendFile(Scratch, fileName);   // temporary files, whatever their type
      }
      else if(extName == "dat" || fullExtName == "dat.ngspice" ||
         fullExtName == "dat.xyce" || fullExtName == "dat.spopus" ) {
        appendFile(Datasets, fileName);
      }
      else if(extName == "dpl") {
        appendFile(DataDisplays, fileName);
      }
      else if(extName == "v") {
        appendFile(Verilog, fileName);
      }
      else if(extName == "va") {
        appendFile(VerilogA, fileName);
      }
      else if(extName == "osdi") {
        appendFile(Osdi, fileName);
      }
      else if((extName == "vhdl") || (extName == "vhd")) {
        appendFile(VHDL, fileName);
      }
      else if((extName == "m") || (extName == "oct")) {
        appendFile(Octave, fileName);
      }
      else if(extName == "sch") {
        // test if it's a valid schematic file
        int n = Schematic::testFile(info.filePath());
        if(n >= 0) {
          // a subcircuit gets its port count as the note
          appendFile(Schematics, fileName, n > 0 ? QString::number(n)+tr("-port") : QString());
        }
      } else if (extName == "sym") {
          appendFile(Symbols, fileName);
      } else if ((extName == "cir") || (extName=="ckt") ||
               (extName=="sp")) {
          appendFile(SPICE, fileName);
      }
      else {
        appendFile(Others, fileName);
      }
    }
  }

  restoreExpanded(QModelIndex(), expanded);
  resizeColumnToContents(0);
  watchProjectDirectories();
}

void ProjectView::scheduleRefresh()
{
  m_refreshTimer->start();
}

QStringList ProjectView::watchedDirectories() const
{
  return m_watcher->directories();
}

void ProjectView::watchProjectDirectories()
{
  const QStringList before = m_watcher->directories();
  if (!before.isEmpty()) m_watcher->removePaths(before);
  if (!m_valid) return;
  // The project directory and every subdirectory the listing covers
  // (misc::projectFiles() skips hidden ones and symbolic links too).
  QStringList dirs{m_projPath};
  std::function<void(const QDir&)> walk = [&](const QDir& d) {
    // no QDir::Hidden: hidden directories are left out, and not entered
    for (const QFileInfo& info : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name)) {
      dirs << info.absoluteFilePath();
      walk(QDir(info.absoluteFilePath()));
    }
  };
  walk(QDir(m_projPath));
  m_watcher->addPaths(dirs);
}

QStringList ProjectView::exportSchematic()
{
  QStringList list;
  // Every file row below the Schematics category, through the folder rows.
  std::function<void(QStandardItem*)> collect = [&](QStandardItem* parent) {
    for (int i = 0; i < parent->rowCount(); ++i) {
      QStandardItem* item = parent->child(i, 0);
      const QString path = item->data(FilePathRole).toString();
      if (path.isEmpty())
        collect(item);                       // a folder
      else if (parent->child(i, 1) != nullptr && !parent->child(i, 1)->text().isEmpty())
        list.append(path);                   // a subcircuit (it has a note)
    }
  };
  collect(m_model->item(Schematics, 0));
  return list;
}
