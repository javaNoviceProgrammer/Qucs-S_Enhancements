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
#include "workspace.h"
#include "projectlibraries.h"

#include <QString>
#include <QStringList>
#include <QApplication>
#include <QDir>
#include <QStandardItemModel>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QFutureWatcher>
#include <QLocale>
#include <QPromise>
#include <QThreadPool>
#include <memory>
#include <QDebug>
#include <QDrag>
#include <QFileIconProvider>
#include <QMimeData>
#include <QPainter>
#include <QRegularExpression>
#include <algorithm>
#include <functional>
#include <iterator>

ProjectView::ProjectView(QWidget *parent)
  : QTreeView(parent)
{
  m_projPath = QString();
  m_projPath = QString();
  m_valid = false;
  m_model = new QStandardItemModel(0, 2, this);
  // Every so many seconds, a look at the project's files; the panel is
  // rebuilt only when they differ from what it shows.
  m_pollTimer = new QTimer(this);
  connect(m_pollTimer, &QTimer::timeout, this, &ProjectView::refreshIfChanged);
  applyRefreshSettings();

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

const QStringList& ProjectView::imageSuffixes()
{
  static const QStringList suffixes = {
    "png", "jpg", "jpeg", "jpe", "svg", "svgz", "gif", "bmp", "tif", "tiff", "webp",
    "ico", "icns", "pbm", "pgm", "ppm", "xbm", "xpm", "heic", "heif", "jp2", "avif",
  };
  return suffixes;
}

namespace {
struct CategoryInfo {
  const char* key;
  const char* name;
  const char* patterns;   // (Images: imageSuffixes())
};

// In the Category enum's order.
constexpr CategoryInfo kCategories[] = {
  {"Datasets", QT_TRANSLATE_NOOP("ProjectView", "Datasets"), "*.dat, *.dat.ngspice, *.dat.xyce, *.dat.spopus"},
  {"DataDisplays", QT_TRANSLATE_NOOP("ProjectView", "Data Displays"), "*.dpl"},
  {"Verilog", QT_TRANSLATE_NOOP("ProjectView", "Verilog"), "*.v"},
  {"VerilogA", QT_TRANSLATE_NOOP("ProjectView", "Verilog-A"), "*.va"},
  {"Osdi", QT_TRANSLATE_NOOP("ProjectView", "Osdi"), "*.osdi"},
  {"VHDL", QT_TRANSLATE_NOOP("ProjectView", "VHDL"), "*.vhdl, *.vhd"},
  {"Octave", QT_TRANSLATE_NOOP("ProjectView", "Octave"), "*.m, *.oct"},
  {"Schematics", QT_TRANSLATE_NOOP("ProjectView", "Schematics"), "*.sch"},
  {"Symbols", QT_TRANSLATE_NOOP("ProjectView", "Symbols"), "*.sym"},
  {"SPICE", QT_TRANSLATE_NOOP("ProjectView", "SPICE"), "*.cir, *.ckt, *.sp"},
  {"Python", QT_TRANSLATE_NOOP("ProjectView", "Python"), "*.py, *.pyw"},
  {"Images", QT_TRANSLATE_NOOP("ProjectView", "Images"), nullptr},
  {"Text", QT_TRANSLATE_NOOP("ProjectView", "Text"), "*.txt"},
  {"Others", QT_TRANSLATE_NOOP("ProjectView", "Others"), "*"},
  {"Scratch", QT_TRANSLATE_NOOP("ProjectView", "Scratch"), "*"},
};
static_assert(std::size(kCategories) == ProjectView::CategoryCount);

bool isCategory(int category) { return category >= 0 && category < ProjectView::CategoryCount; }

// The user's category's place in QucsSettings.ContentUserCategories, -1.
int userIndex(int category)
{
  const int i = category - ProjectView::UserCategory;
  return i >= 0 && i < QucsSettings.ContentUserCategories.size() ? i : -1;
}

// Every category with its name and patterns, in the panel's order: what
// the listing is built with (a change of any rebuilds it).
QStringList categoryLines()
{
  QStringList lines;
  for (const int category : ProjectView::categories())
    lines << ProjectView::categoryName(category) + QLatin1Char('\t') + ProjectView::patterns(category);
  return lines;
}

// A category's patterns, ready to match a file's name.
QList<QRegularExpression> matchers(int category)
{
  QList<QRegularExpression> list;
  for (const QString& pattern : ProjectView::parsePatterns(ProjectView::patterns(category))) {
    const QRegularExpression re(
        QRegularExpression::wildcardToRegularExpression(pattern, QRegularExpression::NonPathWildcardConversion),
        QRegularExpression::CaseInsensitiveOption);
    if (re.isValid()) list.append(re);
  }
  return list;
}

bool matches(const QList<QRegularExpression>& patterns, const QString& name)
{
  return std::any_of(patterns.begin(), patterns.end(),
                     [&name](const QRegularExpression& re) { return re.match(name).hasMatch(); });
}
} // namespace

bool ProjectView::isUserCategory(int category)
{
  return userIndex(category) >= 0;
}

QList<int> ProjectView::categories()
{
  QList<int> order;
  for (int category = 0; category <= Text; ++category) order << category;
  for (int i = 0; i < QucsSettings.ContentUserCategories.size(); ++i) order << UserCategory + i;
  order << Others << Scratch;
  return order;
}

int ProjectView::rowOf(int category)
{
  return int(categories().indexOf(category));
}

QString ProjectView::categoryKey(int category)
{
  if (isUserCategory(category)) return QStringLiteral("User%1").arg(userIndex(category) + 1);
  return isCategory(category) ? QString::fromLatin1(kCategories[category].key) : QString();
}

QString ProjectView::categoryName(int category)
{
  if (isUserCategory(category)) return QucsSettings.ContentUserCategories.at(userIndex(category)).name;
  return isCategory(category) ? QCoreApplication::translate("ProjectView", kCategories[category].name) : QString();
}

QString ProjectView::defaultPatterns(int category)
{
  if (!isCategory(category)) return QString();
  if (category == Images) {
    QStringList patterns;
    for (const QString& suffix : imageSuffixes()) patterns.append("*." + suffix);
    return patterns.join(", ");
  }
  return QString::fromLatin1(kCategories[category].patterns);
}

QString ProjectView::patterns(int category)
{
  if (isUserCategory(category)) return QucsSettings.ContentUserCategories.at(userIndex(category)).patterns;
  if (!isCategory(category)) return QString();
  const auto chosen = QucsSettings.ContentPatterns.constFind(categoryKey(category));
  return chosen != QucsSettings.ContentPatterns.constEnd() ? *chosen : defaultPatterns(category);
}

void ProjectView::setPatterns(int category, const QString& text)
{
  if (isUserCategory(category)) {
    QucsSettings.ContentUserCategories[userIndex(category)].patterns = normalizedPatterns(text);
    return;
  }
  if (!isCategory(category)) return;
  const QString patterns = normalizedPatterns(text);
  if (patterns == defaultPatterns(category))
    QucsSettings.ContentPatterns.remove(categoryKey(category));
  else
    QucsSettings.ContentPatterns.insert(categoryKey(category), patterns);
}

QStringList ProjectView::parsePatterns(const QString& text)
{
  static const QRegularExpression separators(QStringLiteral("[,;\\s]+"));
  static const QRegularExpression wildcards(QStringLiteral("[*?\\[]"));
  QStringList patterns;
  for (QString pattern : text.split(separators, Qt::SkipEmptyParts)) {
    if (pattern.startsWith('.'))
      pattern.prepend('*');                       // .txt
    else if (!pattern.contains('.') && !pattern.contains(wildcards))
      pattern.prepend(QStringLiteral("*."));     // txt
    if (!patterns.contains(pattern, Qt::CaseInsensitive)) patterns.append(pattern);
  }
  return patterns;
}

QString ProjectView::normalizedPatterns(const QString& text)
{
  return parsePatterns(text).join(QStringLiteral(", "));
}

int ProjectView::categoryOf(const QModelIndex& idx) const
{
  if (!idx.isValid()) return -1;
  QModelIndex top = idx;
  while (top.parent().isValid()) top = top.parent();
  const QVariant category = top.sibling(top.row(), 0).data(CategoryRole);   // (set by refresh())
  return category.isValid() ? category.toInt() : top.row();
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
    // "amp_prj" is amp; a folder that is any folder keeps its name
    m_projName = qucs_s::workspace::projectName(QDir(m_projPath).dirName());
  }
  refresh();
  // A freshly opened project shows its schematics - when the filter is
  // cleared, if one is typed.
  if (m_valid) {
    const QModelIndex schematics = m_model->index(rowOf(Schematics), 0);
    setExpanded(schematics, true);
    if (!m_filter.isEmpty()) m_openUnfiltered = QStringList{rowKey(schematics)};
  }
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
      if (QucsSettings.ContentFolderIcons) found->setIcon(folderIcon);   // off by default: a plain row
    }
    parent = found;
  }
  return parent;
}

void ProjectView::appendFile(int category, const QString& path, const QString& note, const QString& tip)
{
  QStandardItem* cat = m_model->item(rowOf(category), 0);
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
  if (!tip.isEmpty()) name->setToolTip(tip);
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

  // The project's files, looked at once (at most misc::MaxProjectEntries
  // files and folders: said in the header when that was not all).
  bool complete = true;
  const QStringList files = m_valid ? misc::projectFiles(QDir(m_projPath), QStringList(), &complete) : QStringList();
  m_header = complete ? tr("Content of %1").arg(m_projName)
                      : tr("Content of %1 (the first %2 files and folders)")
                            .arg(m_projName, QLocale().toString(misc::MaxProjectEntries));
  m_model->setHorizontalHeaderLabels(QStringList{m_header, tr("Note")});

  const QList<int> order = categories();
  for (const int category : order) {
    appendRow(m_model->invisibleRootItem(), categoryName(category), QString(""));
    m_model->item(m_model->rowCount() - 1, 0)->setData(category, CategoryRole);
  }

  if (m_valid) {
    // put all files into "Content"-ListView: those of the project directory
    // and of any subdirectory, named relative to the project (the files of
    // one directory arrive together, so a folder row's files come first,
    // then its sub-folders), each under the first category that takes it
    QHash<int, QList<QRegularExpression>> taken;   // by category
    for (const int category : order) taken.insert(category, matchers(category));
    const QDir workPath(m_projPath);
    const QString scratchPrefix = QString::fromLatin1(misc::ScratchFolder) + QLatin1Char('/');
    // The folders of library Verilog-A Qucs-S keeps (projectlibraries.h):
    // their links say whose they are.
    QSet<QString> libraryFolders;   // Libraries/<library>
    const QString librariesFolder = QLatin1String(qucs_s::projectlibraries::FolderName);
    for (const QFileInfo& dir : QDir(workPath.filePath(librariesFolder)).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks))
      if (QFileInfo::exists(QDir(dir.absoluteFilePath()).absoluteFilePath(QLatin1String(qucs_s::projectlibraries::RecordName))))
        libraryFolders.insert(librariesFolder + QLatin1Char('/') + dir.fileName());
    for (const QString& fileName : files) {
      const QFileInfo info(workPath.filePath(fileName));
      if (fileName.startsWith(scratchPrefix)) {   // temporary files, of whatever type
        if (matches(taken[Scratch], info.fileName())) appendFile(Scratch, fileName);
        continue;
      }
      for (const int category : order) {   // (in the panel's order: the first that takes it)
        if (category == Scratch) continue;
        if (!matches(taken[category], info.fileName())) continue;
        if (category == Schematics) {
          // Only a schematic; a subcircuit gets its port count as the note.
          const int n = Schematic::testFile(info.filePath());
          if (n < 0) continue;
          appendFile(Schematics, fileName, n > 0 ? QString::number(n) + tr("-port") : QString());
        } else {
          qucs_s::projectlibraries::Entry entry;
          if (libraryFolders.contains(fileName.section(QLatin1Char('/'), 0, 1)))
            entry = qucs_s::projectlibraries::entryOf(info.filePath());
          if (entry.isEmpty())
            appendFile(category, fileName);
          else
            appendFile(category, fileName, tr("library %1").arg(entry.library),
                       tr("The Verilog-A of a device of the library %1 a schematic of the project uses, linked from "
                          "%2: opened read-only. Qucs-S takes it away when no schematic uses the device.")
                           .arg(entry.library, QDir::toNativeSeparators(entry.original)));
        }
        break;
      }
    }
  }

  restoreExpanded(QModelIndex(), expanded);
  if (!m_filter.isEmpty()) applyFilter();
  resizeColumnToContents(0);
  m_signature = m_valid ? signatureOf(m_projPath, files) : QString();
  m_folderIcons = QucsSettings.ContentFolderIcons;
  m_patterns = categoryLines();
}

void ProjectView::setFilterText(const QString& text)
{
  const QString filter = text.trimmed();
  if (filter == m_filter) return;
  // The rows the user had open, for when the filter is cleared: the filter
  // opens those that hold what it finds.
  if (m_filter.isEmpty()) {
    m_openUnfiltered.clear();
    collectExpanded(QModelIndex(), m_openUnfiltered);
  }
  m_filter = filter;
  m_matcher = qucs_s::files::NameFilter(filter);
  applyFilter();
  if (m_filter.isEmpty()) {
    collapseAll();
    restoreExpanded(QModelIndex(), m_openUnfiltered);
  }
}

void ProjectView::applyFilter()
{
  const int found = filterRows(QModelIndex());
  m_model->setHeaderData(0, Qt::Horizontal,
                         m_filter.isEmpty() ? m_header : tr("%1: %n found", "", found).arg(m_header));
  resizeColumnToContents(0);   // (the header too: not cut)
}

int ProjectView::filterRows(const QModelIndex& parent)
{
  int found = 0;
  for (int row = 0; row < m_model->rowCount(parent); ++row) {
    const QModelIndex idx = m_model->index(row, 0, parent);
    bool shown = true;
    if (isFile(idx)) {
      // As the row is named: relative to the project, or to the Scratch
      // folder under Scratch.
      QString name = filePath(idx);
      if (categoryOf(idx) == Scratch) name = name.section('/', 1);
      shown = m_matcher.matchesPath(name);
      found += shown ? 1 : 0;
      // What is hidden is not acted on: no menu or drag takes it along.
      if (!shown && selectionModel() != nullptr)
        selectionModel()->select(idx, QItemSelectionModel::Deselect | QItemSelectionModel::Rows);
    } else {
      const int inside = filterRows(idx);
      found += inside;
      shown = m_filter.isEmpty() || inside > 0;
      if (inside > 0 && !m_filter.isEmpty()) setExpanded(idx, true);
    }
    setRowHidden(row, parent, !shown);
  }
  return found;
}

QString ProjectView::signatureOf(const QString& projPath, const QStringList& files)
{
  const QDir workPath(projPath);
  QString signature;
  for (const QString& fileName : files) {
    const QFileInfo info(workPath.filePath(fileName));
    signature += fileName + QLatin1Char('|') + QString::number(info.size()) + QLatin1Char('|')
                 + QString::number(info.lastModified().toMSecsSinceEpoch()) + QLatin1Char('\n');
  }
  return signature;
}

QString ProjectView::listingSignature() const
{
  if (!m_valid) return QString();
  return signatureOf(m_projPath, misc::projectFiles(QDir(m_projPath)));
}

void ProjectView::applyRefreshSettings()
{
  const int seconds = qBound(1, QucsSettings.ContentRefreshSeconds, 3600);
  m_pollTimer->setInterval(seconds * 1000);
  if (QucsSettings.ContentAutoRefresh)
    m_pollTimer->start();
  else
    m_pollTimer->stop();
  // The rows are built with these.
  if (m_folderIcons != QucsSettings.ContentFolderIcons || m_patterns != categoryLines()) refresh();
}

bool ProjectView::autoRefreshEnabled() const
{
  return m_pollTimer->isActive();
}

void ProjectView::refreshIfChanged()
{
  if (!m_valid) return;
  // Not while the simulator is writing its scratch files (the run's end
  // refreshes the panel anyway), not under an open menu, whose actions
  // refer to the rows as they are, and not during a drag from the panel.
  if (QucsMain != nullptr && QucsMain->simulationConsole() != nullptr
      && QucsMain->simulationConsole()->isRunning())
    return;
  if (QApplication::activePopupWidget() != nullptr || state() == DraggingState)
    return;
  // Looked at aside, not on the window's thread: a big folder took seconds
  // - half a minute for a home folder - every few seconds, and the window
  // never answered. One look at a time.
  if (m_looking) return;
  m_looking = true;
  const QString path = m_projPath;
  auto promise = std::make_shared<QPromise<QString>>();
  auto* watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, path] {
    watcher->deleteLater();
    m_looking = false;
    if (!m_valid || path != m_projPath) return;   // another project meanwhile
    if (QApplication::activePopupWidget() != nullptr || state() == DraggingState) return;   // (the next look)
    if (watcher->result() != m_signature) refresh();
  });
  watcher->setFuture(promise->future());
  promise->start();
  QThreadPool::globalInstance()->start([promise, path] {
    promise->addResult(signatureOf(path, misc::projectFiles(QDir(path))));
    promise->finish();
  });
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
  collect(m_model->item(rowOf(Schematics), 0));
  return list;
}
